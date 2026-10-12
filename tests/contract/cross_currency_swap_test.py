"""Cross-currency swap: parity, pricing identities, flows and request contract.

`POST /price-cross-currency-swap` prices two coupon legs in two currencies
(fixed, IBOR or compounded overnight), each discounted on its own curve with
its own principal exchanges, converted to a reporting currency at the as-of
exchange rate X = S * DF_quote(t_s) / DF_base(t_s). QuantLib has no
cross-currency swap instrument: the price is CashFlows.npv / bps on the legs:

    L_i   = coupons_i + [initial] (-N_i at the schedule start)
                      + [final]   (+N_i with the last coupon)
    leg_i_npv = s_i * npv(L_i, D_i)            s_1 = -1 Payer / +1 Receiver
    npv   = leg1_npv_reporting + leg2_npv_reporting
    fair_rate   = r_f - NPV[fixed ccy] / (s_f * A_f)   (one fixed leg)
    fair_spread = q_2 - NPV[leg2 ccy]  / (s_2 * A_2)   (both floating)

The fixtures (examples/data/cross_currency_swap/) are 5Y EUR/USD swaps on
the USD-collateralised EUR curve + EUR ESTR OIS + USD SOFR OIS market of the
FX helper tests (spot 1.0850, T+2 on TARGET + US settlement): EUR fixed vs
USD SOFR reported in USD and in EUR, the same struck at its fair rate, USD
SOFR vs EUR ESTR - 15 bp with both exchanges and with the final exchange
only, and USD fixed vs EUR Euribor 3M received in EUR. Every example is also
a functional-catalog case.

  * parity: every scalar response field equals the Python QuantLib reference
    (ql_reference.cross_currency_swap_ql) on the same curves — 1e-9 of the
    notional on money amounts, 1e-12 relative on fair rate / spread and X;
  * swap_type flip = exact sign flip of every NPV, same fair quantities;
  * reporting identity: npv_EUR * X == npv_USD, legs unchanged;
  * reprice at fair_rate / fair_spread through the API => |npv| < 1e-9 * N;
  * the fair spread of the ESTR vs SOFR swap reproduces the 5Y basis pillar
    the collateral curve was bootstrapped from;
  * exchange flags change the NPV by exactly the PV of the notional flows;
  * flows: coupons and exchanges listed in leg order with `kind`, their PVs
    sum to the leg NPV, exchanges carry no accrual fields;
  * fair_rate / fair_spread present exactly when the swap shape defines them;
  * every validation is a 400 naming the field; unknown references are 404.
"""

import copy
import json

import pytest

from ql_reference import cross_currency_swap_ql, fx_as_of_exchange_rate_ql, load_json

FIXED_USD = "cross_currency_swap/xccy_eur_fixed_vs_usd_sofr_5y_payer_usd.json"
FIXED_EUR = "cross_currency_swap/xccy_eur_fixed_vs_usd_sofr_5y_payer_eur.json"
FIXED_AT_FAIR = "cross_currency_swap/xccy_eur_fixed_at_fair_rate_vs_usd_sofr_5y.json"
BASIS_BOTH = "cross_currency_swap/xccy_usd_sofr_vs_eur_estr_minus15bp_5y_both_exchanges.json"
BASIS_FINAL_ONLY = "cross_currency_swap/xccy_usd_sofr_vs_eur_estr_minus15bp_5y_final_exchange_only.json"
USD_FIXED_EURIBOR = "cross_currency_swap/xccy_usd_fixed_vs_eur_euribor3m_5y_receiver_eur.json"
ALL_FIXTURES = [FIXED_USD, FIXED_EUR, FIXED_AT_FAIR, BASIS_BOTH, BASIS_FINAL_ONLY,
                USD_FIXED_EURIBOR]

RATE_TOLERANCE = 1e-11       # absolute, on fair rate / spread / X: the JSON
                             # printer carries 12 decimals (5e-13 rounding)
IDENTITY_TOLERANCE = 1e-9    # relative, on client-side products of two
                             # 12-decimal numbers (npv * X)
SCALAR_FIELDS = {"npv", "leg1_npv", "leg2_npv", "leg1_npv_reporting", "leg2_npv_reporting",
                 "fx_spot_used", "fx_as_of_rate", "spot_date"}
