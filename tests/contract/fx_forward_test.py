"""FX forward: parity, pricing identities and request contract.

`POST /price-fx-forward` prices outright FX forwards off an FX spot
(`pricing.fx.spots`) and one discount curve per currency, with plain
discount-factor algebra (there is no FX forward instrument in QuantLib):

    base_leg_pv  = sign * N * DF_b(T)              (base ccy)
    quote_leg_pv = -sign * N * K * DF_q(T)         (quote ccy)
    X            = S * DF_q(t_s) / DF_b(t_s)       (as-of exchange rate)
    npv          = base_leg_pv * X + quote_leg_pv  (reporting = quote ccy)
                 = base_leg_pv + quote_leg_pv / X  (reporting = base ccy)
    fair         = X * DF_b(T) / DF_q(T),  forward_points = fair - S

The fixtures (examples/data/fx_forward/) price EUR/USD forwards on the
USD-collateralised EUR curve + USD SOFR OIS market of the FX helper tests, a
USD/JPY forward on SOFR + TONA OIS curves, and one with the spot through an
Fx quote. Every example is also a functional-catalog case.

  * parity: every response field equals the Python QuantLib reference
    (ql_reference.fx_forward_ql) built on the same curves — 1e-12 of the
    notional (the bootstrap accuracy of the two curve builds, C++ and
    Python, which agree to ~1e-14 on the discount factors) on the money
    amounts, 1e-12 relative on the fair forward;
  * sign: the sold twin is the exact negative (npv and both legs), with the
    same fair forward and points;
  * reporting currency: npv_EUR * X == npv_USD;
  * K = fair_forward_rate reprices to |npv| < 1e-9 * N;
  * the fair forward at an FX swap pillar of the collateral curve reproduces
    that pillar's spot + points;
  * the quote_id spelling of the spot is bit-identical to the inline one;
  * multi-trade requests answer in request order; no response field is
    conditional (every field is present on every row);
  * every validation is a 400 naming the field; unknown references are 404.
"""

import copy
import json

import pytest

from ql_reference import fx_forward_ql, load_json

EURUSD_6M = "fx_forward/fx_forward_eurusd_6m_buy_eur.json"
EURUSD_6M_SELL = "fx_forward/fx_forward_eurusd_6m_sell_eur.json"
EURUSD_6M_IN_EUR = "fx_forward/fx_forward_eurusd_6m_buy_eur_reported_in_eur.json"
EURUSD_6M_QUOTE_ID = "fx_forward/fx_forward_eurusd_6m_buy_eur_spot_quote_id.json"
EURUSD_1Y = "fx_forward/fx_forward_eurusd_1y_on_market.json"
USDJPY_3M = "fx_forward/fx_forward_usdjpy_3m_sell_usd.json"
ALL_FIXTURES = [EURUSD_6M, EURUSD_6M_SELL, EURUSD_6M_IN_EUR, EURUSD_6M_QUOTE_ID,
                EURUSD_1Y, USDJPY_3M]

RATE_TOLERANCE = 1e-12       # relative, on the fair forward
IDENTITY_TOLERANCE = 1e-12   # relative, on server-vs-server identities


def _money_tolerance(request):
    """Absolute tolerance on an amount of the request's forward: 1e-12 of
    the larger of the two notionals (base, and base x contract rate in quote
    units). Both curve builds are QuantLib bootstraps accurate to ~1e-12 on
    the discount factors, so the amounts agree to that times the notional."""
    fwd = request["fx_forwards"][0]["fx_forward"]
    return 1e-12 * fwd["base_notional"] * max(1.0, fwd["forward_rate"])
RESPONSE_FIELDS = {"npv", "fair_forward_rate", "forward_points", "base_leg_pv",
                   "quote_leg_pv", "spot_date", "maturity_date", "fx_spot_used"}


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _post(client, request):
    r = client.session.post(f"{client.base_url}/{client.ENDPOINTS['fx_forward']}",
                            json=request)
    return r.status_code, r.text


