"""QUANTRA_ENVOY_ADMIN parse contract for GET /status.

The Dockerfile defaults QUANTRA_ENVOY_ADMIN to a scheme-prefixed URL
(http://127.0.0.1:9901), so /status must accept both host:port and URL forms
of the variable and only report "invalid ... target" for genuinely malformed
values. The env var is read once at startup, so each scenario spawns its own
json_server (against the harness's gRPC backend) on a dedicated HTTP port.

Envoy itself is not running during the suite: a successfully parsed target
shows up as reachable=false with a connect error, never the invalid-target
message. That split (parse failure vs connect failure) is the contract.
"""

import os
import subprocess
import time
from pathlib import Path

import pytest
import requests

JSON_SERVER = Path(__file__).resolve().parents[2] / "build" / "jsonserver" / "json_server"
STATUS_PORT = 8099


def _envoy_status(env_value):
    """Spawn json_server with QUANTRA_ENVOY_ADMIN=env_value, return /status envoy section."""
    env = dict(os.environ, QUANTRA_ENVOY_ADMIN=env_value)
    proc = subprocess.Popen(
        [str(JSON_SERVER), "localhost:50051", str(STATUS_PORT)],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    base = f"http://localhost:{STATUS_PORT}"
    try:
        for _ in range(50):
            if proc.poll() is not None:
                pytest.fail(f"json_server exited early (rc={proc.returncode})", pytrace=False)
            try:
                if requests.get(f"{base}/health", timeout=0.2).ok:
                    break
            except requests.RequestException:
                time.sleep(0.1)
        else:
            pytest.fail("json_server did not become healthy", pytrace=False)
        r = requests.get(f"{base}/status", timeout=5)
        assert r.status_code == 200
        return r.json()["envoy"]
    finally:
        proc.terminate()
        proc.wait(timeout=5)


@pytest.mark.skipif(not JSON_SERVER.is_file(), reason="json_server binary not built")
@pytest.mark.parametrize(
    "target",
    [
        "127.0.0.1:9901",
        "http://127.0.0.1:9901",
        "http://127.0.0.1:9901/",
    ],
)
def test_envoy_admin_target_parses(target):
    envoy = _envoy_status(target)
    assert envoy["configured"] is True
    assert envoy["admin_target"] == target
    # Envoy is not running: parsing succeeded iff the failure is a connect
    # error rather than the invalid-target message.
    assert envoy["reachable"] is False
    assert "invalid QUANTRA_ENVOY_ADMIN" not in envoy.get("error", "")


@pytest.mark.skipif(not JSON_SERVER.is_file(), reason="json_server binary not built")
def test_envoy_admin_target_malformed_rejected():
    envoy = _envoy_status("not-a-target")
    assert envoy["reachable"] is False
    assert "invalid QUANTRA_ENVOY_ADMIN" in envoy.get("error", "")
