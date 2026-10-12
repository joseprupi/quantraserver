"""Cross-currency basis helper: parity and request contract.

`CrossCcyBasisHelper` pillars bootstrap the discount curve of one currency of
a pair from constant-notional cross-currency basis swaps, given the other
currency's discount curve, both legs' forwarding curves and the spot (which
names the pair and gives the settlement lag; its rate cancels from a
constant-notional par condition). The fixture is the classic USD-collateralised
EUR discount curve (examples/data/curves/curves_eur_usd_collateral_full.json):
short end from EUR/USD FX swap points (1W..1Y), long end from EUR/USD basis
swaps 2Y/3Y/5Y/10Y (ESTR vs SOFR, compounded quarterly legs, negative basis on
the EUR leg), with a EUR ESTR OIS curve and a USD SOFR OIS curve in the same
request. Its twin spells every FX point, basis spread and the spot through
pricing.quotes.

  * parity: the sampled discount factors and zero rates equal an
    independently built QuantLib curve (ql.FxSwapRateHelper +
    ql.ConstNotionalCrossCurrencyBasisSwapRateHelper on the same inputs) to
    1e-12, and the reported pillar dates are the QuantLib helpers' own;
  * the quote_id twin is bit-identical to the inline fixture;
  * wire order does not matter: the collateral-adjusted curve is listed first
    and the engine builds the two OIS curves before it;
  * the basis side matters (spread on the quote leg is a different curve);
  * the spot rate does not change a basis-only bootstrap (constant notional);
  * every CrossCcyBasisHelper validation is a 400 naming the field and every
    unknown reference is a 404.
"""

import copy
import json

import pytest

from ql_reference import bootstrap_curves_ql, curve_pillar_dates_ql, load_json

FIXTURE = "curves/curves_eur_usd_collateral_full.json"
TWIN = "curves/curves_eur_usd_collateral_full_quote_id.json"
PARITY_TOLERANCE = 1e-12
CURVE = "EUR_USD_COLLATERAL"
USD_CURVE = "USD_SOFR_OIS"
EUR_CURVE = "EUR_ESTR_OIS"


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _post(client, request):
    r = client.session.post(f"{client.base_url}/{client.ENDPOINTS['bootstrap_curves']}",
                            json=request)
    return r.status_code, r.text


def _result(response):
    results = response["results"]
    assert len(results) == 1, response
    entry = results[0]
    assert not entry.get("error"), entry["error"]
    return entry


def _series(response):
    """{measure: [values]} of the single sampled curve."""
    return {s.get("measure", "DF"): s["values"] for s in _result(response)["series"]}


def _curve(request, curve_id=CURVE):
    return next(c for c in request["pricing"]["rates"]["curves"] if c["id"] == curve_id)


def _xccy_points(request):
    return [p for p in _curve(request)["points"] if p["point_type"] == "CrossCcyBasisHelper"]


def _spot(request):
    return request["pricing"]["fx"]["spots"][0]


def _mutated(request, mutate):
    out = copy.deepcopy(request)
    mutate(out)
    return out


def _set_all(field, value):
    """Set `field` on every basis pillar."""
    def f(r):
        for p in _xccy_points(r):
            p["point"][field] = value
    return f


def _del_all(field):
    def f(r):
        for p in _xccy_points(r):
            p["point"].pop(field, None)
    return f


def _basis_only(r):
    """Keep only the basis pillars (the short end is then the 2Y helper's own
    bootstrap); the spot cannot enter such a curve."""
    _curve(r)["points"] = _xccy_points(r)
    r["queries"][0]["grid"]["grid"]["tenors"] = [
        t for t in r["queries"][0]["grid"]["grid"]["tenors"]
        if t["unit"] == "Years" and t["n"] >= 2]


