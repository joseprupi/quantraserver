"""Commodity swap: reference values, identities, flows and request contract.

`POST /price-commodity-swap` prices two legs, one paid and one received,
each a fixed price or the average over each period's pricing days of a
futures contract (nearby month, roll before the last trading date) or of a
spot price (forward prices by date), plus a spread:

    quantity  = period quantity (x pricing days with PerPricingDay)
    amount    = sign * quantity * (fixed price | average + spread)
    PV        = amount * DF(payment date)
    legs[i].npv = sum of PVs, npv = legs[0].npv + legs[1].npv

Reference case (synthetic data, tests/contract/data/commodity/
price_commodity_swap_benchmark_request.json): as-of 2026-11-13, a monthly
swap Nov-26..Oct-27 on 10,000 per period, 61.00 paid fixed against the front
futures month (roll 0 and roll 2) and against a spot price, paid 5 US
business days after each period end, discounted on dated discount factors
(log-linear, Act/365F). The pricing calendar is WeekendsOnly plus nine
`pricing_holidays`, so the object's own holidays are exercised. The values
are reference values computed independently on the same synthetic data.

  * NPVs, fair fixed prices, the first floating flow (20 pricing days, 9
    fixings, average, payment date, discount) and the number of flows;
  * observations: fixing days read the fixings, the roll day reads the
    right contract month, spot observations carry no contract month;
  * deltas: a finite difference of the NPV in one futures price and one
    spot point equals the reported delta;
  * identities: npv = sum of leg NPVs, a leg NPV = sum of its PVs, PV =
    amount * discount; repricing at the fair fixed price / fair spread gives
    a zero NPV;
  * the catalog examples (examples/data/commodity_swap/, synthetic data):
    every swap, flow, observation and delta against the independent
    reference ql_reference.commodity_swap_ql (1e-10 relative on values,
    1e-8 on deltas, dates / contract months / sources exactly); the
    reference alone reproduces the pinned benchmark values;
  * optional fields present only when they apply (never a 0 stand-in);
  * every validation is a 400 naming the field, unknown ids a 404.
"""

import copy
import json
from pathlib import Path

import pytest

from ql_reference import commodity_swap_ql

DATA = Path(__file__).resolve().parent / "data" / "commodity"
BENCHMARK = DATA / "price_commodity_swap_benchmark_request.json"

NPV_TOLERANCE = 1e-5
PRICE_TOLERANCE = 1e-9
DF_TOLERANCE = 1e-9

REFERENCE = [
    # (npv, fair fixed price, first-period average)
    (36038.838994, 61.3066474593, 60.025),
    (38048.121125, 61.3237440495, 60.065),
    (-15486.170077, 60.8682311958, 59.48375),
]
FIRST_DISCOUNT = 0.9973733188


# Catalog examples (examples/data/commodity_swap/), each compared in full
# with the independent reference ql_reference.commodity_swap_ql.
EXAMPLES = sorted(p.name for p in (Path(__file__).resolve().parents[2] / "examples" / "data"
                                   / "commodity_swap").glob("*.json"))
# Every number of the response against the reference: relative to the larger
# magnitude (absolute below 1). Both sides add the same day prices and
# multiply the same discount factors; the gaps sit at the last double digits.
VALUE_TOLERANCE = 1e-10
DELTA_TOLERANCE = 1e-8


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _benchmark():
    with open(BENCHMARK) as f:
        return json.load(f)


def _post(client, request):
    r = client.session.post(f"{client.base_url}/{client.ENDPOINTS['commodity_swap']}",
                            json=request)
    return r.status_code, r.text


def _price(client, request):
    status, text = _post(client, request)
    assert status == 200, f"expected 200, got {status}: {text[:500]}"
    return json.loads(text)


def _leg(request, i, leg):
    return request["swaps"][i]["commodity_swap"][f"leg{leg}"]


def _only(request, i):
    out = copy.deepcopy(request)
    out["swaps"] = [out["swaps"][i]]
    return out


def _contract(request):
    return request["pricing"]["commodity"]["contracts"][0]


def _spot(request):
    return request["pricing"]["commodity"]["spot_prices"][0]


