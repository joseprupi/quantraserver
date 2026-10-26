"""Vanilla repricing through the Monte Carlo (`diagnostics.vanilla_repricing`
on POST /price-structured-note): the acceptance check of the listed quotes
-> implied forwards -> Black surface -> Dupire local vol -> simulated paths
chain. Every usable quote of the request's sheets is priced as a European
option on one extra simulation of the note's model and compared with its
bid/ask.

What this suite locks:

  * the block's shape on the two Scotia examples (two sheets, 383 used
    quotes, every expiry reported with the model forward and the plain
    discount factor, the dropped lines with reasons, the note walked on the
    repricing paths within the combined Monte Carlo noise of the headline,
    the timing stage), and that the flag does not move the headline;
  * the ACCEPTANCE CRITERION on the surfaces `/calibrate-vol-surface`
    returns — the grid read `CubicStrikeLinearTime` (the committed
    `_with_synthetic_surface` example) and the SVI payload (the committed
    `_with_synthetic_svi` example): at the example's 2048 Sobol paths every
    option of the core set (expiries from the second one on, out-of-the-money
    mid at least 0.25 % of the spot) is within 3.5 standard errors of its
    bid/ask band (the worst sits at 2.5 SE; `inside_bid_ask` itself is
    sampling-limited at 2048 paths, where the standard error of an ATM
    option is 4 % of its price against a 1 % bid/ask — at 16 384 paths 152
    of 383 are strictly inside, at 65 536 paths more) and
    the mean vol error per expiry is within 1.5 vol points (0.5 from six
    months on; at 16 384 paths it is within 0.35 from the second expiry on).
    The separate set — the 16-day expiry (11 Euler steps of the local vol:
    at 1008 steps per year its mean vol error halves and 17/17 are within
    3 SE) and the deep wings priced below 0.25 % of the spot (the sampling
    limit) — is reported, not required: 13 of its 88 options are beyond
    3.5 SE at 2048 paths, 10 at 16 384, 2 at 16 384 paths with 1008 steps.
    The two payloads agree with each other to the Monte Carlo noise (same
    stream, same model: the grid nodes sit on the slices);
  * the documented limit: the same grid read `Bilinear` loses the smile's
    curvature between the nodes and reprices 2 to 3 vol points HIGH from six
    months on (inside < 10 %; the put wing of the shortest expiry low), the
    numbers recorded in docs/versioning.md (0.15.0);
  * the chain on the 0.14.0 vol-surface examples (implied forwards ->
    surface -> a single-underlying note on the sheet's own market), on the
    SVI example meeting the criterion on both payloads and on the
    Andreasen-Huge example the documented 400 (its flat-filled wings);
  * the error rows: the flag without sheets, sheets without the flag, two
    sheets for one underlying, a sheet for an underlying of no note, a
    sheet on a quanto underlying, a non-European sheet, a sheet with no
    usable quote, a bad filter value.
"""

import copy
import math
import sys
from pathlib import Path

import pytest

from ql_reference import load_json

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "tools"))

import make_synthetic_quote_sheet as gen  # noqa: E402

NOTE_EXAMPLE = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_surface.json"
NOTE_SVI_EXAMPLE = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_svi.json"
BNP_EXAMPLE = "structured_note/bnp_quad_index_classic_autocall_xs2741509801.json"
AH_EXAMPLE = "vol_surface/synthetic_svi_sheet_2026-09-30_andreasen_huge.json"
SVI_EXAMPLE = "vol_surface/synthetic_svi_sheet_2026-09-30_svi.json"

# The acceptance criterion (docstring).
CORE_MIN_MID_OF_SPOT = 2.5e-3
CORE_Z = 3.5


def _post(client, request, product="structured_note"):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _ok(client, request, product="structured_note"):
    r = _post(client, request, product)
    assert r.status_code == 200, r.text[:600]
    return r.json()


def _400(client, request, needle):
    r = _post(client, request)
    assert r.status_code == 400, (r.status_code, r.text[:400])
    assert needle in r.text, (needle, r.text[:600])


