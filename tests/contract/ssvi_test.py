"""eSSVI equity vol surfaces: Python QuantLib parity and calibration recovery.

`BlackVolSpec` shapes `SsviParams` (slices given) and `SsviCalibrate` (slices
fitted from option quotes by `POST /calibrate-equity-vol`).

  * Equity option on `SsviParams`: the server's NPV and greeks against
    `ql_reference.price_equity_option_ql(greeks=True)`, which rebuilds the
    surface in Python (`SsviReferenceSurface`: eSSVI total variance, linear
    (theta, psi, rho psi) in time, both reference-forward modes) and prices the
    analytic European option on the process curves. Pillar, between-slices,
    before-first and after-last expiries, with and without slice forwards.
  * Calibration recovery: option quotes generated in Python from a known eSSVI
    surface (premiums, and implied vols) come back as the generating
    (theta, rho, psi) and forwards; the diagnostics report no violations and
    non-negative scan minima.
  * Round trip: the response slices pasted back as `SsviParams` price like the
    `SsviCalibrate` surface they came from.
  * `/sample-vol-surfaces` on both shapes: sqrt(w / T) of the Python surface.

The calibrator itself has no Python port: the reference surfaces are built from
the generating parameters or from the response slices.

The JSON gateway prints doubles with 12 decimals (absolute 5e-13), so a
quantity read back from a response carries that floor.
"""

import datetime
import math

import pytest

import ql_reference
from ql_reference import (SsviReferenceSurface, build_ssvi_surface,
                          price_equity_option_ql, sample_vol_surfaces_ql)

AS_OF = "2026-10-21"
SPOT = 100.0
RATE = 0.04
DIV_CURVE = 0.015  # the "spx_div" curve; also the underlying's yield

REL_TOL = 1e-9
ABS_TOL = 1e-11  # the gateway prints 12 decimal places

# Total variance of the "good set" (tests/parity/ssvi_test.cpp): ATM vols
# 20 / 19 / 18.5 / 18 % at 91, 182, 365, 730 days, theta = vol^2 T.
GOOD_DAYS = [91, 182, 365, 730]
GOOD_VOLS = [0.20, 0.19, 0.185, 0.18]
GOOD_RHO = [-0.70, -0.68, -0.65, -0.60]
GOOD_PSI = [0.10, 0.14, 0.20, 0.28]


def _iso(days):
    ref = datetime.date.fromisoformat(AS_OF)
    return (ref + datetime.timedelta(days=days)).isoformat()


def _good_slices():
    """[(days, T, theta, rho, psi)] of the good set."""
    return [(d, d / 365.0, v * v * d / 365.0, rho, psi)
            for d, v, rho, psi in zip(GOOD_DAYS, GOOD_VOLS, GOOD_RHO, GOOD_PSI)]


# ---------------------------------------------------------------------------
# Request builders
# ---------------------------------------------------------------------------

def _flat_curve(curve_id, rate):
    return {
        "id": curve_id,
        "day_counter": "Actual365Fixed",
        "interpolator": "Linear",
        "reference_date": AS_OF,
        "bootstrap_trait": "InterpolatedZero",
        "points": [
            {"point_type": "ZeroRatePoint",
             "point": {"zero_rate": rate, "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing",
                       "compounding": "Continuous", "frequency": "Annual",
                       "date": d}}
            for d in (AS_OF, "2036-10-21")
        ],
    }


def _vol_base(shape):
    return {"reference_date": AS_OF, "calendar": "TARGET",
            "business_day_convention": "ModifiedFollowing",
            "day_counter": "Actual365Fixed", "shape": shape}


def _params_vol(vol_id, slices, use_flat_market=False):
    """A shape=SsviParams BlackVolSpec. `slices` are wire dicts. With
    use_flat_market the surface market is the spot value and flat rates
    instead of the quote and the curves."""
    if use_flat_market:
        market = {"spot": SPOT, "use_flat_discount_rate": True,
                  "flat_discount_rate": RATE, "use_flat_dividend_rate": True,
                  "flat_dividend_rate": DIV_CURVE}
    else:
        market = {"spot_quote_id": "EQ_SPOT", "discount_curve_id": "usd_rf",
                  "dividend_curve_id": "spx_div"}
    return {"id": vol_id, "payload_type": "BlackVolSpec", "payload": {
        "base": _vol_base("SsviParams"), **market, "ssvi_slices": slices}}


