"""QUANTRA_MAX_MESSAGE_BYTES: the largest gRPC message a worker takes, live.

Requests are basis swap books built from the multi-curve example (one-year
swaps, copied until the message has the size wanted) and compiled with flatc.

Covered:
- unset: a worker refuses a request above 8 MiB with RESOURCE_EXHAUSTED
  "Received message larger than max (... vs. 8388608)", today's behaviour;
- 1 MiB: a 1.2 MB request is refused naming 1048576, a 0.5 MB one prices,
  and the worker logs the value it uses;
- an invalid value logs a line and keeps 8 MiB;
- through the process manager and Envoy (2 workers) at 16 MiB: a 9 MB request
  prices, so the manager passes the variable to the workers and Envoy has no
  limit of its own below it.
"""

import copy
import json
import os
import shutil
import signal
import subprocess
import time
from pathlib import Path

import pytest

grpc = pytest.importorskip("grpc")
pytest.importorskip("yaml")

from load_control_test import JSON_SERVER, SYNC_SERVER, Cluster, _flatc, _poll  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
FBS_DIR = REPO_ROOT / "flatbuffers" / "fbs"
EXAMPLE = (REPO_ROOT / "examples" / "data" / "ir_swaps" /
           "basis_eur_5y_3m_vs_6m_ois_discounted_multicurve.json")
BASIS_RPC = "/quantra.QuantraServer/PriceBasisSwap"
DEFAULT_LIMIT = 8 * 1024 * 1024
OPTIONS = [("grpc.max_send_message_length", -1), ("grpc.max_receive_message_length", -1)]

pytestmark = pytest.mark.skipif(not (SYNC_SERVER.is_file() and JSON_SERVER.is_file()),
                                reason="needs the built engine (run inside the test image)")


@pytest.fixture(scope="module")
def book(tmp_path_factory):
    """book(min_bytes) -> the flatbuffer of a basis swap request of at least that size."""
    tmp = tmp_path_factory.mktemp("book")
    request = json.loads(EXAMPLE.read_text())
    swap = request["swaps"][0]
    for leg in ("leg1", "leg2"):
        swap["basis_swap"][leg]["schedule"]["termination_date"] = "2026-01-17"
    cache = {}

    def build(count):
        if count not in cache:
            body = dict(request, swaps=[copy.deepcopy(swap) for _ in range(count)])
            path = tmp / f"book_{count}.json"
            path.write_text(json.dumps(body))
            subprocess.run([_flatc(), "--binary", "-I", str(FBS_DIR), "-o", str(tmp),
                            str(FBS_DIR / "price_basis_swap_request.fbs"), str(path)],
                           check=True, capture_output=True, text=True)
            cache[count] = (tmp / f"book_{count}.bin").read_bytes()
        return cache[count]

    per_trade = (len(build(101)) - len(build(1))) / 100

    def of_size(min_bytes):
        count = max(1, int((min_bytes - len(build(1))) / per_trade) + 1)
        while len(build(count)) < min_bytes:
            count = int(count * 1.01) + 1
        return build(count)

    return of_size


class Worker:
    """One sync_server on its own port, with an environment of its own."""

    def __init__(self, tmp_path, port, value):
        env = {k: v for k, v in os.environ.items() if k != "QUANTRA_MAX_MESSAGE_BYTES"}
        if value is not None:
            env["QUANTRA_MAX_MESSAGE_BYTES"] = value
        self.log_path = tmp_path / f"worker{port}.log"
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([str(SYNC_SERVER), str(port)], env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        self.channel = grpc.insecure_channel(f"127.0.0.1:{port}", options=OPTIONS)
        grpc.channel_ready_future(self.channel).result(timeout=15)

    def call(self, data):
        """(status code name, details) of one basis swap call."""
        try:
            self.channel.unary_unary(BASIS_RPC)(data, timeout=60)
            return "OK", ""
        except grpc.RpcError as e:
            return e.code().name, e.details() or ""

    def output(self):
        self.log.flush()
        return self.log_path.read_text()

    def stop(self):
        self.channel.close()
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.log.close()


@pytest.fixture
def worker(tmp_path):
    started = []

    def start(port, value):
        started.append(Worker(tmp_path, port, value))
        return started[-1]

    yield start
    for w in started:
        w.stop()


def test_unset_keeps_eight_mib(worker, book):
    w = worker(50470, None)
    status, details = w.call(book(DEFAULT_LIMIT + 1024))
    assert status == "RESOURCE_EXHAUSTED"
    assert "larger than max" in details and f"vs. {DEFAULT_LIMIT})" in details
    assert "QUANTRA_MAX_MESSAGE_BYTES" not in w.output()
    assert "max message" not in w.output()


def test_a_set_value_is_the_worker_limit(worker, book):
    w = worker(50471, "1048576")
    status, details = w.call(book(1_200_000))
    assert status == "RESOURCE_EXHAUSTED"
    assert "larger than max" in details and "vs. 1048576)" in details
    assert w.call(book(500_000)) == ("OK", "")
    assert "[engine] max message 1048576 bytes" in w.output()


@pytest.mark.parametrize("value", ["8MB", "0", "-5", "4294967296"])
def test_an_invalid_value_logs_and_keeps_the_default(worker, book, value):
    port = 50472 + ["8MB", "0", "-5", "4294967296"].index(value)
    w = worker(port, value)
    status, details = w.call(book(DEFAULT_LIMIT + 1024))
    assert status == "RESOURCE_EXHAUSTED" and f"vs. {DEFAULT_LIMIT})" in details
    _poll(lambda: "QUANTRA_MAX_MESSAGE_BYTES" in w.output(), 5, "the warning line")
    assert f"QUANTRA_MAX_MESSAGE_BYTES='{value}'" in w.output()
    assert f"using {DEFAULT_LIMIT} bytes" in w.output()


@pytest.mark.skipif(not shutil.which("envoy"), reason="needs envoy")
def test_through_the_manager_and_envoy(tmp_path, book):
    cluster = Cluster(tmp_path, 13, env={"QUANTRA_MAX_MESSAGE_BYTES": str(16 * 1024 * 1024)})
    try:
        data = book(9 * 1000 * 1000)
        assert len(data) > DEFAULT_LIMIT
        channel = grpc.insecure_channel(f"127.0.0.1:{cluster.port}", options=OPTIONS)
        start = time.monotonic()
        try:
            response = channel.unary_unary(BASIS_RPC)(data, timeout=120)
        finally:
            channel.close()
        assert len(response) > 0
        assert time.monotonic() - start < 120
    finally:
        cluster.stop()