def _row(response, i=0):
    rows = response["fx_forwards"]
    assert RESPONSE_FIELDS <= set(rows[i]), (
        f"response row {i} is missing fields: {RESPONSE_FIELDS - set(rows[i])}")
    return rows[i]


def _price(client, request):
    return _row(client.price("fx_forward", request))


def _mutated(request, mutate):
    out = copy.deepcopy(request)
    mutate(out)
    return out


def _entry(request):
    return request["fx_forwards"][0]


def _fwd(request):
    return request["fx_forwards"][0]["fx_forward"]


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
def test_fx_forward_parity(client, data_dir, fixture):
    request = load_json(data_dir / fixture)
    api = _price(client, request)
    ref = fx_forward_ql(request)
    for name in ("npv", "base_leg_pv", "quote_leg_pv"):
        assert abs(api[name] - ref[name]) < _money_tolerance(request), (
            f"{fixture}: {name} API {api[name]!r} vs QuantLib {ref[name]!r}")
    assert abs(api["fair_forward_rate"] - ref["fair_forward_rate"]) < \
        RATE_TOLERANCE * ref["fair_forward_rate"], (
            f"{fixture}: fair forward API {api['fair_forward_rate']!r} vs "
            f"QuantLib {ref['fair_forward_rate']!r}")
    assert abs(api["forward_points"] - ref["forward_points"]) < RATE_TOLERANCE, (
        f"{fixture}: forward points API {api['forward_points']!r} vs "
        f"QuantLib {ref['forward_points']!r}")
    assert api["fx_spot_used"] == ref["fx_spot_used"]
    assert api["spot_date"] == ref["spot_date"]
    assert api["maturity_date"] == ref["maturity_date"]


def test_fx_forward_dates_and_sign_of_the_baseline(client, data_dir):
    """EUR/USD 6M buy at 1.0950 on 2025-01-15: spot T+2 on the joint TARGET +
    US calendar is 2025-01-17; 2025-07-17 is a business day and stays; the
    fair forward (~1.0981) is above the contract rate so the buyer of EUR is
    in the money; the EUR leg is received, the USD leg paid."""
    request = load_json(data_dir / EURUSD_6M)
    row = _price(client, request)
    assert row["spot_date"] == "2025-01-17"
    assert row["maturity_date"] == "2025-07-17"
    assert row["fx_spot_used"] == 1.0850
    assert row["fair_forward_rate"] > 1.0950
    assert row["npv"] > 0
    assert row["base_leg_pv"] > 0 and row["quote_leg_pv"] < 0
    assert row["forward_points"] > 0


def test_fx_forward_maturity_is_adjusted_with_the_spot_calendar(client, data_dir):
    """The 1Y example gives 2026-01-17 (a Saturday); ModifiedFollowing on the
    joint calendar skips Monday 2026-01-19 (US holiday) to 2026-01-20, and the
    response reports the adjusted date."""
    request = load_json(data_dir / EURUSD_1Y)
    row = _price(client, request)
    assert _fwd(request)["maturity_date"] == "2026-01-17"
    assert row["maturity_date"] == "2026-01-20"


# ---------------------------------------------------------------------------
# pricing identities
# ---------------------------------------------------------------------------

def test_fx_forward_sell_is_the_exact_negative_of_buy(client, data_dir):
    buy = _price(client, load_json(data_dir / EURUSD_6M))
    sell = _price(client, load_json(data_dir / EURUSD_6M_SELL))
    # The same operations with the sign flipped: exact negatives.
    assert buy["npv"] == -sell["npv"]
    assert buy["base_leg_pv"] == -sell["base_leg_pv"]
    assert buy["quote_leg_pv"] == -sell["quote_leg_pv"]
    # The market quantities do not depend on the side.
    assert buy["fair_forward_rate"] == sell["fair_forward_rate"]
    assert buy["forward_points"] == sell["forward_points"]
    assert buy["spot_date"] == sell["spot_date"]
    assert buy["maturity_date"] == sell["maturity_date"]


