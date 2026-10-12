"""Cross-currency swap: parity, pricing identities, flows and request contract.

`POST /price-cross-currency-swap` prices two `SwapLeg`s in two currencies
(FixedCoupons, IborCoupons or OvernightCoupons), each paid or received,
discounted on its own curve with its own principal exchanges, converted to a
reporting currency at the as-of exchange rate X = S * DF_quote(t_s) /
DF_base(t_s). QuantLib has no cross-currency swap instrument: the price is
CashFlows.npv / bps on the legs:

    L_i   = coupons_i + [initial] (-N_i at the schedule start)
                      + [final]   (+N_i with the last coupon)
    legs[i].npv = s_i * npv(L_i, D_i)          s_i = -1 Pay / +1 Receive
    npv   = legs[0].npv_reporting + legs[1].npv_reporting
    fair_rate   = r_f - NPV[fixed ccy]   / (s_f * A_f)   (one fixed leg)
    fair_spread = q_1 - NPV[legs[1] ccy] / (s_1 * A_1)   (no fixed leg)

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
  * flipping both pay_receive sides = exact sign flip of every NPV, same fair
    quantities;
  * reporting identity: npv_EUR * X == npv_USD, legs unchanged;
  * reprice at fair_rate / fair_spread through the API => |npv| < 1e-9 * N;
  * the fair spread of the ESTR vs SOFR swap reproduces the 5Y basis pillar
    the collateral curve was bootstrapped from;
  * exchange flags change the NPV by exactly the PV of the notional flows;
  * flows: coupons and exchanges listed in leg order with `kind`, their PVs
    sum to the leg NPV, exchanges carry no accrual fields;
  * fair_rate / fair_spread present exactly when the swap shape defines them;
  * the coupon table must match the index kind (IborCoupons on an overnight
    index names OvernightCoupons and vice versa); CmsCoupons is a 400;
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
SCALAR_FIELDS = {"npv", "legs", "fx_spot_used", "fx_as_of_rate", "spot_date"}
LEG_FIELDS = {"npv", "npv_reporting"}
OPTIONAL_FIELDS = {"fair_rate", "fair_spread"}
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
    assert len(rows[i]["legs"]) == 2
    for leg in rows[i]["legs"]:
        assert LEG_FIELDS <= set(leg), f"leg result is missing fields: {LEG_FIELDS - set(leg)}"
    return rows[i]


def _leg_npv(row, i):
    return row["legs"][i]["npv"]


def _leg_rep(row, i):
    return row["legs"][i]["npv_reporting"]


def _scalars(row):
    """The row without its flows (the leg lists compared as scalars)."""
    return {**{k: v for k, v in row.items() if k != "legs"},
            "legs": [{k: v for k, v in leg.items() if k != "flows"} for leg in row["legs"]]}


def _flip_sides(request):
    for i in (1, 2):
        leg = _leg(request, i)
        leg["pay_receive"] = "Receive" if leg["pay_receive"] == "Pay" else "Pay"


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


def _coupons(leg):
    return leg["coupons"]


def _is_fixed(leg):
    return leg["coupons_type"] == "FixedCoupons"


def _money_tolerance(request):
    """1e-9 of the larger notional: both curve builds are QuantLib bootstraps
    of the same helper set (agreeing to ~1e-14 on the discount factors), and
    the notional exchanges turn that into a few 1e-7 currency units on 1e7
    notionals; 1e-9 of the notional leaves a safe margin."""
    return 1e-9 * max(_coupons(_leg(request, 1))["notional"],
                      _coupons(_leg(request, 2))["notional"])


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
    assert abs(api["npv"] - ref["npv"]) < tol, (
        f"{fixture}: npv API {api['npv']!r} vs QuantLib {ref['npv']!r}")
    for i in (0, 1):
        for name in ("npv", "npv_reporting"):
            assert abs(api["legs"][i][name] - ref["legs"][i][name]) < tol, (
                f"{fixture}: legs[{i}].{name} API {api['legs'][i][name]!r} "
                f"vs QuantLib {ref['legs'][i][name]!r}")
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
    assert abs(_leg_npv(row, 1)) < 1e-3 * N_USD                    # par floater + exchanges
    assert _leg_rep(row, 1) == _leg_npv(row, 1)                    # USD leg, reported in USD
    assert _leg_npv(row, 0) < 0 and _leg_rep(row, 0) < 0           # pays 2.55% above fair
    assert abs(row["npv"] - (_leg_rep(row, 0) + _leg_rep(row, 1))) < 1e-6
    assert row["npv"] < 0
    assert "fair_rate" in row and "fair_spread" not in row
    assert 0.0 < row["fair_rate"] < 0.0255


# ---------------------------------------------------------------------------
# pricing identities
# ---------------------------------------------------------------------------

def test_xccy_pay_receive_flip_is_the_exact_negative(client, data_dir):
    """Swapping Pay/Receive on both legs is the other side of the trade: every
    NPV flips sign exactly."""
    request = load_json(data_dir / FIXED_USD)
    payer = _price(client, request)
    receiver = _price(client, _mutated(request, _flip_sides))
    assert payer["npv"] == -receiver["npv"]
    for i in (0, 1):
        assert _leg_npv(payer, i) == -_leg_npv(receiver, i), i
        assert _leg_rep(payer, i) == -_leg_rep(receiver, i), i
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
    assert _leg_npv(in_eur, 0) == _leg_npv(in_usd, 0)
    assert _leg_npv(in_eur, 1) == _leg_npv(in_usd, 1)
    assert _leg_rep(in_eur, 0) == _leg_npv(in_eur, 0)                      # EUR leg in EUR
    assert abs(_leg_rep(in_usd, 0) - _leg_npv(in_usd, 0) * X) < 1e-4       # EUR leg in USD
    assert abs(_leg_rep(in_eur, 1) - _leg_npv(in_eur, 1) / X) < 1e-4       # USD leg in EUR
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
    i = 1 if _is_fixed(_leg(request, 1)) else 2
    at_fair = _mutated(request, lambda r: _coupons(_leg(r, i)).update(rate=fair))
    row = _price(client, at_fair)
    assert abs(row["npv"]) < 1e-9 * N_EUR, f"{fixture}: npv at the fair rate is {row['npv']}"
    assert abs(row["fair_rate"] - fair) < RATE_TOLERANCE


def test_xccy_fixed_at_fair_rate_example(client, data_dir):
    """The committed at-fair example (the baseline struck at its fair rate)
    has a zero NPV and reports the same fair rate."""
    request = load_json(data_dir / FIXED_AT_FAIR)
    row = _price(client, request)
    assert abs(row["npv"]) < 1e-9 * N_EUR, row["npv"]
    assert abs(row["fair_rate"] - _coupons(_leg(request, 1))["rate"]) < RATE_TOLERANCE


@pytest.mark.parametrize("fixture", [BASIS_BOTH, BASIS_FINAL_ONLY])
def test_xccy_repriced_at_fair_spread_has_zero_npv(client, data_dir, fixture):
    """The reported fair_spread (on leg2) rebuilt through the API — not by
    linear extrapolation — zeroes the NPV."""
    request = load_json(data_dir / fixture)
    fair = _price(client, request)["fair_spread"]
    at_fair = _mutated(request, lambda r: _coupons(_leg(r, 2)).update(spread=fair))
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
    assert _coupons(_leg(request, 2))["spread"] == -0.0015
    row = _price(client, request)
    assert abs(row["fair_spread"] - BASIS_5Y) < 1e-7, row["fair_spread"]
    # Struck at -15 bp the receiver of the EUR leg gets 2 bp more than fair:
    # the NPV is positive, 2 bp x the EUR annuity (~4.6 years) in USD.
    assert 0.0005 * N_USD < row["npv"] < 0.0015 * N_USD
    at_pillar = _price(client, _mutated(request, lambda r: _coupons(_leg(r, 2)).update(spread=BASIS_5Y)))
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
    for i, sign in ((0, -1.0), (1, +1.0)):   # pays leg1, receives leg2
        initial = [f for f in rows_both["legs"][i]["flows"]
                   if f["kind"] == "NotionalExchange" and f["amount"] < 0]
        assert len(initial) == 1
        pv_initial = initial[0]["present_value"]           # -N * DF(start) in the own frame
        expected = _leg_npv(rows_both, i) - sign * pv_initial
        assert abs(_leg_npv(row_final, i) - expected) < 1e-6 * N_USD, (
            f"legs[{i}]: {_leg_npv(row_final, i)} vs {expected}")
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
    assert abs(_leg_rep(row, 0) - _leg_npv(row, 0) / X) < 1e-4
    assert _leg_rep(row, 1) == _leg_npv(row, 1)
    assert _leg_npv(row, 0) > 0 > _leg_npv(row, 1)       # receives USD fixed, pays EUR float
    assert "fair_rate" in row and "fair_spread" not in row


def test_xccy_fixed_fixed_has_no_fair_quantity(client, data_dir):
    request = load_json(data_dir / FIXED_USD)
    usd_fixed = copy.deepcopy(_leg(load_json(data_dir / USD_FIXED_EURIBOR), 1))
    usd_fixed["pay_receive"] = "Receive"
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
    for i, sign, notional, n_coupons in ((0, -1.0, N_EUR, 5), (1, +1.0, N_USD, 20)):
        flows = row["legs"][i]["flows"]
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
        floating = i == 1
        for f in coupons:
            assert ("fixing_date" in f) == floating and ("spread" in f) == floating
        assert abs(sum(f["present_value"] for f in flows) - _leg_npv(row, i) / sign) < 1e-6
        assert len(flows) == len(ref["legs"][i]["flows"])
        for api, (date, amount, discount) in zip(flows, ref["legs"][i]["flows"]):
            assert api["payment_date"] == date
            assert abs(api["amount"] - amount) < 1e-6
            assert abs(api["discount"] - discount) < RATE_TOLERANCE
    # Without the flag the lists are absent, and the scalars are the same.
    plain = _price(client, load_json(data_dir / FIXED_USD))
    assert all("flows" not in leg for leg in plain["legs"])
    assert _scalars(row) == plain


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
    coupons = [f for f in row["legs"][0]["flows"] if f["kind"] == "Coupon"]
    assert len(coupons) == 20
    for f, (_, amount, _) in zip(coupons, ref["legs"][0]["flows"]):
        assert 0.02 < f["rate"] < 0.07
        assert abs(f["amount"] - N_USD * f["rate"] * f["accrual_year_fraction"]) < 1e-3
        assert abs(f["amount"] - amount) < 1e-6          # the compounded coupon, QuantLib's
        assert abs(f["index_fixing"] - f["rate"]) < 0.002   # first-day fixing vs compounded
    eur = [f for f in row["legs"][1]["flows"] if f["kind"] == "Coupon"]
    for f, (_, amount, _) in zip(eur, ref["legs"][1]["flows"]):
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


@pytest.mark.parametrize("leg", [1, 2])
def test_xccy_pay_receive_absent_400(client, data_dir, leg):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, leg).pop("pay_receive"))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg{leg}.pay_receive is required")


@pytest.mark.parametrize("side", ["Pay", "Receive"])
def test_xccy_both_legs_same_side_400(client, data_dir, side):
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: [_leg(r, i).update(pay_receive=side) for i in (1, 2)])
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg1.pay_receive and "
                                        f"CrossCurrencySwap.leg2.pay_receive are both {side}")


@pytest.mark.parametrize("field", ["initial_exchange", "final_exchange"])
def test_xccy_exchange_flags_presence_required_400(client, data_dir, field):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2).pop(field))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg2.{field}")


def test_xccy_coupons_absent_400(client, data_dir):
    """`coupons` is a required union: the JSON parser rejects a leg without
    it (and without its `coupons_type` tag)."""
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: [_leg(r, 1).pop("coupons"), _leg(r, 1).pop("coupons_type")])
    status, text = _post(client, request)
    assert status == 400, text[:300]


def test_xccy_coupons_type_without_value_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 1).pop("coupons"))
    status, text = _post(client, request)
    assert status == 400, text[:300]


def test_xccy_cms_coupons_400(client, data_dir):
    """CmsCoupons is a member of the SwapCoupons union but not priced on this
    endpoint in this version: a 400 saying so."""
    cms = {"notional": N_EUR, "schedule": _coupons(_leg(load_json(data_dir / FIXED_USD), 1))["schedule"],
           "swap_index_id": "EUR_SWAP_6M", "swap_tenor": {"n": 10, "unit": "Years"},
           "swaption_vol_id": "VOL", "day_counter": "Actual360",
           "payment_convention": "ModifiedFollowing"}
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 1).update(coupons_type="CmsCoupons", coupons=cms))
    _assert_400_naming(client, request, "CmsCoupons is not supported on price-cross-currency-swap")


def test_xccy_ibor_coupons_on_overnight_index_400(client, data_dir):
    """IborCoupons on SOFR: the index is overnight, so the leg must be
    OvernightCoupons (a daily-tenor IBOR coupon fixed in advance is not an
    OIS leg). The 400 names the table to use."""
    ibor = {**_coupons(_leg(load_json(data_dir / FIXED_USD), 2))}
    for k in ("payment_calendar", "payment_lag", "averaging_method", "lookback_days",
              "lockout_days", "apply_observation_shift", "telescopic_value_dates"):
        ibor.pop(k)
    ibor["day_counter"] = "Actual360"
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 2).update(coupons_type="IborCoupons", coupons=ibor))
    status, text = _post(client, request)
    assert status == 400, text[:300]
    assert "CrossCurrencySwap.leg2.coupons is IborCoupons" in text and "OvernightCoupons" in text
    assert "USD_SOFR" in text


def test_xccy_overnight_coupons_on_ibor_index_400(client, data_dir):
    """OvernightCoupons on Euribor 3M: not an overnight index, use IborCoupons."""
    base = load_json(data_dir / USD_FIXED_EURIBOR)
    sofr_leg = _coupons(_leg(load_json(data_dir / FIXED_USD), 2))
    on = {**sofr_leg, "notional": N_EUR, "schedule": _coupons(_leg(base, 2))["schedule"],
          "index": {"id": "EUR_3M"}, "payment_calendar": "TARGET"}
    request = _mutated(base, lambda r: _leg(r, 2).update(coupons_type="OvernightCoupons", coupons=on))
    status, text = _post(client, request)
    assert status == 400, text[:300]
    assert "CrossCurrencySwap.leg2.coupons is OvernightCoupons" in text and "IborCoupons" in text
    assert "EUR_3M" in text


def test_xccy_forwarding_curve_required_with_floating_coupons_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _leg(r, 2).pop("forwarding_curve"))
    _assert_400_naming(client, request,
                       "CrossCurrencySwap.leg2.forwarding_curve is required with OvernightCoupons")
    request = _mutated(load_json(data_dir / USD_FIXED_EURIBOR), lambda r: _leg(r, 2).pop("forwarding_curve"))
    _assert_400_naming(client, request,
                       "CrossCurrencySwap.leg2.forwarding_curve is required with IborCoupons")


def test_xccy_forwarding_curve_rejected_with_fixed_coupons_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD),
                       lambda r: _leg(r, 1).update(forwarding_curve="EUR_ESTR_OIS"))
    _assert_400_naming(client, request,
                       "CrossCurrencySwap.leg1.forwarding_curve must be absent with FixedCoupons")


@pytest.mark.parametrize("value", [0.0, -1_000_000.0])
def test_xccy_nonpositive_notional_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _coupons(_leg(r, 1)).update(notional=value))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg1.coupons.notional")
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _coupons(_leg(r, 2)).update(notional=value))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg2.coupons.notional")


def test_xccy_notional_absent_400(client, data_dir):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _coupons(_leg(r, 1)).pop("notional"))
    _assert_400_naming(client, request, "CrossCurrencySwap.leg1.coupons.notional is required")


@pytest.mark.parametrize("fixture,leg", [(FIXED_USD, 1), (USD_FIXED_EURIBOR, 2)])
def test_xccy_per_period_notionals_rejected_400(client, data_dir, fixture, leg):
    """FixedCoupons and IborCoupons carry an optional per-period `notionals`
    vector for the vanilla swap; a SwapLeg rejects it in this version."""
    request = _mutated(load_json(data_dir / fixture),
                       lambda r: _coupons(_leg(r, leg)).update(notionals=[N_EUR] * 5))
    status, text = _post(client, request)
    assert status == 400 and "notionals" in text, text[:300]


@pytest.mark.parametrize("fixture,leg,field", [
    (FIXED_USD, 1, "day_counter"), (FIXED_USD, 1, "payment_convention"),
    (USD_FIXED_EURIBOR, 2, "day_counter"), (USD_FIXED_EURIBOR, 2, "payment_convention"),
    (FIXED_USD, 2, "payment_convention"), (FIXED_USD, 2, "payment_calendar"),
    (FIXED_USD, 2, "payment_lag"), (FIXED_USD, 2, "averaging_method"),
    (FIXED_USD, 2, "lookback_days"), (FIXED_USD, 2, "lockout_days"),
    (FIXED_USD, 2, "apply_observation_shift")])
def test_xccy_coupon_conventions_presence_required_400(client, data_dir, fixture, leg, field):
    request = _mutated(load_json(data_dir / fixture), lambda r: _coupons(_leg(r, leg)).pop(field))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg{leg}.coupons.{field}")


def test_xccy_overnight_day_counter_is_ignored(client, data_dir):
    """OvernightCoupons.day_counter is deprecated and not read: the coupons
    take the index's day counter, as on price-ois-swap."""
    request = load_json(data_dir / FIXED_USD)
    base = _price(client, request)
    with_dc = _price(client, _mutated(request, lambda r: _coupons(_leg(r, 2)).update(day_counter="Actual365Fixed")))
    assert with_dc == base


