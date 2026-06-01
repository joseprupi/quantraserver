"""Inflation endpoint contract/smoke tests.

Wraps the ported legacy checks (contract_checks.check_*) and asserts on the
returned result dict, preserving every original assertion:

  * test_bootstrap_inflation_curves_smoke           — inflation bootstrap
    endpoint exposure + response shape.
  * test_bootstrap_inflation_curves_rejects_legacy_payload — breaking-change
    guard: old generic inflation pillar payload must be rejected.
  * test_zero_coupon_inflation_swap_smoke           — ZCIIS npv/fair_rate +
    flow counts.
  * test_year_on_year_inflation_swap_smoke          — YYIIS npv/fair_rate/
    fair_spread + flow counts.
"""

from contract_checks import (
    check_bootstrap_inflation_curves_rejects_legacy_payload,
    check_bootstrap_inflation_curves_smoke,
    check_price_year_on_year_inflation_swap_smoke,
    check_price_zero_coupon_inflation_swap_smoke,
)


def test_bootstrap_inflation_curves_smoke(client):
    result = check_bootstrap_inflation_curves_smoke(client)
    assert result["passed"], result["error"]


def test_bootstrap_inflation_curves_rejects_legacy_payload(client):
    result = check_bootstrap_inflation_curves_rejects_legacy_payload(client)
    assert result["passed"], result["error"]


def test_zero_coupon_inflation_swap_smoke(client):
    result = check_price_zero_coupon_inflation_swap_smoke(client)
    assert result["passed"], result["error"]


def test_year_on_year_inflation_swap_smoke(client):
    result = check_price_year_on_year_inflation_swap_smoke(client)
    assert result["passed"], result["error"]
