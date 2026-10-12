"""FX market data + FX swap helper: parity and request contract.

`pricing.fx.spots[]` carries FX spot rates (FxSpotSpec) and `FxSwapHelper`
pillars bootstrap the discount curve of the non-collateral currency of a pair
from FX swap points, given the spot and the collateral currency's discount
curve. The fixture is a USD-collateralised EUR discount curve
(examples/data/curves/curves_eur_usd_collateral_fxswap_short_end.json): USD
SOFR OIS curve, EUR/USD spot (T+2, TARGET + US settlement joint calendar), FX
pillars 1W/1M/3M/6M/1Y. Its twin spells every FX point and the spot itself
through pricing.quotes (points as Curve quotes, the spot as an Fx quote).

  * parity: the sampled discount factors and zero rates equal an
    independently built QuantLib curve (ql.FxSwapRateHelper on the same
    inputs) to 1e-12;
  * the quote_id twin is bit-identical to the inline fixture;
  * wire order does not matter: the FX curve is listed before its collateral
    curve and the bootstrapper still builds the collateral curve first;
  * every FxSpotSpec / FxSwapHelper validation is a 400 naming the field, an
    unknown fx_spot / quote / collateral curve is a 404, and
    CrossCcyBasisHelper is still a 501 with a clear message.
"""

import copy
import json

import pytest

from ql_reference import bootstrap_curves_ql, load_json

FIXTURE = "curves/curves_eur_usd_collateral_fxswap_short_end.json"
TWIN = "curves/curves_eur_usd_collateral_fxswap_short_end_quote_id.json"
PARITY_TOLERANCE = 1e-12
FX_CURVE = "EUR_USD_COLLATERAL"
USD_CURVE = "USD_SOFR_OIS"


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _post(client, request):
    r = client.session.post(f"{client.base_url}/{client.ENDPOINTS['bootstrap_curves']}",
                            json=request)
    return r.status_code, r.text


def _series(response):
    """{measure: [values]} of the single sampled curve, failing on a per-curve error."""
    results = response["results"]
    assert len(results) == 1, response
    entry = results[0]
    assert not entry.get("error"), entry["error"]
    return {s.get("measure", "DF"): s["values"] for s in entry["series"]}


def _fx_curve(request):
    return next(c for c in request["pricing"]["rates"]["curves"] if c["id"] == FX_CURVE)


def _spot(request):
    return request["pricing"]["fx"]["spots"][0]


def _mutated(request, mutate):
    out = copy.deepcopy(request)
    mutate(out)
    return out


# ---------------------------------------------------------------------------
# parity
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("fixture", [FIXTURE, TWIN], ids=["inline", "quote_id"])
def test_fx_implied_curve_matches_quantlib(client, data_dir, fixture):
    request = load_json(data_dir / fixture)
    api = _series(client.price("bootstrap_curves", request))
    ql_values = bootstrap_curves_ql(request)
    assert set(api) == set(ql_values) == {"DF", "ZERO"}
    worst = 0.0
    for measure, expected in ql_values.items():
        assert len(api[measure]) == len(expected)
        for a, b in zip(api[measure], expected):
            worst = max(worst, abs(a - b))
            assert a == pytest.approx(b, abs=PARITY_TOLERANCE), (measure, a, b)
    print(f"[fx] {fixture}: max |api - QuantLib| = {worst:.3e} over DF+ZERO")
    # The curve is genuinely FX-implied: positive EUR/USD points mean EUR
    # rates below USD rates, so the EUR 1Y DF sits above the USD one.
    usd = copy.deepcopy(request)
    usd["queries"][0]["curve_id"] = USD_CURVE
    usd_df = _series(client.price("bootstrap_curves", usd))["DF"]
    assert api["DF"][-1] > usd_df[-1] + 1e-4


def test_quote_id_twin_is_bit_identical_to_inline(client, data_dir):
    inline = client.price("bootstrap_curves", load_json(data_dir / FIXTURE))
    twin = client.price("bootstrap_curves", load_json(data_dir / TWIN))
    assert json.dumps(_series(inline), sort_keys=True) == \
        json.dumps(_series(twin), sort_keys=True)
    assert inline["results"][0]["grid_dates"] == twin["results"][0]["grid_dates"]


def test_curve_order_on_the_wire_does_not_matter(client, data_dir):
    request = load_json(data_dir / FIXTURE)
    curves = request["pricing"]["rates"]["curves"]
    assert curves[0]["id"] == FX_CURVE, "fixture lists the FX curve first on purpose"
    first = client.price("bootstrap_curves", request)
    swapped = copy.deepcopy(request)
    swapped["pricing"]["rates"]["curves"] = list(reversed(curves))
    second = client.price("bootstrap_curves", swapped)
    assert json.dumps(_series(first), sort_keys=True) == \
        json.dumps(_series(second), sort_keys=True)


