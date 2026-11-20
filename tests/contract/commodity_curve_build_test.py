"""Commodity curve build: examples against the independent twin, the round
trip into the swap request, and the request contract.

`POST /build-commodity-curves` completes futures contracts whose months are
only partly priced. A month is live when its last trading date is on or
after the valuation date; a live month without a price is filled from the
quoted live months, placed on their last trading dates and read on the
month's own last trading date (calendar days from the valuation date /
365) with one of six methods; outside the quotes only with
`extrapolate_flat` (nearest quote). Quoted months, expired months, fixings, the pricing calendar and the
pricing holidays come back as sent.

  * the catalog examples (examples/data/commodity_curve/, synthetic data):
    every returned contract against the independent twin
    ql_reference.commodity_curve_build_ql, prices at 1e-12 relative, every
    other field exactly, the report (status, counts, sources, repriced
    quotes) at the same tolerance;
  * the round trip: a contract built from a strip that is linear in the
    days between last trading dates, with interior months removed, pastes
    unchanged into a /price-commodity-swap request and gives the NPV of the
    full strip to 1e-9 relative;
  * the six methods (Linear, LogLinear, BackwardFlat, ForwardFlat,
    KrugerCubic, NaturalCubic) on the swap reference strip with five months
    removed: independent reference values at 1e-9 (no pinned value for
    NaturalCubic: the twin is its check), the twin on every method, flat
    extrapolation for the cubics;
  * every validation is a 400 naming `curves[i]` and the field once.
"""

import copy
import datetime
import json
from pathlib import Path

import pytest

from ql_reference import commodity_curve_build_ql, commodity_swap_ql

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES_DIR = ROOT / "examples" / "data" / "commodity_curve"
# Built examples; `refused_*.json` are requests the endpoint refuses (tested
# on their own).
EXAMPLES = sorted(p.name for p in EXAMPLES_DIR.glob("*.json")
                  if not p.name.startswith("refused_"))
CONTRACT_EXAMPLES = [e for e in EXAMPLES if "_spot_" not in e]
SWAP_BENCHMARK = (Path(__file__).resolve().parent / "data" / "commodity"
                  / "price_commodity_swap_benchmark_request.json")

PRICE_TOLERANCE = 1e-12
ROUND_TRIP_TOLERANCE = 1e-9


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _load(name):
    with open(EXAMPLES_DIR / name) as f:
        return json.load(f)


def _linear():
    return _load("synthetic_linear_gaps.json")


def _post(client, product, request):
    r = client.session.post(f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)
    return r.status_code, r.text


def _build(client, request):
    status, text = _post(client, "build_commodity_curves", request)
    assert status == 200, f"expected 200, got {status}: {text[:500]}"
    return json.loads(text)


def _assert_status(client, request, status_expected, *fragments):
    status, text = _post(client, "build_commodity_curves", request)
    assert status == status_expected, (
        f"expected {status_expected} naming {fragments}, got {status}: {text[:400]}")
    for fragment in fragments:
        assert fragment in text, f"{status} does not name {fragment!r}: {text[:400]}"
    return text


def _assert_named_once(text, path):
    error = json.loads(text)["error"]
    assert error.count(path) == 1, f"{path!r} not named exactly once: {error}"
    assert error.count("curves[") == 1, f"build named more than once: {error}"


def _close(a, b, tolerance=PRICE_TOLERANCE):
    return abs(a - b) <= tolerance * max(1.0, abs(a), abs(b))


def _assert_contract(where, api, ref, tolerance=PRICE_TOLERANCE):
    """Every field of the returned contract against the expected one: prices
    within PRICE_TOLERANCE, presence of every optional field and every other
    value exactly."""
    assert set(api) == set(ref), f"{where}: fields {sorted(api)} != {sorted(ref)}"
    for key in ref:
        if key == "months":
            continue
        assert api[key] == ref[key], f"{where}.{key}: {api[key]!r} != {ref[key]!r}"
    assert len(api["months"]) == len(ref["months"]), f"{where}: month count"
    for j, (a, r) in enumerate(zip(api["months"], ref["months"])):
        at = f"{where}.months[{j}]"
        assert a["month"] == r["month"] and a["last_trade_date"] == r["last_trade_date"], at
        assert ("price" in a) == ("price" in r), f"{at}: price presence {a} vs {r}"
        if "price" in r:
            assert _close(a["price"], r["price"], tolerance), (
                f"{at}: {a['price']} vs {r['price']}")


def _assert_spot(where, api, ref, tolerance=PRICE_TOLERANCE):
    """Every field of the returned spot price against the expected one."""
    assert set(api) == set(ref), f"{where}: fields {sorted(api)} != {sorted(ref)}"
    for key in ref:
        if key != "points":
            assert api[key] == ref[key], f"{where}.{key}: {api[key]!r} != {ref[key]!r}"
    assert [p["date"] for p in api["points"]] == [p["date"] for p in ref["points"]], where
    for a, r in zip(api["points"], ref["points"]):
        assert _close(a["price"], r["price"], tolerance), f"{where} {a['date']}"


def _interpolated(row):
    """Months the fill priced (no quote touches them), in contract order."""
    return [m["month"] for m in row["report"]["months"]
            if m["source"] in ("Interpolated", "Extrapolated")]


def _assert_report(where, api, ref, tolerance=PRICE_TOLERANCE):
    """The report against the twin's: counts, status, rule, every live
    month's price and source, every quote's status and repriced value."""
    for key in ("status", "unknowns", "equations", "rank"):
        assert api[key] == ref[key], f"{where}.{key}: {api[key]} != {ref[key]}"
    assert api.get("rule") == ref.get("rule"), f"{where}.rule"
    # Contract builds list months, spot builds points.
    rows, key = ("points", "date") if "points" in ref else ("months", "month")
    assert ("months" in api) == (rows == "months"), where
    assert [m[key] for m in api[rows]] == [m[key] for m in ref[rows]], where
    for a, r in zip(api[rows], ref[rows]):
        assert a["source"] == r["source"], f"{where} {a[key]}: {a['source']} != {r['source']}"
        assert abs(a["price"] - r["price"]) <= tolerance * max(1.0, abs(r["price"])), (
            f"{where} {a[key]}: {a['price']} != {r['price']}")
        # Basis builds: the base price and the basis of every month.
        assert ("base_price" in a) == ("base_price" in r), f"{where} {a[key]}: base_price"
        for field in ("base_price", "basis"):
            if field in r:
                assert abs(a[field] - r[field]) <= tolerance * max(1.0, abs(r[field])), (
                    f"{where} {a[key]}.{field}: {a[field]} != {r[field]}")
    assert len(api["average_quotes"]) == len(ref["average_quotes"]), where
    for k, (a, r) in enumerate(zip(api["average_quotes"], ref["average_quotes"])):
        assert a["status"] == r["status"], f"{where} average_quotes[{k}].status"
        assert abs(a["repriced"] - r["repriced"]) <= tolerance * max(1.0, abs(r["repriced"])), (
            f"{where} average_quotes[{k}].repriced: {a['repriced']} != {r['repriced']}")


# ---------------------------------------------------------------------------
# examples against the independent twin
# ---------------------------------------------------------------------------

def test_examples_present():
    assert len(EXAMPLES) >= 4, EXAMPLES


@pytest.mark.parametrize("example", EXAMPLES)
def test_example_matches_twin(client, example):
    request = _load(example)
    response = _build(client, request)
    assert len(response["curves"]) == len(request["curves"])
    for i, row in enumerate(response["curves"]):
        ref = commodity_curve_build_ql(request, i)
        # The ill-conditioned example amplifies rounding: 1e-6 there.
        tolerance = 1e-6 if "fragile" in example else 1e-9
        if "spot" in ref:
            _assert_spot(f"{example} curves[{i}].spot", row["spot"], ref["spot"], tolerance)
        else:
            _assert_contract(f"{example} curves[{i}].contract", row["contract"],
                             ref["contract"], tolerance)
        _assert_report(f"{example} curves[{i}].report", row["report"], ref["report"],
                       tolerance)


def test_linear_example_values(client):
    # 2027-02 (last traded 20 Jan) between 2027-01 (21 Dec, 60.40) and
    # 2027-03 (22 Feb, 61.00): 30 of 63 days.
    row = _build(client, _linear())["curves"][0]
    months = {m["month"]: m for m in row["contract"]["months"]}
    assert _close(months["2027-02"]["price"], 60.4 + 0.6 * 30 / 63)
    # The expired November month comes back without a price.
    assert "price" not in months["2026-11"]
    assert _interpolated(row) == ["2027-02", "2027-04", "2027-05", "2027-07",
                                  "2027-08", "2027-09", "2027-10", "2027-11"]
    report = row["report"]
    assert report["status"] == "Unique" and report["average_quotes"] == []
    assert report["unknowns"] == report["equations"] == report["rank"] == 5
    assert report["max_repricing_difference"] == 0.0
    assert "rule" not in report and "max_sensitivity" not in report


def test_fully_priced_is_identity(client):
    request = _load("synthetic_fully_priced_identity.json")
    assert "interpolation" not in request["curves"][0]
    row = _build(client, request)["curves"][0]
    assert row["contract"] == request["curves"][0]["contract"]
    assert _interpolated(row) == []
    assert {m["source"] for m in row["report"]["months"]} == {"Quoted"}


def test_twin_refuses_what_the_endpoint_refuses():
    request = _linear()
    del request["curves"][0]["interpolation"]
    with pytest.raises(ValueError):
        commodity_curve_build_ql(request)


# ---------------------------------------------------------------------------
# round trip into the swap request
# ---------------------------------------------------------------------------

def _days(a, b):
    return (datetime.date.fromisoformat(b) - datetime.date.fromisoformat(a)).days


def _linear_strip_swap_request():
    """The swap benchmark request (futures swaps only) with every live month
    priced on a line in the days since the first live last trading date."""
    with open(SWAP_BENCHMARK) as f:
        request = json.load(f)
    request["swaps"] = [s for s in request["swaps"]
                        if s["commodity_swap"]["leg2"]["price_type"] == "FuturesFloatingPrice"]
    request["include_flows"] = False
    request["include_observations"] = False
    as_of = request["pricing"]["as_of_date"]
    contract = request["pricing"]["commodity"]["contracts"][0]
    live = [m for m in contract["months"] if m["last_trade_date"] >= as_of]
    first = live[0]["last_trade_date"]
    for m in live:
        m["price"] = 58.0 + 0.0125 * _days(first, m["last_trade_date"])
    return request


def test_round_trip_into_the_swap_request(client):
    swap_request = _linear_strip_swap_request()
    status, text = _post(client, "commodity_swap", swap_request)
    assert status == 200, text[:400]
    full_npvs = [s["npv"] for s in json.loads(text)["swaps"]]
    assert len(full_npvs) == 2

    # Remove the prices of interior live months, keep the first and last
    # live quotes and two in between.
    contract = copy.deepcopy(swap_request["pricing"]["commodity"]["contracts"][0])
    as_of = swap_request["pricing"]["as_of_date"]
    live = [m for m in contract["months"] if m["last_trade_date"] >= as_of]
    keep = {live[0]["month"], live[4]["month"], live[9]["month"], live[-1]["month"]}
    removed = []
    for m in live:
        if m["month"] not in keep:
            del m["price"]
            removed.append(m["month"])
    assert len(removed) >= 8
    build_request = {"pricing": {"as_of_date": as_of},
                     "curves": [{"contract": contract, "interpolation": "Linear"}]}
    row = _build(client, build_request)["curves"][0]
    assert _interpolated(row) == removed

    # (a) the returned contract is the request's with the filled prices, and
    # the swap endpoint accepts it as is.
    expected = swap_request["pricing"]["commodity"]["contracts"][0]
    _assert_contract("round trip", row["contract"], expected)
    pasted = copy.deepcopy(swap_request)
    pasted["pricing"]["commodity"]["contracts"] = [row["contract"]]
    status, text = _post(client, "commodity_swap", pasted)
    assert status == 200, text[:400]
    # (b) same NPVs as the full strip.
    npvs = [s["npv"] for s in json.loads(text)["swaps"]]
    for a, b in zip(npvs, full_npvs):
        assert abs(a - b) <= ROUND_TRIP_TOLERANCE * max(1.0, abs(b)), (npvs, full_npvs)