OPTIONAL_FIELDS = {"fair_rate", "fair_spread"}
FLOW_FIELDS = {"leg1_flows", "leg2_flows"}
N_EUR = 10_000_000.0
N_USD = 10_850_000.0
BASIS_5Y = -0.0017   # the 5Y CrossCcyBasisHelper spread of the collateral curve


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _post(client, request):
    r = client.session.post(f"{client.base_url}/{client.ENDPOINTS['cross_currency_swap']}",
                            json=request)
    return r.status_code, r.text


def _row(response, i=0):
    rows = response["swaps"]
    assert SCALAR_FIELDS <= set(rows[i]), (
        f"response row {i} is missing fields: {SCALAR_FIELDS - set(rows[i])}")
    return rows[i]


def _price(client, request):
    return _row(client.price("cross_currency_swap", request))


def _mutated(request, mutate):
    out = copy.deepcopy(request)
    mutate(out)
    return out


def _entry(request):
    return request["swaps"][0]


def _swap(request):
    return request["swaps"][0]["cross_currency_swap"]


def _leg(request, i):
    return _swap(request)[f"leg{i}"]


def _coupon_leg(leg):
    return leg["fixed_leg"] if "fixed_leg" in leg else leg["floating_leg"]


def _money_tolerance(request):
    """1e-9 of the larger notional: both curve builds are QuantLib bootstraps
    of the same helper set (agreeing to ~1e-14 on the discount factors), and
    the notional exchanges turn that into a few 1e-7 currency units on 1e7
    notionals; 1e-9 of the notional leaves a safe margin."""
    return 1e-9 * max(_leg(request, 1)[list(_leg(request, 1))[1]]["notional"],
                      _leg(request, 2)[list(_leg(request, 2))[1]]["notional"])


def _assert_400_naming(client, request, field):
    status, text = _post(client, request)
    assert status == 400, f"expected 400 naming {field}, got {status}: {text[:300]}"
    assert field in text, f"400 does not name {field}: {text[:300]}"


def _assert_404(client, request, token):
    status, text = _post(client, request)
    assert status == 404, f"expected 404 for {token}, got {status}: {text[:300]}"
    assert token in text, f"404 does not name {token}: {text[:300]}"


# ---------------------------------------------------------------------------
# parity
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("fixture", ALL_FIXTURES)
def test_xccy_parity(client, data_dir, fixture):
    request = load_json(data_dir / fixture)
    api = _price(client, request)
    ref = cross_currency_swap_ql(request)
    tol = _money_tolerance(request)
    for name in ("npv", "leg1_npv", "leg2_npv", "leg1_npv_reporting", "leg2_npv_reporting"):
        assert abs(api[name] - ref[name]) < tol, (
            f"{fixture}: {name} API {api[name]!r} vs QuantLib {ref[name]!r}")
    for name in ("fair_rate", "fair_spread"):
        assert (name in api) == (name in ref), f"{fixture}: {name} presence differs"
        if name in ref:
            assert abs(api[name] - ref[name]) < RATE_TOLERANCE, (
                f"{fixture}: {name} API {api[name]!r} vs QuantLib {ref[name]!r}")
    assert abs(api["fx_as_of_rate"] - ref["fx_as_of_rate"]) < RATE_TOLERANCE
    assert api["fx_spot_used"] == ref["fx_spot_used"]
    assert api["spot_date"] == ref["spot_date"]