# ---------------------------------------------------------------------------
# parity
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("fixture", [FIXTURE, TWIN], ids=["inline", "quote_id"])
def test_collateral_curve_matches_quantlib(client, data_dir, fixture):
    request = load_json(data_dir / fixture)
    response = client.price("bootstrap_curves", request)
    api = _series(response)
    ql_values = bootstrap_curves_ql(request)
    assert set(api) == set(ql_values) == {"DF", "ZERO"}
    worst = 0.0
    for measure, expected in ql_values.items():
        assert len(api[measure]) == len(expected)
        for a, b in zip(api[measure], expected):
            worst = max(worst, abs(a - b))
            assert a == pytest.approx(b, abs=PARITY_TOLERANCE), (measure, a, b)
    print(f"[xccy] {fixture}: max |api - QuantLib| = {worst:.3e} over DF+ZERO")
    # The reported pillar dates are the helpers' own: FX swap pillars settle
    # T+2 on the joint calendar, basis pillars T+2 on the helper calendar
    # with the schedule end adjusted ModifiedFollowing.
    assert _result(response)["pillar_dates"] == curve_pillar_dates_ql(request)


def test_quote_id_twin_is_bit_identical_to_inline(client, data_dir):
    inline = client.price("bootstrap_curves", load_json(data_dir / FIXTURE))
    twin = client.price("bootstrap_curves", load_json(data_dir / TWIN))
    assert json.dumps(_series(inline), sort_keys=True) == \
        json.dumps(_series(twin), sort_keys=True)
    assert inline["results"][0]["grid_dates"] == twin["results"][0]["grid_dates"]
    assert inline["results"][0]["pillar_dates"] == twin["results"][0]["pillar_dates"]


def test_curve_order_on_the_wire_does_not_matter(client, data_dir):
    request = load_json(data_dir / FIXTURE)
    curves = request["pricing"]["rates"]["curves"]
    assert [c["id"] for c in curves] == [CURVE, EUR_CURVE, USD_CURVE], \
        "fixture lists the dependent curve first on purpose"
    first = _series(client.price("bootstrap_curves", request))
    for order in ([USD_CURVE, EUR_CURVE, CURVE], [EUR_CURVE, CURVE, USD_CURVE]):
        permuted = copy.deepcopy(request)
        permuted["pricing"]["rates"]["curves"] = [_curve(request, cid) for cid in order]
        assert _series(client.price("bootstrap_curves", permuted)) == first, order


def test_basis_side_changes_the_curve(client, data_dir):
    """spread_on_base_leg decides which leg carries the basis; the two
    spellings are both well-formed and build different curves."""
    request = load_json(data_dir / FIXTURE)
    on_base = _series(client.price("bootstrap_curves", request))["DF"]
    on_quote_request = _mutated(request, _set_all("spread_on_base_leg", False))
    on_quote = _series(client.price("bootstrap_curves", on_quote_request))["DF"]
    assert on_base[:5] == on_quote[:5], "the FX-swap short end is untouched"
    assert on_base[-1] != on_quote[-1]
    # ... and the flipped request still matches QuantLib.
    for a, b in zip(on_quote, bootstrap_curves_ql(on_quote_request)["DF"]):
        assert a == pytest.approx(b, abs=PARITY_TOLERANCE)


def test_payment_lag_moves_the_pillars_and_matches_quantlib(client, data_dir):
    request = _mutated(load_json(data_dir / FIXTURE), _set_all("payment_lag", 2))
    response = client.price("bootstrap_curves", request)
    assert _result(response)["pillar_dates"] == curve_pillar_dates_ql(request)
    plain = client.price("bootstrap_curves", load_json(data_dir / FIXTURE))
    assert _result(response)["pillar_dates"] != _result(plain)["pillar_dates"]
    for a, b in zip(_series(response)["DF"], bootstrap_curves_ql(request)["DF"]):
        assert a == pytest.approx(b, abs=PARITY_TOLERANCE)


def test_spot_rate_does_not_enter_a_basis_only_curve(client, data_dir):
    """Constant notional: the spot cancels from the par condition. A curve
    built from basis pillars alone is identical under any spot rate (the
    full fixture is not, through its FX-swap short end)."""
    base = _mutated(load_json(data_dir / FIXTURE), _basis_only)
    moved = _mutated(base, lambda r: _spot(r).__setitem__("rate", 1.25))
    assert _series(client.price("bootstrap_curves", base)) == \
        _series(client.price("bootstrap_curves", moved))
    full = load_json(data_dir / FIXTURE)
    full_moved = _mutated(full, lambda r: _spot(r).__setitem__("rate", 1.25))
    assert _series(client.price("bootstrap_curves", full))["DF"] != \
        _series(client.price("bootstrap_curves", full_moved))["DF"]


