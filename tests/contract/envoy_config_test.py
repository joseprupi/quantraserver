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
import os
import re
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


def test_gateway_deadline_default_equals_route_timeout_default():
    # The JSON gateway's default RPC deadline (QUANTRA_GATEWAY_DEADLINE_MS,
    # clients/cpp) is the default route timeout, so neither cuts a request
    # the other would let through.
    header = (REPO_ROOT / "clients" / "cpp" / "include" / "quantra_client.h").read_text()
    m = re.search(r"constexpr long kDefaultGatewayDeadlineMs = (\d+);", header)
    assert m, "kDefaultGatewayDeadlineMs not found"
    generator = _load_module("envoy_config_for_deadline", ENVOY_CONFIG_SCRIPT)
    assert int(m.group(1)) == generator.DEFAULT_ROUTE_TIMEOUT_MS == 30000


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


def _generate_env(env_value, *extra_args):
    """Run the generator with QUANTRA_ROUTE_TIMEOUT_MS set (or removed when
    None); returns (config, stderr)."""
    env = dict(os.environ)
    env.pop("QUANTRA_ROUTE_TIMEOUT_MS", None)
    if env_value is not None:
        env["QUANTRA_ROUTE_TIMEOUT_MS"] = env_value
    cmd = [sys.executable, str(ENVOY_CONFIG_SCRIPT), "--workers", "2", "--format", "json",
           *extra_args]
    result = subprocess.run(cmd, capture_output=True, text=True, check=True, env=env)
    return json.loads(result.stdout), result.stderr


@pytest.mark.parametrize("value,expected,cap", [
    ("45000", "45s", "60s"), ("120000", "120s", "120s"), ("1500", "1.5s", "60s"),
    ("30000", "30s", "60s"), ("90500", "90.5s", "90.5s"),
])
def test_route_timeout_env_knob_sets_route_timeout(value, expected, cap):
    # Envoy bounds a gRPC request by the caller's grpc-timeout header capped at
    # grpc_timeout_header_max (the route timeout is not applied to it), so the
    # cap follows the knob above 60s: max(60s, route timeout).
    config, _ = _generate_env(value)
    route = _route(config)
    assert route["timeout"] == expected
    assert route["max_stream_duration"]["grpc_timeout_header_max"] == cap


@pytest.mark.parametrize("value", ["0", "-5", "abc", "30s", ""])
def test_route_timeout_env_knob_invalid_keeps_default(value):
    config, stderr = _generate_env(value)
    assert _route(config)["timeout"] == "30s"
    if value.strip():
        assert "QUANTRA_ROUTE_TIMEOUT_MS" in stderr


def test_request_timeout_flag_wins_over_env_knob():
    config, _ = _generate_env("45000", "--request-timeout", "12s")
    assert _route(config)["timeout"] == "12s"


@pytest.mark.parametrize("value", [None, "45000", "1500", "bogus"])
def test_manager_reads_the_same_route_timeout_knob(value, monkeypatch):
    pytest.importorskip("yaml")
    if value is None:
        monkeypatch.delenv("QUANTRA_ROUTE_TIMEOUT_MS", raising=False)
    else:
        monkeypatch.setenv("QUANTRA_ROUTE_TIMEOUT_MS", value)
    generator = _load_module("envoy_config_under_test_env", ENVOY_CONFIG_SCRIPT)
    manager = _load_module("quantra_manager_under_test_env", MANAGER_SCRIPT)
    assert manager.route_timeout_from_env() == generator.route_timeout_from_env()
    assert manager.generate_envoy_config(50051, 50055, 2, 9901) == \
        generator.generate_envoy_config(port=50051, base_port=50055, workers=2, admin_port=9901)
    expected = {None: "30s", "45000": "45s", "1500": "1.5s", "bogus": "30s"}[value]
    assert _route(manager.generate_envoy_config(50051, 50055, 2, 9901))["timeout"] == expected
    monkeypatch.setenv("QUANTRA_ROUTE_TIMEOUT_MS", "180000")
    route = _route(manager.generate_envoy_config(50051, 50055, 2, 9901))
    assert route["timeout"] == "180s"
    assert route["max_stream_duration"]["grpc_timeout_header_max"] == "180s"