def test_xccy_baseline_shape_and_signs(client, data_dir):
    """EUR fixed 2.55% paid vs USD SOFR received, both exchanges, 5Y from the
    spot date 2025-01-17, reported in USD. The SOFR leg with its exchanges is
    a par floater on its own curve: its NPV is a rounding of zero (the
    initial exchange sits two days out). The fixed leg is paid above its fair
    rate, so the payer's NPV is negative; the as-of rate sits within a few
    pips of the spot; only fair_rate is reported."""
    request = load_json(data_dir / FIXED_USD)
    row = _price(client, request)
    assert row["spot_date"] == "2025-01-17"
    assert row["fx_spot_used"] == 1.0850
    assert abs(row["fx_as_of_rate"] - 1.0850) < 0.001
    assert abs(row["leg2_npv"]) < 1e-3 * N_USD                   # par floater + exchanges
    assert row["leg2_npv_reporting"] == row["leg2_npv"]          # USD leg, reported in USD
    assert row["leg1_npv"] < 0 and row["leg1_npv_reporting"] < 0  # pays 2.55% above fair
    assert abs(row["npv"] - (row["leg1_npv_reporting"] + row["leg2_npv_reporting"])) < 1e-6
    assert row["npv"] < 0
    assert "fair_rate" in row and "fair_spread" not in row
    assert 0.0 < row["fair_rate"] < 0.0255


# ---------------------------------------------------------------------------
# pricing identities
# ---------------------------------------------------------------------------

def test_xccy_swap_type_flip_is_the_exact_negative(client, data_dir):
    request = load_json(data_dir / FIXED_USD)
    payer = _price(client, request)
    receiver = _price(client, _mutated(request, lambda r: _swap(r).update(swap_type="Receiver")))
    for name in ("npv", "leg1_npv", "leg2_npv", "leg1_npv_reporting", "leg2_npv_reporting"):
        assert payer[name] == -receiver[name], name
    # Market quantities do not depend on the side.
    assert payer["fair_rate"] == receiver["fair_rate"]
    assert payer["fx_as_of_rate"] == receiver["fx_as_of_rate"]


def test_xccy_reporting_currency_identity(client, data_dir):
    """npv reported in EUR times X equals the npv reported in USD; the leg
    NPVs in their own currencies are unchanged; the reporting columns convert
    the leg that is not in the reporting currency."""
    in_usd_req = load_json(data_dir / FIXED_USD)
    in_eur_req = load_json(data_dir / FIXED_EUR)
    assert _entry(in_eur_req)["reporting_currency"] == "EUR"
    in_usd = _price(client, in_usd_req)
    in_eur = _price(client, in_eur_req)
    X = in_usd["fx_as_of_rate"]
    assert in_eur["fx_as_of_rate"] == X
    assert abs(in_eur["npv"] * X - in_usd["npv"]) < IDENTITY_TOLERANCE * abs(in_usd["npv"])
    assert in_eur["leg1_npv"] == in_usd["leg1_npv"]
    assert in_eur["leg2_npv"] == in_usd["leg2_npv"]
    assert in_eur["leg1_npv_reporting"] == in_eur["leg1_npv"]          # EUR leg in EUR
    assert abs(in_usd["leg1_npv_reporting"] - in_usd["leg1_npv"] * X) < 1e-4   # EUR leg in USD
    assert abs(in_eur["leg2_npv_reporting"] - in_eur["leg2_npv"] / X) < 1e-4   # USD leg in EUR
    assert in_eur["fair_rate"] == in_usd["fair_rate"]


def test_xccy_as_of_rate_is_the_fx_forward_identity(client, data_dir):
    """X = S * DF_USD(spot) / DF_EUR(spot) on the two legs' discounting
    curves (the same algebra as the FX forward and the FxSwapRateHelper)."""
    request = load_json(data_dir / FIXED_USD)
    ref = cross_currency_swap_ql(request)
    row = _price(client, request)
    assert abs(row["fx_as_of_rate"] - ref["fx_as_of_rate"]) < RATE_TOLERANCE


@pytest.mark.parametrize("fixture", [FIXED_USD, FIXED_EUR, USD_FIXED_EURIBOR])
def test_xccy_repriced_at_fair_rate_has_zero_npv(client, data_dir, fixture):
    """Setting the fixed rate to the reported fair_rate and repricing through
    the API gives an NPV of zero to rounding, in either reporting currency,
    for either side and either base/quote role of the fixed leg."""
    request = load_json(data_dir / fixture)
    fair = _price(client, request)["fair_rate"]
    i = 1 if "fixed_leg" in _leg(request, 1) else 2
    at_fair = _mutated(request, lambda r: _leg(r, i)["fixed_leg"].update(rate=fair))
    row = _price(client, at_fair)
    assert abs(row["npv"]) < 1e-9 * N_EUR, f"{fixture}: npv at the fair rate is {row['npv']}"
    assert abs(row["fair_rate"] - fair) < RATE_TOLERANCE