def _assert_status(client, request, status_expected, *fragments):
    status, text = _post(client, request)
    assert status == status_expected, (
        f"expected {status_expected} naming {fragments}, got {status}: {text[:400]}")
    for fragment in fragments:
        assert fragment in text, f"{status} does not name {fragment!r}: {text[:400]}"
    return text


def _check_identities(row):
    total = 0.0
    for leg in row["legs"]:
        pv_sum = 0.0
        for flow in leg.get("flows", []):
            assert abs(flow["amount"] * flow["discount"] - flow["present_value"]) < 1e-6
            pv_sum += flow["present_value"]
        if "flows" in leg:
            assert abs(pv_sum - leg["npv"]) < 1e-6
        total += leg["npv"]
    assert abs(total - row["npv"]) < 1e-6


# ---------------------------------------------------------------------------
# reference values
# ---------------------------------------------------------------------------

def test_benchmark_reference_values(client):
    response = _price(client, _benchmark())
    rows = response["swaps"]
    assert len(rows) == 3
    for row, (npv, fair, average) in zip(rows, REFERENCE):
        assert abs(row["npv"] - npv) < NPV_TOLERANCE, f"npv {row['npv']!r} vs {npv!r}"
        assert abs(row["fair_fixed_price"] - fair) < PRICE_TOLERANCE, (
            f"fair_fixed_price {row['fair_fixed_price']!r} vs {fair!r}")
        assert "fair_spread" not in row
        assert len(row["legs"]) == 2
        for leg in row["legs"]:
            assert len(leg["flows"]) == 12
        first = row["legs"][1]["flows"][0]
        assert first["start_date"] == "2026-11-01"
        assert first["end_date"] == "2026-11-30"
        assert first["pricing_days"] == 20
        assert first["realised_days"] == 9
        assert abs(first["average"] - average) < 1e-10
        assert first["payment_date"] == "2026-12-07"
        assert abs(first["discount"] - FIRST_DISCOUNT) < DF_TOLERANCE
        assert len(first["observations"]) == 20
        assert "realised_average" in first and "expected_average" in first
        assert first["spread"] == 0.0
        assert "price" not in first
        fixed_first = row["legs"][0]["flows"][0]
        assert fixed_first["price"] == 61.0
        assert fixed_first["amount"] == -610000.0
        assert "average" not in fixed_first and "spread" not in fixed_first
        assert "observations" not in fixed_first or fixed_first["observations"] == []
        _check_identities(row)


def test_benchmark_observations(client):
    rows = _price(client, _benchmark())["swaps"]

    def obs(row, date):
        for o in row["legs"][1]["flows"][0]["observations"]:
            if o["date"] == date:
                return o
        raise AssertionError(f"no observation on {date}")

    roll0, roll2, spot = rows
    # Past days read the fixings of the month read.
    fixing = obs(roll0, "2026-11-02")
    assert fixing["source"] == "Fixing"
    assert fixing["contract_month"] == "2026-12"
    assert fixing["price"] == 59.0
    # Valuation day without a fixing: today's price.
    assert obs(roll0, "2026-11-13")["source"] == "Curve"
    # Roll 0: the expiring month is read on its last trading date (20 Nov),
    # the next month from the next pricing day.
    assert obs(roll0, "2026-11-20")["contract_month"] == "2026-12"
    assert obs(roll0, "2026-11-20")["price"] == 60.0
    assert obs(roll0, "2026-11-23")["contract_month"] == "2027-01"
    assert obs(roll0, "2026-11-23")["price"] == 60.4
    # Roll 2: two pricing days earlier (18 Nov is the roll date).
    assert obs(roll2, "2026-11-18")["contract_month"] == "2026-12"
    assert obs(roll2, "2026-11-19")["contract_month"] == "2027-01"
    # The own holiday (26 Nov) is not a pricing day.
    dates = [o["date"] for o in roll0["legs"][1]["flows"][0]["observations"]]
    assert "2026-11-26" not in dates and "2026-11-27" in dates
    # Spot price: no contract month; fixings, then interpolated forwards.
    s = obs(spot, "2026-11-12")
    assert s["source"] == "Fixing" and s["price"] == 59.8 and "contract_month" not in s
    assert obs(spot, "2026-11-16")["source"] == "Curve"


