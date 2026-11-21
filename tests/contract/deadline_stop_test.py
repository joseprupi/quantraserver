"""A worker stops a long computation when the request's time is over.

One engine worker, no Envoy, and JSON gateways in front of it with different
deadlines (QUANTRA_GATEWAY_DEADLINE_MS). A structured note on a local-vol
surface takes several seconds; sent with a 1 s deadline it is answered 504 at
the deadline, and the worker stops the Monte Carlo run inside its sample loop:
a bond sent to the same single worker right after the 504 is priced at once
instead of waiting for the abandoned note. Then the same note with a long
deadline returns its usual NPV (nothing was left half-done), and the same
again with the engine's own budget (QUANTRA_REQUEST_BUDGET_MS) instead of
the caller's deadline. An Andreasen-Huge calibration (about 10 s on the
steep-wings sheet) stops between expiries the same way.
"""

import json
import os
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SYNC_SERVER = REPO_ROOT / "build" / "server" / "sync_server"
JSON_SERVER = REPO_ROOT / "build" / "jsonserver" / "json_server"
DATA = REPO_ROOT / "examples" / "data"
NOTE = DATA / "structured_note" / "synthetic_ndx_rty_5y_autocall_on_svi_flat_local_vol.json"
BOND = DATA / "bonds" / "fixed_rate_bond_request.json"
AH_SHEET = DATA / "vol_surface" / "synthetic_spx_steep_wings_sheet_svi.json"

# The note's NPV as 0.17.0 (before the checkpoint existed) prices it: a run
# the budget does not stop is unchanged bit for bit.
PINNED_NOTE_NPV = 954.052589479687
# An Andreasen-Huge calibration stops between expiries: the worker is free
# once the expiry in progress is done (well under a second on this sheet; the
# whole calibration takes about 10 s).
AH_FREE_AFTER_S = 1.5

pytestmark = pytest.mark.skipif(
    not (SYNC_SERVER.is_file() and JSON_SERVER.is_file()),
    reason="needs the built engine and gateway")


def _post(port, path, body, timeout=120):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


class Worker:
    """One engine worker and gateways (name -> deadline ms) in front of it."""

    def __init__(self, tmp_path, port, gateways, budget_ms=None):
        env = dict(os.environ)
        env.pop("QUANTRA_REQUEST_BUDGET_MS", None)
        if budget_ms is not None:
            env["QUANTRA_REQUEST_BUDGET_MS"] = str(budget_ms)
        self.log = open(tmp_path / f"worker{port}.log", "w")
        self.procs = [subprocess.Popen([str(SYNC_SERVER), str(port)], env=env,
                                       stdout=self.log, stderr=subprocess.STDOUT)]
        self.ports = {}
        for name, (http_port, deadline_ms) in gateways.items():
            genv = dict(os.environ, QUANTRA_GATEWAY_DEADLINE_MS=str(deadline_ms))
            genv.pop("QUANTRA_ENVOY_ADMIN", None)
            self.procs.append(subprocess.Popen(
                [str(JSON_SERVER), f"127.0.0.1:{port}", str(http_port)], env=genv,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            self.ports[name] = http_port
        bond = BOND.read_bytes()
        deadline = time.monotonic() + 30
        for name, http_port in self.ports.items():
            while True:
                try:
                    if _post(http_port, "/price-fixed-rate-bond", bond, timeout=5)[0] == 200:
                        break
                except OSError:
                    pass
                if time.monotonic() > deadline:
                    self.stop()
                    pytest.fail(f"gateway {name} / worker {port} did not come up", pytrace=False)
                time.sleep(0.05)

    def post(self, name, path, body):
        start = time.monotonic()
        code, text = _post(self.ports[name], path, body)
        return code, text, time.monotonic() - start

    def stop(self):
        for p in reversed(self.procs):
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        self.log.close()


def _worker_is_free_right_after(worker, gateway):
    """Seconds a bond sent right now takes on the single worker."""
    code, _, seconds = worker.post(gateway, "/price-fixed-rate-bond", BOND.read_bytes())
    assert code == 200
    return seconds


def test_client_deadline_stops_the_note_and_frees_the_worker(tmp_path):
    worker = Worker(tmp_path, 50461, {"short": (8061, 1000), "long": (8062, 60000)})
    try:
        note = NOTE.read_bytes()
        code, body, seconds = worker.post("short", "/price-structured-note", note)
        assert code == 504, body
        assert seconds < 3.0
        # The note takes several seconds; the worker stopped it at the deadline.
        assert _worker_is_free_right_after(worker, "long") < 1.0
        # Nothing half-done is kept: the next run is the usual one.
        code, body, _ = worker.post("long", "/price-structured-note", note)
        assert code == 200, body
        assert json.loads(body)["notes"][0]["npv"] == PINNED_NOTE_NPV
    finally:
        worker.stop()


def test_engine_budget_stops_the_note_and_frees_the_worker(tmp_path):
    worker = Worker(tmp_path, 50462, {"long": (8063, 60000)}, budget_ms=1000)
    try:
        code, body, seconds = worker.post("long", "/price-structured-note", NOTE.read_bytes())
        assert code == 504, body
        assert json.loads(body)["code_name"] == "DEADLINE_EXCEEDED"
        assert seconds < 3.0
        assert _worker_is_free_right_after(worker, "long") < 1.0
    finally:
        worker.stop()


def test_client_deadline_stops_an_andreasen_huge_calibration(tmp_path):
    request = json.loads(AH_SHEET.read_text())
    request["method"] = "AndreasenHuge"
    body = json.dumps(request).encode()
    worker = Worker(tmp_path, 50463, {"short": (8064, 1000), "long": (8065, 60000)})
    try:
        code, text, seconds = worker.post("short", "/calibrate-vol-surface", body)
        assert code == 504, text
        assert _worker_is_free_right_after(worker, "long") < AH_FREE_AFTER_S
    finally:
        worker.stop()