def test_xccy_fixed_at_fair_rate_example(client, data_dir):
    """The committed at-fair example (the baseline struck at its fair rate)
    has a zero NPV and reports the same fair rate."""
    request = load_json(data_dir / FIXED_AT_FAIR)
    row = _price(client, request)
    assert abs(row["npv"]) < 1e-9 * N_EUR, row["npv"]
    assert abs(row["fair_rate"] - _leg(request, 1)["fixed_leg"]["rate"]) < RATE_TOLERANCE


@pytest.mark.parametrize("fixture", [BASIS_BOTH, BASIS_FINAL_ONLY])
def test_xccy_repriced_at_fair_spread_has_zero_npv(client, data_dir, fixture):
    """The reported fair_spread (on leg2) rebuilt through the API — not by
    linear extrapolation — zeroes the NPV."""
    request = load_json(data_dir / fixture)
    fair = _price(client, request)["fair_spread"]
    at_fair = _mutated(request, lambda r: _leg(r, 2)["floating_leg"].update(spread=fair))
    row = _price(client, at_fair)
    assert abs(row["npv"]) < 1e-9 * N_USD, f"{fixture}: npv at the fair spread is {row['npv']}"
    assert abs(row["fair_spread"] - fair) < RATE_TOLERANCE


def test_xccy_fair_spread_reprices_the_basis_pillar(client, data_dir):
    """USD SOFR vs EUR ESTR + spread, quarterly, both exchanges, 5Y from the
    spot date, with the EUR leg discounted on the collateral curve: that is
    the 5Y CrossCcyBasisHelper of the curve (ESTR - 17 bp vs SOFR), so the
    fair spread on the EUR leg must come back as the pillar's -17 bp. The
    helper is QuantLib's constant-notional one, which nets the two unit
    notionals without the two-day as-of conversion (X vs S); on this market
    that convention is worth ~1e-8 on the spread, the only difference."""
    request = load_json(data_dir / BASIS_BOTH)
    assert _leg(request, 2)["floating_leg"]["spread"] == -0.0015
    row = _price(client, request)
    assert abs(row["fair_spread"] - BASIS_5Y) < 1e-7, row["fair_spread"]
    # Struck at -15 bp the receiver of the EUR leg gets 2 bp more than fair:
    # the NPV is positive, 2 bp x the EUR annuity (~4.6 years) in USD.
    assert 0.0005 * N_USD < row["npv"] < 0.0015 * N_USD
    at_pillar = _price(client, _mutated(request, lambda r: _leg(r, 2)["floating_leg"].update(spread=BASIS_5Y)))
    assert abs(at_pillar["npv"]) < 1e-6 * N_USD, at_pillar["npv"]


def test_xccy_exchange_flags_move_npv_by_the_pv_of_the_notional_flows(client, data_dir):
    """Dropping the initial exchange on both legs changes each leg's NPV by
    exactly the PV of its initial notional flow: s_i * N_i * DF_i(start)
    (the receiver of the coupons pays the notional, so removing the payment
    raises the own-frame PV by N_i * DF_i(start))."""
    both = load_json(data_dir / BASIS_BOTH)
    final_only = load_json(data_dir / BASIS_FINAL_ONLY)
    for i in (1, 2):
        assert _leg(final_only, i)["initial_exchange"] is False
    with_flows = _mutated(both, lambda r: r.update(include_flows=True))
    rows_both = client.price("cross_currency_swap", with_flows)["swaps"][0]
    row_final = _price(client, final_only)
    for i, sign in ((1, -1.0), (2, +1.0)):   # Payer: pays leg1, receives leg2
        initial = [f for f in rows_both[f"leg{i}_flows"]
                   if f["kind"] == "NotionalExchange" and f["amount"] < 0]
        assert len(initial) == 1
        pv_initial = initial[0]["present_value"]           # -N * DF(start) in the own frame
        expected = rows_both[f"leg{i}_npv"] - sign * pv_initial
        assert abs(row_final[f"leg{i}_npv"] - expected) < 1e-6 * N_USD, (
            f"leg{i}: {row_final[f'leg{i}_npv']} vs {expected}")
    # No exchange at all leaves the pure coupon legs; the swap then has no
    # principal at risk and the fair spread moves away from the pillar.
    none = _mutated(both, lambda r: [_leg(r, i).update(initial_exchange=False, final_exchange=False)
                                     for i in (1, 2)])
    row_none = _price(client, none)
    assert abs(row_none["fair_spread"] - BASIS_5Y) > 1e-4