def _check_block_shape(note, request):
    vr = note["vanilla_repricing"]
    mc = request["notes"][0]["monte_carlo"]
    assert vr["paths"] == mc["paths"]
    assert vr["time_steps"] > note["model"]["time_steps"]
    assert vr["count"] == sum(u["count"] for u in vr["underlyings"])
    assert vr["inside_bid_ask"] == sum(u["inside_bid_ask"] for u in vr["underlyings"])
    assert vr["max_standard_error"] == max(u["max_standard_error"] for u in vr["underlyings"])
    for u in vr["underlyings"]:
        assert u["count"] == len(u["options"]) == sum(e["quotes_used"] for e in u["expiries"])
        assert u["inside_bid_ask"] == sum(o["inside_bid_ask"] for o in u["options"])
        for e in u["expiries"]:
            assert e["quotes_dropped"] == len(e["dropped"])
            for d in e["dropped"]:
                assert d["expiry"] == e["expiry"] and d["reason"]
            if e["quotes_used"]:
                assert e["forward"] > 0.0 and 0.0 < e["discount_factor"] <= 1.0
        worst_price = worst_vol = worst_se = 0.0
        for o in u["options"]:
            assert o["option_type"] in ("Call", "Put")
            assert o["bid"] <= o["mid"] <= o["ask"]
            assert math.isfinite(o["model_price"]) and o["standard_error"] >= 0.0
            assert o["inside_bid_ask"] == (o["bid"] <= o["model_price"] <= o["ask"])
            assert abs(o["price_error"] - (o["model_price"] - o["mid"])) < 1e-9
            if "vol_error" in o:
                assert abs(o["vol_error"] - (o["model_vol"] - o["market_vol"])) < 1e-11  # 12-decimal JSON
                worst_vol = max(worst_vol, abs(o["vol_error"]))
            worst_price = max(worst_price, abs(o["price_error"]))
            worst_se = max(worst_se, o["standard_error"])
        assert abs(u["max_abs_price_error"] - worst_price) < 1e-9
        assert abs(u["max_abs_vol_error"] - worst_vol) < 1e-11
        assert abs(u["max_standard_error"] - worst_se) < 1e-9
    # The note walked on the repricing paths: the same model on a finer
    # stream, within the combined Monte Carlo noise.
    se = math.sqrt(vr["note_standard_error"] ** 2 + note["standard_error"] ** 2)
    assert abs(vr["note_npv"] - note["npv"]) <= 4.0 * se, (vr["note_npv"], note["npv"], se)
    return vr


def _excess_z(o):
    """How far the model price sits outside its bid/ask band, in standard
    errors (0 when inside)."""
    p = o["model_price"]
    excess = max(o["bid"] - p, p - o["ask"], 0.0)
    if excess == 0.0:
        return 0.0
    return excess / o["standard_error"] if o["standard_error"] > 0.0 else math.inf


def _assert_acceptance(vr, first_expiry, mean_band_short, mean_band_long):
    """The acceptance criterion of the docstring on a repricing block."""
    core = separate = 0
    worst_core = 0.0
    beyond_separate = 0
    for u in vr["underlyings"]:
        spot = u["spot"]
        by_expiry = {}
        for o in u["options"]:
            by_expiry.setdefault(o["expiry"], []).append(o)
            deep = o["mid"] < CORE_MIN_MID_OF_SPOT * spot
            if o["expiry"] == first_expiry or deep:
                separate += 1
                beyond_separate += _excess_z(o) > CORE_Z
                continue
            core += 1
            z = _excess_z(o)
            worst_core = max(worst_core, z)
            assert z <= CORE_Z, (u["underlying_id"], o["expiry"], o["strike"], o["option_type"],
                                 o["mid"] / spot, z, o.get("vol_error"))
        expiries = sorted(by_expiry)
        for n, e in enumerate(expiries):
            if e == first_expiry:
                continue
            errs = [o["vol_error"] for o in by_expiry[e] if "vol_error" in o]
            mean = sum(errs) / len(errs)
            band = mean_band_long if n >= 5 else mean_band_short
            assert abs(mean) < band, (u["underlying_id"], e, mean, band)
    assert core > 0 and separate > 0
    return core, separate, worst_core, beyond_separate


