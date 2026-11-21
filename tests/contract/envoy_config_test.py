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


# ---------------------------------------------------------------------------
# Load control: --max-requests, --max-pending, --retries, --stats-bind/-port.
# With none of them set, both generators must emit exactly the config they
# emitted before the knobs existed: tests/contract/data/envoy_config_golden
# holds that output (yaml.dump, as both writers do), captured from the
# generators before the knobs were added.
# ---------------------------------------------------------------------------
import shutil  # noqa: E402

GOLDEN_DIR = Path(__file__).resolve().parent / "data" / "envoy_config_golden"
LOAD_CONTROL_ENV = ("QUANTRA_MAX_REQUESTS", "QUANTRA_MAX_PENDING", "QUANTRA_RETRIES",
                    "QUANTRA_STATS_BIND", "QUANTRA_STATS_PORT")
GOLDEN_CASES = [("w1", 1, None), ("w2", 2, None), ("w4", 4, None), ("w8", 8, None),
                ("w4_rt90000", 4, "90000")]


@pytest.fixture
def clean_env(monkeypatch):
    for name in LOAD_CONTROL_ENV + ("QUANTRA_ROUTE_TIMEOUT_MS",):
        monkeypatch.delenv(name, raising=False)
    return monkeypatch


def _modules(tag):
    pytest.importorskip("yaml")
    generator = _load_module(f"envoy_config_lc_{tag}", ENVOY_CONFIG_SCRIPT)
    manager = _load_module(f"quantra_manager_lc_{tag}", MANAGER_SCRIPT)
    return generator, manager


def _both(generator, manager, workers=2, **knobs):
    a = generator.generate_envoy_config(port=50051, base_port=50055, workers=workers,
                                        admin_port=9901, **knobs)
    b = manager.generate_envoy_config(50051, 50055, workers, 9901, **knobs)
    assert a == b
    return a


@pytest.mark.parametrize("name,workers,route_timeout_ms", GOLDEN_CASES)
@pytest.mark.parametrize("explicit_defaults", [False, True])
def test_default_config_is_byte_identical_to_golden(clean_env, name, workers,
                                                    route_timeout_ms, explicit_defaults):
    import yaml
    if route_timeout_ms:
        clean_env.setenv("QUANTRA_ROUTE_TIMEOUT_MS", route_timeout_ms)
    if explicit_defaults:
        # The knobs set to their default values change nothing either.
        for var, value in (("QUANTRA_MAX_REQUESTS", "10000"), ("QUANTRA_MAX_PENDING", "10000"),
                           ("QUANTRA_RETRIES", "0"), ("QUANTRA_STATS_BIND", ""),
                           ("QUANTRA_STATS_PORT", "9902")):
            clean_env.setenv(var, value)
    generator, manager = _modules(f"golden_{name}_{explicit_defaults}")
    golden = (GOLDEN_DIR / f"{name}.yaml").read_text()
    text_generator = yaml.dump(
        generator.generate_envoy_config(port=50051, base_port=50055, workers=workers,
                                        admin_port=9901),
        default_flow_style=False)
    text_manager = yaml.dump(manager.generate_envoy_config(50051, 50055, workers, 9901),
                             default_flow_style=False)
    assert text_generator == golden
    assert text_manager == golden


def test_default_cli_output_is_golden(clean_env, tmp_path):
    out = tmp_path / "envoy.yaml"
    subprocess.run([sys.executable, str(ENVOY_CONFIG_SCRIPT), "--workers", "4", "-o", str(out)],
                   check=True, capture_output=True, text=True, env=dict(os.environ))
    assert out.read_text() == (GOLDEN_DIR / "w4.yaml").read_text()


def test_load_control_code_is_identical_in_both_generators():
    marker_begin, marker_end = "# --- BEGIN load control", "# --- END load control ---"

    def block(path):
        text = path.read_text()
        return text[text.index(marker_begin):text.index(marker_end)]

    assert block(ENVOY_CONFIG_SCRIPT) == block(MANAGER_SCRIPT)


def _hcm(config):
    return config["static_resources"]["listeners"][0]["filter_chains"][0]["filters"][0][
        "typed_config"]


def _routes(config):
    return _hcm(config)["route_config"]["virtual_hosts"][0]["routes"]


def _clusters(config):
    return {c["name"]: c for c in config["static_resources"]["clusters"]}