def test_xccy_leg_roles_either_leg_may_be_the_base_currency(client, data_dir):
    """USD fixed (leg1, the spot's quote currency) vs EUR Euribor 3M (leg2,
    the base), received, reported in EUR: the USD leg is converted with 1/X."""
    request = load_json(data_dir / USD_FIXED_EURIBOR)
    row = _price(client, request)
    X = row["fx_as_of_rate"]
    assert abs(row["leg1_npv_reporting"] - row["leg1_npv"] / X) < 1e-4
    assert row["leg2_npv_reporting"] == row["leg2_npv"]
    assert row["leg1_npv"] > 0 > row["leg2_npv"]       # receives USD fixed, pays EUR float
    assert "fair_rate" in row and "fair_spread" not in row


def test_xccy_fixed_fixed_has_no_fair_quantity(client, data_dir):
    request = load_json(data_dir / FIXED_USD)
    usd_fixed = copy.deepcopy(_leg(load_json(data_dir / USD_FIXED_EURIBOR), 1))
    fixed_fixed = _mutated(request, lambda r: _swap(r).update(leg2=usd_fixed))
    row = _price(client, fixed_fixed)
    assert "fair_rate" not in row and "fair_spread" not in row
    assert set(row) == SCALAR_FIELDS


def test_xccy_fair_fields_presence_by_shape(client, data_dir):
    """fair_rate iff exactly one fixed leg; fair_spread iff both float; the
    flows only with include_flows."""
    for fixture, present in ((FIXED_USD, "fair_rate"), (BASIS_BOTH, "fair_spread"),
                             (USD_FIXED_EURIBOR, "fair_rate")):
        row = _price(client, load_json(data_dir / fixture))
        assert set(row) == SCALAR_FIELDS | {present}, f"{fixture}: {sorted(row)}"


def test_xccy_multi_trade_in_request_order(client, data_dir):
    a = load_json(data_dir / FIXED_USD)
    b = load_json(data_dir / BASIS_BOTH)
    both = copy.deepcopy(a)
    both["swaps"] = [_entry(a), _entry(b), _entry(a)]
    rows = client.price("cross_currency_swap", both)["swaps"]
    assert len(rows) == 3
    assert rows[0] == rows[2] == _price(client, a)
    assert rows[1] == _price(client, b)


def test_xccy_moving_the_spot_moves_the_npv(client, data_dir):
    """A spot move re-values the notional exchanges (and the collateral curve
    behind the EUR leg): the EUR-fixed payer's NPV in USD changes."""
    request = load_json(data_dir / FIXED_USD)
    base = _price(client, request)
    moved = _price(client, _mutated(request, lambda r: r["pricing"]["fx"]["spots"][0].update(rate=1.0950)))
    assert moved["fx_spot_used"] == 1.0950
    assert moved["npv"] != base["npv"]
    assert moved["fx_as_of_rate"] > base["fx_as_of_rate"]


# ---------------------------------------------------------------------------
# flows
# ---------------------------------------------------------------------------