def test_built_examples_paste_into_a_swap_request(client):
    # Every example's response contract has the shape the swap request
    # takes: the swap endpoint accepts the block as is (the two-month swap of
    # the documentation, December 2026 and January 2027, on the example's
    # valuation date).
    swap_example = (ROOT / "examples" / "data" / "commodity_swap"
                    / "synthetic_two_month_fixed_vs_front_month.json")
    with open(swap_example) as f:
        swap = json.load(f)
    for example in CONTRACT_EXAMPLES:
        request = _load(example)
        as_of = request["pricing"]["as_of_date"]
        for i, row in enumerate(_build(client, request)["curves"]):
            pasted = copy.deepcopy(swap)
            pasted["pricing"]["as_of_date"] = as_of
            curve = pasted["pricing"]["rates"]["curves"][0]
            curve["reference_date"] = as_of
            curve["points"][0]["point"]["date"] = as_of
            pasted["pricing"]["commodity"] = {"contracts": [row["contract"]]}
            for leg in ("leg1", "leg2"):
                price = pasted["swaps"][0]["commodity_swap"][leg]["price"]
                if "contract_id" in price:
                    price["contract_id"] = row["contract"]["id"]
            status, text = _post(client, "commodity_swap", pasted)
            assert status == 200, f"{example} curves[{i}]: {status} {text[:400]}"


# ---------------------------------------------------------------------------
# request contract
# ---------------------------------------------------------------------------

def test_curves_required_and_non_empty_400(client):
    request = _linear()
    request["curves"] = []
    _assert_status(client, request, 400, "curves is required and must be non-empty")
    del request["curves"]
    _assert_status(client, request, 400, "curves")


def test_interpolation_required_when_a_month_is_missing_400(client):
    request = _linear()
    del request["curves"][0]["interpolation"]
    text = _assert_status(client, request, 400, "curves[0].interpolation is required",
                          "2027-02")
    _assert_named_once(text, "interpolation")


def test_log_linear_needs_positive_quotes_400(client):
    request = _load("synthetic_log_linear.json")
    request["curves"][0]["contract"]["months"][3]["price"] = 0.0
    text = _assert_status(client, request, 400,
                          "curves[0].contract.months[3] (2027-02): price 0 must be > 0 for "
                          "interpolation LogLinear")
    _assert_named_once(text, "months[3]")


def test_unknown_interpolation_value_400(client):
    # Names of the rate-curve interpolator that are not commodity methods.
    for value in ("Cubic", "LogCubic"):
        request = _linear()
        request["curves"][0]["interpolation"] = value
        _assert_status(client, request, 400, value)


# ---------------------------------------------------------------------------
# independent reference values: the six methods on one strip
# ---------------------------------------------------------------------------

# The futures strip of the swap reference request (as of 2026-11-13, months
# 2026-12 to 2028-01) with the prices of five months removed; the filled
# prices are independent reference values.
REMOVED = ["2027-02", "2027-03", "2027-06", "2027-09", "2027-10"]
REFERENCE_FILLS = {
    "Linear": [60.6637362637, 60.9538461538, 61.4952380952, 61.8010869565, 61.9054347826],
    "LogLinear": [60.6625740259, 60.9527240494, 61.4951569808, 61.8009244485, 61.9052776863],
    "BackwardFlat": [61.2, 61.2, 61.6, 62.0, 62.0],
    "ForwardFlat": [60.4, 60.4, 61.4, 61.7, 61.7],
    "KrugerCubic": [60.6932374651, 60.9780335193, 61.5034978519, 61.8028587986, 61.9058753205],
}
REFERENCE_TOLERANCE = 1e-9


def _reference_request(method, removed, extrapolate_flat=False):
    with open(SWAP_BENCHMARK) as f:
        swap = json.load(f)
    contract = swap["pricing"]["commodity"]["contracts"][0]
    for m in contract["months"]:
        if m["month"] in removed:
            del m["price"]
    return {"pricing": {"as_of_date": swap["pricing"]["as_of_date"]},
            "curves": [{"contract": contract, "interpolation": method,
                        "extrapolate_flat": extrapolate_flat}]}


def _filled(row, months):
    by_month = {m["month"]: m for m in row["contract"]["months"]}
    return [by_month[m]["price"] for m in months]


@pytest.mark.parametrize("method", sorted(REFERENCE_FILLS))
def test_reference_values(client, method):
    request = _reference_request(method, REMOVED)
    row = _build(client, request)["curves"][0]
    assert _interpolated(row) == REMOVED
    for got, want, month in zip(_filled(row, REMOVED), REFERENCE_FILLS[method], REMOVED):
        assert abs(got - want) < REFERENCE_TOLERANCE, (method, month, got, want)
    # The twin gives the same values.
    twin = commodity_curve_build_ql(request)
    for got, want in zip(_filled(twin, REMOVED), REFERENCE_FILLS[method]):
        assert abs(got - want) < REFERENCE_TOLERANCE, (method, got, want)


def test_reference_values_linear_flat_after_the_last_quote(client):
    removed = REMOVED + ["2027-12", "2028-01"]
    request = _reference_request("Linear", removed, extrapolate_flat=True)
    row = _build(client, request)["curves"][0]
    assert _interpolated(row) == removed
    expected = REFERENCE_FILLS["Linear"] + [62.0, 62.0]
    for got, want in zip(_filled(row, removed), expected):
        assert abs(got - want) < REFERENCE_TOLERANCE, (got, want)


@pytest.mark.parametrize("method", ["Linear", "LogLinear", "BackwardFlat", "ForwardFlat",
                                    "KrugerCubic", "NaturalCubic"])
def test_every_method_matches_twin_on_the_reference_strip(client, method):
    request = _reference_request(method, REMOVED)
    row = _build(client, request)["curves"][0]
    ref = commodity_curve_build_ql(request)
    _assert_contract(method, row["contract"], ref["contract"])


@pytest.mark.parametrize("method", ["KrugerCubic", "NaturalCubic"])
def test_cubics_are_never_continued_beyond_the_quotes(client, method):
    request = _reference_request(method, REMOVED + ["2026-12", "2028-01"])
    _assert_status(client, request, 400, "curves[0].contract.months[1] (2026-12)",
                   "extrapolate_flat is false")
    request["curves"][0]["extrapolate_flat"] = True
    row = _build(client, request)["curves"][0]
    first, last = _filled(row, ["2026-12", "2028-01"])
    assert first == 60.4 and last == 62.1


def test_second_curve_named_once_400(client):
    request = _load("synthetic_backward_and_forward_flat.json")
    del request["curves"][1]["interpolation"]
    text = _assert_status(client, request, 400, "curves[1].interpolation is required")
    _assert_named_once(text, "curves[1].interpolation")

    request = _load("synthetic_backward_and_forward_flat.json")
    del request["curves"][1]["contract"]["pricing_calendar"]
    text = _assert_status(client, request, 400,
                          "curves[1].contract.pricing_calendar is required")
    _assert_named_once(text, "pricing_calendar")


def test_month_outside_quotes_without_extrapolation_400(client):
    request = _load("synthetic_extrapolate_flat_both_ends.json")
    request["curves"][0]["extrapolate_flat"] = False
    text = _assert_status(client, request, 400,
                          "curves[0].contract.months[1] (2026-12): last_trade_date 2026-11-20 "
                          "is before the first quoted live month", "extrapolate_flat is false")
    _assert_named_once(text, "months[1]")


def test_no_quoted_live_month_400(client):
    request = _linear()
    for m in request["curves"][0]["contract"]["months"]:
        m.pop("price", None)
    _assert_status(client, request, 400, "curves[0].contract.months: no live month has a price")


def test_contract_rules_400(client):
    request = _linear()
    months = request["curves"][0]["contract"]["months"]
    months[3]["last_trade_date"] = months[2]["last_trade_date"]
    text = _assert_status(client, request, 400,
                          "curves[0].contract.months[3].last_trade_date 2026-12-21 of 2027-02 "
                          "is not after 2026-12-21 of 2027-01")
    _assert_named_once(text, "months[3]")

    request = _linear()
    request["curves"][0]["contract"]["months"][2]["month"] = "2027-13"
    _assert_status(client, request, 400, "curves[0].contract", "2027-13", "YYYY-MM")

    request = _linear()
    request["curves"][0]["contract"]["months"][2]["last_trade_date"] = "2026-12-32"
    _assert_status(client, request, 400,
                   "curves[0].contract.months[2].last_trade_date: Invalid date")


def test_pricing_commodity_refused_400(client):
    request = _linear()
    request["pricing"]["commodity"] = {"contracts": [request["curves"][0]["contract"]]}
    _assert_status(client, request, 400, "pricing.commodity", "curves[].contract")


def test_no_curve_needed(client):
    # The request carries no pricing.rates block at all.
    request = _linear()
    assert set(request["pricing"]) == {"as_of_date"}
    _build(client, request)


def test_calendar_overrides_accepted(client):
    request = _linear()
    request["pricing"]["calendar_overrides"] = [
        {"calendar": "WeekendsOnly", "added_holidays": ["2027-01-04"]}]
    row = _build(client, request)["curves"][0]
    # The fill is in calendar days: a holiday changes nothing.
    assert row == _build(client, _linear())["curves"][0]


# ---------------------------------------------------------------------------
# average-price quotes: one linear system
# ---------------------------------------------------------------------------
#
# The small examples: a contract whose months expire on the last pricing day
# of their own calendar month (a calendar month reads exactly one contract
# month), single-period monthly quotes and a three-period quarter quote paid
# monthly and discounted on USD_OIS (synthetic discount factors).

UNIQUE = "synthetic_average_quotes_months_and_quarter_unique.json"
FLAT_SMOOTH = "synthetic_average_quotes_quarter_flat_and_smooth.json"
STRIP = "synthetic_average_quotes_monthly_strip.json"
FRAGILE = "synthetic_average_quotes_monthly_strip_rounded_fragile.json"
REFERENCES = (Path(__file__).resolve().parent / "data" / "commodity"
              / "curve_build_reference_values.json")


def _months(row):
    return {m["month"]: m for m in row["report"]["months"]}


def _month_quote(month, last_day, price):
    return {"price": price,
            "periods": {"start_date": f"{month}-01", "end_date": f"{month}-{last_day}",
                        "frequency": "Once"},
            "nearby": 1, "roll_days_before_expiry": 0}


def test_case_a_unique_march_solved(client):
    request = _load(UNIQUE)
    row = _build(client, request)["curves"][0]
    report = row["report"]
    assert report["status"] == "Unique"
    assert (report["unknowns"], report["equations"], report["rank"]) == (3, 3, 3)
    months = _months(row)
    assert [months[m]["source"] for m in ("2027-01", "2027-02", "2027-03")] == ["Solved"] * 3
    # A one-month quote on a contract month read for the whole month is that
    # month's price; March follows from the quarter with its real weights.
    assert abs(months["2027-01"]["price"] - 50.0) < 1e-9
    assert abs(months["2027-02"]["price"] - 52.0) < 1e-9
    q1 = report["average_quotes"][2]
    weights = {w["month"]: w["weight"] for w in q1["weights"]}
    march = (53.0 - q1["constant"] - weights["2027-01"] * 50.0
             - weights["2027-02"] * 52.0) / weights["2027-03"]
    assert abs(months["2027-03"]["price"] - march) < 1e-9
    # Discounting makes the weights unequal: March is not 3 x 53 - 50 - 52.
    assert abs(months["2027-03"]["price"] - 57.0) > 1e-3
    assert abs(sum(weights.values()) - 1.0) < 1e-12
    assert all(q["status"] == "Used" for q in report["average_quotes"])
    assert report["max_repricing_difference"] < 1e-9


