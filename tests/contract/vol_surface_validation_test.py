"""Surface validation as its own endpoint (POST /validate-vol-surface).

What this suite locks, end to end through the JSON gateway:

  * the numbers moved out of the calibrate and note responses are
    reproduced: on the synthetic Scotia sheets (methods AndreasenHuge,
    SviPerExpiry, Ssvi) the fit of every used quote equals the former
    `vanilla_repricing` of /calibrate-vol-surface, and the local vol scan
    and the eSSVI check equal the former `local_vol_check*` /
    `ssvi_arbitrage` blocks (the 0.16.6 numbers in
    fixtures/vol_surface_validation_0_16_6.json; the surfaces are proven the
    same by the hash of their spec, Andreasen-Huge's within 0.5 vol points
    of the former one since its calibration has a weighted second stage); the
    simulation equals the former
    `diagnostics.vanilla_repricing` of a single-underlying note whose
    observation dates are sheet expiries (same seed, paths and grid);
  * the response on the three payloads of the Scotia NDX sheet: every quote
    inside its bid/ask, band cells adding up, arbitrage free, verdicts;
  * the acceptance criterion of the Monte Carlo repricing (formerly a note
    diagnostic) on the committed Scotia surfaces (grid and SVI payload);
  * a vendor-style constant vol validated against the sheet: mostly outside
    the spread (verdict fail), arbitrage free;
  * the Python twin (validate_vol_surface_ql): the fit to 1e-11 and the
    simulation on the engine's stream;
  * the committed examples (examples/data/vol_surface_validation/) run and
    carry the surfaces the calibrate endpoint returns today;
  * the 400 / 404 rows, each naming the field.
"""

import copy
import hashlib
import json
import math
import sys
from pathlib import Path

import pytest

from ql_reference import _sn_band_at, load_json, validate_vol_surface_ql
from vol_validation import post_validation, validate, validation_request

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "tools"))

import make_synthetic_quote_sheet as gen  # noqa: E402

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "vol_surface_validation_0_16_6.json"
SVI_EXAMPLE = "vol_surface/synthetic_svi_sheet_2026-09-30_svi.json"
NOTE_EXAMPLE = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_surface.json"
NOTE_SVI_EXAMPLE = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_svi.json"
EXAMPLES = "vol_surface_validation"
PRESETS = {"scotia_ndx": gen.SYNTHETIC_SVI_SCOTIA_NDX, "scotia_rty": gen.SYNTHETIC_SVI_SCOTIA_RTY}

# The acceptance criterion of the Monte Carlo repricing: every option of the
# core set (expiries from the second one on, out-of-the-money mid at least
# 0.25 % of the spot) within 3.5 standard errors of its bid/ask band, the
# mean vol error per expiry within 1.5 vol points (1.0 from the sixth expiry
# on). The 16-day expiry and the deep wings are reported, not required.
CORE_MIN_MID_OF_SPOT = 2.5e-3
CORE_Z = 3.5

# The largest move of a used quote's Andreasen-Huge vol against the 0.16.6
# numbers (measured: 4e-5 on the NDX sheet, 1.1e-3 on the 16-day expiry of
# the RTY sheet, which the model does not match).
ANDREASEN_HUGE_VOL_SHIFT = 5e-3