def _calibrate_vol(vol_id, quotes, fit=None):
    payload = {"base": _vol_base("SsviCalibrate"), "spot_quote_id": "EQ_SPOT",
               "discount_curve_id": "usd_rf", "dividend_curve_id": "spx_div",
               "option_quotes": quotes}
    if fit is not None:
        payload["ssvi_fit"] = fit
    return {"id": vol_id, "payload_type": "BlackVolSpec", "payload": payload}


def _pricing(vols):
    return {
        "as_of_date": AS_OF,
        "quotes": [{"id": "EQ_SPOT", "kind": "Price", "value": SPOT,
                    "quote_type": "Curve"}],
        "rates": {"curves": [_flat_curve("usd_rf", RATE),
                             _flat_curve("spx_div", DIV_CURVE)]},
        "volatility": {
            "vol_surfaces": list(vols),
            "models": [{"id": "bs", "payload_type": "EquityVanillaModelSpec",
                        "payload": {"model_type": "BlackScholesAnalytic"}}],
        },
        "equity": {"equity_underlyings": [
            {"id": "EQ", "currency": "USD", "spot_quote_id": "EQ_SPOT",
             "dividend_yield_curve_id": "spx_div"}]},
    }


def _option_request(vol, strike, days, option_type="Call"):
    return {
        "pricing": _pricing([vol]),
        "options": [{
            "option": {
                "trade_id": "OPT", "underlying_id": "EQ", "quantity": 1.0,
                "settlement": "Physical",
                "payoff_type": "EquityPlainVanillaPayoff",
                "payoff": {"option_type": option_type, "strike": strike},
                "exercise_type": "EquityEuropeanExercise",
                "exercise": {"expiry_date": _iso(days)},
            },
            "discounting_curve": "usd_rf",
            "volatility": vol["id"],
            "model": "bs",
        }],
    }


def _wire_slice(days, theta, rho, psi, forward=None):
    s = {"expiry_date": _iso(days), "theta": theta, "rho": rho, "psi": psi}
    if forward is not None:
        s["forward"] = forward
    return s


def _good_wire_slices(with_forward):
    """The good set as wire slices; forwards at yields different from the
    curve's (1.0 .. 1.75 %) when requested."""
    yields = [0.010, 0.0125, 0.015, 0.0175]
    return [
        _wire_slice(d, theta, rho, psi,
                    SPOT * math.exp((RATE - q) * T) if with_forward else None)
        for (d, T, theta, rho, psi), q in zip(_good_slices(), yields)
    ]


# ---------------------------------------------------------------------------
# Quote generation (Python Black-76 on the generating surface)
# ---------------------------------------------------------------------------

def _black_premium(option_type, strike, forward, std_dev, discount):
    """Discounted Black-76 premium."""
    d1 = (math.log(forward / strike) + 0.5 * std_dev ** 2) / std_dev
    d2 = d1 - std_dev
    cdf = lambda x: 0.5 * math.erfc(-x / math.sqrt(2.0))
    if option_type == "Call":
        return discount * (forward * cdf(d1) - strike * cdf(d2))
    return discount * (strike * cdf(-d2) - forward * cdf(-d1))


def _truth_quotes(forwards, as_prices, multiples=(0.7, 0.8, 0.9, 0.95, 1.0,
                                                  1.05, 1.1, 1.2, 1.3)):
    """Calls and puts at `multiples` of each forward, from the good set, on a
    one-point strike grid: the forwards are not integers, so no strike sits
    exactly at the forward (as with listed strikes)."""
    quotes = []
    for (days, T, theta, rho, psi), forward in zip(_good_slices(), forwards):
        discount = math.exp(-RATE * T)
        for m in multiples:
            strike = float(round(forward * m))
            assert strike != forward
            std_dev = math.sqrt(ql_reference._ssvi_w(
                theta, rho, psi, math.log(strike / forward)))
            for option_type in ("Put", "Call"):
                quote = {"expiry_date": _iso(days), "strike": strike,
                         "option_type": option_type}
                if as_prices:
                    quote["price"] = _black_premium(
                        option_type, strike, forward, std_dev, discount)
                else:
                    quote["implied_vol"] = std_dev / math.sqrt(T)
                quotes.append(quote)
    return quotes


def _curve_forwards():
    return [SPOT * math.exp((RATE - DIV_CURVE) * T) for _, T, *_ in _good_slices()]