def test_case_b_conflict_names_the_gap(client):
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"].append(_month_quote("2027-03", "31", 58.0))
    solved = _months(_build(client, _load(UNIQUE))["curves"][0])["2027-03"]["price"]
    text = _assert_status(client, request, 400, "curves[0].average_quotes[3]: conflicts",
                          "it quotes 58", "average_quotes[2]")
    error = json.loads(text)["error"]
    implied = float(error.split("they imply ")[1].split(",")[0])
    gap = float(error.split("a gap of ")[1].split(" ")[0])
    assert abs(implied - solved) < 1e-6
    assert abs(gap - (58.0 - solved)) < 1e-6
    _assert_named_once(text, "average_quotes[3]")


def test_redundant_quote_is_accepted_and_flagged(client):
    base = _build(client, _load(UNIQUE))["curves"][0]
    march = _months(base)["2027-03"]["price"]
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"].append(_month_quote("2027-03", "31", march))
    row = _build(client, request)["curves"][0]
    report = row["report"]
    assert report["average_quotes"][3]["status"] == "Redundant"
    assert (report["equations"], report["rank"]) == (4, 3)
    assert abs(_months(row)["2027-03"]["price"] - march) < 1e-9


def test_case_c_needs_a_rule_then_flat_and_smooth(client):
    request = _load(FLAT_SMOOTH)
    del request["curves"][0]["rule"]
    text = _assert_status(client, request, 400, "curves[0].rule is required",
                          "2027-02, 2027-03", "Flat or Smooth")
    _assert_named_once(text, "rule")
    rows = _build(client, _load(FLAT_SMOOTH))["curves"]
    flat, smooth = (_months(r) for r in rows)
    for row in rows:
        report = row["report"]
        assert report["status"] == "Underdetermined"
        assert (report["unknowns"], report["equations"], report["rank"]) == (3, 2, 2)
        assert [m["source"] for m in report["months"]] == ["Solved", "Assumed", "Assumed"]
        assert report["max_repricing_difference"] < 1e-9
    assert rows[0]["report"]["rule"] == "Flat" and rows[1]["report"]["rule"] == "Smooth"
    assert abs(flat["2027-02"]["price"] - flat["2027-03"]["price"]) < 1e-12
    # Smooth: zero second divided difference on the last trading dates, so
    # the three months lie on a line in time.
    names = ("2027-01", "2027-02", "2027-03")
    p = [smooth[m]["price"] for m in names]
    ltd = {m["month"]: datetime.date.fromisoformat(m["last_trade_date"])
           for m in rows[1]["contract"]["months"]}
    t = [(ltd[m] - ltd[names[0]]).days for m in names]
    assert abs((p[2] - p[1]) / (t[2] - t[1]) - (p[1] - p[0]) / (t[1] - t[0])) < 1e-11
    assert abs(flat["2027-03"]["price"] - smooth["2027-03"]["price"]) > 0.1


def test_rule_echoed_and_harmless_when_unique(client):
    request = _load(UNIQUE)
    request["curves"][0]["rule"] = "Smooth"
    row = _build(client, request)["curves"][0]
    assert row["report"]["rule"] == "Smooth" and row["report"]["status"] == "Unique"
    base = _build(client, _load(UNIQUE))["curves"][0]
    assert row["contract"] == base["contract"]


def _two_period(start, end, price):
    return {"price": price,
            "periods": {"start_date": start, "end_date": end, "frequency": "Monthly"},
            "nearby": 1, "roll_days_before_expiry": 0, "quantity_basis": "PerPeriod",
            "payment_calendar": "UnitedStatesSettlement", "payment_lag": 5,
            "payment_convention": "Following", "discounting_curve": "USD_OIS"}


def test_flat_cannot_resolve_overlapping_quotes(client):
    # Jan-Feb and Feb-Mar: three groups of months, two equations.
    request = _load(UNIQUE)
    build = request["curves"][0]
    build["average_quotes"] = [_two_period("2027-01-01", "2027-02-28", 51.0),
                               _two_period("2027-02-01", "2027-03-31", 53.0)]
    build["rule"] = "Flat"
    _assert_status(client, request, 400, "curves[0].rule Flat cannot resolve these quotes")
    build["rule"] = "Smooth"
    row = _build(client, request)["curves"][0]
    assert row["report"]["max_repricing_difference"] < 1e-9


def test_smooth_not_unique_suggests_flat(client):
    # Two months, one two-month quote: no second difference to minimise.
    request = _load(UNIQUE)
    build = request["curves"][0]
    build["contract"]["months"] = build["contract"]["months"][:2]
    build["average_quotes"] = [_two_period("2027-01-01", "2027-02-28", 51.0)]
    build["rule"] = "Smooth"
    _assert_status(client, request, 400, "curves[0].rule Smooth has no unique solution",
                   "use Flat")
    build["rule"] = "Flat"
    row = _build(client, request)["curves"][0]
    prices = [m["price"] for m in row["report"]["months"]]
    assert abs(prices[0] - prices[1]) < 1e-12 and abs(prices[0] - 51.0) < 1e-9


def test_multi_period_quote_requires_its_payment_fields(client):
    for field in ("quantity_basis", "payment_calendar", "payment_lag", "payment_convention",
                  "discounting_curve"):
        request = _load(UNIQUE)
        del request["curves"][0]["average_quotes"][2][field]
        text = _assert_status(client, request, 400,
                              f"curves[0].average_quotes[2].{field} is required for a quote "
                              "of 3 periods")
        _assert_named_once(text, f"average_quotes[2].{field}")


def test_multi_period_quote_uses_discounting(client):
    # Undiscounted, the quarter's weights would be one third each.
    row = _build(client, _load(UNIQUE))["curves"][0]
    weights = [w["weight"] for w in row["report"]["average_quotes"][2]["weights"]]
    assert weights[0] > weights[1] > weights[2]
    assert weights[0] - 1 / 3 > 1e-4 and 1 / 3 - weights[2] > 1e-4


def test_unknown_discounting_curve_404(client):
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"][2]["discounting_curve"] = "NO_SUCH_CURVE"
    _assert_status(client, request, 404,
                   "curves[0].average_quotes[2].discounting_curve: discounting curve not found: "
                   "NO_SUCH_CURVE")


def test_second_quote_named_once(client):
    request = _load(UNIQUE)
    del request["curves"][0]["average_quotes"][1]["nearby"]
    text = _assert_status(client, request, 400, "curves[0].average_quotes[1].nearby is required")
    _assert_named_once(text, "average_quotes[1].nearby")
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"][1]["nearby"] = 0
    text = _assert_status(client, request, 400, "curves[0].average_quotes[1].nearby must be >= 1")
    _assert_named_once(text, "average_quotes[1].nearby")


def _benchmark_contract(keep):
    with open(SWAP_BENCHMARK) as f:
        contract = json.load(f)["pricing"]["commodity"]["contracts"][0]
    for m in contract["months"]:
        if m["month"] not in keep:
            m.pop("price", None)
    return contract


def test_quote_partly_in_the_past_reads_its_fixings(client):
    # November 2026 as of 13 November: 2 to 12 November are fixings of the
    # December month, the rest of the month reads December and January.
    contract = _benchmark_contract({"2027-01"})
    request = {"pricing": {"as_of_date": "2026-11-13"},
               "curves": [{"contract": contract,
                           "average_quotes": [_month_quote("2026-11", "30", 60.1)],
                           "interpolation": "Linear", "extrapolate_flat": True}],
               "include_details": True}
    row = _build(client, request)["curves"][0]
    quote = row["report"]["average_quotes"][0]
    fixings = [f["value"] for f in contract["fixings"]]
    weights = {w["month"]: w["weight"] for w in quote["weights"]}
    days = len(fixings) / (1.0 - sum(weights.values()))   # pricing days of November
    assert abs(days - round(days)) < 1e-9
    assert abs(quote["constant"] - sum(fixings) / round(days)) < 1e-12
    months = _months(row)
    assert months["2026-12"]["source"] == "Solved"
    priced = quote["constant"] + sum(w * months[m]["price"] for m, w in weights.items())
    assert abs(priced - 60.1) < 1e-9
    ref = commodity_curve_build_ql(request)
    _assert_report("partly past", row["report"], ref["report"], 1e-9)


def test_quote_with_no_live_month_400(client):
    request = {"pricing": {"as_of_date": "2026-11-13"},
               "curves": [{"contract": _benchmark_contract({"2027-01"}),
                           "average_quotes": [
                               {"price": 59.8,
                                "periods": {"start_date": "2026-11-02",
                                            "end_date": "2026-11-12", "frequency": "Once"},
                                "nearby": 1, "roll_days_before_expiry": 0}],
                           "interpolation": "Linear", "extrapolate_flat": True}]}
    _assert_status(client, request, 400, "curves[0].average_quotes[0]: no live month")


def test_details_weights_reprice_and_sensitivities_match_bumps(client):
    request = _load(UNIQUE)
    row = _build(client, request)["curves"][0]
    report = row["report"]
    months = _months(row)
    for k, quote in enumerate(report["average_quotes"]):
        priced = quote["constant"] + sum(w["weight"] * months[w["month"]]["price"]
                                         for w in quote["weights"])
        assert abs(priced - request["curves"][0]["average_quotes"][k]["price"]) < 1e-9
    h = 1e-3
    largest = 0.0
    for k in range(3):
        bumped = _load(UNIQUE)
        bumped["curves"][0]["average_quotes"][k]["price"] += h
        moved = _months(_build(client, bumped)["curves"][0])
        for name, month in months.items():
            sens = next(s["value"] for s in month["sensitivities"] if s.get("average_quote") == k)
            assert abs((moved[name]["price"] - month["price"]) / h - sens) < 1e-6, (k, name)
            largest = max(largest, abs(sens))
    assert abs(report["max_sensitivity"] - largest) < 1e-12
    # Without details: no weights, no sensitivities, no max_sensitivity.
    request["include_details"] = False
    plain = _build(client, request)["curves"][0]["report"]
    assert "max_sensitivity" not in plain
    assert all("weights" not in q for q in plain["average_quotes"])
    assert all("sensitivities" not in m for m in plain["months"])