def _scotia_acceptance(client, data_dir, example):
    request = load_json(data_dir / example)
    assert request["diagnostics"]["vanilla_repricing"] is True
    sheets = request["diagnostics"]["vanilla_repricing_sheets"]
    assert [s["underlying_id"] for s in sheets] == ["NDX", "RTY"]
    assert [s["id"] for s in sheets] == gen_sheet_ids()
    request["diagnostics"]["timing"] = True
    note = _ok(client, request)["notes"][0]
    vr = _check_block_shape(note, request)
    assert [u["underlying_id"] for u in vr["underlyings"]] == ["NDX", "RTY"]
    assert [u["sheet_id"] for u in vr["underlyings"]] == [s["id"] for s in sheets]
    assert vr["count"] == 189 + 194  # the quotes /calibrate-vol-surface used
    assert [len(u["expiries"]) for u in vr["underlyings"]] == [8, 8]
    assert any(s["stage"] == "vanilla_repricing" for s in note["timing"]["stages"])
    # Every expiry's forward is the note's own: spot x D_div / D_note on the
    # Scotia curves, which the sheets were generated on (the generator's
    # svi_scotia_* presets), so the market and the model price the same
    # forward and the comparison is about volatility alone.
    for u, preset in zip(vr["underlyings"], (gen.SYNTHETIC_SVI_SCOTIA_NDX,
                                             gen.SYNTHETIC_SVI_SCOTIA_RTY)):
        truth = gen.truth(preset)
        for e in u["expiries"]:
            assert abs(e["forward"] / truth[e["expiry"]]["forward"] - 1.0) < 1e-9, e
            assert abs(e["discount_factor"] / truth[e["expiry"]]["discount_factor"] - 1.0) < 1e-9
    core, separate, worst, beyond = _assert_acceptance(vr, "2026-10-16", 0.015, 0.010)
    assert core == 295 and separate == 88, (core, separate)
    assert beyond <= 20, beyond  # 13 at 2048 paths: the short deep wings
    # The flag changes nothing else: the headline is the same run.
    plain = copy.deepcopy(request)
    del plain["diagnostics"]
    other = _ok(client, plain)["notes"][0]
    assert other["npv"] == note["npv"] and other["standard_error"] == note["standard_error"]
    assert "vanilla_repricing" not in other
    return note


def test_scotia_grid_example_meets_the_acceptance_criterion(client, data_dir):
    note = _scotia_acceptance(client, data_dir, NOTE_EXAMPLE)
    assert note["model"]["underlyings"][0]["vol_shape"] == "SmileCube3D"
    assert 940.0 < note["npv"] < 965.0, note["npv"]  # 951.9 +- 6.2 at 2048 paths


def test_scotia_svi_example_meets_the_acceptance_criterion(client, data_dir):
    note = _scotia_acceptance(client, data_dir, NOTE_SVI_EXAMPLE)
    assert note["model"]["underlyings"][0]["vol_shape"] == "SviSlices"
    assert 940.0 < note["npv"] < 965.0, note["npv"]  # 951.6 +- 6.3 at 2048 paths


def test_the_two_payloads_agree_to_the_monte_carlo_noise(client, data_dir):
    """Grid (cubic) and SVI payload on the same stream: the grid nodes sit on
    the slices, so the two local vols differ by the spline's error between
    nodes only — note prices within one standard error, every repriced
    option within a quarter of its standard error."""
    grid = load_json(data_dir / NOTE_EXAMPLE)
    svi = load_json(data_dir / NOTE_SVI_EXAMPLE)
    assert grid["notes"] == svi["notes"] and grid["diagnostics"] == svi["diagnostics"]
    a = _ok(client, grid)["notes"][0]
    b = _ok(client, svi)["notes"][0]
    assert abs(a["npv"] - b["npv"]) < a["standard_error"], (a["npv"], b["npv"])
    for ua, ub in zip(a["vanilla_repricing"]["underlyings"], b["vanilla_repricing"]["underlyings"]):
        for oa, ob in zip(ua["options"], ub["options"]):
            assert (oa["expiry"], oa["strike"]) == (ob["expiry"], ob["strike"])
            assert abs(oa["model_price"] - ob["model_price"]) <= 0.25 * max(oa["standard_error"], 1e-9)


def test_scotia_grid_read_bilinear_is_the_documented_limit(client, data_dir):
    """The same grid read Bilinear: the smile's curvature is lost between
    the nodes (Dupire's d2w/dk2 term is zero inside every cell), the options
    reprice 2 to 3 vol points HIGH from six months on and the note is about
    12 per 1,000 lower (939.6 vs 951.9 at 2048 paths; 938.4 +- 2.5 vs 950.9
    +- 2.2 at 16 384). Locked so that a change in either direction is noticed."""
    request = load_json(data_dir / NOTE_EXAMPLE)
    for vs in request["pricing"]["volatility"]["vol_surfaces"]:
        assert vs["payload"]["surface_interpolator"] == "CubicStrikeLinearTime"
        vs["payload"]["surface_interpolator"] = "Bilinear"
    note = _ok(client, request)["notes"][0]
    vr = _check_block_shape(note, request)
    assert vr["inside_bid_ask"] < vr["count"] // 10
    assert 0.03 < vr["max_abs_vol_error"] < 0.20
    for u in vr["underlyings"]:
        last = [o for o in u["options"] if o["expiry"] == "2027-12-17" and "vol_error" in o]
        assert len(last) == 25
        mean = sum(o["vol_error"] for o in last) / len(last)
        assert 0.01 < mean < 0.045, (u["underlying_id"], mean)
    assert 925.0 < note["npv"] < 950.0, note["npv"]  # 939.6 +- 7.1 at 2048 paths


