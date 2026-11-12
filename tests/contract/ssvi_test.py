"""eSSVI surfaces: `/calibrate-vol-surface` method `Ssvi` and the `SsviVolSpec` payload.

End to end through the JSON gateway:

  * the committed examples equal the generator's output
    (tests/tools/make_synthetic_quote_sheet.py, presets `svi` and `spx_essvi`);
  * calibration recovery: both sheets are generated from eSSVI surfaces (the
    `svi` preset's SSVI power law, psi = sqrt(theta), rho = -0.7; the
    `spx_essvi` preset's five slices of the contributed example), and the
    Ssvi method returns the generating theta, rho, psi and forwards; the
    arbitrage report is clean and the closed-form local vol check passes;
  * the returned `vol_surface_spec_ssvi` prices a European option at the
    Black price of the surface's own variance (`price_equity_option_ql` on
    the Python twin of the surface) and `/sample-vol-surfaces` samples it
    like the Python twin, at the slices, between them, before the first and
    after the last;
  * a structured note simulates the payload with the closed-form local vol
    (no Dupire stencil): it prices, with and without the floor, and its
    Greeks run (the vega bump keeps the slices eSSVI);
  * the 400s of the payload and of the method's settings, each naming the
    field.

The calibrator has no Python port: the references are the generating
parameters. The JSON gateway prints doubles with 12 decimals.
"""

import copy
import math
import sys
from pathlib import Path

import pytest

import ql_reference
from ql_reference import load_json, price_equity_option_ql, sample_vol_surfaces_ql
from vol_validation import validate

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "tools"))

import make_synthetic_quote_sheet as gen  # noqa: E402

SVI_SHEET = "vol_surface/synthetic_svi_sheet_2026-09-30_ssvi.json"
SPX_SHEET = "vol_surface/synthetic_spx_essvi_sheet_2026-10-21_ssvi.json"
DAILY_SHEET = "vol_surface/synthetic_spx_daily_expiries_sheet_svi.json"
DAILY_NOTE_SSVI = "structured_note/synthetic_spx_1y_autocall_on_daily_expiry_ssvi.json"