def test_xccy_flows_list_coupons_and_exchanges(client, data_dir):
    """With include_flows each leg lists its future flows in leg order:
    coupons first (accrual dates, rate, fixing for floating ones), then the
    initial (-N, schedule start) and final (+N, last payment date) exchanges,
    all with `kind`; their PVs sum to the leg's own-frame PV (leg_npv / s_i)
    and match the reference flow by flow."""
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: r.update(include_flows=True))
    row = client.price("cross_currency_swap", request)["swaps"][0]
    ref = cross_currency_swap_ql(request)
    for i, sign, notional, n_coupons in ((1, -1.0, N_EUR, 5), (2, +1.0, N_USD, 20)):
        flows = row[f"leg{i}_flows"]
        coupons = [f for f in flows if f["kind"] == "Coupon"]
        exchanges = [f for f in flows if f["kind"] == "NotionalExchange"]
        assert len(coupons) == n_coupons and len(exchanges) == 2
        assert flows[:n_coupons] == coupons and flows[n_coupons:] == exchanges
        assert exchanges[0]["amount"] == -notional and exchanges[1]["amount"] == notional
        assert exchanges[0]["payment_date"] == "2025-01-17"
        assert exchanges[1]["payment_date"] == coupons[-1]["payment_date"]
        for f in exchanges:
            assert not ({"accrual_start_date", "accrual_end_date", "accrual_year_fraction",
                         "rate", "fixing_date", "index_fixing", "spread", "gearing"} & set(f)), f
            assert abs(f["present_value"] - f["amount"] * f["discount"]) < 1e-3
        for f in coupons:
            assert {"accrual_start_date", "accrual_end_date", "accrual_year_fraction", "rate",
                    "gearing"} <= set(f)
            assert f["amount"] > 0
        floating = i == 2
        for f in coupons:
            assert ("fixing_date" in f) == floating and ("spread" in f) == floating
        assert abs(sum(f["present_value"] for f in flows) - row[f"leg{i}_npv"] / sign) < 1e-6
        assert len(flows) == len(ref[f"leg{i}_flows"])
        for api, (date, amount, discount) in zip(flows, ref[f"leg{i}_flows"]):
            assert api["payment_date"] == date
            assert abs(api["amount"] - amount) < 1e-6
            assert abs(api["discount"] - discount) < RATE_TOLERANCE
    # Without the flag the lists are absent, and the scalars are the same.
    plain = _price(client, load_json(data_dir / FIXED_USD))
    assert not (FLOW_FIELDS & set(plain))
    assert {k: v for k, v in row.items() if k not in FLOW_FIELDS} == plain


def test_xccy_overnight_flows_are_compounded_coupons(client, data_dir):
    """The SOFR leg's coupons are compounded overnight coupons: one per
    quarterly period, `rate` the compounded rate over the period (plus the
    spread) and the amount N * rate * accrual. As on the OIS endpoint,
    `index_fixing` of an overnight coupon is the index fixing on the
    coupon's first fixing date, not the compounded rate: on this smooth
    curve the two sit within a few basis points of each other."""
    request = _mutated(load_json(data_dir / BASIS_BOTH), lambda r: r.update(include_flows=True))
    row = client.price("cross_currency_swap", request)["swaps"][0]
    ref = cross_currency_swap_ql(request)
    coupons = [f for f in row["leg1_flows"] if f["kind"] == "Coupon"]
    assert len(coupons) == 20
    for f, (_, amount, _) in zip(coupons, ref["leg1_flows"]):
        assert 0.02 < f["rate"] < 0.07
        assert abs(f["amount"] - N_USD * f["rate"] * f["accrual_year_fraction"]) < 1e-3
        assert abs(f["amount"] - amount) < 1e-6          # the compounded coupon, QuantLib's
        assert abs(f["index_fixing"] - f["rate"]) < 0.002   # first-day fixing vs compounded
    eur = [f for f in row["leg2_flows"] if f["kind"] == "Coupon"]
    for f, (_, amount, _) in zip(eur, ref["leg2_flows"]):
        assert f["spread"] == -0.0015
        assert abs(f["amount"] - amount) < 1e-6
        assert abs(f["rate"] - f["spread"] - f["index_fixing"]) < 0.002


# ---------------------------------------------------------------------------
# request contract: 400 naming the field, 404 for unknown references
# ---------------------------------------------------------------------------

def test_xccy_empty_list_400(client, data_dir):
    request = load_json(data_dir / FIXED_USD)
    request["swaps"] = []
    _assert_400_naming(client, request, "PriceCrossCurrencySwapRequest.swaps")


def test_xccy_swap_type_absent_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _swap(r).pop("swap_type"))
    _assert_400_naming(client, request, "CrossCurrencySwap.swap_type")


@pytest.mark.parametrize("field", ["initial_exchange", "final_exchange"])
def test_xccy_exchange_flags_presence_required_400(client, data_dir, field):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2).pop(field))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg2.{field}")