@pytest.mark.parametrize("field,value,token", [
    ("fixing_days", -1, "CrossCurrencySwap.leg2.coupons.fixing_days"),
    ("spread", "nan", "CrossCurrencySwap.leg2.coupons.spread")])
def test_xccy_ibor_coupon_values_400(client, data_dir, field, value, token):
    request = _mutated(load_json(data_dir / USD_FIXED_EURIBOR),
                       lambda r: _coupons(_leg(r, 2)).update({field: value}))
    status, text = _post(client, request)
    assert status == 400, text[:300]
    assert token in text or field in text


@pytest.mark.parametrize("field", ["payment_lag", "lookback_days", "lockout_days"])
def test_xccy_overnight_negative_days_400(client, data_dir, field):
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: _coupons(_leg(r, 2)).update({field: -1}))
    _assert_400_naming(client, request, f"CrossCurrencySwap.leg2.coupons.{field}")


def test_xccy_in_arrears_on_ibor_index_is_priced(client, data_dir):
    request = _mutated(load_json(data_dir / USD_FIXED_EURIBOR),
                       lambda r: _coupons(_leg(r, 2)).update(in_arrears=True))
    row = _price(client, request)
    assert row["npv"] != _price(client, load_json(data_dir / USD_FIXED_EURIBOR))["npv"]


