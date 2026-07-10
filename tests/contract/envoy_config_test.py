"""Envoy config generator contract.

The edge proxy needs real bounds: a finite per-request route timeout, a cap on
any client-supplied grpc-timeout header, and a load-balancing policy that steers
away from a busy single-threaded worker. These are pure config-generation
assertions -- no Envoy binary or running server is required. The test invokes
``scripts/envoy_config.py`` the same way the process manager does (with a workers
arg) and inspects the emitted config.
"""

import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ENVOY_CONFIG_SCRIPT = REPO_ROOT / "scripts" / "envoy_config.py"


def _generate(*extra_args):
    """Run the generator and return the parsed config dict."""
    cmd = [
        sys.executable,
        str(ENVOY_CONFIG_SCRIPT),
        "--workers",
        "4",
        "--format",
        "json",
        *extra_args,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def _route(config):
    listener = config["static_resources"]["listeners"][0]
    hcm = listener["filter_chains"][0]["filters"][0]["typed_config"]
    vhost = hcm["route_config"]["virtual_hosts"][0]
    return vhost["routes"][0]["route"]


def _cluster(config):
    return config["static_resources"]["clusters"][0]


def test_route_timeout_default_is_30s():
    route = _route(_generate())
    assert route["timeout"] == "30s"


def test_grpc_timeout_header_capped_at_60s():
    route = _route(_generate())
    assert route["max_stream_duration"]["grpc_timeout_header_max"] == "60s"


def test_lb_policy_is_least_request():
    cluster = _cluster(_generate())
    assert cluster["lb_policy"] == "LEAST_REQUEST"


def test_health_check_is_grpc():
    # The engine serves the standard grpc.health.v1.Health service, so Envoy
    # must probe it over gRPC (not a bare TCP connect). A gRPC probe reflects
    # actual serving status rather than just an open socket.
    cluster = _cluster(_generate())
    health_check = cluster["health_checks"][0]
    assert "grpc_health_check" in health_check
    assert "tcp_health_check" not in health_check


def test_request_timeout_override_changes_route_timeout():
    route = _route(_generate("--request-timeout", "12s"))
    assert route["timeout"] == "12s"
    # The cap on the client grpc-timeout header is independent of the route
    # timeout override.
    assert route["max_stream_duration"]["grpc_timeout_header_max"] == "60s"