def _swap_for_quote(as_of, contract, quote, curves, spot=None, base=None):
    """A swap paying the quote and receiving the average it quotes: on the
    contract, or on the spot price `spot` (then `contract` is None). A
    spread quote (versus_base, `base` = the built base row) is a swap of two
    floating legs: receive this curve's average, pay the base's average plus
    the quoted spread."""
    pay = {"payment_calendar": quote.get("payment_calendar", "NullCalendar"),
           "payment_lag": quote.get("payment_lag", 1),
           "payment_convention": quote.get("payment_convention", "Following"),
           "discounting_curve": quote.get("discounting_curve", "FLAT")}
    quantity = {"value": 1.0, "basis": quote.get("quantity_basis", "PerPeriod")}
    flat = {"id": "FLAT", "reference_date": as_of, "day_counter": "Actual365Fixed",
            "interpolator": "LogLinear", "bootstrap_trait": "InterpolatedDiscount",
            "points": [{"point_type": "DiscountFactorPoint",
                        "point": {"date": as_of, "discount_factor": 1.0}},
                       {"point_type": "DiscountFactorPoint",
                        "point": {"date": "2030-12-31", "discount_factor": 0.9}}]}
    if spot is None:
        floating = dict(price_type="FuturesFloatingPrice",
                        price={"contract_id": contract["id"], "nearby": quote["nearby"],
                               "roll_days_before_expiry": quote["roll_days_before_expiry"]})
        commodity = {"contracts": [contract]}
    else:
        floating = dict(price_type="SpotFloatingPrice", price={"spot_id": spot["id"]})
        commodity = {"spot_prices": [spot]}
    paid = dict(price_type="CommodityFixedPrice", price={"price": quote["price"]})
    if quote.get("versus_base"):
        if "spot" in base:
            paid = dict(price_type="SpotFloatingPrice",
                        price={"spot_id": base["spot"]["id"], "spread": quote["price"]})
            commodity.setdefault("spot_prices", []).append(base["spot"])
        else:
            paid = dict(price_type="FuturesFloatingPrice",
                        price={"contract_id": base["contract"]["id"],
                               "nearby": quote["base_nearby"],
                               "roll_days_before_expiry": quote["base_roll_days_before_expiry"],
                               "spread": quote["price"]})
            commodity.setdefault("contracts", []).append(base["contract"])
    return {"pricing": {"as_of_date": as_of, "rates": {"curves": list(curves) + [flat]},
                        "commodity": commodity},
            "swaps": [{"commodity_swap": {
                "leg1": dict(pay, pay_receive="Pay", periods=quote["periods"], quantity=quantity,
                             **paid),
                "leg2": dict(pay, pay_receive="Receive", periods=quote["periods"],
                             quantity=quantity, **floating)}}]}


@pytest.mark.parametrize("example", EXAMPLES)
def test_round_trip_every_quote_prices_to_zero(client, example):
    request = _load(example)
    as_of = request["pricing"]["as_of_date"]
    curves = (request["pricing"].get("rates") or {}).get("curves") or []
    rows = _build(client, request)["curves"]
    for i, row in enumerate(rows):
        entry = request["curves"][i]
        base = None
        if "basis" in entry:
            kind = "spot" if "spot" in entry else "contract"
            base = next(r for r, e in zip(rows, request["curves"])
                        if kind in e and e[kind]["id"] == entry["basis"]["base"])
        for k, quote in enumerate(entry.get("average_quotes") or []):
            swap = _swap_for_quote(as_of, row.get("contract"), quote, curves, row.get("spot"),
                                   base)
            status, text = _post(client, "commodity_swap", swap)
            assert status == 200, text[:400]
            npv = json.loads(text)["swaps"][0]["npv"]
            # 1e-8 x quantity; the fragile example amplifies the printed
            # 12-decimal prices.
            limit = 1e-6 if "fragile" in example else 1e-8
            assert abs(npv) < limit, (example, i, k, npv)


@pytest.mark.parametrize("case,example,tolerance",
                         [("monthly_strip", STRIP, 1e-6),
                          ("monthly_strip_rounded_to_cents", FRAGILE, 1e-6)])
def test_independent_reference_values(client, case, example, tolerance):
    with open(REFERENCES) as f:
        ref = json.load(f)[case]
    row = _build(client, _load(example))["curves"][0]
    months = _months(row)
    for month, price in ref["expected_month_prices"].items():
        assert abs(months[month]["price"] - price) < tolerance, (month, months[month], price)
    assert row["report"]["status"] == "Unique"
    # The first swap of the swap reference request priced on the built
    # contract.
    with open(SWAP_BENCHMARK) as f:
        swap = json.load(f)
    swap["swaps"] = swap["swaps"][:1]
    swap["include_flows"] = False
    swap["include_observations"] = False
    swap["pricing"]["commodity"]["contracts"] = [row["contract"]]
    status, text = _post(client, "commodity_swap", swap)
    assert status == 200, text[:400]
    npv = json.loads(text)["swaps"][0]["npv"]
    assert abs(npv - ref["swap_npv_first_benchmark_swap_on_built_contract"]) < 1e-5, npv


def test_fragile_build_shows_in_max_sensitivity(client):
    strip = _build(client, _load(STRIP))["curves"][0]["report"]["max_sensitivity"]
    fragile = _build(client, _load(FRAGILE))["curves"][0]["report"]["max_sensitivity"]
    # The same system: the same sensitivities; rounding the quotes to cents
    # moved the back months by tens because the sensitivities are large.
    assert abs(strip - fragile) < 1e-6 * strip
    assert fragile > 100.0, fragile


def test_twin_refuses_the_same_average_quote_builds():
    conflict = _load(UNIQUE)
    conflict["curves"][0]["average_quotes"].append(_month_quote("2027-03", "31", 58.0))
    with pytest.raises(ValueError, match="conflict"):
        commodity_curve_build_ql(conflict)
    no_rule = _load(FLAT_SMOOTH)
    del no_rule["curves"][0]["rule"]
    with pytest.raises(ValueError, match="rule is required"):
        commodity_curve_build_ql(no_rule)
    overlapping = _load(UNIQUE)
    overlapping["curves"][0]["average_quotes"] = [
        _two_period("2027-01-01", "2027-02-28", 51.0),
        _two_period("2027-02-01", "2027-03-31", 53.0)]
    overlapping["curves"][0]["rule"] = "Flat"
    with pytest.raises(ValueError, match="Flat"):
        commodity_curve_build_ql(overlapping)
    two = _load(UNIQUE)
    two["curves"][0]["contract"]["months"] = two["curves"][0]["contract"]["months"][:2]
    two["curves"][0]["average_quotes"] = [_two_period("2027-01-01", "2027-02-28", 51.0)]
    two["curves"][0]["rule"] = "Smooth"
    with pytest.raises(ValueError, match="Smooth"):
        commodity_curve_build_ql(two)


def test_twin_agrees_on_the_constructed_cases(client):
    overlapping = _load(UNIQUE)
    overlapping["curves"][0]["average_quotes"] = [
        _two_period("2027-01-01", "2027-02-28", 51.0),
        _two_period("2027-02-01", "2027-03-31", 53.0)]
    overlapping["curves"][0]["rule"] = "Smooth"
    redundant = _load(UNIQUE)
    march = _months(_build(client, _load(UNIQUE))["curves"][0])["2027-03"]["price"]
    redundant["curves"][0]["average_quotes"].append(_month_quote("2027-03", "31", march))
    for request in (overlapping, redundant):
        row = _build(client, request)["curves"][0]
        ref = commodity_curve_build_ql(request)
        _assert_report("constructed", row["report"], ref["report"], 1e-9)


# ---------------------------------------------------------------------------
# structured details of a refused build
# ---------------------------------------------------------------------------
#
# A refusal found by the solve or the fill keeps `error`, `code`,
# `code_name` and `message` and adds `details`: reason, curve and the fields
# that apply. A refusal for a plain input reason has no `details`.

def _refused(client, request, *fragments):
    text = _assert_status(client, request, 400, *fragments)
    body = json.loads(text)
    assert set(body) == {"error", "code", "code_name", "message", "details"}, sorted(body)
    assert body["error"] == body["message"] and body["code_name"] == "INVALID_ARGUMENT"
    return body["details"]


def test_details_quote_conflict(client):
    solved = _months(_build(client, _load(UNIQUE))["curves"][0])["2027-03"]["price"]
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"].append(_month_quote("2027-03", "31", 58.0))
    details = _refused(client, request, "curves[0].average_quotes[3]: conflicts with the other "
                                        "quotes: they imply 57.0246026561, it quotes 58, a gap "
                                        "of 0.975397343879 (it is determined by "
                                        "average_quotes[0], average_quotes[1], average_quotes[2])")
    assert set(details) == {"reason", "curve", "path", "average_quote", "implied_value",
                            "quoted_value", "gap", "depends_on"}
    assert details["path"] == "curves[0].average_quotes[3]"
    assert details["reason"] == "QuoteConflict"
    assert details["curve"] == 0 and details["average_quote"] == 3
    assert details["quoted_value"] == 58.0
    assert abs(details["implied_value"] - solved) < 1e-9
    assert details["gap"] == details["quoted_value"] - details["implied_value"]
    assert details["depends_on"] == [{"average_quote": 0}, {"average_quote": 1},
                                     {"average_quote": 2}]
    # Acting on the details: the quote at the implied value is accepted
    # (Redundant); without it the build is the unique one.
    request["curves"][0]["average_quotes"][3]["price"] = details["implied_value"]
    row = _build(client, request)["curves"][0]
    assert row["report"]["average_quotes"][3]["status"] == "Redundant"
    del request["curves"][0]["average_quotes"][3]
    _build(client, request)


def test_details_quote_conflict_with_a_direct_price(client):
    # January given as a direct price: the conflict names it as a month.
    request = _load(UNIQUE)
    build = request["curves"][0]
    build["contract"]["months"][0]["price"] = 50.0
    build["average_quotes"][0]["price"] = 50.5
    details = _refused(client, request, "curves[0].average_quotes[0]: conflicts")
    assert details["depends_on"] == [{"month": "2027-01"}]
    assert details["average_quote"] == 0 and abs(details["implied_value"] - 50.0) < 1e-12


def test_details_rule_required(client):
    request = _load(FLAT_SMOOTH)
    del request["curves"][1]["rule"]
    details = _refused(client, request, "curves[1].rule is required")
    assert details == {"reason": "RuleRequired", "curve": 1, "path": "curves[1].rule",
                       "months": ["2027-02", "2027-03"]}


def test_details_flat_cannot_resolve(client):
    request = _load(UNIQUE)
    build = request["curves"][0]
    build["average_quotes"] = [_two_period("2027-01-01", "2027-02-28", 51.0),
                               _two_period("2027-02-01", "2027-03-31", 53.0)]
    build["rule"] = "Flat"
    details = _refused(client, request, "curves[0].rule Flat cannot resolve these quotes")
    assert details["reason"] == "FlatCannotResolve" and details["rule"] == "Flat"
    assert details["path"] == "curves[0].rule"
    assert details["curve"] == 0 and details["months"]


def test_details_smooth_not_unique(client):
    request = _load(UNIQUE)
    build = request["curves"][0]
    build["contract"]["months"] = build["contract"]["months"][:2]
    build["average_quotes"] = [_two_period("2027-01-01", "2027-02-28", 51.0)]
    build["rule"] = "Smooth"
    details = _refused(client, request, "curves[0].rule Smooth has no unique solution")
    assert details == {"reason": "SmoothNotUnique", "curve": 0, "path": "curves[0].rule",
                       "months": ["2027-01", "2027-02"], "rule": "Smooth"}


def test_details_month_outside_range(client):
    request = _load("synthetic_extrapolate_flat_both_ends.json")
    request["curves"][0]["extrapolate_flat"] = False
    details = _refused(client, request, "curves[0].contract.months[1] (2026-12): last_trade_date "
                                        "2026-11-20 is before the first quoted live month")
    assert details == {"reason": "MonthOutsideRange", "curve": 0,
                       "path": "curves[0].contract.months[1]", "month": "2026-12",
                       "months": ["2026-12", "2027-01", "2027-09", "2027-10", "2027-11",
                                  "2027-12"]}
    # Acting on the details: flat extrapolation accepts them.
    request["curves"][0]["extrapolate_flat"] = True
    row = _build(client, request)["curves"][0]
    flat = [m["month"] for m in row["report"]["months"] if m["source"] == "Extrapolated"]
    assert flat == details["months"]


def test_details_quote_has_no_live_month(client):
    request = {"pricing": {"as_of_date": "2026-11-13"},
               "curves": [{"contract": _benchmark_contract({"2027-01"}),
                           "average_quotes": [
                               {"price": 59.8,
                                "periods": {"start_date": "2026-11-02",
                                            "end_date": "2026-11-12", "frequency": "Once"},
                                "nearby": 1, "roll_days_before_expiry": 0}],
                           "interpolation": "Linear", "extrapolate_flat": True}]}
    details = _refused(client, request, "curves[0].average_quotes[0]: no live month")
    assert details == {"reason": "QuoteHasNoLiveMonth", "curve": 0,
                       "path": "curves[0].average_quotes[0]", "average_quote": 0}


