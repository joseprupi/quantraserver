"""Where a structured note's Dupire local vol goes beyond the quoted surface.

Two extensions of a strike-dependent Black surface are locked here:

  * IN TIME, beyond the last expiry (`local_vol_time_extrapolation` on
    `BlackVolSpec` / `SviVolSpec`). The surface extends its implied vol flat
    in time; on a skew that is not arbitrage-free at long horizons (the
    -t^2 v_k^2 / 16 term of Durrleman's condition grows with t), so a note
    running years past the last expiry is a 400. `FlatLocalVol` holds the
    local vol at its value 0.001 years before the last expiry instead, and
    the same note prices. `/calibrate-vol-surface` returns FlatLocalVol on
    both specs.
  * IN STRIKE, beyond the quoted strikes of an SVI slice (`strike_min` /
    `strike_max`). A raw SVI wing fitted to quotes from 60 % to 115 % of the
    spot is butterfly-free on the calibrator's output range (50 % to 150 %)
    and not beyond it; a path that leaves the quotes reads a negative local
    variance INSIDE the expiry range. The calibrator now returns the quoted
    range per slice and reports the violations beyond its output range; a
    note on the returned slices holds the edge local vol beyond the quotes
    and prices, the same slices stripped of the edges are the 400.
"""

import copy
import sys
from pathlib import Path

import pytest

from ql_reference import load_json

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "tools"))

import make_synthetic_quote_sheet as gen  # noqa: E402

LONG_DATED = "structured_note/synthetic_ndx_rty_5y_autocall_on_svi_flat_local_vol.json"
SPX_2Y = "structured_note/synthetic_spx_2y_autocall_on_svi_quoted_strike_edges.json"
SVI_SHEET = "vol_surface/synthetic_svi_sheet_2026-09-30_svi.json"
STEEP_SHEET = "vol_surface/synthetic_spx_steep_wings_sheet_svi.json"


def _post(client, request, product="structured_note"):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _ok(client, request, product="structured_note"):
    r = _post(client, request, product)
    assert r.status_code == 200, r.text[:600]
    return r.json()


def _400(client, request, *needles):
    r = _post(client, request)
    assert r.status_code == 400, (r.status_code, r.text[:400])
    for needle in needles:
        assert needle in r.text, (needle, r.text[:800])
    return r.text


def _vols(request):
    return request["pricing"]["volatility"]["vol_surfaces"]


# --------------------------------------------------------------------------
# Time: local_vol_time_extrapolation
# --------------------------------------------------------------------------

def test_long_dated_note_prices_with_flat_local_vol(client, data_dir):
    request = load_json(data_dir / LONG_DATED)
    for v in _vols(request):
        assert v["payload"]["local_vol_time_extrapolation"] == "FlatLocalVol"
        assert v["payload"]["slices"][-1]["expiry"] == "2027-12-17"
    note = _ok(client, request)["notes"][0]
    assert note["npv"] > 0.0 and note["standard_error"] > 0.0


@pytest.mark.parametrize("mode", ["absent", "FlatImpliedVol"])
def test_long_dated_note_with_flat_implied_vol_is_400(client, data_dir, mode):
    request = load_json(data_dir / LONG_DATED)
    for v in _vols(request):
        if mode == "absent":
            v["payload"].pop("local_vol_time_extrapolation")
        else:
            v["payload"]["local_vol_time_extrapolation"] = mode
    _400(client, request, "could not be evaluated along the simulated paths",
         "negative local vol^2", "local_vol_time_extrapolation: FlatLocalVol")


def test_flat_local_vol_inside_the_expiry_range_is_unchanged(client, data_dir):
    """A note that ends before the last expiry: the flag moves nothing (the
    clamp only acts in the last 0.001 years and beyond; no grid step starts
    there on this note)."""
    request = load_json(
        data_dir / "structured_note/"
        "scotia_autocallable_trigger_ndx_rty_us06419jme72_with_synthetic_svi.json")
    request.pop("diagnostics", None)
    for n in request["notes"]:
        n["events"] = [n["events"][0]]
        n["events"][0].pop("autocall_trigger", None)
        n["events"][0]["is_final"] = True
        n["events"][0].pop("coupon", None)
        n["maturity_payment_date"] = n["events"][0]["payment_date"]
    base = _ok(client, request)["notes"][0]
    flagged = copy.deepcopy(request)
    for v in _vols(flagged):
        v["payload"]["local_vol_time_extrapolation"] = "FlatLocalVol"
    assert _ok(client, flagged)["notes"][0]["npv"] == base["npv"]


