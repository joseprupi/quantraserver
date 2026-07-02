"""Fixtures for the functional parity catalog suite (tests/functional).

This suite reuses the contract harness — tests/contract/ql_reference.py
provides the ApiClient and the independent QuantLib reference pricers — so
tests/contract is added to sys.path here.

The gate (tests/run_all_tests.sh, suite 3) collects tests/contract and
tests/functional in one pytest invocation. Both directories' conftests are
then "initial" conftests, so both pytest_addoption hooks run; registration is
tolerant of the option already existing so either directory can also be run
standalone with the same --url/--data-dir flags.
"""

import sys
from pathlib import Path

import pytest

TESTS_DIR = Path(__file__).resolve().parents[1]
CONTRACT_DIR = TESTS_DIR / "contract"
if str(CONTRACT_DIR) not in sys.path:
    sys.path.insert(0, str(CONTRACT_DIR))

from ql_reference import ApiClient  # noqa: E402  (needs sys.path above)


def _addoption(parser, *args, **kwargs):
    """Register a CLI option, tolerating prior registration.

    When tests/contract and tests/functional are collected together, whichever
    conftest loads second would otherwise crash re-registering --url/--data-dir.
    """
    try:
        parser.addoption(*args, **kwargs)
    except ValueError:
        pass


def pytest_addoption(parser):
    _addoption(
        parser,
        "--url",
        action="store",
        default="http://localhost:8080",
        help="Base URL of the running JSON API server.",
    )
    _addoption(
        parser,
        "--data-dir",
        action="store",
        default=str(TESTS_DIR.parent / "examples" / "data"),
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