def test_details_non_positive_price(client):
    request = _load("synthetic_log_linear.json")
    request["curves"][0]["contract"]["months"][3]["price"] = 0.0
    details = _refused(client, request, "price 0 must be > 0 for interpolation LogLinear")
    assert details == {"reason": "NonPositivePrice", "curve": 0,
                       "path": "curves[0].contract.months[3].price", "month": "2027-02"}


def test_details_interpolation_required_and_no_priced_month(client):
    request = _linear()
    del request["curves"][0]["interpolation"]
    details = _refused(client, request, "curves[0].interpolation is required")
    assert details["reason"] == "InterpolationRequired" and details["curve"] == 0
    assert details["path"] == "curves[0].interpolation"
    assert details["months"] == ["2027-02", "2027-04", "2027-05", "2027-07", "2027-08",
                                 "2027-09", "2027-10", "2027-11"]
    request = _linear()
    for m in request["curves"][0]["contract"]["months"]:
        m.pop("price", None)
    details = _refused(client, request, "curves[0].contract.months: no live month has a price")
    assert details["reason"] == "NoPricedMonth" and len(details["months"]) == 13
    assert details["path"] == "curves[0].contract.months"


def test_input_errors_carry_their_path(client):
    # A field the decoder finds missing or malformed: InvalidInput with the
    # path the decoder composed, and the indexes it holds.
    request = _load(UNIQUE)
    del request["curves"][0]["average_quotes"][1]["nearby"]
    details = _refused(client, request, "curves[0].average_quotes[1].nearby is required")
    assert details == {"reason": "InvalidInput", "curve": 0,
                       "path": "curves[0].average_quotes[1].nearby", "average_quote": 1}
    request = _load(UNIQUE)
    del request["curves"][0]["average_quotes"][1]["price"]
    details = _refused(client, request, "curves[0].average_quotes[1].price is required")
    assert details["path"] == "curves[0].average_quotes[1].price"
    request = _linear()
    request["curves"][0]["contract"]["months"][2]["last_trade_date"] = "2026-12-32"
    details = _refused(client, request, "Invalid date")
    assert details == {"reason": "InvalidInput", "curve": 0,
                       "path": "curves[0].contract.months[2].last_trade_date"}
    request = _linear()
    request["curves"] = []
    assert _refused(client, request, "curves is required") == {"reason": "InvalidInput",
                                                               "path": "curves"}
    request = _linear()
    request["pricing"]["commodity"] = {"contracts": [request["curves"][0]["contract"]]}
    assert _refused(client, request, "pricing.commodity")["path"] == "pricing.commodity"
    # A value rule of the library: the field it names, relative paths made
    # absolute with the curve.
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"][1]["nearby"] = 0
    details = _refused(client, request, "curves[0].average_quotes[1].nearby must be >= 1")
    assert details == {"reason": "InvalidInput", "curve": 0,
                       "path": "curves[0].average_quotes[1].nearby", "average_quote": 1}
    request = _load(UNIQUE)
    del request["curves"][0]["average_quotes"][2]["quantity_basis"]
    details = _refused(client, request, "quantity_basis is required for a quote of 3 periods")
    assert details["path"] == "curves[0].average_quotes[2].quantity_basis"
    request = _linear()
    months = request["curves"][0]["contract"]["months"]
    months[3]["last_trade_date"] = months[2]["last_trade_date"]
    details = _refused(client, request, "curves[0].contract.months[3].last_trade_date")
    assert details == {"reason": "InvalidInput", "curve": 0,
                       "path": "curves[0].contract.months[3].last_trade_date"}


def test_swap_pricer_refusals_carry_details(client):
    # The swap pricer's own refusals about a quote's market data name the
    # quote's period and carry their data; an unknown discounting curve is a
    # 404 with details; so are the swap endpoint's errors.
    request = {"pricing": {"as_of_date": "2026-11-13"},
               "curves": [{"contract": _benchmark_contract({"2027-01"}),
                           "average_quotes": [
                               {"price": 59.8,
                                "periods": {"start_date": "2026-11-02",
                                            "end_date": "2026-11-30", "frequency": "Once"},
                                "nearby": 1, "roll_days_before_expiry": 0}],
                           "interpolation": "Linear", "extrapolate_flat": True}]}
    request["curves"][0]["contract"]["fixings"] = []
    body = json.loads(_assert_status(
        client, request, 400, "curves[0].average_quotes[0].periods[0] (2026-11-02 to "
                              "2026-11-30): commodity contract 'SYN_CL': no fixing"))
    assert body["details"] == {"reason": "MissingFixing", "curve": 0,
                               "path": "curves[0].average_quotes[0].periods[0]",
                               "average_quote": 0, "month": "2026-12", "date": "2026-11-02"}
    request = _load(UNIQUE)
    request["curves"][0]["average_quotes"][2]["discounting_curve"] = "NO_SUCH_CURVE"
    body = json.loads(_assert_status(client, request, 404, "discounting curve not found"))
    assert body["details"] == {"reason": "UnknownDiscountingCurve", "curve": 0,
                               "path": "curves[0].average_quotes[2].discounting_curve",
                               "average_quote": 2}
    # The swap endpoint.
    with open(SWAP_BENCHMARK) as f:
        swap = json.load(f)
    swap["swaps"][0]["commodity_swap"]["leg2"]["pay_receive"] = "Pay"
    status, text = _post(client, "commodity_swap", swap)
    assert status == 400 and json.loads(text)["details"] == {
        "reason": "InvalidInput", "swap": 0, "path": "swaps[0].commodity_swap.leg1.pay_receive"}
    with open(SWAP_BENCHMARK) as f:
        swap = json.load(f)
    del swap["swaps"][0]["commodity_swap"]["leg2"]["price"]["nearby"]
    status, text = _post(client, "commodity_swap", swap)
    assert status == 400 and "nearby is required" in text
    assert json.loads(text)["details"] == {
        "reason": "InvalidInput", "swap": 0, "path": "swaps[0].commodity_swap.leg2.price.nearby"}


# ---------------------------------------------------------------------------
# spot prices from dated points and average quotes
# ---------------------------------------------------------------------------

SPOT_STRIP = "synthetic_spot_monthly_strip_three_readings.json"
SPOT_QUARTER = "synthetic_spot_quarter_flat_and_smooth.json"
SPOT_REDUNDANT = "synthetic_spot_redundant_quarter_and_flat_tail.json"
SPOT_REFUSED = "refused_spot_conflicting_quarter.json"
SPOT_REFERENCES = (Path(__file__).resolve().parent / "data" / "commodity"
                   / "curve_build_spot_reference_values.json")
TRUE_STRIP = [60.0, 60.4, 60.8, 61.1, 61.3]


def _points(row):
    return {p["date"]: p for p in row["report"]["points"]}


def test_spot_strip_gives_back_the_points_its_quotes_came_from(client):
    # The quotes are the fair prices on a known strip (60.0 ... 61.3 on the
    # month ends, on month starts for ForwardFlat); each reading solves it
    # back, the first point pinned.
    rows = _build(client, _load(SPOT_STRIP))["curves"]
    for row in rows:
        assert "contract" not in row and "months" not in row["report"]
        report = row["report"]
        assert report["status"] == "Unique"
        assert (report["unknowns"], report["equations"], report["rank"]) == (5, 5, 5)
        prices = [p["price"] for p in row["spot"]["points"]]
        for got, want in zip(prices, TRUE_STRIP):
            assert abs(got - want) < 1e-9, (row["spot"]["interpolation"], prices)
        sources = [p["source"] for p in report["points"]]
        assert sources == ["Quoted", "Solved", "Solved", "Solved", "Solved"]
        assert report["max_repricing_difference"] < 1e-9
        weights = report["average_quotes"][0]["weights"]
        assert all("month" not in w and "date" in w for w in weights)
        sens = report["points"][1]["sensitivities"]
        first = row["spot"]["points"][0]["date"]
        assert sens[0] == {"date": first, "value": sens[0]["value"]}


def test_spot_backward_flat_month_quote_is_its_month_end(client):
    # BackwardFlat: every December day reads the 31 December point.
    row = _build(client, _load(SPOT_STRIP))["curves"][1]
    weights = row["report"]["average_quotes"][0]["weights"]
    assert weights == [{"date": "2026-12-31", "weight": 1.0}]


def test_spot_quarter_needs_a_rule_then_flat_and_smooth(client):
    request = _load(SPOT_QUARTER)
    del request["curves"][0]["rule"]
    details = _refused(client, request, "curves[0].rule is required",
                       "2027-01-31, 2027-02-28, 2027-03-31", "(4 points, 2 independent quotes)")
    assert details == {"reason": "RuleRequired", "curve": 0, "path": "curves[0].rule",
                       "dates": ["2027-01-31", "2027-02-28", "2027-03-31"]}
    flat, smooth = (_points(r) for r in _build(client, _load(SPOT_QUARTER))["curves"])
    assert flat["2027-01-31"]["price"] == flat["2027-02-28"]["price"] == \
        flat["2027-03-31"]["price"]
    assert [p["source"] for p in flat.values()] == ["Quoted", "Assumed", "Assumed", "Assumed"]
    # Smooth: second divided differences on the date axis are zero, so the
    # four points lie on one straight line in time.
    t = [(datetime.date.fromisoformat(d) - datetime.date(2026, 11, 13)).days / 365.0
         for d in smooth]
    p = [v["price"] for v in smooth.values()]
    slopes = [(p[i + 1] - p[i]) / (t[i + 1] - t[i]) for i in range(3)]
    assert max(slopes) - min(slopes) < 1e-9
    assert abs(flat["2027-03-31"]["price"] - smooth["2027-03-31"]["price"]) > 0.01


def test_spot_redundant_quote_and_filled_tail(client):
    row = _build(client, _load(SPOT_REDUNDANT))["curves"][0]
    report = row["report"]
    assert [q["status"] for q in report["average_quotes"]] == ["Used"] * 4 + ["Redundant"]
    points = _points(row)
    # 15 April lies after the last determined point and no quote reads it:
    # held at the last price (extrapolate_flat).
    assert points["2027-04-15"]["source"] == "Extrapolated"
    assert points["2027-04-15"]["price"] == points["2027-03-31"]["price"]


def test_spot_refused_example_is_a_conflict(client):
    details = _refused(client, _load(SPOT_REFUSED), "curves[0].average_quotes[4]: conflicts")
    assert details["reason"] == "QuoteConflict" and details["average_quote"] == 4
    assert abs(details["gap"] - 0.25) < 1e-9
    assert details["depends_on"] == [{"average_quote": k} for k in range(1, 4)]


def _spot_request(points, quotes, interpolation="Linear", extrapolate=False, **extra):
    build = {"spot": {"id": "SYN_SPOT", "pricing_calendar": "WeekendsOnly",
                      "interpolation": interpolation, "extrapolate_flat": extrapolate,
                      "points": points},
             "average_quotes": quotes}
    build.update(extra)
    return {"pricing": {"as_of_date": "2026-11-13"}, "curves": [build]}


def _once(start, end, price):
    return {"price": price,
            "periods": {"start_date": start, "end_date": end, "frequency": "Once"}}