def test_benchmark_flows_and_observations_optional(client):
    request = _benchmark()
    request["include_observations"] = False
    for row in _price(client, request)["swaps"]:
        for leg in row["legs"]:
            assert len(leg["flows"]) == 12
            for flow in leg["flows"]:
                assert not flow.get("observations")
    request["include_flows"] = False
    for row in _price(client, request)["swaps"]:
        for leg in row["legs"]:
            assert not leg.get("flows")
        assert row["deltas"], "deltas are always reported"


def test_futures_delta_matches_finite_difference(client):
    request = _only(_benchmark(), 0)
    request["include_flows"] = False
    request["include_observations"] = False
    base = _price(client, request)["swaps"][0]
    deltas = {d["month"]: d for d in base["deltas"]}
    assert all(d["contract_id"] == "SYN_CL" and "spot_id" not in d for d in deltas.values())
    # Every month read from today's prices carries a delta, nothing else: from
    # the December month (read 13-20 Nov) to the 2027-12 month (read after the
    # 2027-11 month's last trading date, 20 Oct 2027).
    assert set(deltas) == {"2026-12", "2027-01", "2027-02", "2027-03", "2027-04", "2027-05",
                           "2027-06", "2027-07", "2027-08", "2027-09", "2027-10", "2027-11",
                           "2027-12"}
    bump = 0.01
    for month in ("2026-12", "2027-03"):
        bumped = copy.deepcopy(request)
        for m in _contract(bumped)["months"]:
            if m["month"] == month:
                m["price"] += bump
        npv = _price(client, bumped)["swaps"][0]["npv"]
        fd = (npv - base["npv"]) / bump
        assert abs(fd - deltas[month]["delta"]) < 1e-6 * max(1.0, abs(fd)), (
            f"{month}: finite difference {fd!r} vs delta {deltas[month]['delta']!r}")


def test_spot_delta_matches_finite_difference(client):
    request = _only(_benchmark(), 2)
    request["include_flows"] = False
    request["include_observations"] = False
    base = _price(client, request)["swaps"][0]
    deltas = {d["date"]: d for d in base["deltas"]}
    assert all(d["spot_id"] == "SYN_SPOT" and "contract_id" not in d for d in deltas.values())
    bump = 0.01
    date = "2027-02-15"
    bumped = copy.deepcopy(request)
    for p in _spot(bumped)["points"]:
        if p["date"] == date:
            p["price"] += bump
    npv = _price(client, bumped)["swaps"][0]["npv"]
    fd = (npv - base["npv"]) / bump
    assert abs(fd - deltas[date]["delta"]) < 1e-6 * max(1.0, abs(fd))


def test_reprice_at_fair_fixed_price_is_zero(client):
    request = _benchmark()
    request["include_flows"] = False
    request["include_observations"] = False
    rows = _price(client, request)["swaps"]
    for i, row in enumerate(rows):
        _leg(request, i, 1)["price"]["price"] = row["fair_fixed_price"]
    for row in _price(client, request)["swaps"]:
        assert abs(row["npv"]) < 1e-5


def test_flipping_sides_flips_npv(client):
    request = _only(_benchmark(), 0)
    base = _price(client, request)["swaps"][0]
    _leg(request, 0, 1)["pay_receive"] = "Receive"
    _leg(request, 0, 2)["pay_receive"] = "Pay"
    flipped = _price(client, request)["swaps"][0]
    assert abs(flipped["npv"] + base["npv"]) < 1e-6
    assert abs(flipped["fair_fixed_price"] - base["fair_fixed_price"]) < 1e-9


def test_reference_reproduces_benchmark_values():
    """The reference alone (no server) gives the pinned benchmark values."""
    request = _benchmark()
    for i, (npv, fair, average) in enumerate(REFERENCE):
        row = commodity_swap_ql(request, i)
        assert abs(row["npv"] - npv) < NPV_TOLERANCE
        assert abs(row["fair_fixed_price"] - fair) < PRICE_TOLERANCE
        first = row["legs"][1]["flows"][0]
        assert abs(first["average"] - average) < 1e-10
        assert first["payment_date"] == "2026-12-07"
        assert abs(first["discount"] - FIRST_DISCOUNT) < DF_TOLERANCE


def _close(a, b, tolerance):
    return abs(a - b) <= tolerance * max(1.0, abs(a), abs(b))