def test_both_currencies_of_the_pair_are_valid_collateral(client, data_dir):
    """collateral_currency may be either side of the pair; which side it is
    decides QuantLib's isFxBaseCurrencyCollateralCurrency flag, so the two
    spellings build different curves and both are well-formed requests."""
    request = load_json(data_dir / FIXTURE)
    usd_coll = _series(client.price("bootstrap_curves", request))["DF"]
    eur_coll_request = _mutated(request, lambda r: [
        p["point"].__setitem__("collateral_currency", "EUR") for p in _fx_curve(r)["points"]])
    eur_coll = _series(client.price("bootstrap_curves", eur_coll_request))["DF"]
    assert usd_coll != eur_coll


# ---------------------------------------------------------------------------
# request contract: FxSpotSpec
# ---------------------------------------------------------------------------

def _spot_set(field, value):
    return lambda r: _spot(r).__setitem__(field, value)


def _spot_del(field):
    return lambda r: _spot(r).pop(field)


def _spot_quote(quote_type):
    def f(r):
        _spot(r).pop("rate")
        _spot(r)["quote_id"] = "EURUSD"
        r["pricing"]["quotes"] = [{"id": "EURUSD", "kind": "FxSpot", "value": 1.085,
                                   "quote_type": quote_type}]
    return f


def _spot_duplicate(r):
    r["pricing"]["fx"]["spots"].append(copy.deepcopy(_spot(r)))


SPOT_SCENARIOS = [
    ("spot id missing", _spot_del("id"), 400, "id in FxSpotSpec"),
    ("base == quote", _spot_set("quote_currency", "EUR"), 400, "must differ"),
    ("lowercase currency", _spot_set("base_currency", "eur"), 400,
     "three-letter uppercase ISO currency code"),
    ("pair string instead of a code", _spot_set("base_currency", "EURUSD"), 400,
     "FxSpotSpec.base_currency"),
    ("rate and quote_id both absent", _spot_del("rate"), 400,
     "exactly one of rate or quote_id"),
    ("rate and quote_id both present", lambda r: (
        _spot(r).__setitem__("quote_id", "EURUSD"),
        r["pricing"].__setitem__("quotes", [{"id": "EURUSD", "value": 1.085,
                                             "quote_type": "Fx"}])), 400,
     "exactly one of rate or quote_id"),
    ("rate zero", _spot_set("rate", 0.0), 400, "FxSpotSpec.rate must be positive"),
    ("rate negative", _spot_set("rate", -1.085), 400, "FxSpotSpec.rate must be positive"),
    ("quote_id with Curve quote type", _spot_quote("Curve"), 400, "quote_type Fx"),
    ("quote_id unknown", lambda r: (_spot(r).pop("rate"),
                                    _spot(r).__setitem__("quote_id", "NO_SUCH_QUOTE")), 404,
     "NO_SUCH_QUOTE"),
    ("spot_days absent", _spot_del("spot_days"), 400, "FxSpotSpec.spot_days is required"),
    ("spot_days negative", _spot_set("spot_days", -1), 400,
     "FxSpotSpec.spot_days must be non-negative"),
    ("settlement_calendars empty", _spot_set("settlement_calendars", []), 400,
     "FxSpotSpec.settlement_calendars"),
    ("business_day_convention absent", _spot_del("business_day_convention"), 400,
     "FxSpotSpec.business_day_convention is required"),
    ("duplicate spot id", _spot_duplicate, 400, "duplicate FX spot id"),
]


@pytest.mark.parametrize("scenario", SPOT_SCENARIOS, ids=[s[0] for s in SPOT_SCENARIOS])
def test_fx_spot_validation(client, data_dir, scenario):
    label, mutate, expected, fragment = scenario
    request = _mutated(load_json(data_dir / FIXTURE), mutate)
    status, body = _post(client, request)
    assert status == expected, f"{label}: expected HTTP {expected}, got {status} :: {body[:300]}"
    assert fragment in body, f"{label}: {fragment!r} not in {body[:300]}"


def test_fx_spot_quote_id_with_fx_type_is_accepted(client, data_dir):
    request = _mutated(load_json(data_dir / FIXTURE), _spot_quote("Fx"))
    status, body = _post(client, request)
    assert status == 200, body


