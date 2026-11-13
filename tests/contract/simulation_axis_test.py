"""Curves of any day counter under a simulation; vols on its axis.

A structured note and /validate-vol-surface measure time in Actual/365
(Fixed) years from as_of_date and read every curve by date on that axis
(src/market/simulation_axis.h). What this suite locks:

  * relabelling: a zero curve put on Actual/360 with its continuous zero
    rates scaled by 360/365 has the same discount factor at every date; the
    validation of the synthetic Scotia NDX SVI example (discount curve,
    dividend curve, both) gives the same forwards, fit, arbitrage scan and
    simulation as the base request;
  * the same relabelling on a local-vol note (synthetic SPX SVI, 1 year), a
    constant-vol note (synthetic two-asset worst-of) and a quanto note (the
    quanto underlyings' own curves and dividend curves) gives the same NPV;
  * a vol off that axis (day counter other than Actual365Fixed, reference
    date other than as_of_date) is a 400 naming the vol and the field.
"""

import copy

import pytest

from ql_reference import load_json
from vol_validation import post_validation

VALIDATION_EXAMPLE = "vol_surface_validation/scotia_ndx_svi.json"
LOCAL_VOL_NOTE = "structured_note/synthetic_spx_1y_autocall_on_daily_expiry_svi.json"
CONSTANT_VOL_NOTE = "structured_note/synthetic_two_asset_worst_of_flat_vol_autocall.json"
QUANTO_NOTE = "structured_note/bnp_quad_index_classic_autocall_xs2741509801.json"

# The relabelled curve reads the same discount factors through the
# simulation-axis view; what is left is rounding of the zero rates and of
# the times (measured: below 1e-14 relative on forwards).
FORWARD_RTOL = 1e-10
# A Monte Carlo price on the relabelled curves: same paths, same seed, every
# curve read a few ulps apart; the local vol's finite differences in time
# and strike amplify those (measured: 9e-10 relative on the local-vol note,
# 0 on the constant-vol note).
NPV_RTOL = 1e-8
# The repricing of the quotes by simulation: the same amplification, on
# some deep quotes up to 2.5e-6 relative (measured: 7.5e-5 standard errors).
SIM_SE_TOL = 1e-3


def relabel(request, curve_ids):
    """The curves `curve_ids` on Actual/360 with every continuous zero rate
    scaled by 360/365: exp(-r' t360) = exp(-r t365) at every node, and the
    linear zero interpolation in time keeps that between nodes."""
    out = copy.deepcopy(request)
    seen = set()
    for c in out["pricing"]["rates"]["curves"]:
        if c["id"] not in curve_ids:
            continue
        assert c["day_counter"] == "Actual365Fixed" and c["bootstrap_trait"] == "InterpolatedZero"
        assert c["interpolator"] == "Linear"
        c["day_counter"] = "Actual360"
        for p in c["points"]:
            assert p["point_type"] == "ZeroRatePoint"
            assert p["point"]["compounding"] == "Continuous"
            p["point"]["zero_rate"] *= 360.0 / 365.0
        seen.add(c["id"])
    assert seen == set(curve_ids)
    return out


def _close(a, b, rtol):
    return abs(a - b) <= rtol * max(abs(a), abs(b), 1e-300)


def _validate(client, request):
    r = post_validation(client, request)
    assert r.status_code == 200, r.text[:600]
    return r.json()


def _note(client, request):
    endpoint = client.ENDPOINTS["structured_note"]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    assert r.status_code == 200, r.text[:600]
    return r.json()["notes"][0]


def _status(client, request, endpoint, status, *needles):
    if endpoint == "validate":
        r = post_validation(client, request)
    else:
        r = client.session.post(
            f"{client.base_url}/{client.ENDPOINTS['structured_note']}", json=request)
    assert r.status_code == status, (r.status_code, r.text[:600])
    for n in needles:
        assert n in r.text, (n, r.text[:600])


# --------------------------------------------------------------------------
# /validate-vol-surface
# --------------------------------------------------------------------------

@pytest.fixture(scope="module")
def validation_base(client, data_dir):
    request = load_json(data_dir / VALIDATION_EXAMPLE)
    return request, _validate(client, request)


@pytest.mark.parametrize("curves", [["USD_TSY"], ["NDX_DIV"], ["USD_TSY", "NDX_DIV"]],
                         ids=["discount", "dividend", "both"])