def gen_sheet_ids():
    return [gen.SYNTHETIC_SVI_SCOTIA_NDX["sheet_id"], gen.SYNTHETIC_SVI_SCOTIA_RTY["sheet_id"]]


def test_scotia_sheets_equal_the_generator(data_dir):
    """The pasted sheets are the generator's svi_scotia_ndx / svi_scotia_rty
    output (nothing in the example is market data); both examples carry the
    same sheets and the same note."""
    for example in (NOTE_EXAMPLE, NOTE_SVI_EXAMPLE):
        request = load_json(data_dir / example)
        for sheet, preset in zip(request["diagnostics"]["vanilla_repricing_sheets"],
                                 (gen.SYNTHETIC_SVI_SCOTIA_NDX, gen.SYNTHETIC_SVI_SCOTIA_RTY)):
            assert sheet == gen.build_request(preset)["sheet"]


def _chain_request(client, example, data_dir, paths=2048, steps_per_year=252, payload="grid"):
    """implied forwards -> surface -> a single-underlying note on the sheet's
    own market (the example's curves, the implied dividend curve, the
    returned surface: the grid or, with payload="svi", the SVI slices), the
    sheet under the diagnostics."""
    vs_req = load_json(data_dir / example)
    fwd_req = {"pricing": vs_req["pricing"], "sheet": vs_req["sheet"],
               "discounting_curve": vs_req["discounting_curve"], "filter": vs_req["filter"],
               "dividend_curve_id": "NDX_SYN_DIV_IMPLIED"}
    fwd = _ok(client, fwd_req, "calibrate_implied_forwards")
    vs_req = copy.deepcopy(vs_req)
    vs_req["forwards"] = fwd["expiries"]
    vs = _ok(client, vs_req, "calibrate_vol_surface")
    assert vs["vanilla_repricing"]["count"] > 150
    pricing = copy.deepcopy(vs_req["pricing"])
    pricing["rates"]["curves"].append(fwd["dividend_curve_spec"])
    und = pricing["equity"]["equity_underlyings"][0]
    und["dividend_yield_curve_id"] = fwd["dividend_curve_spec"]["id"]
    spec = vs["vol_surface_spec_svi"] if payload == "svi" else vs["vol_surface_spec"]
    pricing["volatility"] = {"vol_surfaces": [spec]}
    expiries = sorted({q["expiry"] for q in vs_req["sheet"]["quotes"]})
    mid, last = expiries[4], expiries[-1]
    return {
        "pricing": pricing,
        "notes": [{
            "trade_id": "CHAIN", "currency": "USD", "notional": 1000.0,
            "underlyings": [{"underlying_id": und["id"],
                             "volatility_id": spec["id"],
                             "initial_level": pricing["quotes"][0]["value"]}],
            "performance_rule": "WorstOf",
            "events": [{"observation_date": mid, "payment_date": mid,
                        "autocall_trigger": 1.0, "coupon": 0.05},
                       {"observation_date": last, "payment_date": last,
                        "autocall_trigger": 1.0, "coupon": 0.10}],
            "coupon_rule": {"memory": True, "pay_only_on_call": True},
            "capital": {"barrier": 0.7, "monitoring": "European",
                        "put_strike": 1.0, "participation": 1.0},
            "maturity_payment_date": last,
            "discounting_curve": vs_req["discounting_curve"], "funding_spread_bp": 0.0,
            "monte_carlo": {"paths": paths, "seed": 42, "sequence": "Sobol",
                            "antithetic": True, "brownian_bridge": True,
                            "steps_per_year": steps_per_year},
        }],
        "diagnostics": {"vanilla_repricing": True,
                        "vanilla_repricing_sheets": [vs_req["sheet"]],
                        "vanilla_repricing_filter": vs_req["filter"]},
    }, vs