def _compare_row(where, api, ref, request, gaps):
    """Every field of one response row against the reference row: the same
    optional fields present, strings and counts equal, numbers within the
    relative tolerances. Records the largest relative gap per kind."""

    def number(path, a, b, tolerance, kind):
        assert isinstance(a, (int, float)), f"{path}: {a!r} is not a number"
        gap = abs(a - b) / max(1.0, abs(a), abs(b))
        if gap > gaps.get(kind, (0.0, None))[0]:
            gaps[kind] = (gap, path)
        assert _close(a, b, tolerance), f"{path}: engine {a!r} vs reference {b!r}"

    for name in ("npv", "fair_fixed_price", "fair_spread"):
        assert (name in api) == (name in ref), f"{where}.{name}: presence differs"
        if name in ref:
            number(f"{where}.{name}", api[name], ref[name], VALUE_TOLERANCE, "value")
    assert len(api["legs"]) == 2
    for j, (leg, ref_leg) in enumerate(zip(api["legs"], ref["legs"])):
        at = f"{where}.legs[{j}]"
        number(f"{at}.npv", leg["npv"], ref_leg["npv"], VALUE_TOLERANCE, "value")
        flows = leg.get("flows") or []
        if not request.get("include_flows"):
            assert not flows, f"{at}: flows without include_flows"
            continue
        assert len(flows) == len(ref_leg["flows"]), (
            f"{at}: {len(flows)} flows vs reference {len(ref_leg['flows'])}")
        for k, (flow, ref_flow) in enumerate(zip(flows, ref_leg["flows"])):
            fat = f"{at}.flows[{k}]"
            for name in ("start_date", "end_date", "payment_date"):
                assert flow[name] == ref_flow[name], f"{fat}.{name}: {flow[name]} vs {ref_flow[name]}"
            for name in ("pricing_days", "realised_days"):
                assert flow.get(name, 0) == ref_flow[name], (
                    f"{fat}.{name}: {flow.get(name, 0)} vs {ref_flow[name]}")
            for name in ("average", "realised_average", "expected_average", "spread", "price"):
                assert (name in flow) == (name in ref_flow), f"{fat}.{name}: presence differs"
                if name in ref_flow:
                    number(f"{fat}.{name}", flow[name], ref_flow[name], VALUE_TOLERANCE, "value")
            for name in ("quantity", "amount", "discount", "present_value"):
                number(f"{fat}.{name}", flow[name], ref_flow[name], VALUE_TOLERANCE, "value")
            observations = flow.get("observations") or []
            if not request.get("include_observations"):
                assert not observations
                continue
            expected = ref_flow.get("observations", [])
            assert len(observations) == len(expected), (
                f"{fat}: {len(observations)} observations vs reference {len(expected)}")
            for m, (o, r) in enumerate(zip(observations, expected)):
                oat = f"{fat}.observations[{m}]"
                assert o["date"] == r["date"], f"{oat}.date: {o['date']} vs {r['date']}"
                assert o.get("contract_month") == r.get("contract_month"), (
                    f"{oat}.contract_month: {o.get('contract_month')} vs {r.get('contract_month')}")
                assert o["source"] == r["source"], f"{oat}.source: {o['source']} vs {r['source']}"
                number(f"{oat}.price", o["price"], r["price"], VALUE_TOLERANCE, "value")
    deltas = api.get("deltas") or []
    keys = [(d.get("contract_id"), d.get("month"), d.get("spot_id"), d.get("date"))
            for d in deltas]
    ref_keys = [(d.get("contract_id"), d.get("month"), d.get("spot_id"), d.get("date"))
                for d in ref["deltas"]]
    assert keys == ref_keys, f"{where}.deltas: {keys} vs reference {ref_keys}"
    for k, (d, r) in enumerate(zip(deltas, ref["deltas"])):
        number(f"{where}.deltas[{k}].delta", d["delta"], r["delta"], DELTA_TOLERANCE, "delta")


@pytest.mark.parametrize("example", EXAMPLES)
def test_example_matches_reference(client, data_dir, example):
    request = json.loads((data_dir / "commodity_swap" / example).read_text())
    rows = _price(client, request)["swaps"]
    assert len(rows) == len(request["swaps"])
    gaps = {}
    for i, row in enumerate(rows):
        _compare_row(f"swaps[{i}]", row, commodity_swap_ql(request, i), request, gaps)
        _check_identities(row)
    print(f"[commodity] {example}: largest relative gaps {gaps}")