def _parity_forwards():
    return [SPOT * math.exp((RATE - q) * T)
            for (_, T, *_), q in zip(_good_slices(), [0.010, 0.0125, 0.015, 0.0175])]


def _calibrate(client, vol):
    request = {"pricing": _pricing([vol]), "vol_id": vol["id"]}
    return client.price("calibrate_equity_vol", request)


def _rel(a, b):
    return abs(a - b) / max(abs(b), 1e-300)


def _close(api, ref):
    return abs(api - ref) <= REL_TOL * abs(ref) + ABS_TOL


# ---------------------------------------------------------------------------
# Equity option on SsviParams: server vs Python reference
# ---------------------------------------------------------------------------

# (strike, days): before the first slice (45), on a pillar (91), between
# slices (140, 250, 500), after the last (900, 1100).
PARITY_POINTS = [(95.0, 45), (100.0, 91), (110.0, 140), (90.0, 250),
                 (100.0, 365), (105.0, 500), (85.0, 900), (120.0, 1100)]


@pytest.mark.parametrize("with_forward", [False, True],
                         ids=["curve_forward", "slice_forwards"])
def test_equity_option_on_ssvi_params_matches_python_reference(client, with_forward):
    vol = _params_vol("ssvi", _good_wire_slices(with_forward),
                      use_flat_market=with_forward)
    worst = 0.0
    for strike, days in PARITY_POINTS:
        for option_type in ("Call", "Put"):
            request = _option_request(vol, strike, days, option_type)
            api = client.price("equity_option", request)["options"][0]
            ref = price_equity_option_ql(request, greeks=True)
            for name, expected in ref.items():
                actual = api.get(name, 0.0)
                worst = max(worst, abs(actual - expected)
                            / max(abs(expected), 1.0))
                assert _close(actual, expected), (
                    f"{option_type} K={strike} {days}d {name}: "
                    f"api={actual!r} ql={expected!r}")
    print(f"[ssvi] forwards={with_forward}: max diff / max(|ql|, 1) = {worst:.3g}")


def test_ssvi_forward_slices_change_the_price(client):
    """The two forward modes are different surfaces on the same slices."""
    plain = _params_vol("ssvi", _good_wire_slices(False))
    fwd = _params_vol("ssvi", _good_wire_slices(True))
    a = client.price("equity_option", _option_request(plain, 100.0, 250))
    b = client.price("equity_option", _option_request(fwd, 100.0, 250))
    assert abs(a["options"][0]["npv"] - b["options"][0]["npv"]) > 1e-3


# ---------------------------------------------------------------------------
# /calibrate-equity-vol: recovery from a known surface
# ---------------------------------------------------------------------------

def _assert_recovers(response, forwards, param_tol=1e-6):
    slices = response["slices"]
    assert len(slices) == len(GOOD_DAYS)
    for s, (days, T, theta, rho, psi), forward in zip(slices, _good_slices(), forwards):
        assert s["expiry_date"] == _iso(days)
        assert abs(s["theta"] - theta) <= param_tol, (days, "theta", s["theta"], theta)
        assert abs(s["rho"] - rho) <= param_tol, (days, "rho", s["rho"], rho)
        assert abs(s["psi"] - psi) <= param_tol, (days, "psi", s["psi"], psi)
        assert _rel(s["forward"], forward) <= 1e-8, (days, "forward", s["forward"], forward)
    diag = response["diagnostics"]
    assert not diag.get("violations")
    assert diag.get("scan_min_g", 0.0) >= 0.0
    assert diag.get("scan_min_w_t", 0.0) >= 0.0
    return max(max(abs(s["theta"] - t[2]), abs(s["rho"] - t[3]), abs(s["psi"] - t[4]))
               for s, t in zip(slices, _good_slices()))


def test_calibration_recovers_surface_from_prices_and_parity_forwards(client):
    forwards = _parity_forwards()  # not the curve's: found by put-call parity
    vol = _calibrate_vol("cal", _truth_quotes(forwards, as_prices=True))
    response = _calibrate(client, vol)
    worst = _assert_recovers(response, forwards)
    print(f"[ssvi] price quotes: max |param error| = {worst:.3g}")
    for d, forward in zip(response["diagnostics"]["slices"], forwards):
        assert _rel(d["pcp_forward"], forward) <= 1e-8


