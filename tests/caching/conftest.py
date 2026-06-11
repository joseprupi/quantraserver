"""Shared pytest fixtures for the curve-cache correctness suite (Suite 6).

Posts each example request to two JSON servers — one with the curve cache OFF
(the default :8080 server) and one with it ON (QUANTRA_CURVE_CACHE_ENABLED=1) —
and asserts the responses are bit-for-bit identical. Cache transparency is the
property under test, so no QuantLib reference is needed here (that is Suite 3):
the cache-OFF server is itself the reference.

Both servers are launched by run_all_tests.sh (start_servers / the cache-ON
start_cache_servers) before pytest is invoked; --url-nocache / --url-cache /
--data-dir / --cache-log mirror the flags the other suites use.
"""

import sys
from pathlib import Path

import pytest

# Reuse the contract suite's ApiClient (it parses the product->route catalog
# from common/product_catalog.h) and its load_json helper rather than
# duplicating that machinery here.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "contract"))
from ql_reference import ApiClient, load_json  # noqa: E402,F401  (re-exported)


def pytest_addoption(parser):
    parser.addoption(
        "--url-nocache",
        action="store",
        default="http://localhost:8080",
        help="Base URL of the cache-OFF JSON server (the reference).",
    )
    parser.addoption(
        "--url-cache",
        action="store",
        default="http://localhost:8081",
        help="Base URL of the cache-ON JSON server (QUANTRA_CURVE_CACHE_ENABLED=1).",
    )
    parser.addoption(
        "--data-dir",
        action="store",
        default=str(Path(__file__).resolve().parents[2] / "examples" / "data"),
        help="Directory holding the example JSON request files.",
    )
    parser.addoption(
        "--cache-log",
        action="store",
        default="/tmp/grpc_cache.log",
        help="Path to the cache-ON gRPC server log (holds CurveCache hit/miss lines).",
    )


@pytest.fixture(scope="session")
def data_dir(pytestconfig) -> Path:
    return Path(pytestconfig.getoption("--data-dir"))


@pytest.fixture(scope="session")
def cache_log_path(pytestconfig) -> Path:
    return Path(pytestconfig.getoption("--cache-log"))


@pytest.fixture(scope="session")
def nocache_client(pytestconfig) -> ApiClient:
    """cache-OFF server (the reference); fails the run fast if it is down."""
    api = ApiClient(pytestconfig.getoption("--url-nocache"))
    if not api.health():
        pytest.fail(
            f"Cannot connect to cache-OFF JSON server at {api.base_url} "
            "(is the server running?)",
            pytrace=False,
        )
    return api


@pytest.fixture(scope="session")
def cache_client(pytestconfig) -> ApiClient:
    """cache-ON server; fails the run fast if it is down."""
    api = ApiClient(pytestconfig.getoption("--url-cache"))
    if not api.health():
        pytest.fail(
            f"Cannot connect to cache-ON JSON server at {api.base_url} "
            "(is the server running?)",
            pytrace=False,
        )
    return api


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    """Emit the greppable case count run_all_tests.sh parses for Suite 6.

    The count is the number of cache-OFF vs cache-ON comparisons that passed
    (one per CASES row, plus the beyond-pillar error-transparency case).
    Only emitted on a fully green run, mirroring the
    Suite 0 boundary guard's "OK (N ... checked)" convention; on failure the
    runner reports the count as unknown.
    """
    if exitstatus != 0:
        return
    passed = terminalreporter.stats.get("passed", [])
    comparisons = sum(1 for rep in passed if "test_cache_transparency" in rep.nodeid)
    terminalreporter.write_line(
        f"Cache transparency OK ({comparisons} comparisons checked)"
    )