def test_fx_forward_reporting_currency_identity(client, data_dir):
    """npv reported in EUR times the as-of exchange rate X equals the npv
    reported in USD; the legs are unchanged (each stays in its own currency)."""
    in_usd_req = load_json(data_dir / EURUSD_6M)
    in_eur_req = load_json(data_dir / EURUSD_6M_IN_EUR)
    assert _entry(in_eur_req)["reporting_currency"] == "EUR"
    in_usd = _price(client, in_usd_req)
    in_eur = _price(client, in_eur_req)
    X = fx_forward_ql(in_usd_req)["as_of_exchange_rate"]
    assert abs(in_eur["npv"] * X - in_usd["npv"]) < IDENTITY_TOLERANCE * abs(in_usd["npv"]), (
        f"npv_EUR {in_eur['npv']} * X {X} != npv_USD {in_usd['npv']}")
    assert in_eur["base_leg_pv"] == in_usd["base_leg_pv"]
    assert in_eur["quote_leg_pv"] == in_usd["quote_leg_pv"]
    assert in_eur["fair_forward_rate"] == in_usd["fair_forward_rate"]


@pytest.mark.parametrize("fixture", [EURUSD_6M, EURUSD_6M_IN_EUR, USDJPY_3M, EURUSD_1Y])
def test_fx_forward_repriced_at_fair_has_zero_npv(client, data_dir, fixture):
    """Setting the contract rate to the reported fair forward gives an NPV of
    zero to rounding, in either reporting currency and for either side."""
    request = load_json(data_dir / fixture)
    fair = _price(client, request)["fair_forward_rate"]
    at_fair = _mutated(request, lambda r: _fwd(r).update(forward_rate=fair))
    row = _price(client, at_fair)
    N = _fwd(request)["base_notional"]
    assert abs(row["npv"]) < 1e-9 * N, f"{fixture}: npv at K = fair is {row['npv']}"
    assert row["fair_forward_rate"] == fair


def test_fx_forward_fair_reprices_the_fx_swap_pillar(client, data_dir):
    """The 6M maturity is the 6M FxSwapHelper pillar of the EUR curve, which
    was bootstrapped from spot 1.0850 + 6M points 0.01310 on the same USD
    curve: the fair forward must be 1.09810 to bootstrap accuracy."""
    request = load_json(data_dir / EURUSD_6M)
    curve = next(c for c in request["pricing"]["rates"]["curves"]
                 if c["id"] == _entry(request)["base_discounting_curve"])
    six_m = next(p["point"] for p in curve["points"]
                 if p["point_type"] == "FxSwapHelper" and p["point"]["tenor"] == {"n": 6, "unit": "Months"})
    row = _price(client, request)
    assert abs(row["fair_forward_rate"] - (1.0850 + six_m["fx_points"])) < 1e-9
    assert abs(row["forward_points"] - six_m["fx_points"]) < 1e-9


def test_fx_forward_on_market_1y_has_near_zero_npv(client, data_dir):
    """K = spot + 1Y points on the 1Y pillar date: the forward is on-market
    and its NPV is zero to bootstrap accuracy (1e-12 on a 10m notional)."""
    row = _price(client, load_json(data_dir / EURUSD_1Y))
    assert abs(row["npv"]) < 1e-3, row["npv"]
    assert abs(row["forward_points"] - 0.02702) < 1e-9


def test_fx_forward_spot_quote_id_identical_to_inline(client, data_dir):
    inline = client.price("fx_forward", load_json(data_dir / EURUSD_6M))
    twin = client.price("fx_forward", load_json(data_dir / EURUSD_6M_QUOTE_ID))
    assert json.dumps(inline, sort_keys=True) == json.dumps(twin, sort_keys=True)


def test_fx_forward_moving_the_spot_moves_the_npv(client, data_dir):
    request = load_json(data_dir / EURUSD_6M)
    base = _price(client, request)
    moved = _price(client, _mutated(request, lambda r: r["pricing"]["fx"]["spots"][0].update(rate=1.0950)))
    assert moved["fx_spot_used"] == 1.0950
    assert moved["npv"] > base["npv"]  # buyer of EUR gains when EUR/USD rises
    assert moved["fair_forward_rate"] > base["fair_forward_rate"]