def test_spot_input_errors(client):
    request = _load(SPOT_STRIP)
    request["curves"][0]["contract"] = _load("synthetic_linear_gaps.json")["curves"][0]["contract"]
    details = _refused(client, request, "curves[0]: give exactly one of contract")
    assert details == {"reason": "InvalidInput", "curve": 0, "path": "curves[0]"}
    request = _load(SPOT_STRIP)
    del request["curves"][0]["spot"]
    _refused(client, request, "curves[0]: give exactly one of contract")
    request = _load(SPOT_STRIP)
    request["curves"][1]["average_quotes"][2]["nearby"] = 1
    details = _refused(client, request, "curves[1].average_quotes[2].nearby is not used by a spot")
    assert details == {"reason": "InvalidInput", "curve": 1,
                       "path": "curves[1].average_quotes[2].nearby", "average_quote": 2}
    request = _load(SPOT_STRIP)
    request["curves"][0]["interpolation"] = "Linear"
    details = _refused(client, request, "curves[0].interpolation is not used by a spot")
    assert details["path"] == "curves[0].interpolation"
    request = _load(SPOT_STRIP)
    points = request["curves"][0]["spot"]["points"]
    points[2]["date"] = points[1]["date"]
    details = _refused(client, request, "curves[0].spot.points[2].date 2026-12-31 is not after "
                                        "the previous point")
    assert details == {"reason": "InvalidInput", "curve": 0,
                       "path": "curves[0].spot.points[2].date"}


def test_spot_quote_day_outside_the_points(client):
    # March days lie after the last point and extrapolate_flat is false: the
    # swap's own sentence, with the quote and the first such day.
    request = _spot_request([{"date": "2026-11-30", "price": 60.0}, {"date": "2027-02-28"}],
                            [_once("2027-02-01", "2027-03-31", 60.5)])
    details = _refused(client, request, "curves[0].average_quotes[0].periods[0] (2027-02-01 to "
                                        "2027-03-31): commodity spot price 'SYN_SPOT': 2027-03-01 "
                                        "is outside its points")
    assert details == {"reason": "DateOutsideRange", "curve": 0,
                       "path": "curves[0].average_quotes[0].periods[0]", "average_quote": 0,
                       "date": "2027-03-01"}


def test_spot_point_to_fill_outside_and_no_priced_point(client):
    request = _spot_request([{"date": "2026-11-30", "price": 60.0}, {"date": "2026-12-31"},
                             {"date": "2027-01-31"}],
                            [_once("2026-12-01", "2026-12-31", 60.2)])
    details = _refused(client, request, "curves[0].spot.points[2] (2027-01-31) is after the last "
                                        "determined point")
    assert details == {"reason": "DateOutsideRange", "curve": 0,
                       "path": "curves[0].spot.points[2]", "date": "2027-01-31",
                       "dates": ["2027-01-31"]}
    request["curves"][0]["spot"]["extrapolate_flat"] = True
    row = _build(client, request)["curves"][0]
    assert _points(row)["2027-01-31"]["source"] == "Extrapolated"
    request = _spot_request([{"date": "2026-11-30"}, {"date": "2026-12-31"}], [])
    details = _refused(client, request, "curves[0].spot.points: no point has a price")
    assert details == {"reason": "NoPricedPoint", "curve": 0, "path": "curves[0].spot.points",
                       "dates": ["2026-11-30", "2026-12-31"]}


def test_spot_interpolated_point(client):
    # 15 December, between two determined points, read with the spot's own
    # Linear interpolation: 16 of 31 days from 30 November to 31 December.
    request = _spot_request([{"date": "2026-11-30", "price": 60.0}, {"date": "2026-12-15"},
                             {"date": "2026-12-31", "price": 61.0}], [])
    points = _points(_build(client, request)["curves"][0])
    assert points["2026-12-15"]["source"] == "Interpolated"
    assert abs(points["2026-12-15"]["price"] - (60.0 + 15.0 / 31.0)) < 1e-12


def test_spot_point_before_the_valuation_date_is_a_node(client):
    # A point dated before the valuation date is read by a later day that
    # lies between it and the next point (Linear), so it is solved like any
    # other point.
    request = _spot_request([{"date": "2026-10-31", "price": 59.0}, {"date": "2026-12-31"}],
                            [_once("2026-11-16", "2026-11-30", 59.7)])
    row = _build(client, request)["curves"][0]
    assert _points(row)["2026-12-31"]["source"] == "Solved"
    assert row["report"]["max_repricing_difference"] < 1e-9


def test_swap_still_refuses_an_unpriced_spot_point(client):
    built = _build(client, _load(SPOT_STRIP))["curves"][0]["spot"]
    del built["points"][2]["price"]
    quote = _load(SPOT_STRIP)["curves"][0]["average_quotes"][0]
    swap = _swap_for_quote("2026-11-13", None, quote, [], built)
    status, text = _post(client, "commodity_swap", swap)
    assert status == 400 and "points[2].price is required" in text, text[:300]


@pytest.mark.parametrize("case", ["spot_avg_linear", "spot_avg_backflat",
                                  "spot_avg_linear_pinned", "spot_avg_quarter_fills_gap"])
def test_spot_independent_reference_values(client, case):
    # Points on the last pricing day of each quote period, the first one held
    # flat back to the valuation date (extrapolate_flat): the point prices and
    # two swaps on the built spot price against independent reference values.
    with open(SPOT_REFERENCES) as f:
        ref = json.load(f)["cases"][case]
    row = _build(client, ref["request"])["curves"][0]
    points = _points(row)
    for date, price in ref["expected_point_prices"].items():
        assert abs(points[date]["price"] - price) < 1e-9, (date, points[date], price)
    assert row["report"]["status"] == "Unique"
    for swap in ref["swaps"]:
        request = {"pricing": {"as_of_date": ref["request"]["pricing"]["as_of_date"],
                               "rates": ref["request"]["pricing"]["rates"],
                               "commodity": {"spot_prices": [row["spot"]]}},
                   "swaps": [{"commodity_swap": swap["commodity_swap"]}]}
        status, text = _post(client, "commodity_swap", request)
        assert status == 200, text[:400]
        npv = json.loads(text)["swaps"][0]["npv"]
        assert abs(npv - swap["npv"]) < 1e-6, (swap["id"], npv, swap["npv"])


# ---------------------------------------------------------------------------
# basis: a contract priced as another contract of the request plus a basis
# ---------------------------------------------------------------------------

BASIS_REFERENCES = (Path(__file__).resolve().parent / "data" / "commodity"
                    / "curve_build_basis_reference_values.json")


def _basis_case(name):
    with open(BASIS_REFERENCES) as f:
        return json.load(f)["cases"][name]


@pytest.mark.parametrize("case", ["basis_full", "basis_gaps", "basis_subtract",
                                  "basis_monthoffset1", "basis_avg_base",
                                  "basis_avg_nobaseavg"])
def test_basis_independent_reference_values(client, case):
    # The basis entry comes first and its base second: the base is built
    # first whatever the order, and both come back in request order.
    # basis_monthoffset1: the reference shifts the base month twice for an
    # offset of 1 (a defect of that implementation); the same base months are
    # read here with base_month_offset 2. basis_subtract: the reference
    # subtracts the basis; the request sends the negated values.
    ref = _basis_case(case)
    response = _build(client, ref["request"])
    row, base = response["curves"]
    assert row["contract"]["id"] == "SYN_BAS" and base["contract"]["id"] == "SYN_CL"
    months = _months(row)
    for month, price in ref["expected_month_prices"].items():
        assert abs(months[month]["price"] - price) < 1e-9, (month, months[month], price)
        assert abs(months[month]["base_price"] + months[month]["basis"] - price) < 1e-9
    quoted = {q["month"] for q in ref["request"]["curves"][0]["basis"]["quotes"]}
    for m in row["report"]["months"]:
        if m["month"] in quoted:
            assert m["source"] == "Basis", m
        else:
            assert m["source"] in ("Interpolated", "Extrapolated"), m
    for swap in ref["swaps"]:
        request = {"pricing": {"as_of_date": ref["request"]["pricing"]["as_of_date"],
                               "rates": ref["request"]["pricing"]["rates"],
                               "commodity": {"contracts": [row["contract"]]}},
                   "swaps": [{"commodity_swap": swap["commodity_swap"]}]}
        status, text = _post(client, "commodity_swap", request)
        assert status == 200, text[:400]
        npv = json.loads(text)["swaps"][0]["npv"]
        assert abs(npv - swap["npv"]) < 1e-6, (swap["id"], npv, swap["npv"])


def test_basis_gaps_fill_the_basis_not_the_price(client):
    # February has no basis quote: its basis is linear in time between the
    # January (0.75, 23 Dec) and April (0.78, 24 Mar) quotes, 30 of 91 days.
    row = _build(client, _basis_case("basis_gaps")["request"])["curves"][0]
    feb = _months(row)["2027-02"]
    assert feb["source"] == "Interpolated"
    assert abs(feb["basis"] - (0.75 + 0.03 * 30 / 91)) < 1e-12
    assert abs(feb["price"] - (60.7 + feb["basis"])) < 1e-12


def test_basis_months_without_basis_have_no_base_fields(client):
    row = _build(client, _load("synthetic_linear_gaps.json"))["curves"][0]
    assert all("base_price" not in m and "basis" not in m for m in row["report"]["months"])


def test_basis_with_an_average_quote_on_top(client):
    # A basis quote on two months and a one-month average quote on the
    # basis contract: the quote is solved on top of the basis prices.
    ref = _basis_case("basis_full")
    request = copy.deepcopy(ref["request"])
    build = request["curves"][0]
    build["basis"]["quotes"] = [q for q in build["basis"]["quotes"]
                                if q["month"] in ("2027-01", "2027-03")]
    build["average_quotes"] = [{"price": 61.5, "periods": {"start_date": "2027-01-01",
                                                           "end_date": "2027-01-31",
                                                           "frequency": "Once"},
                                "nearby": 1, "roll_days_before_expiry": 0}]
    build["extrapolate_flat"] = True
    row = _build(client, request)["curves"][0]
    months = _months(row)
    assert months["2027-01"]["source"] == "Basis" and months["2027-03"]["source"] == "Basis"
    assert months["2027-02"]["source"] == "Solved"
    assert row["report"]["max_repricing_difference"] < 1e-9


def _refused_basis(client, request, status, *fragments):
    text = _assert_status(client, request, status, *fragments)
    return json.loads(text)["details"]