def test_xccy_both_fixed_and_floating_400(client, data_dir):
    usd_fixed = _leg(load_json(data_dir / USD_FIXED_EURIBOR), 1)["fixed_leg"]
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2).update(fixed_leg=usd_fixed))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg2 must contain exactly one of fixed_leg or floating_leg")


def test_xccy_neither_fixed_nor_floating_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 1).pop("fixed_leg"))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg1 must contain exactly one of fixed_leg or floating_leg")


def test_xccy_forwarding_curve_required_with_floating_leg_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2).pop("forwarding_curve"))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg2.forwarding_curve is required")


def test_xccy_forwarding_curve_rejected_with_fixed_leg_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 1).update(forwarding_curve="EUR_ESTR_OIS"))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg1.forwarding_curve must be absent")


@pytest.mark.parametrize("value", [0.0, -1_000_000.0])
def test_xccy_nonpositive_notional_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 1)["fixed_leg"].update(notional=value))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg1.fixed_leg.notional")
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2)["floating_leg"].update(notional=value))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg2.floating_leg.notional")


def test_xccy_notional_absent_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 1)["fixed_leg"].pop("notional"))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg1.fixed_leg.notional is required")


def test_xccy_per_period_notionals_rejected_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 1)["fixed_leg"].update(notionals=[N_EUR] * 5))
    status, text = _post(client, request)
    assert status == 400 and "notionals" in text, text[:300]


@pytest.mark.parametrize("leg,sub,field", [(1, "fixed_leg", "day_counter"),
                                           (1, "fixed_leg", "payment_convention"),
                                           (2, "floating_leg", "day_counter"),
                                           (2, "floating_leg", "payment_convention")])
def test_xccy_leg_conventions_presence_required_400(client, data_dir, leg, sub, field):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, leg)[sub].pop(field))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg{leg}.{sub}.{field}")


def test_xccy_negative_fixing_days_400(client, data_dir):
    request = _mutated(load_json(data_dir / USD_FIXED_EURIBOR),
                       lambda r: _leg(r, 2)["floating_leg"].update(fixing_days=-1))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg2.floating_leg.fixing_days")


def test_xccy_in_arrears_on_overnight_index_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 2)["floating_leg"].update(in_arrears=True))
    _assert_400_naming(client, request, "XccyLeg.floating_leg.in_arrears of leg2")


def test_xccy_in_arrears_on_ibor_index_is_priced(client, data_dir):
    request = _mutated(load_json(data_dir / USD_FIXED_EURIBOR),
                       lambda r: _leg(r, 2)["floating_leg"].update(in_arrears=True))
    row = _price(client, request)
    assert row["npv"] != _price(client, load_json(data_dir / USD_FIXED_EURIBOR))["npv"]


@pytest.mark.parametrize("field,value", [("currency", "EURUSD"), ("currency", "eur"),
                                         ("currency", "US")])
def test_xccy_currency_not_iso_400(client, data_dir, field, value):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 1).update({field: value}))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg1.{field}")


def test_xccy_same_currency_both_legs_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2).update(currency="EUR"))
    status, text = _post(client, request)
    assert status == 400, text[:300]
    assert "CrossCurrencySwap.leg1.currency and CrossCurrencySwap.leg2.currency must differ" in text
    assert "price-basis-swap" in text


EURGBP_SPOT = {"id": "EURGBP_SPOT", "base_currency": "EUR", "quote_currency": "GBP",
               "rate": 0.8350, "spot_days": 2,
               "settlement_calendars": ["TARGET", "UnitedKingdom"],
               "business_day_convention": "ModifiedFollowing"}


def test_xccy_leg_currency_not_the_index_currency_400(client, data_dir):
    """A GBP leg on the SOFR index (with an EUR/GBP spot so the pair check
    passes first): the leg currency is explicit and must be the index's own
    (`IndexDef.currency`). The collateral curve keeps its EUR/USD spot."""
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: (
        _leg(r, 2).update(currency="GBP"),
        r["pricing"]["fx"]["spots"].append(dict(EURGBP_SPOT)),
        _entry(r).update(fx_spot="EURGBP_SPOT", reporting_currency="GBP")))
    status, text = _post(client, request)
    assert status == 400, text[:300]
    assert "XccyLeg.currency 'GBP' of leg2" in text and "USD_SOFR" in text


