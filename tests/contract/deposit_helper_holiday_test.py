"""Deposit helpers and a valuation date that is a holiday of their calendar.

Since QuantLib 1.44 the deposit/swap helpers count their settlement days from
the ACTUAL evaluation date even when it is a holiday. With `fixing_days > 0`
the deposit's fixing date then precedes the evaluation date and the bootstrap
needs a fixing of the helper's internal index, which the API cannot supply:
the engine now rejects that pillar with a 400 naming the remedies (the 400 row
lives in error_contract_test.py). These tests pin the two remedies:

  - `fixing_days: 0` on the deposit pillars still prices (200) and returns
    the same numbers as the previous release (captured from the 0.13.0
    container on the probe the upgrade notes cite);
  - a business-day valuation date is untouched.
"""

import copy
import json
from pathlib import Path

PROBES = Path(__file__).resolve().parents[1] / "version_diff" / "probes" / "price-vanilla-swap"


def _load(name):
    with open(PROBES / name) as fh:
        return json.load(fh)


def _post(client, product, request):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def test_holiday_as_of_prices_with_fixing_days_zero(client):
    req = _load("vanilla_as_of_on_target_holiday.json")
    req = copy.deepcopy(req)
    for pt in req["pricing"]["rates"]["curves"][0]["points"]:
        if pt["point_type"] == "DepositHelper":
            pt["point"]["fixing_days"] = 0
    r = _post(client, "vanilla_swap", req)
    assert r.status_code == 200, r.text[:300]
    swap = r.json()["swaps"][0]
    # Numbers returned by the 0.13.0 release for this exact request.
    assert abs(swap["fair_rate"] - 0.03) < 1e-12
    assert abs(swap["npv"]) < 1e-6
    assert abs(swap["legs"][0]["npv"] - (-57389.296863753894)) < 1e-6
    assert abs(swap["legs"][1]["npv"] - 57389.29686375372) < 1e-6


def test_business_day_as_of_is_unaffected(client):
    req = _load("vanilla_as_of_on_business_day_control.json")
    r = _post(client, "vanilla_swap", req)
    assert r.status_code == 200, r.text[:300]
    swap = r.json()["swaps"][0]
    assert abs(swap["fair_rate"] - 0.030002668649) < 1e-9
    assert abs(swap["npv"] - 5.104635382078) < 1e-6