def test_basis_refusals(client):
    ref = _basis_case("basis_full")
    # Unknown base: a 404 with details.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base"] = "NO_SUCH"
    details = _refused_basis(client, request, 404, "curves[0].basis.base: no contract build")
    assert details == {"reason": "UnknownBase", "curve": 0, "path": "curves[0].basis.base"}
    # Its own base.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base"] = "SYN_BAS"
    details = _refused_basis(client, request, 400, "cannot be its own base")
    assert details["reason"] == "BasisCycle" and details["cycle"] == [0]
    # A cycle of two.
    request = copy.deepcopy(ref["request"])
    request["curves"][1]["basis"] = {"base": "SYN_BAS", "base_price": "SameMonth"}
    details = _refused_basis(client, request, 400, "the bases form a cycle")
    assert details["reason"] == "BasisCycle" and sorted(details["cycle"]) == [0, 1]
    # A month with both a price and a basis quote.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["contract"]["months"][1]["price"] = 61.0
    details = _refused_basis(client, request, 400, "also has a price")
    assert details == {"reason": "BasisAndPrice", "curve": 0,
                       "path": "curves[0].basis.quotes[1]", "month": "2027-01"}
    # A base month that is not listed.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base_month_offset"] = 4
    details = _refused_basis(client, request, 400, "curves[0].basis.base: no base price for "
                                                   "2026-12")
    assert details == {"reason": "BaseNotDetermined", "curve": 0, "path": "curves[0].basis.base",
                       "month": "2026-12", "base_curve": 1, "base_month": "2026-08"}
    # An expired base month without its fixing.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base_month_offset"] = 2
    request["curves"][1]["contract"]["fixings"] = \
        request["curves"][1]["contract"]["fixings"][1:]
    details = _refused_basis(client, request, 400, "expired on 2026-09-22 and has no fixing")
    assert details["base_month"] == "2026-10" and details["base_curve"] == 1
    # MonthAverage needs its nearby and roll; SameMonth refuses them.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base_price"] = "MonthAverage"
    details = _refused_basis(client, request, 400, "basis.base_nearby is required")
    assert details["path"] == "curves[0].basis.base_nearby"
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base_nearby"] = 1
    details = _refused_basis(client, request, 400, "is not used when base_price is SameMonth")
    assert details["path"] == "curves[0].basis.base_nearby"
    # A basis quote on an expired month.
    request = copy.deepcopy(ref["request"])
    request["pricing"]["as_of_date"] = "2026-12-01"
    request["pricing"]["rates"]["curves"][0]["reference_date"] = "2026-12-01"
    request["pricing"]["rates"]["curves"][0]["points"][0]["point"]["date"] = "2026-12-01"
    details = _refused_basis(client, request, 400, "expired on 2026-11-24")
    assert details["reason"] == "BasisOnExpiredMonth" and details["month"] == "2026-12"
    # A contract field on a spot basis, and a base that is a spot price.
    request = _load(SPOT_STRIP)
    request["curves"][0]["basis"] = {"base": "X", "base_price": "SameMonth"}
    _refused_basis(client, request, 400, "curves[0].basis.base_price is not used on a spot "
                                         "price build")
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["base"] = "SYN_SPOT_LINEAR"
    request["curves"].append(_load(SPOT_STRIP)["curves"][0])
    details = _refused_basis(client, request, 400, "is a spot price build")
    assert details["reason"] == "BaseNotContract"
    # A month that is not in the contract.
    request = copy.deepcopy(ref["request"])
    request["curves"][0]["basis"]["quotes"][0]["month"] = "2030-01"
    details = _refused_basis(client, request, 400, "is not a month of the contract")
    assert details["path"] == "curves[0].basis.quotes[0].month"


def test_basis_log_linear_on_a_non_positive_basis(client):
    # LogLinear reads the log of the basis: a basis <= 0 at a node is the
    # same 400 as a non-positive price, worded for the basis.
    request = copy.deepcopy(_basis_case("basis_gaps")["request"])
    request["curves"][0]["interpolation"] = "LogLinear"
    request["curves"][0]["basis"]["quotes"][1]["value"] = -0.1
    details = _refused_basis(client, request, 400, "(2027-04): basis -0.1",
                             "must be > 0 for interpolation LogLinear (it reads log basis)")
    assert details["reason"] == "NonPositivePrice"


def test_basis_refused_example_is_a_cycle(client):
    details = _refused(client, _load("refused_basis_cycle.json"), "the bases form a cycle")
    assert details["reason"] == "BasisCycle" and sorted(details["cycle"]) == [0, 1]


def test_basis_month_average_example_reads_the_base_front_month(client):
    # November 2026: nine fixings (2-12 November) of the December base month,
    # then 13-20 November at 60.0 and 23-30 November (26th a holiday) on the
    # January month at 60.4: 9 + 6 + 5 = 20 pricing days.
    request = _load("synthetic_basis_month_average.json")
    row = _build(client, request)["curves"][1]
    nov = _months(row)["2026-11"]
    fixings = [f["value"] for f in request["curves"][0]["contract"]["fixings"]]
    expected = (sum(fixings) + 6 * 60.0 + 5 * 60.4) / 20
    assert abs(nov["base_price"] - expected) < 1e-12
    assert abs(nov["price"] - (expected + 0.80)) < 1e-12
    assert nov["source"] == "Basis"


# ---------------------------------------------------------------------------
# basis on spot builds, spread quotes to the base, sensitivities through the
# base
# ---------------------------------------------------------------------------

SPOT_BASIS = "synthetic_spot_basis_by_date_with_spread_quote.json"
SPREAD_QUARTER = "synthetic_basis_quarter_spread_to_base.json"


def _spot_read(spot, date):
    """The twin's reading of a built spot price on a date."""
    import ql_reference as ref
    weights = ref._commodity_spot_weights(spot["points"], ref.parse_date(date),
                                          spot["interpolation"],
                                          bool(spot.get("extrapolate_flat", False)), spot["id"])
    return sum(w * float(spot["points"][i]["price"]) for i, w in weights)


def test_spot_basis_reads_the_base_on_each_date(client):
    request = _load(SPOT_BASIS)
    row, base = _build(client, request)["curves"]
    points = {p["date"]: p for p in row["report"]["points"]}
    quotes = {q["date"]: q["value"] for q in request["curves"][0]["basis"]["quotes"]}
    for date, p in points.items():
        assert _close(p["base_price"], _spot_read(base["spot"], date))
        assert _close(p["price"], p["base_price"] + p["basis"])
        if date in quotes:
            assert p["source"] == "Basis" and _close(p["basis"], quotes[date])
    # December is solved from the spread quote, the rest filled on the basis.
    assert points["2026-12-31"]["source"] == "Solved"
    assert points["2027-01-29"]["source"] == "Interpolated"
    assert row["report"]["max_repricing_difference"] < 1e-9


def test_spread_quotes_reprice_as_two_swaps(client):
    # A spread quote's swap: receive this curve's average, pay the base's
    # average plus the spread (worth zero, test_round_trip_every_quote_...);
    # and the same as two fixed-price swaps: fair(this) - fair(base) = spread.
    for name in (SPOT_BASIS, SPREAD_QUARTER):
        request = _load(name)
        rows = _build(client, request)["curves"]
        for i, entry in enumerate(request["curves"]):
            for quote in entry.get("average_quotes") or []:
                if not quote.get("versus_base"):
                    continue
                kind = "spot" if "spot" in entry else "contract"
                base = next(r for r, e in zip(rows, request["curves"])
                            if kind in e and e[kind]["id"] == entry["basis"]["base"])
                fairs = []
                for curve, nearby in ((rows[i], quote.get("nearby")),
                                      (base, quote.get("base_nearby"))):
                    q = dict(quote, versus_base=False)
                    if kind == "contract":
                        q["nearby"] = nearby
                        q["roll_days_before_expiry"] = (
                            quote["roll_days_before_expiry"] if curve is rows[i]
                            else quote["base_roll_days_before_expiry"])
                    swap = _swap_for_quote(request["pricing"]["as_of_date"],
                                           curve.get("contract"), q, [], curve.get("spot"))
                    status, text = _post(client, "commodity_swap", swap)
                    assert status == 200, text[:400]
                    fairs.append(json.loads(text)["swaps"][0]["fair_fixed_price"])
                assert abs(fairs[0] - fairs[1] - quote["price"]) < 1e-9, (name, fairs)


def test_spot_basis_reproduces_a_known_curve(client):
    # A known local spot price (base + a basis that moves month to month) on
    # month-end points; its quotes generated from it through the swap
    # endpoint: one basis quote on the first point, then each month's
    # average spread to the base. The build gives every point back.
    request = _load(SPOT_BASIS)
    base = request["curves"][1]["spot"]
    local = copy.deepcopy(request["curves"][0]["spot"])
    true_basis = [0.40, 0.47, 0.43, 0.52, 0.55, 0.58, 0.62, 0.60]
    for p, b in zip(local["points"], true_basis):
        p["price"] = _spot_read(base, p["date"]) + b
    as_of = request["pricing"]["as_of_date"]
    quotes = []
    months = [("2026-12-01", "2026-12-31"), ("2027-01-01", "2027-01-29"),
              ("2027-02-01", "2027-02-26"), ("2027-03-01", "2027-03-31"),
              ("2027-04-01", "2027-04-30"), ("2027-05-01", "2027-05-28"),
              ("2027-06-01", "2027-06-30")]
    for start, end in months:
        q = {"price": 0.0, "periods": {"start_date": start, "end_date": end,
                                       "frequency": "Once"}}
        fairs = []
        for spot in (local, base):
            status, text = _post(client, "commodity_swap",
                                 _swap_for_quote(as_of, None, q, [], spot))
            assert status == 200, text[:400]
            fairs.append(json.loads(text)["swaps"][0]["fair_fixed_price"])
        quotes.append(dict(q, price=fairs[0] - fairs[1], versus_base=True))
    unpriced = copy.deepcopy(local)
    for p in unpriced["points"]:
        del p["price"]
    build = {"spot": unpriced, "average_quotes": quotes,
             "basis": {"base": base["id"], "quotes": [{"date": local["points"][0]["date"],
                                                       "value": true_basis[0]}]}}
    row = _build(client, {"pricing": {"as_of_date": as_of},
                          "curves": [build, {"spot": base}]})["curves"][0]
    for got, want in zip(row["spot"]["points"], local["points"]):
        assert abs(got["price"] - want["price"]) < 1e-9, (got, want)
    assert row["report"]["status"] == "Unique"


def test_sensitivities_through_the_base_match_bump_and_rebuild(client):
    # A basis entry's determined months list the base's inputs too (`curve`
    # = the base's index): each equals the twin's central difference of the
    # basis build with that base input moved.
    import ql_reference as ref
    request = _load(SPREAD_QUARTER)
    row = _build(client, request)["curves"][1]
    base_contract = request["curves"][0]["contract"]
    h = 1e-4
    checked = 0
    for month in row["report"]["months"]:
        through = [s for s in month.get("sensitivities", []) if s.get("curve") == 0]
        if not month.get("sensitivities"):
            continue
        assert len(through) == len([m for m in base_contract["months"] if "price" in m])
        for s in through:
            values = []
            for step in (h, -h):
                bumped = copy.deepcopy(request)
                m = next(x for x in bumped["curves"][0]["contract"]["months"]
                         if x["month"] == s["month"])
                m["price"] += step
                built = ref.commodity_curve_build_ql(bumped, 1)["contract"]
                values.append(next(x["price"] for x in built["months"]
                                   if x["month"] == month["month"]))
            fd = (values[0] - values[1]) / (2 * h)
            assert abs(s["value"] - fd) <= 1e-6 * max(1.0, abs(fd)), (month["month"], s, fd)
            checked += 1
    assert checked > 0
    # No basis: no `curve` field anywhere (reports unchanged).
    plain = _build(client, _load("synthetic_average_quotes_quarter_flat_and_smooth.json"))
    assert "\"curve\":" not in json.dumps(plain)


def test_basis_stage_two_refusals(client):
    request = _load(SPOT_BASIS)
    # A spot basis on a contract base.
    bad = copy.deepcopy(request)
    bad["curves"].append({"contract": copy.deepcopy(_load(SPREAD_QUARTER)["curves"][0]["contract"])})
    bad["curves"][0]["basis"]["base"] = "SYN_CL"
    details = json.loads(_assert_status(client, bad, 400, "is a contract build; the base of a "
                                                          "spot price must be a spot price"))
    assert details["details"]["reason"] == "BaseNotSpot"
    # A basis quote on a date that is not a point.
    bad = copy.deepcopy(request)
    bad["curves"][0]["basis"]["quotes"][0]["date"] = "2026-12-01"
    _assert_status(client, bad, 400, "curves[0].basis.quotes[0].date 2026-12-01 is not the date "
                                     "of a point")
    # Contract fields on a spot basis.
    for field, value in (("base_price", "SameMonth"), ("base_nearby", 1)):
        bad = copy.deepcopy(request)
        bad["curves"][0]["basis"][field] = value
        _assert_status(client, bad, 400, f"curves[0].basis.{field} is not used on a spot price")
    bad = copy.deepcopy(request)
    bad["curves"][0]["basis"]["quotes"][0]["month"] = "2026-11"
    _assert_status(client, bad, 400, "curves[0].basis.quotes[0].month is not used on a spot")
    # A base read outside its points without extrapolation.
    bad = copy.deepcopy(request)
    bad["curves"][1]["spot"]["extrapolate_flat"] = False
    bad["curves"][1]["spot"]["points"] = bad["curves"][1]["spot"]["points"][:7]
    details = json.loads(_assert_status(client, bad, 400, "curves[0].basis.base: no base price "
                                                          "for 2027-05-28"))["details"]
    assert details["reason"] == "BaseNotDetermined" and details["date"] == "2027-05-28"
    assert details["base_curve"] == 1
    # Spread quote fields.
    bad = copy.deepcopy(request)
    del bad["curves"][0]["basis"]
    _assert_status(client, bad, 400, "curves[0].average_quotes[0].versus_base needs basis")
    bad = _load(SPREAD_QUARTER)
    del bad["curves"][1]["average_quotes"][0]["base_nearby"]
    _assert_status(client, bad, 400, "curves[1].average_quotes[0].base_nearby is required with "
                                     "versus_base")
    bad = _load(SPREAD_QUARTER)
    bad["curves"][1]["average_quotes"][0]["versus_base"] = False
    _assert_status(client, bad, 400, "curves[1].average_quotes[0].base_nearby is only used with "
                                     "versus_base")
    bad = copy.deepcopy(request)
    bad["curves"][0]["average_quotes"][0]["base_nearby"] = 1
    _assert_status(client, bad, 400, "base_nearby is not used on a spot price build")