def _post(client, request, product):
    return client.session.post(f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _ok(client, request, product="validate_vol_surface"):
    r = _post(client, request, product)
    assert r.status_code == 200, r.text[:600]
    return r.json()


def _sha(spec):
    return hashlib.sha256(json.dumps(spec, sort_keys=True, separators=(",", ":")).encode()) \
        .hexdigest()


def _close(a, b, rel=1e-10, abs_=1e-12):
    return abs(a - b) <= max(abs_, rel * max(abs(a), abs(b)))


def _fixed_strike(spec, former=None):
    """`spec` as /calibrate-vol-surface returned it before 0.16.10: the same
    rows / slices read at fixed strike (no surface forward); a grid gets the
    former local-vol strike ranges back from the fixture (since 0.16.10
    they are pulled in by the forward's move over each interval)."""
    spec = copy.deepcopy(spec)
    payload = spec["payload"]
    if payload.get("surface_interpolator") == "CubicStrikeLinearTimeForwardMoneyness":
        payload["surface_interpolator"] = "CubicStrikeLinearTime"
        payload.pop("expiry_forwards")
        payload.pop("spot")
        if former is not None:
            lo, hi = former["grid_local_vol_strike_ranges"]
            payload["local_vol_strike_min"], payload["local_vol_strike_max"] = lo, hi
    if payload.get("time_interpolation") == "LinearTotalVarianceForwardMoneyness":
        payload["time_interpolation"] = "LinearTotalVariance"
        payload.pop("spot")
    return spec


def _calibrate(client, preset, method):
    fwd = _ok(client, gen.build_request(preset), "calibrate_implied_forwards")
    req = gen.build_vol_surface_request(preset, method, forwards=fwd["expiries"])
    return req, _ok(client, req, "calibrate_vol_surface")


def _same_verdict(api, former):
    """The scan covers more since 0.16.9 (each expiry itself, the regions
    around an expiry, the clamp's edges): against the 0.16.6 numbers only
    the verdict is compared."""
    assert api["arbitrage_free"] == former["arbitrage_free"]
    assert (api["refused"] > 0) == (former["refused"] > 0)
    assert api["times"] > former["times"] and api["points"] > former["points"]


def _same_check(api, former):
    for key in ("arbitrage_free", "times", "strikes", "points", "refused"):
        assert api[key] == former[key], (key, api[key], former[key])
    assert api.get("first_refusal") == former.get("first_refusal")
    for key in ("min_local_variance", "min_local_variance_time", "min_local_variance_strike"):
        assert (key in api) == (key in former), key
        if key in api:
            # The surface passed through the JSON print (12 decimals): the
            # second derivatives of the local vol carry that, ~1e-8 relative.
            assert _close(api[key], former[key], rel=1e-6), (key, api[key], former[key])


# --------------------------------------------------------------------------
# The former numbers (0.16.6 fixture)
# --------------------------------------------------------------------------

def _former():
    return json.loads(FIXTURE.read_text())


@pytest.mark.parametrize("case", range(6))
def test_reproduces_the_former_calibrate_checks(client, case):
    former = _former()["calibrations"][case]
    req, out = _calibrate(client, PRESETS[former["name"]], former["method"])
    payloads = [p for p in ("grid", "svi", "ssvi") if former["spec_sha256"][p]]
    # Since 0.16.11 the Andreasen-Huge calibration has a second stage on the
    # weighted vol errors (the former one stopped at the fit to the prices):
    # its surface is close to the former one, not equal. The quotes are
    # compared within ANDREASEN_HUGE_VOL_SHIFT below; the verdicts are the
    # former ones.
    andreasen_huge = former["method"] == "AndreasenHuge"
    for p in payloads:
        if p == "ssvi" or andreasen_huge:
            # Since 0.16.9 the eSSVI payload carries no strike range: the
            # slices are compared through the eSSVI check below (to 1e-9).
            continue
        key = {"grid": "vol_surface_spec", "svi": "vol_surface_spec_svi",
               "ssvi": "vol_surface_spec_ssvi"}[p]
        # Since 0.16.10 the payloads are read at fixed forward moneyness;
        # the rows and slices themselves are the former ones.
        assert _sha(_fixed_strike(out[key], former)) == former["spec_sha256"][p], \
            f"{p}: not the 0.16.6 surface"
    # One flag per payload on the calibrate response, the former block's.
    assert out["arbitrage_free"] == former["local_vol_check"]["arbitrage_free"]
    if former["local_vol_check_svi"]:
        assert out["arbitrage_free_svi"] == former["local_vol_check_svi"]["arbitrage_free"]
    if former["local_vol_check_ssvi"]:
        assert out["arbitrage_free_ssvi"] == former["local_vol_check_ssvi"]["arbitrage_free"]

    grid = validate(client, req, out, "grid")
    # The fit: every used quote of the former repricing, the same numbers.
    fit = {(q["expiry"], q["strike"]): q for q in grid["fit"]["quotes"]}
    rows = former["vanilla_repricing"]["quotes"]
    assert len(rows) == former["vanilla_repricing"]["count"]
    for expiry, strike, model_vol, model_price, inside in rows:
        q = fit[(expiry, strike)]
        if andreasen_huge:
            assert abs(q["model_vol"] - model_vol) <= ANDREASEN_HUGE_VOL_SHIFT, \
                (expiry, strike, q["model_vol"], model_vol)
            continue
        assert _close(q["model_vol"], model_vol), (expiry, strike, q["model_vol"], model_vol)
        assert _close(q["model_price"], model_price, rel=1e-9, abs_=1e-9), (expiry, strike)
        assert q["inside_spread"] == inside
    if andreasen_huge:
        assert grid["fit"]["count"] == len(rows)
        assert grid["fit"]["inside"] >= former["vanilla_repricing"]["inside_bid_ask"]
    elif former["method"] != "Ssvi":  # Ssvi left |k| > 1 out of its fit, not of the sheet
        assert grid["fit"]["count"] == len(rows)
        assert grid["fit"]["inside"] == former["vanilla_repricing"]["inside_bid_ask"]
    if former["method"] == "AndreasenHuge":
        # The Andreasen-Huge grid's strike axis is the model's own, not the
        # output range the calibration clamped its scan regions to, so the
        # scan over the spec's own limits is a slightly different set of
        # points; the verdict is the same (refused points, not arbitrage free).
        # Read the former way (fixed strike) or at fixed forward moneyness
        # (since 0.16.10) the grid still refuses, first at the same time as
        # the former one (the first expiry's lower wing).
        check = grid["arbitrage"]["local_vol_check"]
        assert check["arbitrage_free"] is False and former["local_vol_check"]["arbitrage_free"] is False
        assert check["refused"] > 0 and former["local_vol_check"]["refused"] > 0
        fixed = validate(client, req, {**out, "vol_surface_spec": _fixed_strike(
            out["vol_surface_spec"], former)}, "grid", checks={"fit": False, "arbitrage": True})
        assert fixed["arbitrage"]["local_vol_check"]["refused"] > 0
        assert fixed["arbitrage"]["local_vol_check"]["first_refusal"].split(" and time ")[1] == \
            former["local_vol_check"]["first_refusal"].split(" and time ")[1]
    else:
        _same_verdict(grid["arbitrage"]["local_vol_check"], former["local_vol_check"])
    if "svi" in payloads:
        arb = validate(client, req, out, "svi", checks={"fit": False, "arbitrage": True})
        _same_verdict(arb["arbitrage"]["local_vol_check"], former["local_vol_check_svi"])
    if "ssvi" in payloads:
        arb = validate(client, req, out, "ssvi", checks={"fit": False, "arbitrage": True})
        _same_verdict(arb["arbitrage"]["local_vol_check"], former["local_vol_check_ssvi"])
        api, old = arb["arbitrage"]["ssvi_check"], former["ssvi_arbitrage"]
        assert api["violations"] == old["violations"]
        for key in ("scan_min_g", "scan_min_dw_dt"):
            assert _close(api[key], old[key], rel=1e-9), key


def _chain(client, data_dir, method, payload):
    """The former note chain: implied forwards -> surface -> the example's
    market with the implied dividend curve and the returned payload."""
    vs_req = load_json(data_dir / SVI_EXAMPLE)
    fwd_req = {"pricing": vs_req["pricing"], "sheet": vs_req["sheet"],
               "discounting_curve": vs_req["discounting_curve"], "filter": vs_req["filter"],
               "dividend_curve_id": "NDX_SYN_DIV_IMPLIED"}
    fwd = _ok(client, fwd_req, "calibrate_implied_forwards")
    vs_req = copy.deepcopy(vs_req)
    vs_req["forwards"] = fwd["expiries"]
    vs_req["method"] = method
    vs = _ok(client, vs_req, "calibrate_vol_surface")
    pricing = copy.deepcopy(vs_req["pricing"])
    pricing["rates"]["curves"].append(fwd["dividend_curve_spec"])
    und = pricing["equity"]["equity_underlyings"][0]
    und["dividend_yield_curve_id"] = fwd["dividend_curve_spec"]["id"]
    spec = vs[{"grid": "vol_surface_spec", "svi": "vol_surface_spec_svi",
               "ssvi": "vol_surface_spec_ssvi"}[payload]]
    pricing["volatility"] = {"vol_surfaces": [spec]}
    return vs_req, pricing, spec


@pytest.mark.parametrize("case", range(3))
def test_simulation_reproduces_the_former_note_repricing(client, data_dir, case):
    """The former `diagnostics.vanilla_repricing` of a one-underlying note
    whose two observation dates are sheet expiries: same stream, same grid
    (the used expiries), so the same numbers."""
    former = _former()["notes"][case]
    vs_req, pricing, spec = _chain(client, data_dir, former["method"], former["payload"])
    returned = copy.deepcopy(spec)
    if former["payload"] == "ssvi":
        # Since 0.16.9 the eSSVI payload carries no strike range.
        old = copy.deepcopy(spec)
        for sl in old["payload"]["slices"]:
            sl.pop("strike_min", None)
            sl.pop("strike_max", None)
        assert "local_vol_strike_range" not in spec["payload"]
        exact = True
    else:
        # The former surface read the former way (fixed strike) reproduces
        # the former numbers on the same stream; read at fixed forward
        # moneyness (since 0.16.10) the local vol between the expiries
        # differs and the prices land within the noise.
        assert _sha(_fixed_strike(spec, former if case == 0 else None)) == \
            former["spec_sha256"]
        exact = case == 0
        if exact:
            spec = _fixed_strike(spec, former)
            pricing["volatility"] = {"vol_surfaces": [spec]}
    req = {"pricing": pricing, "volatility_id": spec["id"], "underlying_id": "NDX_SYN",
           "discounting_curve": vs_req["discounting_curve"], "quotes": vs_req["sheet"],
           "filter": vs_req["filter"],
           "checks": {"fit": False, "arbitrage": False, "simulation": True},
           "simulation": former["monte_carlo"]}
    out = _ok(client, req)
    assert "fit" not in out and "arbitrage" not in out
    sim = out["simulation"]
    assert sim["paths"] == former["paths"]
    assert sim["time_steps"] == former["time_steps"]
    assert sim["count"] == former["count"]
    for q, (expiry, strike, price, se, model_vol) in zip(sim["quotes"], former["options"]):
        assert (q["expiry"], q["strike"]) == (expiry, strike)
        if not exact:
            # Unclamped since 0.16.9 (eSSVI), read at fixed forward
            # moneyness since 0.16.10 (grid, SVI).
            assert abs(q["mc_price"] - price) <= 3.0 * se + 1e-9, (expiry, strike)
            assert _close(q["standard_error"], se, rel=0.05, abs_=1e-9), (expiry, strike)
            continue
        if former["payload"] == "ssvi":
            # Unclamped since 0.16.9: a path beyond the former strike range
            # reads the eSSVI formula there, so the same stream lands within
            # the noise of the former number, not on it.
            assert abs(q["mc_price"] - price) <= 3.0 * se + 1e-9, (expiry, strike)
            assert _close(q["standard_error"], se, rel=0.05, abs_=1e-9), (expiry, strike)
            continue
        assert _close(q["mc_price"], price, rel=1e-9, abs_=1e-9), (expiry, strike)
        assert _close(q["standard_error"], se, rel=1e-9, abs_=1e-9), (expiry, strike)
        if model_vol is not None:
            assert _close(q["mc_vol"], model_vol, rel=1e-8), (expiry, strike)
    if case == 0:
        # The Python twin on the engine's stream (curve market), on the
        # surface as returned (fixed forward moneyness).
        req["pricing"]["volatility"] = {"vol_surfaces": [returned]}
        sim = _ok(client, req)["simulation"]
        ref = validate_vol_surface_ql(req)
        assert ref["simulation.time_steps"] == sim["time_steps"]
        for n, q in enumerate(sim["quotes"]):
            assert _close(q["mc_price"], ref[f"simulation.quotes.{n}.mc_price"], rel=1e-9,
                          abs_=1e-9), n
            assert _close(q["standard_error"], ref[f"simulation.quotes.{n}.standard_error"],
                          rel=1e-9, abs_=1e-9), n


# --------------------------------------------------------------------------
# The response on the Scotia NDX sheet, three payloads
# --------------------------------------------------------------------------

@pytest.mark.parametrize("method,payload", [("SviPerExpiry", "grid"), ("SviPerExpiry", "svi"),
                                            ("Ssvi", "ssvi")])
def test_scotia_sheet_validates_on_every_payload(client, method, payload):
    req, out = _calibrate(client, gen.SYNTHETIC_SVI_SCOTIA_NDX, method)
    v = validate(client, req, out, payload)
    echo = v["inputs_echo"]
    assert echo["forwards_source"] == "request"
    assert echo["shape"] == {"grid": "SmileCube3D", "svi": "SviSlices", "ssvi": "SsviSlices"}[payload]
    assert echo["moneyness_edges"] == [0.8, 0.9, 0.95, 1.05, 1.1, 1.2]
    assert echo["maturity_edges"] == pytest.approx([1 / 12, 0.25, 0.5, 1.0, 2.0])
    assert echo["dividend_curve"] == "NDX_DIV_PLACEHOLDER"
    fit = v["fit"]
    used = sum(e["quotes_used"] for e in v["expiries"])
    assert fit["count"] == used == len(fit["quotes"]) > 150
    assert fit["inside"] == fit["count"], [
        (q["expiry"], q["strike"]) for q in fit["quotes"] if not q["inside_spread"]]
    # The band cells add up and every cell is inside.
    assert sum(c["count"] for c in fit["bands"]) == fit["count"]
    for c in fit["bands"]:
        assert c["inside"] == c["count"]
        assert c["rms_vol_error"] <= c["max_abs_vol_error"] + 1e-15
    assert len(fit["atm_error_per_expiry"]) == 8
    for a in fit["atm_error_per_expiry"]:
        assert abs(a["vol_error"]) < 5e-3
        assert 0.9 < a["moneyness"] < 1.1
    assert fit["verdict"].startswith("pass"), fit["verdict"]
    arb = v["arbitrage"]
    assert arb["arbitrage_free"] and arb["violations"] == [], arb["verdict"]
    assert arb["verdict"].startswith("pass")
    assert ("svi_slices" in arb) == (payload == "svi")
    assert ("ssvi_slices" in arb) == ("ssvi_check" in arb) == (payload == "ssvi")
    assert "simulation" not in v
    assert v["timing"]["total_milliseconds"] >= v["timing"]["fit_milliseconds"]
    # Custom bands: one cell per maturity bucket around the money.
    v2 = validate(client, req, out, payload,
                  checks={"fit": True, "arbitrage": False},
                  bands={"moneyness": [0.97, 1.03], "maturities": [0.5]})
    assert "arbitrage" not in v2
    assert {(c.get("moneyness_min"), c.get("moneyness_max"), c.get("time_min"), c.get("time_max"))
            for c in v2["fit"]["bands"]} <= {
        (None, 0.97, None, 0.5), (0.97, 1.03, None, 0.5), (1.03, None, None, 0.5),
        (None, 0.97, 0.5, None), (0.97, 1.03, 0.5, None), (1.03, None, 0.5, None)}


def test_vendor_constant_vol_is_mostly_outside_the_spread(client):
    """A vendor-style flat 20 % against the Scotia NDX smile: the wings
    are far from it (fail), the local vol is trivially arbitrage free."""
    req, out = _calibrate(client, gen.SYNTHETIC_SVI_SCOTIA_NDX, "SviPerExpiry")
    request = validation_request(req, out, sheet_forwards=False)
    request["pricing"]["volatility"]["vol_surfaces"] = [{
        "id": "VENDOR_FLAT", "payload_type": "BlackVolSpec",
        "payload": {"base": {"reference_date": "2026-09-30", "calendar": "NullCalendar",
                             "business_day_convention": "Unadjusted",
                             "day_counter": "Actual365Fixed", "shape": "Constant",
                             "constant_vol": 0.20}}}]
    request["volatility_id"] = "VENDOR_FLAT"
    v = _ok(client, request)
    fit = v["fit"]
    assert fit["inside"] * 4 < fit["count"], (fit["inside"], fit["count"])
    assert fit["verdict"].startswith("fail")
    assert v["inputs_echo"]["shape"] == "Constant"
    assert v["arbitrage"]["arbitrage_free"]
    # Twin: the fit numbers.
    ref = validate_vol_surface_ql(request)
    assert ref["fit.count"] == fit["count"] and ref["fit.inside"] == fit["inside"]
    for n, q in enumerate(fit["quotes"]):
        assert _close(q["model_price"], ref[f"fit.quotes.{n}.model_price"], rel=1e-10, abs_=1e-9)


@pytest.mark.parametrize("payload", ["grid", "svi"])
def test_python_twin_reproduces_the_fit(client, payload):
    req, out = _calibrate(client, gen.SYNTHETIC_SVI_SCOTIA_NDX, "SviPerExpiry")
    request = validation_request(req, out, payload)
    v = _ok(client, request)
    ref = validate_vol_surface_ql(request)
    assert ref["fit.count"] == v["fit"]["count"] and ref["fit.inside"] == v["fit"]["inside"]
    worst = 0.0
    for n, q in enumerate(v["fit"]["quotes"]):
        worst = max(worst, abs(q["model_vol"] - ref[f"fit.quotes.{n}.model_vol"]))
        assert _close(q["model_price"], ref[f"fit.quotes.{n}.model_price"], rel=1e-10, abs_=1e-9)
    assert worst < 1e-11, worst
    assert abs(v["fit"]["rms_vol_error"] - ref["fit.rms_vol_error"]) < 1e-11
    for i, e in enumerate(v["expiries"]):
        assert ref[f"expiries.{i}.quotes_used"] == e["quotes_used"]
        assert _close(e["forward"], ref[f"expiries.{i}.forward"], rel=1e-12)


# --------------------------------------------------------------------------
# Acceptance of the Monte Carlo repricing on the committed Scotia surfaces
# --------------------------------------------------------------------------

def _excess_z(q):
    p = q["mc_price"]
    excess = max(q["bid"] - p, p - q["ask"], 0.0)
    if excess == 0.0:
        return 0.0
    return excess / q["standard_error"] if q["standard_error"] > 0.0 else math.inf


@pytest.mark.parametrize("example", [NOTE_EXAMPLE, NOTE_SVI_EXAMPLE])
@pytest.mark.parametrize("underlying", ["NDX", "RTY"])
def test_scotia_surfaces_meet_the_acceptance_criterion(client, data_dir, example, underlying):
    note = load_json(data_dir / example)
    sheet_file = load_json(data_dir / f"vol_surface/synthetic_scotia_{underlying.lower()}_sheet.json")
    mc = dict(note["notes"][0]["monte_carlo"])  # 2048 Sobol paths, 252 steps per year
    req = {"pricing": note["pricing"], "volatility_id": f"{underlying}_VOL",
           "underlying_id": underlying, "discounting_curve": note["notes"][0]["discounting_curve"],
           "quotes": sheet_file["sheet"],
           "checks": {"fit": True, "arbitrage": True, "simulation": True}, "simulation": mc}
    v = _ok(client, req)
    assert v["fit"]["inside"] == v["fit"]["count"]
    assert v["arbitrage"]["arbitrage_free"]
    sim = v["simulation"]
    spot = v["inputs_echo"]["spot"]
    first = v["expiries"][0]["expiry"]
    core = separate = 0
    by_expiry = {}
    for q in sim["quotes"]:
        by_expiry.setdefault(q["expiry"], []).append(q)
        if q["expiry"] == first or q["mid"] < CORE_MIN_MID_OF_SPOT * spot:
            separate += 1
            continue
        core += 1
        assert _excess_z(q) <= CORE_Z, (q["expiry"], q["strike"], q["mid"] / spot, _excess_z(q))
    for n, e in enumerate(sorted(by_expiry)):
        if e == first:
            continue
        errs = [q["vol_error"] for q in by_expiry[e] if "vol_error" in q]
        mean = sum(errs) / len(errs)
        assert abs(mean) < (0.010 if n >= 5 else 0.015), (e, mean)
    assert core > 120 and separate > 20, (core, separate)
    assert sum(c["count"] for c in sim["bands"]) == sim["count"]
    assert sim["within_noise"] >= core


# --------------------------------------------------------------------------
# The committed examples
# --------------------------------------------------------------------------

@pytest.mark.parametrize("name,payload,method", [
    ("scotia_ndx_grid.json", "grid", "SviPerExpiry"),
    ("scotia_ndx_svi.json", "svi", "SviPerExpiry"),
    ("scotia_ndx_ssvi.json", "ssvi", "Ssvi"),
])
def test_examples_carry_todays_surfaces_and_run(client, data_dir, name, payload, method):
    example = load_json(data_dir / EXAMPLES / name)
    _req, out = _calibrate(client, gen.SYNTHETIC_SVI_SCOTIA_NDX, method)
    key = {"grid": "vol_surface_spec", "svi": "vol_surface_spec_svi",
           "ssvi": "vol_surface_spec_ssvi"}[payload]
    assert example["pricing"]["volatility"]["vol_surfaces"] == [out[key]]
    assert example["quotes"] == gen.build_request(gen.SYNTHETIC_SVI_SCOTIA_NDX)["sheet"]
    v = _ok(client, example)
    assert v["fit"]["verdict"].startswith("pass") and v["arbitrage"]["arbitrage_free"]
    if "simulation" in example:
        assert v["simulation"]["verdict"].startswith(("pass", "warn")), v["simulation"]["verdict"]


def test_constant_vol_example(client, data_dir):
    v = _ok(client, load_json(data_dir / EXAMPLES / "scotia_ndx_vendor_constant_vol.json"))
    assert v["fit"]["verdict"].startswith("fail")


# --------------------------------------------------------------------------
# Errors
# --------------------------------------------------------------------------

def _base(client, data_dir):
    return load_json(data_dir / EXAMPLES / "scotia_ndx_grid.json")


def _status(client, request, status, *needles):
    r = post_validation(client, request)
    assert r.status_code == status, (r.status_code, r.text[:600])
    for n in needles:
        assert n in r.text, (n, r.text[:600])


def test_missing_surface_is_404(client, data_dir):
    request = _base(client, data_dir)
    request["volatility_id"] = "NOPE_VOL"
    _status(client, request, 404, "NOPE_VOL")


def test_wrong_shape_is_400(client, data_dir):
    """An interest-rate vol id is not an equity Black vol."""
    request = _base(client, data_dir)
    cap = load_json(data_dir / "cap_floor/cap_eur_5y_itm_strike_2pct.json")
    ir = next(s for s in cap["pricing"]["volatility"]["vol_surfaces"]
              if s["payload_type"] == "OptionletVolSpec")
    request["pricing"]["volatility"]["vol_surfaces"].append(ir)
    request["volatility_id"] = ir["id"]
    _status(client, request, 400, "volatility_id", "equity Black vol")


def test_simulation_without_its_spec_is_400(client, data_dir):
    request = _base(client, data_dir)
    request["checks"] = {"simulation": True}
    request.pop("simulation", None)
    _status(client, request, 400, "simulation: required")
    request = _base(client, data_dir)
    request["checks"] = {"simulation": False}
    request["simulation"] = {"paths": 1024, "steps_per_year": 52}
    _status(client, request, 400, "simulation: must be absent")
    request["checks"] = {"simulation": True}
    request["simulation"] = {"paths": 1024}
    _status(client, request, 400, "simulation.steps_per_year: required")
    request["simulation"] = {"paths": 1023, "steps_per_year": 52}
    _status(client, request, 400, "simulation.paths: must be even")


@pytest.mark.parametrize("bands,needle", [
    ({"moneyness": [1.1, 0.9]}, "bands.moneyness must be strictly increasing"),
    ({"maturities": [0.5, 0.25]}, "bands.maturities must be strictly increasing"),
    ({"moneyness": [0.0, 1.0]}, "bands.moneyness[0] must be finite and > 0"),
])
def test_unsorted_bands_are_400(client, data_dir, bands, needle):
    request = _base(client, data_dir)
    request["bands"] = bands
    _status(client, request, 400, needle)


def test_sheet_of_another_underlying_is_400(client, data_dir):
    request = _base(client, data_dir)
    request["quotes"]["underlying_id"] = "RTY"
    _status(client, request, 400, "quotes.underlying_id 'RTY' must equal underlying_id 'NDX'")


def test_other_rows_are_400(client, data_dir):
    request = _base(client, data_dir)
    request["checks"] = {"fit": False, "arbitrage": False}
    _status(client, request, 400, "checks: at least one")
    request = _base(client, data_dir)
    request["quotes"]["option_style"] = "American"
    _status(client, request, 400, "quotes.option_style must be European")
    request = _base(client, data_dir)
    del request["volatility_id"]
    _status(client, request, 400, "volatility_id")
    request = _base(client, data_dir)
    request["discounting_curve"] = "NOPE_CURVE"
    _status(client, request, 404, "NOPE_CURVE")


def test_the_note_diagnostic_is_gone(client, data_dir):
    """`diagnostics.vanilla_repricing` (0.16.6) is no longer part of the
    note request: the JSON parser refuses the field."""
    note = load_json(data_dir / NOTE_EXAMPLE)
    note["diagnostics"] = {"vanilla_repricing": True}
    r = _post(client, note, "structured_note")
    assert r.status_code == 400, (r.status_code, r.text[:400])
    assert "vanilla_repricing" in r.text, r.text[:400]


# --------------------------------------------------------------------------
# bid_vol / ask_vol, the scanned grid, the clamp edges
# --------------------------------------------------------------------------

def test_bid_and_ask_vols(client, data_dir):
    request = load_json(data_dir / EXAMPLES / "scotia_ndx_grid.json")
    # A zero bid on an out-of-the-money put (kept by a wide spread filter):
    # it has no implied vol, so bid_vol is absent, never 0.
    request["filter"] = {"max_relative_spread": 10.0, "min_quotes_per_expiry": 4}
    line = next(q for q in request["quotes"]["quotes"]
                if q["expiry"] == "2027-06-18" and q["strike"] < 0.8 * 30408.5)
    line["put_bid"] = 0.0
    v = _ok(client, request)
    quotes = v["fit"]["quotes"]
    zero = next(q for q in quotes if q["expiry"] == "2027-06-18" and q["strike"] == line["strike"])
    assert zero["bid"] == 0.0 and "bid_vol" not in zero and zero["ask_vol"] > 0.0
    for q in quotes:
        assert q.get("bid_vol", 1.0) > 0.0 and q.get("ask_vol", 1.0) > 0.0
        if "bid_vol" in q and "ask_vol" in q:
            assert q["bid_vol"] <= q["market_vol"] <= q["ask_vol"], q
    # The Python twin: the same vols where they solve, absent where not.
    ref = validate_vol_surface_ql(request)
    for n, q in enumerate(quotes):
        for side in ("bid_vol", "ask_vol"):
            key = f"fit.quotes.{n}.{side}"
            assert (key in ref) == (side in q), (n, side)
            if side in q:
                assert abs(q[side] - ref[key]) < 1e-11, (n, side)


def _grid_check(arb):
    check, grid = arb["local_vol_check"], arb["local_vol_grid"]
    rows = grid["rows"]
    assert len(rows) == check["times"]
    assert all(a["time"] < b["time"] for a, b in zip(rows, rows[1:]))
    assert sum(len(r["strikes"]) for r in rows) == check["points"]
    assert sum(len(r["refused"]) for r in rows) == check["refused"]
    smallest = None
    for r in rows:
        # 201 scan strikes, plus up to 6 points at the clamp's edges.
        assert len(r["strikes"]) == len(r["local_vols"])
        assert check["strikes"] <= len(r["strikes"]) <= check["strikes"] + 6
        assert all(a < b for a, b in zip(r["strikes"], r["strikes"][1:]))
        refused = set(r["refused"])
        for j, lv in enumerate(r["local_vols"]):
            if j in refused:
                assert lv == 0.0
                continue
            assert lv > 0.0
            if smallest is None or lv * lv < smallest[0]:
                smallest = (lv * lv, r["time"], r["strikes"][j])
    if "min_local_variance" in check:
        assert _close(smallest[0], check["min_local_variance"], rel=1e-9), (smallest, check)
        assert _close(smallest[1], check["min_local_variance_time"], rel=1e-12)
        assert _close(smallest[2], check["min_local_variance_strike"], rel=1e-12)


@pytest.mark.parametrize("name", ["scotia_ndx_grid.json", "scotia_ndx_svi.json",
                                  "scotia_ndx_ssvi.json"])
def test_arbitrage_grid_is_the_scan(client, data_dir, name):
    request = load_json(data_dir / EXAMPLES / name)
    request.pop("simulation", None)
    request["checks"] = {"fit": False, "arbitrage": True, "arbitrage_grid": True}
    v = _ok(client, request)
    assert v["inputs_echo"]["checks"]["arbitrage_grid"] is True
    _grid_check(v["arbitrage"])
    request["checks"]["arbitrage_grid"] = False
    assert "local_vol_grid" not in _ok(client, request)["arbitrage"]
    request["checks"] = {"fit": True, "arbitrage": False, "arbitrage_grid": True}
    _status(client, request, 400, "checks.arbitrage_grid: requires checks.arbitrage")


def test_arbitrage_grid_carries_the_refusals(client):
    """The Andreasen-Huge grid of the Scotia NDX sheet is refused at some
    scan points: the grid lists them and the smallest local variance is
    taken over the others."""
    req, out = _calibrate(client, gen.SYNTHETIC_SVI_SCOTIA_NDX, "AndreasenHuge")
    v = validate(client, req, out, checks={"fit": False, "arbitrage": True,
                                           "arbitrage_grid": True})
    assert v["arbitrage"]["local_vol_check"]["refused"] > 0
    _grid_check(v["arbitrage"])


def test_clamp_edges(client, data_dir):
    m = math.exp(1e-3)
    grid = load_json(data_dir / EXAMPLES / "scotia_ndx_grid.json")
    spec = grid["pricing"]["volatility"]["vol_surfaces"][0]["payload"]
    edges = _ok(client, grid)["arbitrage"]["clamp_edges"]
    assert [e["scope"] for e in edges] == ["PerExpiry"] * len(spec["expiries"])
    assert [round(e["time"] * 365) for e in edges] == [p["n"] for p in spec["expiries"]]
    # The edges a step at each expiry reads at: its range intersected with
    # the next one's (FlatLocalVol: the last read at
    # T_last - 1e-3), each moved inside by 1e-3.
    bands = [(e["time"], lo * m, hi / m) for e, lo, hi in
             zip(edges, spec["local_vol_strike_min"], spec["local_vol_strike_max"])]
    for i, e in enumerate(edges):
        t = e["time"] - (1e-3 if i == len(edges) - 1 else 0.0)
        lo, hi = _sn_band_at(bands, t)
        assert _close(e["strike_min"], lo, rel=1e-12) and _close(e["strike_max"], hi, rel=1e-12)
        assert e["strike_min"] >= bands[i][1] * (1 - 1e-12)
        assert e["strike_max"] <= bands[i][2] * (1 + 1e-12)
    # The same grid without per-expiry ranges: its edge strikes, surface-wide.
    del spec["local_vol_strike_min"], spec["local_vol_strike_max"]
    edges = _ok(client, grid)["arbitrage"]["clamp_edges"]
    assert {e["scope"] for e in edges} == {"Surface"} and len(edges) == len(spec["expiries"])
    for e in edges:
        assert _close(e["strike_min"], spec["strikes"][0] * m, rel=1e-12)
        assert _close(e["strike_max"], spec["strikes"][-1] / m, rel=1e-12)
    # The SVI payload: each slice's strike_min / strike_max.
    svi = load_json(data_dir / EXAMPLES / "scotia_ndx_svi.json")
    svi.pop("simulation")
    svi["checks"] = {"fit": False, "arbitrage": True}
    slices = svi["pricing"]["volatility"]["vol_surfaces"][0]["payload"]["slices"]
    edges = _ok(client, svi)["arbitrage"]["clamp_edges"]
    assert [e["expiry"] for e in edges] == [s["expiry"] for s in slices]
    bands = [(e["time"], s["strike_min"] * m, s["strike_max"] / m) for e, s in zip(edges, slices)]
    for i, e in enumerate(edges):
        assert e["scope"] == "PerExpiry"
        t = e["time"] - (1e-3 if i == len(edges) - 1 else 0.0)
        assert _close(e["strike_min"], _sn_band_at(bands, t)[0], rel=1e-12)
        assert e["strike_min"] >= bands[i][1]
    # A constant vol is read unclamped (exact steps, no local vol).
    v = _ok(client, load_json(data_dir / EXAMPLES / "scotia_ndx_vendor_constant_vol.json"))
    assert v["arbitrage"]["clamp_edges"] == []