def test_calibration_recovers_surface_to_1e_9_with_no_strike_at_the_forward(client):
    """The anchor is solved exactly (theta reproduces the total variance of
    the quote nearest the forward), so a strike grid that misses the forward
    still recovers the generating parameters tightly."""
    forwards = _parity_forwards()
    quotes = _truth_quotes(forwards, as_prices=True)
    assert all(q["strike"] != f for q in quotes for f in forwards)
    response = _calibrate(client, _calibrate_vol("cal", quotes))
    worst = _assert_recovers(response, forwards, param_tol=1e-9)
    print(f"[ssvi] no strike at the forward: max |param error| = {worst:.3g}")


def test_calibration_recovers_surface_from_implied_vols(client):
    forwards = _curve_forwards()  # vol quotes carry no parity: the curve forward
    vol = _calibrate_vol("cal", _truth_quotes(forwards, as_prices=False))
    response = _calibrate(client, vol)
    worst = _assert_recovers(response, forwards)
    print(f"[ssvi] vol quotes: max |param error| = {worst:.3g}")


def test_calibration_without_dividend_curve_uses_parity_forwards(client):
    forwards = _parity_forwards()
    vol = _calibrate_vol("cal", _truth_quotes(forwards, as_prices=True))
    del vol["payload"]["dividend_curve_id"]
    response = _calibrate(client, vol)
    _assert_recovers(response, forwards)
    for d in response["diagnostics"]["slices"]:
        assert "curve_forward" not in d
        assert "implied_dividend_yield" in d


def test_calibration_without_dividend_curve_needs_a_parity_pair_per_expiry(client):
    forwards = _parity_forwards()
    quotes = [q for q in _truth_quotes(forwards, as_prices=True)
              if not (q["expiry_date"] == _iso(GOOD_DAYS[0]) and q["option_type"] == "Call")]
    vol = _calibrate_vol("cal", quotes)
    del vol["payload"]["dividend_curve_id"]
    r = client.session.post(
        f"{client.base_url}/{client.ENDPOINTS['calibrate_equity_vol']}",
        json={"pricing": _pricing([vol]), "vol_id": "cal"})
    assert r.status_code == 400
    assert "no forward at expiry" in r.text


def _diag_quote(diag, expiry, strike, option_type):
    return next(q for q in diag["quotes"]
                if q["expiry_date"] == expiry and q["strike"] == strike
                and q["option_type"] == option_type)


def test_quote_below_intrinsic_is_excluded_and_the_fit_goes_on(client):
    """One call priced far under its intrinsic value is excluded as
    price_outside_bounds, its partner put as parity_outlier, and the surface
    is still recovered from the other quotes."""
    forwards = _parity_forwards()
    quotes = _truth_quotes(forwards, as_prices=True)
    expiry = _iso(GOOD_DAYS[0])
    bad = next(q for q in quotes if q["expiry_date"] == expiry
               and q["option_type"] == "Call" and q["strike"] < forwards[0])
    bad["price"] = 1e-3
    response = _calibrate(client, _calibrate_vol("cal", quotes))
    diag = response["diagnostics"]
    call = _diag_quote(diag, expiry, bad["strike"], "Call")
    put = _diag_quote(diag, expiry, bad["strike"], "Put")
    assert (call["used"], call["exclusion_reason"]) == (False, "price_outside_bounds")
    assert (put["used"], put["exclusion_reason"]) == (False, "parity_outlier")
    _assert_recovers(response, forwards)
    slice0 = diag["slices"][0]
    assert slice0["parity_pairs_rejected"] == 1
    assert slice0["parity_pairs_used"] >= 1


def test_calibration_diagnostics_list_every_quote_with_its_residual(client):
    forwards = _parity_forwards()
    quotes = _truth_quotes(forwards, as_prices=True)
    response = _calibrate(client, _calibrate_vol("cal", quotes))
    diag = response["diagnostics"]
    assert len(diag["quotes"]) == len(quotes)
    used = [q for q in diag["quotes"] if q["used"]]
    assert used
    assert all(abs(q.get("residual", 0.0)) < 1e-6 for q in used)


# ---------------------------------------------------------------------------
# Calibrate, paste the slices as SsviParams, price on both
# ---------------------------------------------------------------------------