def _post(client, request, product):
    return client.session.post(f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _ok(client, request, product="calibrate_vol_surface"):
    r = _post(client, request, product)
    assert r.status_code == 200, r.text[:800]
    return r.json()


def _400(client, request, product, *needles):
    r = _post(client, request, product)
    assert r.status_code == 400, (r.status_code, r.text[:800])
    for n in needles:
        assert n in r.text, (n, r.text[:800])


def test_examples_equal_generator(data_dir):
    assert load_json(data_dir / SVI_SHEET) == \
        gen.build_vol_surface_request(gen.SYNTHETIC_SVI_NDX_LIKE, "Ssvi")
    assert load_json(data_dir / SPX_SHEET) == \
        gen.build_vol_surface_request(gen.SYNTHETIC_SPX_ESSVI, "Ssvi")


@pytest.mark.parametrize("example,preset,tol", [
    (SVI_SHEET, gen.SYNTHETIC_SVI_NDX_LIKE, 1e-6),
    (SPX_SHEET, gen.SYNTHETIC_SPX_ESSVI, 1e-6),
])
def test_ssvi_recovers_the_generating_slices(client, data_dir, example, preset, tol):
    request = load_json(data_dir / example)
    out = _ok(client, request)
    assert out["method"] == "Ssvi"
    truth = gen.truth(preset)
    spec = out["vol_surface_spec_ssvi"]
    assert spec["payload_type"] == "SsviVolSpec"
    assert spec["id"] == request["vol_surface_id"]
    payload = spec["payload"]
    assert payload["spot"] == out["spot"]
    # Unclamped: no strike range on the payload or its slices.
    assert "local_vol_strike_range" not in payload
    assert "local_vol_time_extrapolation" not in payload
    slices = payload["slices"]
    assert [s["expiry"] for s in slices] == list(truth)
    for s, e in zip(slices, [e for e in out["expiries"] if e["usable"]]):
        p = gen.ssvi_params(preset, s["expiry"])
        assert s["theta"] == pytest.approx(p["theta"], rel=tol), s["expiry"]
        assert s["rho"] == pytest.approx(p["rho"], abs=tol), s["expiry"]
        assert s["psi"] == pytest.approx(p["psi"], rel=tol), s["expiry"]
        assert s["forward"] == pytest.approx(truth[s["expiry"]]["forward"], rel=1e-9)
        assert "strike_min" not in s and "strike_max" not in s
        assert e["ssvi"] == {"theta": s["theta"], "rho": s["rho"], "psi": s["psi"]}
        assert e["ssvi_anchor_clamped"] is False
    # One flag on the calibrate response; the margins, the closed-form
    # conditions, the scan, the local vol check and the fit are
    # /validate-vol-surface's.
    assert out["arbitrage_free_ssvi"] is True
    for gone in ("ssvi_arbitrage", "local_vol_check_ssvi", "vanilla_repricing"):
        assert gone not in out
    v = validate(client, request, out, "ssvi")
    arb = v["arbitrage"]
    assert arb["arbitrage_free"] is True and arb["violations"] == []
    assert arb["ssvi_check"]["violations"] == []
    assert arb["ssvi_check"]["scan_min_g"] >= 0.0 and arb["ssvi_check"]["scan_min_dw_dt"] >= 0.0
    slices_checked = arb["ssvi_slices"]
    for c in slices_checked:
        assert c["margin_butterfly_psi"] >= 0.0
        assert c["margin_butterfly_psi2"] >= 0.0
    for c in slices_checked[1:]:
        assert c["margin_calendar_theta"] >= 0.0
        assert c["margin_calendar_psi"] >= 0.0
        assert c["margin_calendar_a"] >= 0.0
    assert "margin_calendar_theta" not in slices_checked[0]
    check = arb["local_vol_check"]
    assert check["arbitrage_free"] and check["refused"] == 0, check
    assert check["points"] == check["times"] * check["strikes"]
    assert arb["clamp_edges"] == []
    assert "vol_surface_spec_svi" not in out and "arbitrage_free_svi" not in out
    assert "Ssvi" in out["vol_surface_spec_svi_note"]
    # The slices are the generating surface: every used quote inside.
    assert v["fit"]["inside"] == v["fit"]["count"] > 0


def test_ssvi_settings_drop_quotes_by_moneyness(client, data_dir):
    request = load_json(data_dir / SVI_SHEET)
    request["weighting"] = "Uniform"
    request["ssvi"] = {"max_abs_log_moneyness": 0.1}
    out = _ok(client, request)
    reasons = [d["reason"] for e in out["expiries"] for d in e.get("dropped", [])]
    assert "max_abs_log_moneyness" in reasons
    for e in out["expiries"]:
        assert e["quotes_dropped"] == len(e.get("dropped", []))


def _ssvi_pricing(out, request):
    """The calibration's market with the returned SSVI payload added."""
    pricing = copy.deepcopy(request["pricing"])
    pricing.setdefault("volatility", {})["vol_surfaces"] = [out["vol_surface_spec_ssvi"]]
    pricing["volatility"]["models"] = [
        {"id": "bs", "payload_type": "EquityVanillaModelSpec",
         "payload": {"model_type": "BlackScholesAnalytic"}}]
    return pricing


def test_payload_prices_a_european_option_like_the_python_twin(client, data_dir):
    request = load_json(data_dir / SPX_SHEET)
    out = _ok(client, request)
    pricing = _ssvi_pricing(out, request)
    vol_id = out["vol_surface_spec_ssvi"]["id"]
    underlying = request["sheet"]["underlying_id"]
    # At a slice, between two slices, before the first and after the last.
    for n, (expiry, strike, kind) in enumerate([
            ("2027-04-21", 6100.0, "Call"), ("2027-07-21", 5500.0, "Put"),
            ("2026-11-05", 6000.0, "Call"), ("2029-06-15", 6500.0, "Call")]):
        options = [{"option": {
            "trade_id": f"OPT_{n}", "underlying_id": underlying, "quantity": 1.0,
            "settlement": "Physical", "payoff_type": "EquityPlainVanillaPayoff",
            "payoff": {"option_type": kind, "strike": strike},
            "exercise_type": "EquityEuropeanExercise",
            "exercise": {"expiry_date": expiry}},
            "discounting_curve": request["discounting_curve"],
            "volatility": vol_id, "model": "bs"}]
        req = {"pricing": pricing, "options": options}
        api = client.price("equity_option", req)["options"][0]["npv"]
        ref = price_equity_option_ql(req)
        assert api == pytest.approx(ref, rel=1e-10, abs=1e-9), (expiry, strike)


def test_payload_samples_like_the_python_twin(client, data_dir):
    request = load_json(data_dir / SPX_SHEET)
    out = _ok(client, request)
    pricing = _ssvi_pricing(out, request)
    vol_id = out["vol_surface_spec_ssvi"]["id"]
    days = [10, 91, 274, 1095]
    strikes = [4200.0, 5400.0, 6000.0, 6600.0, 7800.0]
    sample = {"pricing": pricing, "queries": [{
        "vol_id": vol_id, "surface_type": "EquityBlack",
        "expiry_grid": {"grid_type": "TenorGrid",
                        "grid": {"tenors": [{"n": d, "unit": "Days"} for d in days],
                                 "calendar": "NullCalendar",
                                 "business_day_convention": "Unadjusted"}},
        "strike_grid": {"axis": "AbsoluteStrike", "strikes": strikes},
        "output_mode": "Cube",
        "options": {"allow_extrapolation": True, "max_points": 100000, "strict": True}}]}
    r = _post(client, sample, "sample_vol_surfaces")
    assert r.status_code == 200, r.text[:600]
    assert "error" not in r.json()["results"][0]
    api = r.json()["results"][0]["vols"]
    ref = sample_vol_surfaces_ql(sample)
    assert len(api) == len(ref)
    for a, b in zip(api, ref):
        assert a == pytest.approx(b, abs=1e-11)


def test_daily_note_example_carries_the_returned_payload_and_prices(client, data_dir):
    sheet = load_json(data_dir / DAILY_SHEET)
    sheet["method"] = "Ssvi"
    out = _ok(client, sheet)
    assert out["arbitrage_free_ssvi"]
    note = load_json(data_dir / DAILY_NOTE_SSVI)
    assert note["pricing"]["volatility"]["vol_surfaces"] == [out["vol_surface_spec_ssvi"]]
    priced = _ok(client, note, "structured_note")["notes"][0]
    assert priced["npv"] > 0.0
    assert priced["model"]["underlyings"][0]["vol_shape"] == "SsviSlices"
    # A floor below every closed-form local vol never binds and never refuses.
    floored = copy.deepcopy(note)
    floored["notes"][0]["monte_carlo"]["local_vol_floor"] = 0.01
    mu = _ok(client, floored, "structured_note")["notes"][0]["model"]["underlyings"][0]
    assert mu["local_vol_refusals_floored"] == 0
    assert mu["local_vol_reads"] > 0


def test_note_greeks_on_the_payload(client, data_dir):
    """The vega bump shifts the slices' at-the-money vols (still an eSSVI
    surface, closed-form local vol); every Greek is finite."""
    note = load_json(data_dir / DAILY_NOTE_SSVI)
    note["notes"][0]["monte_carlo"]["paths"] = 1024
    note["greeks"] = {"spot_bump": 0.01, "vol_bump": 0.01}
    g = _ok(client, note, "structured_note")["notes"][0]["greeks"]
    for u in g["underlyings"]:
        for f in ("delta", "gamma", "vega"):
            assert math.isfinite(u[f]) and math.isfinite(u[f + "_standard_error"])
    assert g["underlyings"][0]["vega"] > 0.0 or g["underlyings"][0]["vega"] < 0.0


def _payload_note(data_dir):
    return load_json(data_dir / DAILY_NOTE_SSVI)


def _slices(note):
    return note["pricing"]["volatility"]["vol_surfaces"][0]["payload"]["slices"]


@pytest.mark.parametrize("mutate,needle", [
    (lambda p: p.pop("spot"), "SsviVolSpec.spot is required"),
    (lambda p: p.update(spot=-1.0), "SsviVolSpec.spot must be > 0"),
    (lambda p: p.update(slices=[]), "SsviVolSpec.slices must contain at least one slice"),
    (lambda p: p["slices"][0].pop("theta"), "SsviVolSpec.slices[0].theta is required"),
    (lambda p: p["slices"][1].pop("rho"), "SsviVolSpec.slices[1].rho is required"),
    (lambda p: p["slices"][0].pop("forward"), "SsviVolSpec.slices[0].forward is required"),
    (lambda p: p["slices"][0].update(rho=1.0), "SsviVolSpec.slices[0].rho must be in (-1, 1)"),
    (lambda p: p["slices"][0].update(psi=0.0), "SsviVolSpec.slices[0].psi must be > 0"),
    (lambda p: p["slices"][0].update(theta=-0.01), "SsviVolSpec.slices[0].theta must be > 0"),
    (lambda p: p["slices"][0].update(forward=0.0), "SsviVolSpec.slices[0].forward must be > 0"),
    (lambda p: p["slices"][1].update(expiry=p["slices"][0]["expiry"]),
     "SsviVolSpec.slices[1].expiry: slices must be strictly increasing"),
    (lambda p: p["slices"][0].update(expiry="2020-01-01"),
     "SsviVolSpec.slices[0].expiry must be after reference_date"),
    # Caller-supplied ranges (the returned payload carries none).
    (lambda p: p["slices"][0].update(strike_min=30000.0, strike_max=30000.0),
     "SsviVolSpec.slices[0].strike_min must be below strike_max"),
    (lambda p: p.update(local_vol_strike_range="PerExpiry"),
     "strike_min and strike_max are required with local_vol_strike_range PerExpiry"),
    (lambda p: p.update(local_vol_time_extrapolation="FlatImpliedVol"),
     "FlatImpliedVol is not an eSSVI reading"),
    (lambda p: p["base"].update(shape="SmileCube3D"),
     "SsviVolSpec.base.shape must be absent"),
    (lambda p: p["base"].update(constant_vol=0.2),
     "SsviVolSpec.base.constant_vol / quote_id are not part of an SSVI payload"),
    (lambda p: p["base"].pop("day_counter"), "SsviVolSpec.base.day_counter is required"),
    # Butterfly: psi (1 + |rho|) <= 4 broken on one slice.
    (lambda p: p["slices"][3].update(psi=3.0), "SSVI surface admits arbitrage"),
    # Calendar: theta decreasing between two slices.
    (lambda p: p["slices"][4].update(theta=p["slices"][3]["theta"] * 0.5),
     "calendar_theta at slice 4 vs slice 3"),
])
def test_payload_rejected(client, data_dir, mutate, needle):
    note = _payload_note(data_dir)
    mutate(note["pricing"]["volatility"]["vol_surfaces"][0]["payload"])
    _400(client, note, "structured_note", needle)


@pytest.mark.parametrize("ssvi,needle", [
    ({"max_abs_log_moneyness": 0.0}, "ssvi.max_abs_log_moneyness must be finite and > 0"),
    ({"max_abs_log_moneyness": -1.0}, "ssvi.max_abs_log_moneyness must be finite and > 0"),
    ({"max_abs_log_moneyness": 1e-5}, "used quotes, at least 3 are required"),
])
def test_method_settings_rejected(client, data_dir, ssvi, needle):
    request = load_json(data_dir / SVI_SHEET)
    request["ssvi"] = ssvi
    _400(client, request, "calibrate_vol_surface", needle)