# ---------------------------------------------------------------------------
# explicit periods, per-period quantities and prices, floating vs floating
# ---------------------------------------------------------------------------

EXPLICIT_PERIODS = [
    {"start_date": "2026-12-01", "end_date": "2026-12-31"},
    {"start_date": "2027-01-01", "end_date": "2027-01-31"},
    {"start_date": "2027-03-01", "end_date": "2027-03-31"},  # gap: no February
]


def _explicit_request():
    request = _benchmark()
    template = _leg(request, 0, 1)

    def leg(side, price_type, price, quantities):
        out = copy.deepcopy(template)
        out["pay_receive"] = side
        out["periods"] = {"periods": copy.deepcopy(EXPLICIT_PERIODS)}
        out["quantity"] = {"values": quantities, "basis": "PerPricingDay"}
        out["price_type"] = price_type
        out["price"] = price
        return out

    futures = {"contract_id": "SYN_CL", "nearby": 1, "roll_days_before_expiry": 0, "spread": 0.0}
    request["swaps"] = [
        # Floating futures received vs floating spot + 0.5 paid.
        {"commodity_swap": {
            "leg1": leg("Receive", "FuturesFloatingPrice", futures, [1000.0, 1200.0, 1500.0]),
            "leg2": leg("Pay", "SpotFloatingPrice", {"spot_id": "SYN_SPOT", "spread": 0.5},
                        [1000.0, 1200.0, 1500.0]),
        }},
        # Per-period fixed prices paid (quantity per pricing day of the
        # floating leg's periods) vs futures received.
        {"commodity_swap": {
            "leg1": leg("Pay", "CommodityFixedPrice", {"prices": [60.0, 61.0, 62.0]},
                        [1000.0, 1200.0, 1500.0]),
            "leg2": leg("Receive", "FuturesFloatingPrice", futures, [1000.0, 1200.0, 1500.0]),
        }},
    ]
    return request


def test_explicit_periods_float_vs_float(client):
    request = _explicit_request()
    row = _price(client, request)["swaps"][0]
    assert "fair_fixed_price" not in row
    assert "fair_spread" in row
    futures_flows = row["legs"][0]["flows"]
    spot_flows = row["legs"][1]["flows"]
    assert [f["start_date"] for f in futures_flows] == [p["start_date"] for p in EXPLICIT_PERIODS]
    # December: 22 pricing days (25 Dec an own holiday); 1-21 Dec read the
    # 2027-01 month (last trading date 21 Dec, roll 0), 22-31 Dec 2027-02.
    dec = futures_flows[0]
    assert dec["pricing_days"] == 22
    assert dec["realised_days"] == 0
    assert "realised_average" not in dec
    assert abs(dec["average"] - (15 * 60.4 + 7 * 60.7) / 22) < 1e-10
    assert abs(dec["quantity"] - 1000.0 * 22) < 1e-9
    assert abs(dec["amount"] - 1000.0 * 22 * dec["average"]) < 1e-6
    # Spot leg: spread on every flow, no contract month on observations.
    for f in spot_flows:
        assert f["spread"] == 0.5
        assert f["amount"] < 0.0
        assert all("contract_month" not in o for o in f["observations"])
    jan = spot_flows[1]
    assert abs(jan["quantity"] - 1200.0 * jan["pricing_days"]) < 1e-9
    assert abs(jan["amount"] + jan["quantity"] * (jan["average"] + 0.5)) < 1e-6
    _check_identities(row)

    # Reprice at the fair spread on leg2: zero NPV.
    _leg(request, 0, 2)["price"]["spread"] = row["fair_spread"]
    repriced = _price(client, _only(request, 0))["swaps"][0]
    assert abs(repriced["npv"]) < 1e-5


