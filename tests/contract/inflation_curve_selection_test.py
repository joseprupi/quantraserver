"""Inflation trades price on the curve and the index fixings they reference.

Curve selection: a request may carry several inflation curves on one index (a
base curve and a shocked copy, say). Each trade prices on the curve named by
its own `inflation_curve`, whatever the other curves are called, so in a
two-curve request every trade equals the same trade priced in a request that
carries only its curve. Covered for the zero-coupon swap, the year-on-year
swap and the year-on-year cap/floor, with the second curve's id sorting before
and after the first's.

Index fixings: QuantLib stores inflation fixings by index name, made of a
region and a family name, and every currency without a QuantLib region shares
one region. Index specs never read each other's fixings: the same family name
in two such currencies, a zero and a year-on-year index sharing a family name,
and a year-on-year index defined as a ratio of a zero index (next to another
zero index with the same family name) each price as without the other specs.
These cases use seasoned trades, whose first observations are past fixings: a
new trade with the curve helpers' conventions reprices to the curve quotes
whatever the fixings are.
"""

import copy

import pytest

from ql_reference import load_json

ZCIIS = "inflation/zciis_eur_5y_payer_linear_obs.json"
YYIIS = "inflation/yyiis_eur_5y_payer_annual.json"
YOY_CAP = "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json"

# (product, example, key of the trade list in the request and the response)
PRODUCTS = [
    ("zero_coupon_inflation_swap", ZCIIS, "swaps"),
    ("year_on_year_inflation_swap", YYIIS, "swaps"),
    ("year_on_year_inflation_cap_floor", YOY_CAP, "cap_floors"),
]

# Trades on different curves or fixings differ by far more than MIN_GAP; the
# same trade in two requests must agree to NPV_TOL.
MIN_GAP = 100.0
NPV_TOL = 1e-6


def _npvs(client, product, request, list_key="swaps"):
    return [row["npv"] for row in client.price(product, request)[list_key]]


def _shifted_curve(curve, curve_id, shift):
    """Copy of an inflation curve under a new id, every quote moved by shift."""
    out = copy.deepcopy(curve)
    out["id"] = curve_id
    for point in out["points"]:
        point["point"]["quote_value"] += shift
    return out


def _with_inflation(example, indices, curves, list_key="swaps", trades=None):
    request = copy.deepcopy(example)
    request["pricing"]["inflation"] = {
        "inflation_indices": copy.deepcopy(indices),
        "inflation_curves": copy.deepcopy(curves),
    }
    if trades is not None:
        request[list_key] = copy.deepcopy(trades)
    return request


def _seasoned_zciis(example):
    """The example ZCIIS started 2024-09-16: it observes the June/July 2024 CPI."""
    trade = copy.deepcopy(example["swaps"][0])
    trade["zero_coupon_inflation_swap"].update(
        start_date="2024-09-16", maturity_date="2029-09-17")
    return trade


def _seasoned_yyiis(example):
    """The example YYIIS started 2024-02-15: its first coupon observes the
    November 2024 year-on-year rate."""
    trade = copy.deepcopy(example["swaps"][0])
    swap = trade["year_on_year_inflation_swap"]
    for schedule in ("fixed_schedule", "yoy_schedule"):
        swap[schedule].update(effective_date="2024-02-15", termination_date="2029-02-15")
    return trade


@pytest.mark.parametrize("shocked_id", ["A_SHOCKED", "Z_SHOCKED"])
@pytest.mark.parametrize(
    "product,example,list_key", PRODUCTS, ids=[p[0] for p in PRODUCTS])
def test_each_trade_prices_on_its_own_inflation_curve(
        client, data_dir, product, example, list_key, shocked_id):
    base = load_json(data_dir / example)
    indices = base["pricing"]["inflation"]["inflation_indices"]
    base_curve = base["pricing"]["inflation"]["inflation_curves"][0]
    shocked_curve = _shifted_curve(base_curve, shocked_id, 0.005)
    base_trade = base[list_key][0]
    shocked_trade = copy.deepcopy(base_trade)
    shocked_trade["inflation_curve"] = shocked_id

    expected = [
        _npvs(client, product, base, list_key)[0],
        _npvs(client, product,
              _with_inflation(base, indices, [shocked_curve], list_key, [shocked_trade]),
              list_key)[0],
    ]
    assert abs(expected[0] - expected[1]) > MIN_GAP, expected

    both = _with_inflation(base, indices, [base_curve, shocked_curve],
                           list_key, [base_trade, shocked_trade])
    got = _npvs(client, product, both, list_key)
    assert got == pytest.approx(expected, rel=0, abs=NPV_TOL), (
        f"{product}: two curves on one index priced {got}, each curve alone {expected}")