def test_xccy_overnight_payment_lag_moves_the_payment_dates(client, data_dir):
    """A 2-day payment lag on the SOFR leg pays every coupon two business
    days after the period end (the OIS convention); the amounts are
    unchanged and the PVs move only through the discount factors."""
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: r.update(include_flows=True))
    lagged = _mutated(request, lambda r: _coupons(_leg(r, 2)).update(payment_lag=2))
    plain = [f for f in client.price("cross_currency_swap", request)["swaps"][0]["legs"][1]["flows"]
             if f["kind"] == "Coupon"]
    moved = [f for f in client.price("cross_currency_swap", lagged)["swaps"][0]["legs"][1]["flows"]
             if f["kind"] == "Coupon"]
    assert len(plain) == len(moved) == 20
    for a, b in zip(plain, moved):
        assert b["payment_date"] > a["payment_date"]
        assert b["accrual_end_date"] == a["accrual_end_date"]
        assert abs(a["amount"] - b["amount"]) < 1e-6
        assert b["discount"] < a["discount"]


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
    assert "CrossCurrencySwap.leg2.currency 'GBP'" in text and "USD_SOFR" in text


def test_xccy_spot_pair_not_the_legs_pair_400(client, data_dir):
    """EUR and USD legs priced off an EUR/GBP spot: a 400 naming the spot."""
    request = _mutated(load_json(data_dir / FIXED_USD), lambda r: (
        r["pricing"]["fx"]["spots"].append(dict(EURGBP_SPOT)),
        _entry(r).update(fx_spot="EURGBP_SPOT")))
    _assert_400_naming(client, request, "PriceCrossCurrencySwap.fx_spot")


def test_xccy_spot_quoted_the_other_way_round_is_accepted(client, data_dir):
    """USD/EUR spot (1/1.0850) with the same two legs: either leg may be the
    base-currency leg; the as-of rate is then quoted in the spot's own order,
    and the NPV is unchanged."""
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
    assert row["legs"][0]["npv"] == base["legs"][0]["npv"]
    assert row["legs"][1]["npv"] == base["legs"][1]["npv"]


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
                       lambda r: _coupons(_leg(r, 2))["index"].update(id="NOPE"))
    _assert_404(client, request, "Index not found: NOPE")


def test_xccy_response_is_json_serializable_twice(client, data_dir):
    """Same request, same bytes: the response carries no per-call state."""
    request = load_json(data_dir / BASIS_BOTH)
    a = client.price("cross_currency_swap", request)
    b = client.price("cross_currency_swap", request)
    assert json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)
