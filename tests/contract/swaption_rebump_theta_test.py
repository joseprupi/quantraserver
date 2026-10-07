"""Swaption rebump theta pinned to its definition.

`swaption_pricing_rebump` theta is NPV(market rolled one calendar day forward)
minus NPV(as of). The roll is a CLEAN roll of the whole market: the evaluation
date moves AND every explicit `reference_date` (curves, helper curves, vol
surfaces) moves with it, so the curves are rebuilt one day later with the same
quotes. Date-anchored market DATA (zero/discount/forward points pinned to a
date, fixings) is not shifted: those dates describe the observed market, not
the observer, and a rolled curve re-reads the same data from its new origin.

The test therefore re-derives theta from two PLAIN API calls (no greek flags):

    theta == NPV(as_of_date + 1 and every reference_date + 1) - NPV(original)

for a helper-bootstrapped curve (EUR) and a value curve with explicit
reference dates (USD, InterpolatedZero points). Both fixtures use a constant
swaption vol, so shifting the vol surface reference date is exactly neutral and
the identity must hold to floating-point precision.
"""

import copy
import datetime as dt

import pytest

from ql_reference import load_json

FIXTURES = [
    "swaption/swpt_eur_1y5y_payer_near_atm_physical.json",
    "swaption_ois_bbg_zerorate_request.json",
]
VALUE_CURVE_FIXTURE = FIXTURES[1]
ABS_TOL = 1e-6


def _with_options(request, **flags):
    out = copy.deepcopy(request)
    out["pricing"].setdefault("options", {}).update(flags)
    return out


def _row(client, request):
    response = client.price("swaption", request)
    rows = response.get("swaptions")
    assert isinstance(rows, list) and len(rows) == 1, response
    return rows[0]


def _shift_iso(date_str, days):
    return (dt.date.fromisoformat(date_str) + dt.timedelta(days=days)).isoformat()


def _rolled_request(request, days):
    """as_of_date and every explicit `reference_date` move by `days`; nothing
    else (point dates, fixings, schedules) is touched."""
    out = copy.deepcopy(request)
    out["pricing"]["as_of_date"] = _shift_iso(out["pricing"]["as_of_date"], days)
    shifted = []

    def walk(node, path):
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "reference_date" and isinstance(value, str):
                    node[key] = _shift_iso(value, days)
                    shifted.append(path + "/" + key)
                else:
                    walk(value, path + "/" + key)
        elif isinstance(node, list):
            for i, value in enumerate(node):
                walk(value, f"{path}[{i}]")

    walk(out["pricing"], "pricing")
    assert shifted, "fixture carries no explicit reference_date; test is vacuous"
    return out


@pytest.mark.parametrize("filename", FIXTURES)
def test_rebump_theta_equals_one_day_clean_roll(client, data_dir, filename):
    request = load_json(data_dir / filename)

    base = _row(client, request)
    rolled = _row(client, _rolled_request(request, 1))
    rebump = _row(client, _with_options(request, swaption_pricing_rebump=True))

    expected = rolled["npv"] - base["npv"]
    assert "theta" in rebump, f"{filename}: rebump response lacks theta: {rebump}"
    tol = max(ABS_TOL, 1e-6 * abs(base["npv"]))
    assert rebump["theta"] == pytest.approx(expected, abs=tol), (
        f"{filename}: rebump theta {rebump['theta']} vs clean one-day roll "
        f"{expected} (npv {base['npv']} -> {rolled['npv']})"
    )
    print(f"[theta] {filename}: npv {base['npv']:.6f} -> {rolled['npv']:.6f}, "
          f"theta {rebump['theta']:.6f}")


def test_rebump_theta_present_and_nonzero_for_value_curve(client, data_dir):
    """A value curve with an explicit reference_date must roll too: if the
    curve origin stayed pinned to as-of the rebuilt market would be identical
    and theta would collapse to exactly 0.0."""
    request = load_json(data_dir / VALUE_CURVE_FIXTURE)
    rebump = _row(client, _with_options(request, swaption_pricing_rebump=True))
    assert "theta" in rebump
    assert rebump["theta"] != 0.0, f"theta is exactly 0.0: market did not roll: {rebump}"
    assert rebump["theta"] < 0.0, f"a long option must decay: {rebump['theta']}"