@pytest.mark.parametrize("payload", ["grid", "svi"])
def test_chain_on_the_svi_example(client, data_dir, payload):
    request, vs = _chain_request(client, SVI_EXAMPLE, data_dir, payload=payload)
    note = _ok(client, request)["notes"][0]
    vr = _check_block_shape(note, request)
    u = vr["underlyings"][0]
    assert u["underlying_id"] == "NDX_SYN"
    assert note["model"]["underlyings"][0]["vol_shape"] == \
        ("SviSlices" if payload == "svi" else "SmileCube3D")
    # The same quotes the surface was calibrated to (same filter, the same
    # forwards: the implied dividend curve reproduces them).
    assert u["count"] == vs["vanilla_repricing"]["count"]
    used = {(q["expiry"], q["strike"]) for q in vs["vanilla_repricing"]["quotes"]}
    assert {(o["expiry"], o["strike"]) for o in u["options"]} == used
    for e in u["expiries"]:
        f = next(x for x in vs["forwards"] if x["expiry"] == e["expiry"])
        assert abs(e["forward"] / f["forward"] - 1.0) < 1e-9
    core, separate, worst, beyond = _assert_acceptance(vr, "2026-10-16", 0.015, 0.010)
    assert core > 120 and separate > 20, (core, separate)


def test_chain_on_the_andreasen_huge_example_is_the_documented_400(client, data_dir):
    """The Andreasen-Huge surface fills the deep wings of each expiry flat
    from the nearest inverted strike (`grid_points_filled`): the cubic
    spline through a flat segment and the smile beside it bends where the
    fill starts, and the Dupire finite-difference stencil of a path that
    gets there reads a negative local variance. QuantLib refuses it and the
    note is the documented 400 naming the vol id and the remedy; the
    endpoint has no SVI payload for this method."""
    request, vs = _chain_request(client, AH_EXAMPLE, data_dir)
    assert sum(e.get("grid_points_filled", 0) for e in vs["expiries"]) > 0
    assert "vol_surface_spec_svi" not in vs and "vol_surface_spec_svi_note" in vs
    r = _post(client, request)
    assert r.status_code == 400, r.text[:400]
    assert "could not be evaluated along the simulated paths" in r.text
    assert "negative local vol^2" in r.text
    assert "NDX_SYN_VOL" in r.text


def test_rejected_requests(client, data_dir):
    base = load_json(data_dir / NOTE_EXAMPLE)
    base["notes"][0]["monte_carlo"]["paths"] = 256
    sheets = base["diagnostics"]["vanilla_repricing_sheets"]

    req = copy.deepcopy(base)
    req["diagnostics"] = {"vanilla_repricing": True}
    _400(client, req, "vanilla_repricing_sheets: required with vanilla_repricing = true")

    req = copy.deepcopy(base)
    req["diagnostics"] = {"vanilla_repricing_sheets": sheets}
    _400(client, req, "vanilla_repricing_sheets: requires vanilla_repricing = true")

    req = copy.deepcopy(base)
    req["diagnostics"]["vanilla_repricing_sheets"] = [sheets[0], sheets[0]]
    _400(client, req, "vanilla_repricing_sheets[1].underlying_id: 'NDX' already has a sheet")

    req = copy.deepcopy(base)
    other = copy.deepcopy(sheets[0])
    other["underlying_id"] = "SPX"
    req["diagnostics"]["vanilla_repricing_sheets"] = [other]
    _400(client, req, "no sheet for any underlying of notes[0] [NDX, RTY]")

    req = copy.deepcopy(base)
    req["diagnostics"]["vanilla_repricing_sheets"] = [{**sheets[0], "option_style": "American"}]
    _400(client, req, "vanilla_repricing_sheets[0].option_style must be European")

    req = copy.deepcopy(base)
    req["diagnostics"]["vanilla_repricing_filter"] = {"max_relative_spread": 0.0}
    _400(client, req, "max_relative_spread")

    req = copy.deepcopy(base)
    stale = copy.deepcopy(sheets[0])
    for q in stale["quotes"]:
        q["expiry"] = "2026-09-30"  # the valuation date
    req["diagnostics"]["vanilla_repricing_sheets"] = [stale]
    _400(client, req, "no usable quote for underlying 'NDX' (expiry not after as_of_date)")

    req = copy.deepcopy(base)
    empty = copy.deepcopy(sheets[0])
    empty["quotes"] = []
    req["diagnostics"]["vanilla_repricing_sheets"] = [empty]
    _400(client, req, "vanilla_repricing_sheets[0].quotes must contain at least one quote")

    # A sheet on a quanto underlying (the BNP note's SMI, a CHF index in a
    # USD note): the listed options live in their own measure.
    bnp = load_json(data_dir / BNP_EXAMPLE)
    bnp["notes"][0]["monte_carlo"]["paths"] = 256
    quanto = copy.deepcopy(sheets[0])
    quanto["underlying_id"] = "SMI"
    bnp["diagnostics"] = {"vanilla_repricing": True, "vanilla_repricing_sheets": [quanto]}
    _400(client, bnp, "is a quanto underlying of notes[0]")