def test_explicit_periods_per_period_fixed_prices(client):
    request = _explicit_request()
    row = _price(client, request)["swaps"][1]
    assert "fair_spread" not in row
    assert "fair_fixed_price" in row
    fixed = row["legs"][0]["flows"]
    floating = row["legs"][1]["flows"]
    assert [f["price"] for f in fixed] == [60.0, 61.0, 62.0]
    for f, g, q in zip(fixed, floating, [1000.0, 1200.0, 1500.0]):
        # The fixed leg counts the floating leg's pricing days.
        assert f["pricing_days"] == g["pricing_days"]
        assert abs(f["quantity"] - q * g["pricing_days"]) < 1e-9
        assert abs(f["amount"] + f["quantity"] * f["price"]) < 1e-6
        assert "average" not in f
    _check_identities(row)


# ---------------------------------------------------------------------------
# request contract
# ---------------------------------------------------------------------------

def _single():
    return _only(_benchmark(), 0)


def test_both_period_forms_400(client):
    request = _single()
    _leg(request, 0, 1)["periods"]["periods"] = copy.deepcopy(EXPLICIT_PERIODS)
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.periods", "not both")


def test_neither_period_form_400(client):
    request = _single()
    _leg(request, 0, 2)["periods"] = {}
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg2.periods")


def test_incomplete_generated_periods_400(client):
    request = _single()
    del _leg(request, 0, 1)["periods"]["frequency"]
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.periods.frequency is required")


def test_weekly_frequency_400(client):
    request = _single()
    _leg(request, 0, 1)["periods"]["frequency"] = "Weekly"
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.periods.frequency Weekly")


def test_overlapping_explicit_periods_400(client):
    request = _single()
    _leg(request, 0, 1)["periods"] = {"periods": [
        {"start_date": "2026-12-01", "end_date": "2026-12-31"},
        {"start_date": "2026-12-15", "end_date": "2027-01-31"},
    ]}
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.periods.periods[1].start_date", "overlap")


def test_quantity_value_and_values_400(client):
    request = _single()
    _leg(request, 0, 1)["quantity"]["values"] = [10000.0] * 12
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.quantity", "exactly one")


def test_quantity_count_mismatch_400(client):
    request = _single()
    quantity = _leg(request, 0, 2)["quantity"]
    del quantity["value"]
    quantity["values"] = [10000.0] * 11
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg2.quantity.values has 11 entries for 12 periods")


def test_fixed_prices_count_mismatch_400(client):
    request = _single()
    _leg(request, 0, 1)["price"] = {"prices": [61.0] * 13}
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.price.prices has 13 entries for 12 periods")


def test_quantity_basis_required_400(client):
    request = _single()
    del _leg(request, 0, 1)["quantity"]["basis"]
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.quantity.basis")


def test_presence_required_leg_fields_400(client):
    for field in ("pay_receive", "payment_calendar", "payment_lag", "payment_convention"):
        request = _single()
        del _leg(request, 0, 2)[field]
        _assert_status(client, request, 400, f"swaps[0].commodity_swap.leg2.{field}")
    for field in ("nearby", "roll_days_before_expiry"):
        request = _single()
        del _leg(request, 0, 2)["price"][field]
        _assert_status(client, request, 400, f"swaps[0].commodity_swap.leg2.price.{field}")


def test_legs_on_same_side_400(client):
    request = _single()
    _leg(request, 0, 2)["pay_receive"] = "Pay"
    _assert_status(client, request, 400, "swaps[0].commodity_swap.leg1.pay_receive", "opposite")


def test_unknown_contract_id_404(client):
    request = _single()
    _leg(request, 0, 2)["price"]["contract_id"] = "NO_SUCH_CONTRACT"
    _assert_status(client, request, 404,
                   "swaps[0].commodity_swap.leg2.price.contract_id", "NO_SUCH_CONTRACT")


def test_unknown_spot_id_404(client):
    request = _only(_benchmark(), 2)
    _leg(request, 0, 2)["price"]["spot_id"] = "NO_SUCH_SPOT"
    _assert_status(client, request, 404,
                   "swaps[0].commodity_swap.leg2.price.spot_id", "NO_SUCH_SPOT")


def test_unknown_discounting_curve_404(client):
    request = _single()
    _leg(request, 0, 1)["discounting_curve"] = "NO_SUCH_CURVE"
    _assert_status(client, request, 404,
                   "swaps[0].commodity_swap.leg1.discounting_curve", "NO_SUCH_CURVE")