def test_response_slices_pasted_as_ssvi_params_price_like_the_calibrated_surface(client):
    forwards = _parity_forwards()
    cal = _calibrate_vol("ssvi", _truth_quotes(forwards, as_prices=True))
    response = _calibrate(client, cal)
    pasted = _params_vol("ssvi", [
        {k: s[k] for k in ("expiry_date", "theta", "rho", "psi", "forward")}
        for s in response["slices"]])
    worst = 0.0
    for strike, days in [(100.0, 60), (95.0, 91), (100.0, 150), (105.0, 300),
                         (90.0, 700), (110.0, 900)]:
        a = client.price("equity_option", _option_request(cal, strike, days))
        b = client.price("equity_option", _option_request(pasted, strike, days))
        # request rebuilt around each vol: the underlying and curves are the same
        npv_cal, npv_params = a["options"][0]["npv"], b["options"][0]["npv"]
        worst = max(worst, abs(npv_cal - npv_params))
        # the gateway prints the slices with 12 decimals: that floor, not 1e-12
        assert abs(npv_cal - npv_params) <= 1e-9, (strike, days, npv_cal, npv_params)
    print(f"[ssvi] round trip: max |NPV difference| = {worst:.3g}")


# ---------------------------------------------------------------------------
# /sample-vol-surfaces on both shapes
# ---------------------------------------------------------------------------

SAMPLE_TENORS = [(2, "Months"), (4, "Months"), (9, "Months"), (18, "Months"),
                 (3, "Years")]
SAMPLE_STRIKES = [70.0, 90.0, 100.0, 115.0, 140.0]


def _sample_request(vol):
    return {
        "pricing": _pricing([vol]),
        "queries": [{
            "vol_id": vol["id"],
            "surface_type": "EquityBlack",
            "expiry_grid": {"grid_type": "TenorGrid", "grid": {
                "tenors": [{"n": n, "unit": u} for n, u in SAMPLE_TENORS],
                "calendar": "TARGET",
                "business_day_convention": "ModifiedFollowing"}},
            "strike_grid": {"axis": "AbsoluteStrike", "strikes": SAMPLE_STRIKES},
            "output_mode": "Cube",
            "options": {"allow_extrapolation": True, "max_points": 1000,
                        "strict": True},
        }],
    }


def _sampled_vols(client, request):
    response = client.price("sample_vol_surfaces", request)
    entry = response["results"][0]
    assert not entry.get("error"), entry.get("error")
    return entry["vols"]


@pytest.mark.parametrize("with_forward", [False, True],
                         ids=["curve_forward", "slice_forwards"])
def test_sample_vol_surfaces_on_ssvi_params_is_sqrt_w_over_t(client, with_forward):
    vol = _params_vol("ssvi", _good_wire_slices(with_forward))
    request = _sample_request(vol)
    api = _sampled_vols(client, request)
    expected = sample_vol_surfaces_ql(request)
    assert len(api) == len(expected) == len(SAMPLE_TENORS) * len(SAMPLE_STRIKES)
    worst = max(abs(a - b) for a, b in zip(api, expected))
    print(f"[ssvi] sampled SsviParams, forwards={with_forward}: max |diff| = {worst:.3g}")
    assert worst <= 1e-12


def test_sample_vol_surfaces_on_ssvi_calibrate_is_sqrt_w_over_t(client):
    forwards = _parity_forwards()
    cal = _calibrate_vol("ssvi", _truth_quotes(forwards, as_prices=True))
    response = _calibrate(client, cal)
    twin = _params_vol("ssvi", [
        {k: s[k] for k in ("expiry_date", "theta", "rho", "psi", "forward")}
        for s in response["slices"]])
    api = _sampled_vols(client, _sample_request(cal))
    expected = sample_vol_surfaces_ql(_sample_request(twin))
    worst = max(abs(a - b) for a, b in zip(api, expected))
    print(f"[ssvi] sampled SsviCalibrate vs its response slices: max |diff| = {worst:.3g}")
    assert len(api) == len(expected)
    assert worst <= 1e-9


# ---------------------------------------------------------------------------
# The Python surface itself
# ---------------------------------------------------------------------------

def test_reference_surface_hits_the_slices_at_the_pillars():
    """At a pillar the interpolated node is the slice, so w(k = 0) = theta."""
    request = {"pricing": _pricing([_params_vol("ssvi", _good_wire_slices(True))])}
    pricing = ql_reference._reference_pricing_view(request)
    surface = build_ssvi_surface(pricing, "ssvi", request)
    assert isinstance(surface, SsviReferenceSurface)
    for (days, T, theta, rho, psi) in _good_slices():
        forward = surface.forward(T)
        assert surface.variance(T, forward) == pytest.approx(theta, rel=1e-13)