def test_xccy_spot_pair_not_the_legs_pair_400(client, data_dir):
    """EUR and USD legs priced off an EUR/GBP spot: a 400 naming the spot."""
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: (
        r["pricing"]["fx"]["spots"].append(dict(EURGBP_SPOT)),
        _entry(r).update(fx_spot="EURGBP_SPOT")))
    _assert_400_naming(client, request, "PriceCrossCurrencySwap.fx_spot")


def test_xccy_spot_quoted_the_other_way_round_is_accepted(client, data_dir):
    """USD/EUR spot (1/1.0850) with the same two legs: either leg may be the
    base-currency leg; the as-of rate is then USD per EUR... i.e. quoted in
    the spot's own order, and the NPV is unchanged."""
    request = load_json(data_dir / FIXED_USD)
    base = _price(client, request)
    flipped = _mutated(request, lambda r: r["pricing"]["fx"]["spots"][0].update(
        base_currency="USD", quote_currency="EUR", rate=1.0 / 1.0850))
    # The collateral curve's FX helpers reference the spot too; they need the
    # pair in their own order, so give the flipped spot a new id and point
    # only the swap at it.
    flipped["pricing"]["fx"]["spots"][0]["id"] = "USDEUR_SPOT"
    flipped["pricing"]["fx"]["spots"].append(request["pricing"]["fx"]["spots"][0])
    _entry(flipped)["fx_spot"] = "USDEUR_SPOT"
    row = _price(client, flipped)
    assert abs(row["fx_as_of_rate"] * base["fx_as_of_rate"] - 1.0) < RATE_TOLERANCE
    assert abs(row["npv"] - base["npv"]) < 1e-4
    assert row["leg1_npv"] == base["leg1_npv"] and row["leg2_npv"] == base["leg2_npv"]


@pytest.mark.parametrize("value", ["GBP", "eur", "EURUSD"])
def test_xccy_reporting_currency_outside_the_legs_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _entry(r).update(reporting_currency=value))
    _assert_400_naming(client, request, "PriceCrossCurrencySwap.reporting_currency")


@pytest.mark.parametrize("field", ["fx_spot", "reporting_currency", "cross_currency_swap"])
def test_xccy_required_wrapper_fields_400(client, data_dir, field):
    """`(required)` fields: the FlatBuffers JSON parser rejects the request."""
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _entry(r).pop(field))
    status, text = _post(client, request)
    assert status == 400, f"{field}: expected 400, got {status}: {text[:300]}"


@pytest.mark.parametrize("field", ["currency", "discounting_curve"])
def test_xccy_required_leg_strings_400(client, data_dir, field):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 1).pop(field))
    status, text = _post(client, request)
    assert status == 400, f"{field}: expected 400, got {status}: {text[:300]}"


def test_xccy_leg_missing_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _swap(r).pop("leg2"))
    status, text = _post(client, request)
    assert status == 400, text[:300]


def test_xccy_unknown_spot_404(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _entry(r).update(fx_spot="NOPE"))
    _assert_404(client, request, "FX spot not found: NOPE")


@pytest.mark.parametrize("leg,field,token", [(1, "discounting_curve", "leg1 discounting curve not found"),
                                             (2, "discounting_curve", "leg2 discounting curve not found"),
                                             (2, "forwarding_curve", "leg2 forwarding curve not found")])
def test_xccy_unknown_curve_404(client, data_dir, leg, field, token):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, leg).update({field: "NOPE"}))
    _assert_404(client, request, token)


def test_xccy_unknown_index_404(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 2)["floating_leg"]["index"].update(id="NOPE"))
    _assert_404(client, request, "Index not found: NOPE")


def test_xccy_response_is_json_serializable_twice(client, data_dir):
    """Same request, same bytes: the response carries no per-call state."""
    request = load_json(data_dir / BASIS_BOTH)
    a = client.price("cross_currency_swap", request)
    b = client.price("cross_currency_swap", request)
    assert json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)
