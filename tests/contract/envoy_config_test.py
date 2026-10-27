"""Envoy config generator contract.

The edge proxy needs real bounds: a finite per-request route timeout, a cap on
any client-supplied grpc-timeout header, and a load-balancing policy that steers
away from a busy single-threaded worker. These are pure config-generation
assertions -- no Envoy binary or running server is required. The test invokes
``scripts/envoy_config.py`` the same way the process manager does (with a workers
arg) and inspects the emitted config. The process manager
(``tools/quantra-manager/quantra``) carries its own inline copy of the
generator; it must emit the same config.
"""

import importlib.machinery
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
ENVOY_CONFIG_SCRIPT = REPO_ROOT / "scripts" / "envoy_config.py"
MANAGER_SCRIPT = REPO_ROOT / "tools" / "quantra-manager" / "quantra"


def _generate(*extra_args, workers=4):
    """Run the generator and return the parsed config dict."""
    cmd = [
        sys.executable,
        str(ENVOY_CONFIG_SCRIPT),
        "--workers",
        str(workers),
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


@pytest.mark.parametrize("workers,choices", [(1, 4), (4, 16), (8, 32)])
def test_least_request_samples_four_choices_per_worker(workers, choices):
    # LEAST_REQUEST compares choice_count workers sampled with replacement
    # (Envoy 1.28 has no full scan). The default of 2 sends a request behind a
    # busy single-threaded worker with probability (k/n)^2 when k of n are
    # busy; 4 samples per worker brings that to (k/n)^(4n).
    cluster = _cluster(_generate(workers=workers))
    assert cluster["least_request_lb_config"] == {"choice_count": choices}


def _load_module(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


@pytest.mark.parametrize("workers", [1, 4, 8])
def test_manager_inline_config_matches_generator(workers):
    pytest.importorskip("yaml")  # the manager imports PyYAML at module level
    generator = _load_module("envoy_config_under_test", ENVOY_CONFIG_SCRIPT)
    manager = _load_module("quantra_manager_under_test", MANAGER_SCRIPT)
    expected = generator.generate_envoy_config(
        port=50051, base_port=50055, workers=workers, admin_port=9901
    )
    assert manager.generate_envoy_config(50051, 50055, workers, 9901) == expected


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