def test_fx_forward_usdjpy_inverted_size_pair(client, data_dir):
    """USD/JPY: quote amounts ~150x the base ones, Japan + US joint calendar
    (spot 2025-01-17), the sold side of USD at 153.80 against a fair forward
    below the spot (JPY rates far below USD rates)."""
    row = _price(client, load_json(data_dir / USDJPY_3M))
    assert row["spot_date"] == "2025-01-17"
    assert row["maturity_date"] == "2025-04-17"
    assert row["fair_forward_rate"] < 155.0
    assert row["forward_points"] < 0
    assert row["base_leg_pv"] < 0 and row["quote_leg_pv"] > 0   # sold USD, bought JPY
    assert abs(row["quote_leg_pv"]) > 100 * abs(row["base_leg_pv"])


def test_fx_forward_multi_trade_in_request_order(client, data_dir):
    buy = load_json(data_dir / EURUSD_6M)
    sell = load_json(data_dir / EURUSD_6M_SELL)
    both = copy.deepcopy(buy)
    both["fx_forwards"] = [_entry(buy), _entry(sell), _entry(buy)]
    response = client.price("fx_forward", both)
    rows = response["fx_forwards"]
    assert len(rows) == 3
    assert rows[0] == rows[2]
    assert rows[0]["npv"] == -rows[1]["npv"]
    assert rows[0] == _price(client, buy)


def test_fx_forward_no_field_is_conditional(client, data_dir):
    """No FxForwardResponse field is declared `= null`: every field is present
    on every row (a zero is a computed zero)."""
    for fixture in ALL_FIXTURES:
        row = _row(client.price("fx_forward", load_json(data_dir / fixture)))
        assert set(row) == RESPONSE_FIELDS, f"{fixture}: {sorted(row)}"


# ---------------------------------------------------------------------------
# request contract: 400 naming the field, 404 for unknown references
# ---------------------------------------------------------------------------

def test_fx_forward_empty_list_400(client, data_dir):
    request = load_json(data_dir / EURUSD_6M)
    request["fx_forwards"] = []
    _assert_400_naming(client, request, "fx_forwards")


@pytest.mark.parametrize("field", ["base_notional", "forward_rate", "direction"])
def test_fx_forward_presence_required_scalars_400(client, data_dir, field):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).pop(field))
    _assert_400_naming(client, request, f"FxForward.{field}")


@pytest.mark.parametrize("value", [0.0, -1_000_000.0])
def test_fx_forward_nonpositive_notional_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update(base_notional=value))
    _assert_400_naming(client, request, "FxForward.base_notional")


@pytest.mark.parametrize("value", [0.0, -1.095])
def test_fx_forward_nonpositive_forward_rate_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update(forward_rate=value))
    _assert_400_naming(client, request, "FxForward.forward_rate")


@pytest.mark.parametrize("field", ["base_currency", "quote_currency", "maturity_date"])
def test_fx_forward_required_strings_400(client, data_dir, field):
    """`(required)` strings: the FlatBuffers JSON parser rejects the request."""
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).pop(field))
    status, text = _post(client, request)
    assert status == 400, f"{field}: expected 400, got {status}: {text[:300]}"


@pytest.mark.parametrize("field", ["fx_spot", "base_discounting_curve",
                                   "quote_discounting_curve", "reporting_currency"])
def test_fx_forward_required_wrapper_strings_400(client, data_dir, field):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _entry(r).pop(field))
    status, text = _post(client, request)
    assert status == 400, f"{field}: expected 400, got {status}: {text[:300]}"


def test_fx_forward_missing_fx_forward_table_400(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _entry(r).pop("fx_forward"))
    status, text = _post(client, request)
    assert status == 400, f"expected 400, got {status}: {text[:300]}"


@pytest.mark.parametrize("field,value", [("base_currency", "EURUSD"), ("base_currency", "eur"),
                                         ("quote_currency", "US"), ("quote_currency", "")])
def test_fx_forward_currency_not_iso_400(client, data_dir, field, value):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update({field: value}))
    _assert_400_naming(client, request, f"FxForward.{field}")