def test_calibrate_returns_flat_local_vol_on_both_specs(client, data_dir):
    out = _ok(client, load_json(data_dir / SVI_SHEET), "calibrate_vol_surface")
    assert out["vol_surface_spec"]["payload"]["local_vol_time_extrapolation"] == "FlatLocalVol"
    assert out["vol_surface_spec_svi"]["payload"]["local_vol_time_extrapolation"] == \
        "FlatLocalVol"


def test_local_vol_time_extrapolation_unknown_value_is_400(client, data_dir):
    request = load_json(data_dir / LONG_DATED)
    _vols(request)[0]["payload"]["local_vol_time_extrapolation"] = "FlatForever"
    r = _post(client, request)
    assert r.status_code == 400, r.text[:400]


# --------------------------------------------------------------------------
# Strike: SviSlice.strike_min / strike_max
# --------------------------------------------------------------------------

def test_calibrate_reports_the_quoted_strike_range_and_the_wings(client, data_dir):
    request = load_json(data_dir / STEEP_SHEET)
    assert request == gen.build_vol_surface_request(gen.SYNTHETIC_SPX_STEEP_WINGS,
                                                    "SviPerExpiry")
    out = _ok(client, request, "calibrate_vol_surface")
    slices = out["vol_surface_spec_svi"]["payload"]["slices"]
    spot = gen.SYNTHETIC_SPX_STEEP_WINGS["spot"]
    sheet_strikes = sorted({q["strike"] for q in request["sheet"]["quotes"]})
    assert len(slices) == 8
    for sl in slices:
        assert sheet_strikes[0] <= sl["strike_min"] < sl["strike_max"] <= sheet_strikes[-1]
        assert sl["strike_min"] == pytest.approx(0.60 * spot)
    # The raw wings beyond the output range (50 % .. 150 %) carry butterfly
    # arbitrage on the long slices: reported, never enforced.
    beyond = [e for e in out["expiries"]
              if "beyond the output strike range (reported only" in e.get("butterfly_action", "")]
    assert len(beyond) >= 3, [e.get("butterfly_action") for e in out["expiries"]]
    for e in beyond:
        assert e["butterfly_min_g"] >= -1e-5  # the output range itself is clean
        assert "min g -" in e["butterfly_action"]


def test_note_on_svi_wings_prices_with_quoted_strike_edges(client, data_dir):
    request = load_json(data_dir / SPX_2Y)
    sl = _vols(request)[0]["payload"]["slices"]
    assert all("strike_min" in s and "strike_max" in s for s in sl)
    assert request["notes"][0]["monte_carlo"]["steps_per_year"] == 252
    note = _ok(client, request)["notes"][0]
    assert note["npv"] > 0.0


def test_note_on_svi_wings_without_strike_edges_is_400(client, data_dir):
    """The reproduction: the same slices without the quoted range read the
    raw put wing at 30 % of the spot at t = 1.63 years, inside the expiry
    range (last slice 2.46 years), where its local variance is negative."""
    request = load_json(data_dir / SPX_2Y)
    for s in _vols(request)[0]["payload"]["slices"]:
        s.pop("strike_min")
        s.pop("strike_max")
    _400(client, request, "could not be evaluated along the simulated paths",
         "negative local vol^2", "SPX_SYN_VOL")


@pytest.mark.parametrize("edit,needle", [
    ({"strike_min": 0.0}, ".strike_min must be a finite strike > 0"),
    ({"strike_max": -1.0}, ".strike_max must be a finite strike > 0"),
    ({"strike_min": 9000.0, "strike_max": 8000.0}, ".strike_min must be below strike_max"),
])
def test_svi_strike_edge_errors(client, data_dir, edit, needle):
    request = load_json(data_dir / SPX_2Y)
    _vols(request)[0]["payload"]["slices"][2].update(edit)
    _400(client, request, "SviVolSpec.slices[2]" + needle)


def test_one_slice_without_edges_leaves_that_side_open(client, data_dir):
    """The surface range is [min strike_min, max strike_max]: one slice
    without strike_min opens the lower side for the whole surface, and the
    put wing fails again."""
    request = load_json(data_dir / SPX_2Y)
    _vols(request)[0]["payload"]["slices"][3].pop("strike_min")
    _400(client, request, "negative local vol^2")