def test_same_family_name_in_two_currencies_keeps_each_index_fixings(client, data_dir):
    # CAD and JPY have no QuantLib region.
    base = load_json(data_dir / ZCIIS)
    template = base["pricing"]["inflation"]["inflation_indices"][0]
    base_curve = base["pricing"]["inflation"]["inflation_curves"][0]
    cad_fixings = template["fixings"]
    jpy_fixings = [{"date": f["date"], "value": round(0.85 * f["value"] + 0.25 * i, 4)}
                   for i, f in enumerate(cad_fixings)]

    specs, curves, trades = [], [], []
    for index_id, currency, fixings, curve_id, shift in (
            ("CAD_CPI", "CAD", cad_fixings, "CAD_ZC", 0.0),
            ("JPY_CPI", "JPY", jpy_fixings, "JPY_ZC", -0.005)):
        spec = copy.deepcopy(template)
        spec.update(id=index_id, family_name="CPI", currency=currency,
                    fixings=copy.deepcopy(fixings))
        specs.append(spec)
        curve = _shifted_curve(base_curve, curve_id, shift)
        curve["index_id"] = index_id
        curves.append(curve)
        trade = _seasoned_zciis(base)
        trade["zero_coupon_inflation_swap"]["inflation_index_id"] = index_id
        trade["inflation_curve"] = curve_id
        trades.append(trade)

    product = "zero_coupon_inflation_swap"
    alone = [
        _npvs(client, product, _with_inflation(base, [specs[i]], [curves[i]], trades=[trades[i]]))[0]
        for i in range(2)
    ]
    assert abs(alone[0] - alone[1]) > MIN_GAP, alone
    together = _npvs(client, product, _with_inflation(base, specs, curves, trades=trades))
    assert together == pytest.approx(alone, rel=0, abs=NPV_TOL), (
        f"CAD and JPY 'CPI' indices priced together {together}, alone {alone}")


@pytest.mark.parametrize("zero_first", [True, False], ids=["zero_first", "yoy_first"])
def test_zero_and_yoy_indices_sharing_a_family_name_keep_their_fixings(
        client, data_dir, zero_first):
    zero_example = load_json(data_dir / ZCIIS)
    yoy_example = load_json(data_dir / YYIIS)
    zero_spec = copy.deepcopy(zero_example["pricing"]["inflation"]["inflation_indices"][0])
    yoy_spec = copy.deepcopy(yoy_example["pricing"]["inflation"]["inflation_indices"][0])
    zero_spec["family_name"] = yoy_spec["family_name"] = "HICP"
    zero_curve = zero_example["pricing"]["inflation"]["inflation_curves"][0]
    yoy_curve = yoy_example["pricing"]["inflation"]["inflation_curves"][0]
    both_indices = [zero_spec, yoy_spec] if zero_first else [yoy_spec, zero_spec]
    both_curves = [zero_curve, yoy_curve]

    for product, example, spec, curve, trade in (
            ("zero_coupon_inflation_swap", zero_example, zero_spec, zero_curve,
             _seasoned_zciis(zero_example)),
            ("year_on_year_inflation_swap", yoy_example, yoy_spec, yoy_curve,
             _seasoned_yyiis(yoy_example))):
        alone = _npvs(client, product,
                      _with_inflation(example, [spec], [curve], trades=[trade]))
        shared = _npvs(client, product,
                       _with_inflation(example, both_indices, both_curves, trades=[trade]))
        assert shared == pytest.approx(alone, rel=0, abs=NPV_TOL), (
            f"{product}: next to an index with the same family name {shared}, alone {alone}")


def test_ratio_yoy_index_reads_its_zero_index_fixings(client, data_dir):
    zero_example = load_json(data_dir / ZCIIS)
    yoy_example = load_json(data_dir / YYIIS)
    zero_spec = copy.deepcopy(zero_example["pricing"]["inflation"]["inflation_indices"][0])
    # A year of earlier history, so the ratio index has year-on-year values.
    zero_spec["fixings"] = [
        {"date": "2023" + f["date"][4:], "value": round(f["value"] / 1.025, 4)}
        for f in zero_spec["fixings"]] + zero_spec["fixings"]
    ratio_spec = copy.deepcopy(yoy_example["pricing"]["inflation"]["inflation_indices"][0])
    ratio_spec.pop("fixings")
    ratio_spec["underlying_zero_index_id"] = zero_spec["id"]
    # Another zero index with the same family name and currency, other fixings.
    other = copy.deepcopy(zero_spec)
    other["id"] = "EUHICP_OTHER"
    other["fixings"] = [{"date": f["date"], "value": round(1.1 * f["value"] + 0.4 * i, 4)}
                        for i, f in enumerate(zero_spec["fixings"])]
    yoy_curve = yoy_example["pricing"]["inflation"]["inflation_curves"][0]
    trade = _seasoned_yyiis(yoy_example)

    product = "year_on_year_inflation_swap"
    alone = _npvs(client, product, _with_inflation(
        yoy_example, [zero_spec, ratio_spec], [yoy_curve], trades=[trade]))
    with_other = _npvs(client, product, _with_inflation(
        yoy_example, [zero_spec, ratio_spec, other], [yoy_curve], trades=[trade]))
    assert with_other == pytest.approx(alone, rel=0, abs=NPV_TOL), (
        f"ratio YoY index next to another zero index {with_other}, alone {alone}")