def test_validation_reads_curves_by_date(client, validation_base, curves):
    request, base = validation_base
    assert request["discounting_curve"] == "USD_TSY"
    assert request["pricing"]["equity"]["equity_underlyings"][0]["dividend_yield_curve_id"] \
        == "NDX_DIV"
    got = _validate(client, relabel(request, curves))

    assert len(got["expiries"]) == len(base["expiries"])
    for e, b in zip(got["expiries"], base["expiries"]):
        assert e["expiry"] == b["expiry"] and e["usable"] == b["usable"]
        assert e["time"] == b["time"]
        if b["usable"]:
            assert _close(e["forward"], b["forward"], FORWARD_RTOL), (e, b)
            assert _close(e["discount_factor"], b["discount_factor"], FORWARD_RTOL), (e, b)
            assert e["quotes_used"] == b["quotes_used"]

    for k in ("count", "inside"):
        assert got["fit"][k] == base["fit"][k], k
    assert got["fit"]["verdict"] == base["fit"]["verdict"]

    lv, blv = got["arbitrage"]["local_vol_check"], base["arbitrage"]["local_vol_check"]
    for k in ("arbitrage_free", "points", "refused"):
        assert lv[k] == blv[k], k
    assert got["arbitrage"]["arbitrage_free"] == base["arbitrage"]["arbitrage_free"]

    sim, bsim = got["simulation"], base["simulation"]
    for k in ("paths", "time_steps", "count", "inside", "within_noise"):
        assert sim[k] == bsim[k], k
    for q, bq in zip(sim["quotes"], bsim["quotes"]):
        assert q["strike"] == bq["strike"] and q["within_noise"] == bq["within_noise"]
        assert abs(q["mc_price"] - bq["mc_price"]) <= SIM_SE_TOL * max(bq["standard_error"], 1e-12), \
            (q, bq)


# --------------------------------------------------------------------------
# Structured notes
# --------------------------------------------------------------------------

def _note_curves(request):
    """The note curve and every underlying's dividend curve."""
    ids = {request["notes"][0]["discounting_curve"]}
    for u in request["pricing"]["equity"]["equity_underlyings"]:
        ids.add(u["dividend_yield_curve_id"])
    return sorted(ids)


@pytest.mark.parametrize("example", [LOCAL_VOL_NOTE, CONSTANT_VOL_NOTE],
                         ids=["local_vol", "constant_vol"])
def test_note_reads_curves_by_date(client, data_dir, example):
    request = load_json(data_dir / example)
    base = _note(client, request)
    got = _note(client, relabel(request, _note_curves(request)))
    assert got["paths"] == base["paths"]
    assert got["model"]["time_grid"] == base["model"]["time_grid"]
    assert _close(got["npv"], base["npv"], NPV_RTOL), (got["npv"], base["npv"])


def test_quanto_note_reads_curves_by_date(client, data_dir):
    """The quanto underlyings' own curves (CHF, EUR, JPY) and every dividend
    curve relabelled; the note's OIS curve stays as it is."""
    request = load_json(data_dir / QUANTO_NOTE)
    request["notes"][0]["monte_carlo"]["paths"] = 2048
    base = _note(client, request)
    ids = [c["id"] for c in request["pricing"]["rates"]["curves"]
           if c["bootstrap_trait"] == "InterpolatedZero"]
    assert {"CHF_FLAT", "EUR_FLAT", "JPY_FLAT"} <= set(ids)
    got = _note(client, relabel(request, ids))
    assert _close(got["npv"], base["npv"], NPV_RTOL), (got["npv"], base["npv"])


# --------------------------------------------------------------------------
# A vol off the simulation's axis
# --------------------------------------------------------------------------

def _vol_base(request, vol_id):
    spec = next(s for s in request["pricing"]["volatility"]["vol_surfaces"] if s["id"] == vol_id)
    return spec["payload"]["base"]


@pytest.mark.parametrize("field,value,needle", [
    ("day_counter", "Actual360", "base.day_counter must be Actual365Fixed"),
    ("reference_date", "2026-10-01", "base.reference_date must be the as_of_date 2026-09-30"),
])
def test_vol_off_the_axis_is_400_on_validate(client, data_dir, field, value, needle):
    request = load_json(data_dir / VALIDATION_EXAMPLE)
    _vol_base(request, request["volatility_id"])[field] = value
    _status(client, request, "validate", 400, f"volatility '{request['volatility_id']}'", needle)


@pytest.mark.parametrize("example", [LOCAL_VOL_NOTE, CONSTANT_VOL_NOTE],
                         ids=["local_vol", "constant_vol"])
def test_vol_off_the_axis_is_400_on_a_note(client, data_dir, example):
    request = load_json(data_dir / example)
    vol_id = request["notes"][0]["underlyings"][0]["volatility_id"]
    _vol_base(request, vol_id)["day_counter"] = "Actual360"
    _status(client, request, "note", 400, f"volatility '{vol_id}'",
            "base.day_counter must be Actual365Fixed", "got Actual/360")
