"""Bootstrap-curves contract tests.

Wraps the ported legacy checks (contract_checks.check_*) and asserts on the
returned result dict, preserving every original assertion:

  * test_bootstrap_curves           — DF parity vs an independently built
    QuantLib PiecewiseLogLinearDiscount curve (max_diff < 1e-6).
  * test_bootstrap_curves_multicurve_exogenous — 2-curve build + exogenous
    discount wiring + DF shape (near 1.0, non-increasing).
  * test_bootstrap_curves_missing_dependency_fails — missing exogenous
    discount curve fails cleanly (HTTP non-200 or per-curve error).
"""

from contract_checks import (
    check_bootstrap_curves,
    check_bootstrap_curves_missing_dependency_fails,
    check_bootstrap_curves_multicurve_exogenous,
)


def test_bootstrap_curves(client, data_dir):
    result = check_bootstrap_curves(client, data_dir)
    assert result["passed"], result["error"]


def test_bootstrap_curves_multicurve_exogenous(client):
    result = check_bootstrap_curves_multicurve_exogenous(client)
    assert result["passed"], result["error"]


def test_bootstrap_curves_missing_dependency_fails(client):
    result = check_bootstrap_curves_missing_dependency_fails(client)
    assert result["passed"], result["error"]