def test_missing_fixing_for_past_day_400(client):
    request = _single()
    contract = _contract(request)
    contract["fixings"] = [f for f in contract["fixings"] if f["date"] != "2026-11-05"]
    _assert_status(client, request, 400, "swaps[0].commodity_swap: commodity contract",
                   "2026-11-05", "2026-12")


def test_missing_fixing_value_400(client):
    request = _single()
    del _contract(request)["fixings"][0]["value"]
    _assert_status(client, request, 400, "fixings[0].value")


def test_missing_price_for_month_read_after_valuation_400(client):
    request = _single()
    for m in _contract(request)["months"]:
        if m["month"] == "2027-03":
            del m["price"]
    _assert_status(client, request, 400, "2027-03", "no price")


def test_log_interpolation_on_spot_400(client):
    for interpolation in ("LogLinear", "LogCubic"):
        request = _only(_benchmark(), 2)
        _spot(request)["interpolation"] = interpolation
        _assert_status(client, request, 400, "interpolation")


def test_spot_outside_points_without_extrapolation_400(client):
    request = _only(_benchmark(), 2)
    _spot(request)["extrapolate_flat"] = False
    # The valuation day (13 Nov, no fixing) reads the forward price, two days
    # before the first point (15 Nov).
    _assert_status(client, request, 400, "2026-11-13", "extrapolate_flat")


def _assert_named_once(text, path):
    # In the error text (the body repeats it under `error` and `message`),
    # the path appears once and the swap prefix only once.
    error = json.loads(text)["error"]
    assert error.count(path) == 1, f"{path!r} not named exactly once: {error}"
    assert error.count("swaps[") == 1, f"swap named more than once: {error}"
    assert error.count("commodity_swap") == 1, f"swap named more than once: {error}"


def test_errors_name_the_second_swap_once(client):
    # A decode error on swaps[1]: an invalid explicit period date.
    request = _benchmark()
    _leg(request, 1, 1)["periods"] = {"periods": [
        {"start_date": "2026-12-01", "end_date": "2026-12-31"},
        {"start_date": "2027-01-1", "end_date": "2027-01-31"},
    ]}
    text = _assert_status(client, request, 400,
                          "swaps[1].commodity_swap.leg1.periods.periods[1].start_date: "
                          "Invalid date '2027-01-1'")
    _assert_named_once(text, "leg1.periods.periods[1].start_date")

    # A value error found by the pricing library on swaps[1].
    request = _benchmark()
    quantity = _leg(request, 1, 2)["quantity"]
    del quantity["value"]
    quantity["values"] = [10000.0] * 11
    text = _assert_status(client, request, 400,
                          "swaps[1].commodity_swap.leg2.quantity.values has 11 entries")
    _assert_named_once(text, "leg2.quantity.values")

    # An unknown id on swaps[1]: 404.
    request = _benchmark()
    _leg(request, 1, 2)["price"]["contract_id"] = "NO_SUCH_CONTRACT"
    text = _assert_status(client, request, 404,
                          "swaps[1].commodity_swap.leg2.price.contract_id: "
                          "commodity contract not found: NO_SUCH_CONTRACT")
    _assert_named_once(text, "leg2.price.contract_id")


def test_empty_values_list_400(client):
    request = _single()
    _leg(request, 0, 1)["quantity"]["values"] = []
    _assert_status(client, request, 400,
                   "swaps[0].commodity_swap.leg1.quantity.values must not be empty")


def test_include_observations_without_flows_400(client):
    request = _single()
    request["include_flows"] = False
    request["include_observations"] = True
    _assert_status(client, request, 400, "include_observations", "include_flows")


def test_duplicate_contract_id_400(client):
    request = _single()
    contracts = request["pricing"]["commodity"]["contracts"]
    contracts.append(copy.deepcopy(contracts[0]))
    _assert_status(client, request, 400, "duplicate", "SYN_CL")


def test_duplicate_spot_id_400(client):
    request = _single()
    spots = request["pricing"]["commodity"]["spot_prices"]
    spots.append(copy.deepcopy(spots[0]))
    _assert_status(client, request, 400, "duplicate", "SYN_SPOT")


def test_pricing_calendar_required_400(client):
    request = _single()
    del _contract(request)["pricing_calendar"]
    _assert_status(client, request, 400, "pricing_calendar")