def test_eur_collateral_builds_the_usd_curve_instead(client, data_dir):
    """Flipping the collateral side (base_discount_curve given, quote absent)
    makes the same pillars bootstrap a EUR-collateralised USD discount curve;
    the forwarding curves stay exogenous."""
    request = load_json(data_dir / FIXTURE)
    _basis_only(request)
    curve = _curve(request)
    curve["id"] = "USD_EUR_COLLATERAL"
    for p in curve["points"]:
        p["point"].pop("quote_discount_curve")
        p["point"]["base_discount_curve"] = {"id": EUR_CURVE}
    request["queries"][0]["curve_id"] = "USD_EUR_COLLATERAL"
    api = _series(client.price("bootstrap_curves", request))["DF"]
    for a, b in zip(api, bootstrap_curves_ql(request)["DF"]):
        assert a == pytest.approx(b, abs=PARITY_TOLERANCE)
    usd = copy.deepcopy(request)
    usd["queries"][0]["curve_id"] = USD_CURVE
    assert api != _series(client.price("bootstrap_curves", usd))["DF"]


# ---------------------------------------------------------------------------
# request contract
# ---------------------------------------------------------------------------

def _quotes(r, *specs):
    r["pricing"]["quotes"] = list(specs)


SCENARIOS = [
    ("spread and quote_id both absent", _del_all("spread"), 400,
     "exactly one of spread or quote_id"),
    ("spread and quote_id both present", lambda r: (
        _set_all("quote_id", "BASIS")(r),
        _quotes(r, {"id": "BASIS", "value": -0.001, "quote_type": "Curve"})), 400,
     "exactly one of spread or quote_id"),
    ("quote_id unknown", lambda r: (_del_all("spread")(r),
                                    _set_all("quote_id", "NO_SUCH_QUOTE")(r)), 404,
     "NO_SUCH_QUOTE"),
    ("tenor absent", _del_all("tenor"), 400, "tenor"),
    ("fx_spot unknown", _set_all("fx_spot", "GBPUSD_SPOT"), 404,
     "FX spot not found: GBPUSD_SPOT"),
    ("fx_spot absent", _del_all("fx_spot"), 400, "fx_spot"),
    ("base_index in the wrong currency", _set_all("base_index", {"id": "USD_SOFR"}), 400,
     "CrossCcyBasisHelper.base_index 'USD_SOFR' is in USD"),
    ("quote_index in the wrong currency", _set_all("quote_index", {"id": "EUR_ESTR"}), 400,
     "CrossCcyBasisHelper.quote_index 'EUR_ESTR' is in EUR"),
    ("base_index unknown", _set_all("base_index", {"id": "EUR_6M"}), 404, "EUR_6M"),
    ("base_forwarding_curve unknown",
     _set_all("base_forwarding_curve", {"id": "NO_SUCH_CURVE"}), 404, "NO_SUCH_CURVE"),
    ("base_forwarding_curve absent", _del_all("base_forwarding_curve"), 400,
     "base_forwarding_curve"),
    ("base_forwarding_curve is the curve itself",
     _set_all("base_forwarding_curve", {"id": CURVE}), 400,
     "CrossCcyBasisHelper.base_forwarding_curve 'EUR_USD_COLLATERAL' is the curve being "
     "bootstrapped"),
    ("quote_forwarding_curve is the curve itself",
     _set_all("quote_forwarding_curve", {"id": CURVE}), 400,
     "quote_forwarding_curve"),
    ("quote_discount_curve is the curve itself",
     _set_all("quote_discount_curve", {"id": CURVE}), 400,
     "CrossCcyBasisHelper.quote_discount_curve 'EUR_USD_COLLATERAL' is the curve being "
     "bootstrapped"),
    ("quote_discount_curve unknown", _set_all("quote_discount_curve", {"id": "NO_SUCH_CURVE"}),
     404, "NO_SUCH_CURVE"),
    ("both discount curves", _set_all("base_discount_curve", {"id": EUR_CURVE}), 400,
     "exactly one of base_discount_curve or quote_discount_curve"),
    ("neither discount curve", _del_all("quote_discount_curve"), 400,
     "exactly one of base_discount_curve or quote_discount_curve"),
    ("spread_on_base_leg absent", _del_all("spread_on_base_leg"), 400,
     "CrossCcyBasisHelper.spread_on_base_leg is required"),
    ("payment_frequency absent with overnight indices", _del_all("payment_frequency"), 400,
     "CrossCcyBasisHelper.payment_frequency is required"),
    ("payment_frequency not periodic", _set_all("payment_frequency", "Once"), 400,
     "CrossCcyBasisHelper.payment_frequency must be a periodic frequency"),
    ("payment_lag absent", _del_all("payment_lag"), 400, "CrossCcyBasisHelper.payment_lag"),
    ("payment_lag negative", _set_all("payment_lag", -1), 400,
     "CrossCcyBasisHelper.payment_lag"),
    ("calendar absent", _del_all("calendar"), 400, "CrossCcyBasisHelper.calendar is required"),
    ("business_day_convention absent", _del_all("business_day_convention"), 400,
     "CrossCcyBasisHelper.business_day_convention is required"),
    ("end_of_month absent", _del_all("end_of_month"), 400,
     "CrossCcyBasisHelper.end_of_month is required"),
    ("mtm_reset absent", _del_all("mtm_reset"), 400,
     "CrossCcyBasisHelper.mtm_reset is required"),
    ("mtm_reset true", _set_all("mtm_reset", True), 400, "not supported in this version"),
    ("basis pillar on an InterpolatedZero curve",
     lambda r: _curve(r).__setitem__("bootstrap_trait", "InterpolatedZero"), 400,
     "received a FxSwapHelper"),
]


