"""Shared pytest fixtures for the JSON API contract/parity suite.

The suite POSTs the example requests under examples/data to a running JSON
server and compares the API NPVs against the independent QuantLib reference
pricers in ql_reference.py. The server is launched by run_all_tests.sh
(start_servers) before pytest is invoked; --url / --data-dir mirror the old
monolith CLI flags.
"""

from pathlib import Path

import pytest

from ql_reference import ApiClient


def pytest_addoption(parser):
    parser.addoption(
        "--url",
        action="store",
        default="http://localhost:8080",
        help="Base URL of the running JSON API server.",
    )
    parser.addoption(
        "--data-dir",
        action="store",
        default=str(Path(__file__).resolve().parents[2] / "examples" / "data"),
        help="Directory holding the example JSON request files.",
    )


@pytest.fixture(scope="session")
def data_dir(pytestconfig) -> Path:
    return Path(pytestconfig.getoption("--data-dir"))


@pytest.fixture(scope="session")
def client(pytestconfig) -> ApiClient:
    """Session-scoped API client; fails the run fast if the server is down."""
    api = ApiClient(pytestconfig.getoption("--url"))
    if not api.health():
        pytest.fail(
            f"Cannot connect to JSON API server at {api.base_url} "
            "(is the server running?)",
            pytrace=False,
        )
    return api