def test_settlement_calendars_are_joined(client, data_dir):
    """A US-only holiday must move an FX settlement date even though the EUR
    side is open: dropping the US calendar from the spot changes the curve."""
    request = load_json(data_dir / FIXTURE)
    assert _spot(request)["settlement_calendars"] == ["TARGET", "UnitedStatesSettlement"]
    joint = _series(client.price("bootstrap_curves", request))["DF"]
    target_only = _mutated(request, _spot_set("settlement_calendars", ["TARGET"]))
    alone = _series(client.price("bootstrap_curves", target_only))["DF"]
    assert joint != alone


# ---------------------------------------------------------------------------
# request contract: FxSwapHelper
# ---------------------------------------------------------------------------

def _pillar_set(field, value, index=0):
    return lambda r: _fx_curve(r)["points"][index]["point"].__setitem__(field, value)


def _pillar_del(field, index=0):
    return lambda r: _fx_curve(r)["points"][index]["point"].pop(field)


HELPER_SCENARIOS = [
    ("fx_points and quote_id both absent", _pillar_del("fx_points"), 400,
     "exactly one of fx_points or quote_id"),
    ("fx_points and quote_id both present", lambda r: (
        _pillar_set("quote_id", "PTS")(r),
        r["pricing"].__setitem__("quotes", [{"id": "PTS", "value": 0.001}])), 400,
     "exactly one of fx_points or quote_id"),
    ("quote_id unknown", lambda r: (_pillar_del("fx_points")(r),
                                    _pillar_set("quote_id", "NO_SUCH_QUOTE")(r)), 404,
     "NO_SUCH_QUOTE"),
    ("tenor absent", _pillar_del("tenor"), 400, "tenor"),
    ("fx_spot unknown", _pillar_set("fx_spot", "GBPUSD_SPOT"), 404,
     "FX spot not found: GBPUSD_SPOT"),
    ("fx_spot absent", _pillar_del("fx_spot"), 400, "fx_spot"),
    ("end_of_month absent", _pillar_del("end_of_month"), 400,
     "FxSwapHelper.end_of_month is required"),
    ("collateral_currency not in the pair", _pillar_set("collateral_currency", "GBP"), 400,
     "neither currency of FX spot"),
    ("collateral_currency absent", _pillar_del("collateral_currency"), 400,
     "collateral_currency"),
    ("collateral_curve unknown", _pillar_set("collateral_curve", {"id": "NO_SUCH_CURVE"}),
     404, "NO_SUCH_CURVE"),
    ("collateral_curve is the curve itself",
     _pillar_set("collateral_curve", {"id": FX_CURVE}), 400,
     "is the curve being bootstrapped"),
    ("collateral_curve absent", _pillar_del("collateral_curve"), 400, "collateral_curve"),
    ("FX pillar on an InterpolatedZero curve",
     lambda r: _fx_curve(r).__setitem__("bootstrap_trait", "InterpolatedZero"), 400,
     "received a FxSwapHelper"),
]


@pytest.mark.parametrize("scenario", HELPER_SCENARIOS, ids=[s[0] for s in HELPER_SCENARIOS])
def test_fx_swap_helper_validation(client, data_dir, scenario):
    label, mutate, expected, fragment = scenario
    request = _mutated(load_json(data_dir / FIXTURE), mutate)
    status, body = _post(client, request)
    assert status == expected, f"{label}: expected HTTP {expected}, got {status} :: {body[:300]}"
    assert fragment in body, f"{label}: {fragment!r} not in {body[:300]}"


def test_fx_spot_missing_entirely_is_404(client, data_dir):
    request = load_json(data_dir / FIXTURE)
    del request["pricing"]["fx"]
    status, body = _post(client, request)
    assert status == 404, body
    assert "FX spot not found: EURUSD_SPOT" in body


def test_cross_currency_basis_helper_is_still_501(client, data_dir):
    request = load_json(data_dir / FIXTURE)
    _fx_curve(request)["points"].append({
        "point_type": "CrossCcyBasisHelper",
        "point": {
            "spread": -0.0015,
            "tenor": {"n": 2, "unit": "Years"},
            "fx_spot": "EURUSD_SPOT",
            "base_index": {"id": "EUR_ESTR"},
            "quote_index": {"id": "USD_SOFR"},
            "quote_discount_curve": {"id": USD_CURVE},
            "spread_on_base_leg": True,
            "payment_lag": 0,
            "calendar": "TARGET",
            "business_day_convention": "ModifiedFollowing",
            "end_of_month": False,
            "mtm_reset": False,
        },
    })
    status, body = _post(client, request)
    assert status == 501, body
    assert "CrossCcyBasisHelper" in body and "not implemented" in body