@pytest.mark.parametrize("scenario", SCENARIOS, ids=[s[0] for s in SCENARIOS])
def test_cross_ccy_basis_helper_validation(client, data_dir, scenario):
    label, mutate, expected, fragment = scenario
    request = _mutated(load_json(data_dir / FIXTURE), mutate)
    status, body = _post(client, request)
    assert status == expected, f"{label}: expected HTTP {expected}, got {status} :: {body[:300]}"
    assert fragment in body, f"{label}: {fragment!r} not in {body[:300]}"


def test_payment_frequency_rules_for_ibor_legs(client, data_dir):
    """With two IBOR indices the legs pay at their tenors and a
    payment_frequency is rejected; with one IBOR and one overnight index the
    frequency must equal the IBOR tenor."""
    request = load_json(data_dir / FIXTURE)
    request["pricing"]["rates"]["indices"] += [
        {"id": "EUR_3M", "name": "Euribor", "index_type": "Ibor", "currency": "EUR",
         "tenor": {"n": 3, "unit": "Months"}, "fixing_days": 2, "calendar": "TARGET",
         "day_counter": "Actual360", "business_day_convention": "ModifiedFollowing",
         "end_of_month": False},
        {"id": "USD_3M", "name": "USD3M", "index_type": "Ibor", "currency": "USD",
         "tenor": {"n": 3, "unit": "Months"}, "fixing_days": 2,
         "calendar": "UnitedStatesGovernmentBond", "day_counter": "Actual360",
         "business_day_convention": "ModifiedFollowing", "end_of_month": False},
    ]
    mixed = _mutated(request, _set_all("base_index", {"id": "EUR_3M"}))
    status, body = _post(client, mixed)
    assert status == 200, body  # Quarterly == 3M tenor of EUR_3M
    mismatch = _mutated(mixed, _set_all("payment_frequency", "Semiannual"))
    status, body = _post(client, mismatch)
    assert status == 400 and "does not match the tenor" in body, body

    ibor = _mutated(mixed, _set_all("quote_index", {"id": "USD_3M"}))
    status, body = _post(client, ibor)
    assert status == 400 and "payment_frequency must be absent" in body, body
    ibor_ok = _mutated(ibor, _del_all("payment_frequency"))
    status, body = _post(client, ibor_ok)
    assert status == 200, body
    api = _series(json.loads(body))["DF"]
    for a, b in zip(api, bootstrap_curves_ql(ibor_ok)["DF"]):
        assert a == pytest.approx(b, abs=PARITY_TOLERANCE)


def test_fx_spot_missing_entirely_is_404(client, data_dir):
    request = _mutated(load_json(data_dir / FIXTURE), _basis_only)
    del request["pricing"]["fx"]
    status, body = _post(client, request)
    assert status == 404, body
    assert "FX spot not found: EURUSD_SPOT" in body