def test_max_requests_sets_limit_mapper_and_meta_route(clean_env):
    generator, manager = _modules("maxreq")
    config = _both(generator, manager, max_requests=2)
    clusters = _clusters(config)
    assert set(clusters) == {"quantra_workers", "quantra_meta"}
    thresholds = clusters["quantra_workers"]["circuit_breakers"]["thresholds"][0]
    assert thresholds["max_requests"] == 2
    assert thresholds["track_remaining"] is True
    assert thresholds["max_pending_requests"] == 10000
    # Envoy's overflow refusal (response flag UO) becomes RESOURCE_EXHAUSTED.
    mapper = _hcm(config)["local_reply_config"]["mappers"][0]
    assert mapper["filter"] == {"response_flag_filter": {"flags": ["UO"]}}
    assert mapper["status_code"] == 429
    assert mapper["headers_to_add"][0]["header"] == {"key": "grpc-status", "value": "8"}
    # Meta goes first to its own cluster on the same workers, default limits.
    routes = _routes(config)
    assert routes[0]["match"] == {"path": "/quantra.QuantraServer/Meta"}
    assert routes[0]["route"]["cluster"] == "quantra_meta"
    assert routes[1]["match"] == {"prefix": "/"}
    assert routes[1]["route"]["cluster"] == "quantra_workers"
    assert routes[0]["route"]["timeout"] == routes[1]["route"]["timeout"]
    meta = clusters["quantra_meta"]
    assert meta["load_assignment"]["endpoints"] == \
        clusters["quantra_workers"]["load_assignment"]["endpoints"]
    assert meta["load_assignment"]["cluster_name"] == "quantra_meta"
    meta_thresholds = meta["circuit_breakers"]["thresholds"][0]
    assert meta_thresholds["max_requests"] == 10000
    assert "track_remaining" not in meta_thresholds


def test_meta_rpc_path_matches_the_service_definition():
    fbs = (REPO_ROOT / "flatbuffers" / "grpc" / "quantraserver.fbs").read_text()
    assert re.search(r"^namespace quantra;", fbs, re.M)
    assert re.search(r"rpc_service QuantraServer\s*\{", fbs)
    assert re.search(r"^\s*Meta\(MetaRequest\):MetaResponse;", fbs, re.M)
    generator = _load_module("envoy_config_meta_path", ENVOY_CONFIG_SCRIPT)
    assert generator.META_RPC_PATH == "/quantra.QuantraServer/Meta"


def test_max_pending_changes_only_the_pending_breaker(clean_env):
    generator, manager = _modules("maxpend")
    default = _both(generator, manager)
    config = _both(generator, manager, max_pending=7)
    thresholds = _cluster(config)["circuit_breakers"]["thresholds"][0]
    assert thresholds["max_pending_requests"] == 7
    thresholds["max_pending_requests"] = 10000
    assert config == default


def test_retries_add_a_crash_only_retry_policy(clean_env):
    generator, manager = _modules("retries")
    assert "retry_policy" not in _routes(_both(generator, manager))[0]["route"]
    config = _both(generator, manager, retries=2, max_requests=4)
    for route in _routes(config):
        policy = route["route"]["retry_policy"]
        assert policy["retry_on"] == "reset,connect-failure"
        assert policy["num_retries"] == 2
        assert policy["retry_host_predicate"][0]["name"] == \
            "envoy.retry_host_predicates.previous_hosts"
        assert policy["host_selection_retry_max_attempts"] >= 1
        assert "per_try_timeout" not in policy


def test_stats_listener_serves_only_stats_and_ready(clean_env):
    generator, manager = _modules("stats")
    config = _both(generator, manager, stats_bind="0.0.0.0", stats_port=9944)
    listeners = config["static_resources"]["listeners"]
    assert len(listeners) == 2
    stats = listeners[1]
    assert stats["address"]["socket_address"] == {"address": "0.0.0.0", "port_value": 9944}
    vhost = stats["filter_chains"][0]["filters"][0]["typed_config"]["route_config"][
        "virtual_hosts"][0]
    assert [r["match"]["path"] for r in vhost["routes"]] == ["/stats/prometheus", "/ready"]
    for r in vhost["routes"]:
        assert r["match"]["headers"] == [{"name": ":method", "string_match": {"exact": "GET"}}]
        assert r["route"]["cluster"] == "envoy_admin"
    admin = _clusters(config)["envoy_admin"]
    assert admin["load_assignment"]["endpoints"][0]["lb_endpoints"][0]["endpoint"]["address"][
        "socket_address"] == {"address": "127.0.0.1", "port_value": 9901}
    # The admin interface itself stays on loopback.
    assert config["admin"]["address"]["socket_address"]["address"] == "127.0.0.1"


@pytest.mark.parametrize("env,kwargs", [
    ({"QUANTRA_MAX_REQUESTS": "3"}, {"max_requests": 3}),
    ({"QUANTRA_MAX_PENDING": "5"}, {"max_pending": 5}),
    ({"QUANTRA_RETRIES": "1"}, {"retries": 1}),
    ({"QUANTRA_STATS_BIND": "0.0.0.0"}, {"stats_bind": "0.0.0.0"}),
    ({"QUANTRA_STATS_BIND": "0.0.0.0", "QUANTRA_STATS_PORT": "9950"},
     {"stats_bind": "0.0.0.0", "stats_port": 9950}),
])
def test_env_knobs_match_the_arguments(clean_env, env, kwargs):
    generator, manager = _modules("envtwin")
    expected = _both(generator, manager, **kwargs)
    for name, value in env.items():
        clean_env.setenv(name, value)
    assert _both(generator, manager) == expected


def test_argument_wins_over_env_knob(clean_env):
    generator, manager = _modules("argwins")
    clean_env.setenv("QUANTRA_MAX_REQUESTS", "3")
    config = _both(generator, manager, max_requests=6)
    assert _cluster(config)["circuit_breakers"]["thresholds"][0]["max_requests"] == 6