# ---------------------------------------------------------------------------
# curve builds inside the swap request (pricing.commodity.curve_builds)
# ---------------------------------------------------------------------------

QUOTE_DELTA_REFERENCE = (Path(__file__).resolve().parent / "data" / "commodity"
                         / "swap_quote_delta_reference_values.json")
FLAT_DISCOUNT = {"id": "FLAT_DISC", "reference_date": "2026-11-13",
                 "day_counter": "Actual365Fixed", "interpolator": "LogLinear",
                 "bootstrap_trait": "InterpolatedDiscount",
                 "points": [{"point_type": "DiscountFactorPoint",
                             "point": {"date": "2026-11-13", "discount_factor": 1.0}},
                            {"point_type": "DiscountFactorPoint",
                             "point": {"date": "2030-12-31", "discount_factor": 0.85}}]}


def _swaps_on_built(request, rows):
    """One swap per built entry: 1,000 paid fixed at 60 against its average
    from the valuation date to its last live month's last trading date
    (contract: front month, roll 0) or its last point (spot), one period."""
    as_of = request["pricing"]["as_of_date"]
    swaps = []
    for entry, row in zip(request["curves"], rows):
        if "contract" in row:
            live = [m for m in row["contract"]["months"] if m["last_trade_date"] >= as_of]
            end = live[-1]["last_trade_date"]
            price = {"contract_id": row["contract"]["id"], "nearby": 1,
                     "roll_days_before_expiry": 0}
            kind = "FuturesFloatingPrice"
            start = as_of
        else:
            end = row["spot"]["points"][-1]["date"]
            first = row["spot"]["points"][0]["date"]
            start = max(as_of, first)
            price = {"spot_id": row["spot"]["id"]}
            kind = "SpotFloatingPrice"
        leg = {"periods": {"start_date": start, "end_date": end, "frequency": "Once"},
               "quantity": {"value": 1000.0, "basis": "PerPeriod"},
               "payment_calendar": "UnitedStatesSettlement", "payment_lag": 2,
               "payment_convention": "Following", "discounting_curve": "FLAT_DISC"}
        swaps.append({"commodity_swap": {
            "leg1": dict(leg, pay_receive="Pay", price_type="CommodityFixedPrice",
                         price={"price": 60.0}),
            "leg2": dict(leg, pay_receive="Receive", price_type=kind, price=price)}})
    return swaps


def _in_request(build_request, swaps):
    pricing = copy.deepcopy(build_request["pricing"])
    pricing.setdefault("rates", {"curves": []})
    pricing["rates"].setdefault("curves", [])
    pricing["rates"]["curves"].append(FLAT_DISCOUNT)
    pricing["commodity"] = {"curve_builds": copy.deepcopy(build_request["curves"])}
    return {"pricing": pricing, "swaps": swaps}


@pytest.mark.parametrize("example", EXAMPLES)
def test_curve_builds_in_the_swap_request_equal_build_then_paste(client, example):
    # The same entries built inside the swap request and built by
    # /build-commodity-curves then pasted: the same NPVs up to the pasted
    # prices' 12 printed decimals (bounded by the sum of |delta| x 1e-12),
    # and the in-request NPVs against the twin. Examples that build one id
    # twice (two rules on one contract) go one entry at a time: the swap
    # request registers an id once.
    full = _load(example)
    ids = [e.get("contract", e.get("spot"))["id"] for e in full["curves"]]
    if len(set(ids)) == len(ids):
        requests = [full]
    else:
        requests = [dict(full, curves=[e]) for e in full["curves"]]
    for build_request in requests:
        _check_in_request_equals_paste(client, example, build_request)


def _check_in_request_equals_paste(client, example, build_request):
    rows = _build(client, build_request)["curves"]
    swaps = _swaps_on_built(build_request, rows)
    inside = _in_request(build_request, swaps)
    status, text = _post(client, "commodity_swap", inside)
    assert status == 200, text[:500]
    in_rows = json.loads(text)["swaps"]
    pasted = copy.deepcopy(inside)
    pasted["pricing"]["commodity"] = {
        "contracts": [r["contract"] for r in rows if "contract" in r],
        "spot_prices": [r["spot"] for r in rows if "spot" in r]}
    status, text = _post(client, "commodity_swap", pasted)
    assert status == 200, text[:500]
    paste_rows = json.loads(text)["swaps"]
    for i, (a, b) in enumerate(zip(in_rows, paste_rows)):
        bound = sum(abs(d["delta"]) for d in b["deltas"]) * 1e-12 + 1e-9
        assert abs(a["npv"] - b["npv"]) <= bound, (example, i, a["npv"], b["npv"])
        assert a["deltas"] == b["deltas"] or all(
            abs(x["delta"] - y["delta"]) <= 1e-9 * max(1.0, abs(y["delta"]))
            for x, y in zip(a["deltas"], b["deltas"]))
        assert "quote_deltas" in a and "quote_deltas" not in b
        ref = commodity_swap_ql(inside, i)
        assert abs(a["npv"] - ref["npv"]) <= 1e-9 * max(1.0, abs(ref["npv"])), (example, i)


def test_quote_deltas_match_bump_and_rebuild(client):
    # Each quote delta against the twin's central difference: the input moved
    # by h in the request's curve builds, everything rebuilt and repriced.
    for example in ("synthetic_average_quotes_months_and_quarter_unique.json",
                    "synthetic_basis_quarter_spread_to_base.json",
                    "synthetic_spot_basis_by_date_with_spread_quote.json",
                    "synthetic_kruger_cubic.json"):
        build_request = _load(example)
        rows = _build(client, build_request)["curves"]
        inside = _in_request(build_request, _swaps_on_built(build_request, rows))
        status, text = _post(client, "commodity_swap", inside)
        assert status == 200, text[:500]
        result = json.loads(text)
        ids = [e.get("contract", e.get("spot"))["id"] for e in build_request["curves"]]
        assert [s["id"] for s in result["curve_builds"]] == ids
        for i, swap in enumerate(result["swaps"]):
            assert swap["quote_deltas"], (example, i)
            for qd in swap["quote_deltas"]:
                j = ids.index(qd["curve_id"])
                h = 1e-4
                npvs = []
                for step in (h, -h):
                    bumped = copy.deepcopy(inside)
                    entry = bumped["pricing"]["commodity"]["curve_builds"][j]
                    _bump_input(entry, qd, step)
                    npvs.append(commodity_swap_ql(bumped, i)["npv"])
                fd = (npvs[0] - npvs[1]) / (2 * h)
                assert abs(qd["delta"] - fd) <= 1e-6 * max(1.0, abs(fd)), (example, i, qd, fd)


def _bump_input(entry, qd, step):
    if "average_quote" in qd:
        entry["average_quotes"][qd["average_quote"]]["price"] += step
        return
    key, value = ("month", qd["month"]) if "month" in qd else ("date", qd["date"])
    for q in (entry.get("basis") or {}).get("quotes") or []:
        if q.get(key) == value:
            q["value"] += step
            return
    items = entry["contract"]["months"] if key == "month" else entry["spot"]["points"]
    item = next(x for x in items if x[key] == value)
    item["price"] += step


def test_quote_deltas_independent_reference_values(client):
    # The first benchmark swap on the strip built from a pinned front month and
    # eleven monthly average quotes: d NPV / d quote against the independent
    # reference's central differences (step 1e-4, rebuilt each time); the
    # reference's own bumps agree with each other to about 1e-8 relative.
    request = json.loads((ROOT / "examples" / "data" / "commodity_swap"
                          / "synthetic_fixed_vs_contract_built_from_quotes_in_request.json"
                          ).read_text())
    with open(QUOTE_DELTA_REFERENCE) as f:
        ref = json.load(f)
    status, text = _post(client, "commodity_swap", request)
    assert status == 200, text[:500]
    swap = json.loads(text)["swaps"][0]
    assert abs(swap["npv"] - ref["base_npv"]) < 1e-6
    by_input = {(d.get("month"), d.get("average_quote")): d["delta"]
                for d in swap["quote_deltas"]}
    worst = 0.0
    for r in ref["quote_deltas"]:
        got = by_input[(r.get("month"), r.get("average_quote"))]
        worst = max(worst, abs(got - r["central_difference"]))
        assert abs(got - r["central_difference"]) < 1e-3, (r, got)
    print(f"[commodity] quote deltas vs independent reference: max abs gap {worst:.3e}")


def test_curve_builds_in_the_swap_request_refusals(client):
    request = json.loads((ROOT / "examples" / "data" / "commodity_swap"
                          / "synthetic_fixed_vs_contract_built_from_quotes_in_request.json"
                          ).read_text())
    entry = request["pricing"]["commodity"]["curve_builds"][0]
    # The same id given as a finished contract and built.
    bad = copy.deepcopy(request)
    bad["pricing"]["commodity"]["contracts"] = [copy.deepcopy(entry["contract"])]
    for m in bad["pricing"]["commodity"]["contracts"][0]["months"]:
        m.setdefault("price", 60.0)
    status, text = _post(client, "commodity_swap", bad)
    assert status == 400 and ("pricing.commodity.curve_builds[0].contract.id: 'SYN_CL' is also "
                              "given in pricing.commodity.contracts") in text, text[:400]
    # Built twice.
    bad = copy.deepcopy(request)
    bad["pricing"]["commodity"]["curve_builds"].append(copy.deepcopy(entry))
    status, text = _post(client, "commodity_swap", bad)
    assert status == 400 and "curve_builds[1].contract.id: 'SYN_CL' is built twice" in text
    # A build error names the entry in the swap request.
    bad = copy.deepcopy(request)
    bad["pricing"]["commodity"]["curve_builds"][0]["average_quotes"][3]["price"] += 5.0
    bad["pricing"]["commodity"]["curve_builds"][0]["average_quotes"].append(
        copy.deepcopy(request["pricing"]["commodity"]["curve_builds"][0]["average_quotes"][3]))
    status, text = _post(client, "commodity_swap", bad)
    body = json.loads(text)
    assert status == 400 and body["error"].startswith(
        "pricing.commodity.curve_builds[0].average_quotes[11]: conflicts"), text[:400]
    assert body["details"]["reason"] == "QuoteConflict"
    # Without curve builds the response has neither field.
    plain = _price_plain(client)
    assert "curve_builds" not in plain and "quote_deltas" not in plain["swaps"][0]


def _price_plain(client):
    request = json.loads((ROOT / "tests" / "contract" / "data" / "commodity"
                          / "price_commodity_swap_benchmark_request.json").read_text())
    status, text = _post(client, "commodity_swap", request)
    assert status == 200
    return json.loads(text)