def test_fx_forward_same_currency_twice_400(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update(quote_currency="EUR"))
    _assert_400_naming(client, request, "FxForward.quote_currency")


def test_fx_forward_inverted_pair_is_rejected_not_inverted(client, data_dir):
    """USD/EUR against a EUR/USD spot: a 400 naming the spot, never an
    implicit 1/S."""
    request = _mutated(load_json(data_dir / EURUSD_6M),
                       lambda r: _fwd(r).update(base_currency="USD", quote_currency="EUR"))
    status, text = _post(client, request)
    assert status == 400, f"expected 400, got {status}: {text[:300]}"
    assert "PriceFxForward.fx_spot" in text and "EUR/USD" in text


def test_fx_forward_pair_not_the_spots_pair_400(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M),
                       lambda r: _fwd(r).update(base_currency="GBP"))
    _assert_400_naming(client, request, "PriceFxForward.fx_spot")


@pytest.mark.parametrize("value", ["GBP", "eur", "EURUSD"])
def test_fx_forward_reporting_currency_outside_pair_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _entry(r).update(reporting_currency=value))
    _assert_400_naming(client, request, "PriceFxForward.reporting_currency")


@pytest.mark.parametrize("value", ["17/07/2025", "2025-13-01", "2025-07-32"])
def test_fx_forward_unparsable_maturity_400(client, data_dir, value):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update(maturity_date=value))
    status, text = _post(client, request)
    assert status == 400, f"{value}: expected 400, got {status}: {text[:300]}"
    assert "Invalid date" in text or "maturity_date" in text


@pytest.mark.parametrize("value", ["2025-01-17",   # the spot date itself
                                   "2025-01-18",   # Saturday: rolls past the spot date, priced
                                   "2025-01-15",   # as-of
                                   "2024-12-31"])  # before as-of
def test_fx_forward_maturity_not_after_spot_400(client, data_dir, value):
    """Adjusted maturity <= spot date is a 400 naming the field. A date the
    calendar rolls past the spot date is priced on the rolled date: Saturday
    2025-01-18 -> Monday 01-20 is a US holiday -> 2025-01-21, as the
    reference's joint-calendar adjustment gives."""
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update(maturity_date=value))
    status, text = _post(client, request)
    if value == "2025-01-18":
        assert status == 200, text[:300]
        rolled = fx_forward_ql(request)["maturity_date"]
        assert rolled == "2025-01-21"
        assert json.loads(text)["fx_forwards"][0]["maturity_date"] == rolled
    else:
        assert status == 400, f"{value}: expected 400, got {status}: {text[:300]}"
        assert "FxForward.maturity_date" in text and "spot date 2025-01-17" in text


def test_fx_forward_unknown_spot_404(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _entry(r).update(fx_spot="NOPE"))
    _assert_404(client, request, "FX spot not found: NOPE")


@pytest.mark.parametrize("field,token", [("base_discounting_curve", "Base discounting curve not found"),
                                         ("quote_discounting_curve", "Quote discounting curve not found")])
def test_fx_forward_unknown_curve_404(client, data_dir, field, token):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _entry(r).update({field: "NOPE"}))
    _assert_404(client, request, token)


def test_fx_forward_spot_quote_id_unknown_404(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M_QUOTE_ID),
                       lambda r: r["pricing"]["fx"]["spots"][0].update(quote_id="NOPE"))
    status, text = _post(client, request)
    assert status == 404, f"expected 404, got {status}: {text[:300]}"


def test_fx_forward_spot_quote_must_be_fx_type_400(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M_QUOTE_ID),
                       lambda r: r["pricing"]["quotes"][0].update(quote_type="Curve"))
    _assert_400_naming(client, request, "FxSpotSpec.quote_id")


def test_fx_forward_direction_unknown_value_400(client, data_dir):
    request = _mutated(load_json(data_dir / EURUSD_6M), lambda r: _fwd(r).update(direction="Long"))
    status, text = _post(client, request)
    assert status == 400, f"expected 400, got {status}: {text[:300]}"