@pytest.mark.parametrize("name,value", [
    ("QUANTRA_MAX_REQUESTS", "0"), ("QUANTRA_MAX_REQUESTS", "abc"),
    ("QUANTRA_MAX_PENDING", "-1"), ("QUANTRA_RETRIES", "-2"), ("QUANTRA_RETRIES", "1.5"),
    ("QUANTRA_STATS_PORT", "70000"),
])
def test_invalid_env_knob_logs_and_keeps_default(clean_env, capsys, name, value):
    generator, manager = _modules("envbad")
    clean_env.setenv(name, value)
    if name == "QUANTRA_STATS_PORT":
        clean_env.setenv("QUANTRA_STATS_BIND", "0.0.0.0")
        expected_port = 9902
        config = _both(generator, manager)
        assert config["static_resources"]["listeners"][1]["address"]["socket_address"][
            "port_value"] == expected_port
    else:
        clean_env.setenv(name, value)
        assert _both(generator, manager) == _both(generator, manager, **{
            "max_requests": 10000, "max_pending": 10000, "retries": 0})
    assert name in capsys.readouterr().err


def test_cli_flags_reach_the_config(clean_env):
    config = _generate("--max-requests", "2", "--max-pending", "3", "--retries", "1",
                       "--stats-bind", "0.0.0.0", "--stats-port", "9945", workers=2)
    thresholds = _cluster(config)["circuit_breakers"]["thresholds"][0]
    assert (thresholds["max_requests"], thresholds["max_pending_requests"]) == (2, 3)
    assert _routes(config)[1]["route"]["retry_policy"]["num_retries"] == 1
    assert config["static_resources"]["listeners"][1]["address"]["socket_address"][
        "port_value"] == 9945


def test_cli_rejects_negative_retries(clean_env):
    result = subprocess.run([sys.executable, str(ENVOY_CONFIG_SCRIPT), "--retries", "-1"],
                            capture_output=True, text=True)
    assert result.returncode != 0
    assert "retries" in result.stderr


def _envoy():
    exe = shutil.which("envoy")
    if not exe:
        pytest.skip("envoy not installed (run the suite inside the test image)")
    return exe


LOAD_CONTROL_COMBINATIONS = [
    {},
    {"max_requests": 2},
    {"max_pending": 3},
    {"retries": 1},
    {"stats_bind": "0.0.0.0"},
    {"max_requests": 2, "retries": 1},
    {"max_requests": 2, "stats_bind": "0.0.0.0"},
    {"retries": 2, "stats_bind": "127.0.0.1", "stats_port": 9960},
    {"max_requests": 8, "max_pending": 4, "retries": 3, "stats_bind": "0.0.0.0", "stats_port": 9961},
]


@pytest.mark.parametrize("knobs", LOAD_CONTROL_COMBINATIONS, ids=lambda k: ",".join(k) or "default")
def test_every_combination_passes_envoy_validate(clean_env, tmp_path, knobs):
    import yaml
    envoy = _envoy()
    generator, _ = _modules("validate")
    config = generator.generate_envoy_config(port=50051, base_port=50055, workers=2,
                                             admin_port=9901, **knobs)
    path = tmp_path / "envoy.yaml"
    path.write_text(yaml.dump(config, default_flow_style=False))
    result = subprocess.run([envoy, "--mode", "validate", "-c", str(path), "--log-level", "error"],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_manager_refuses_a_broken_envoy_config(tmp_path):
    _envoy()
    pytest.importorskip("yaml")
    broken = tmp_path / "broken.yaml"
    broken.write_text("admin: {address: {socket_address: {address: 127.0.0.1, port_value: 9979}}}\n"
                      "static_resources:\n  clusters:\n  - name: c\n    no_such_field: 1\n")
    state = tmp_path / "state"
    env = dict(os.environ, QUANTRA_STATE_DIR=str(state), QUANTRA_HOME=str(REPO_ROOT))
    result = subprocess.run(
        [sys.executable, str(MANAGER_SCRIPT), "start", "--workers", "1", "--port", "50291",
         "--base-port", "50295", "--admin-port", "9979", "--envoy-config", str(broken),
         "--foreground"],
        capture_output=True, text=True, env=env, timeout=60)
    assert result.returncode != 0
    assert "Envoy rejected" in result.stderr
    assert "no_such_field" in result.stderr  # Envoy's own message is printed
    assert not (state / "quantra.pid").exists()  # nothing was started


def test_manager_warns_when_a_custom_config_lacks_ports(tmp_path):
    generator, manager = _modules("customwarn")
    import yaml
    path = tmp_path / "custom.yaml"
    path.write_text(yaml.dump(generator.generate_envoy_config(
        port=50051, base_port=50055, workers=1, admin_port=9901)))
    assert manager.custom_envoy_config_warnings(path, 50051, 9901) == []
    warnings = manager.custom_envoy_config_warnings(path, 50061, 9911)
    assert any("50061" in w for w in warnings)
    assert any("9911" in w and "capacity" in w for w in warnings)
