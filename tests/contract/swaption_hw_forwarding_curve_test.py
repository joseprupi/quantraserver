"""Hull-White swaptions and calibrations whose index projects on a forwarding
curve other than the discount curve.

QuantLib's TreeSwaptionEngine and JamshidianSwaptionEngine value the floating
leg off the model's (discount) curve. When the floating / overnight leg's
`forwarding_curve` (or `hw_calibration.forwarding_curve_id`) names a curve
other than the discount curve, the engine prices with QuantLib's
FdHullWhiteSwaptionEngine instead, which discounts on the model's curve and
projects the index on its own curve. These tests pin that path against
QuantLib-Python running the same engine, and the requests the FD engine
cannot value (400 naming the field). Same-curve requests keep the tree /
Jamshidian engines and are covered by the functional catalog.
"""

import copy

import pytest

import ql_reference
from ql_reference import load_json

FWD = "euribor_6m_fwd"
BASIS = 0.0020
BERMUDAN_FWD = "swaption/swpt_eur_bermudan_3ex_into_5y_hw_explicit_fwd_basis_20bp.json"
CALIB_FWD = "vol/hwcal_eur_atm_matrix_sigma_only_fwd_basis_20bp.json"
# Same QuantLib FD engine on both sides (bit for bit in practice; see
# HW_FD_TOLERANCE in tests/functional/manifest.py). Calibrated parameters as
# CALIBRATION_TOLERANCE there.
NPV_TOL = 1e-6
SIGMA_TOL = 1e-7


def _post(client, product, request):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _error(client, product, request):
    r = _post(client, product, request)
    assert r.status_code == 400, f"{r.status_code}: {r.text[:400]}"
    return r.json()["error"]


def _row(client, request):
    rows = client.price("swaption", request)["swaptions"]
    assert len(rows) == 1
    return rows[0]


def _shifted_curve(curve, new_id, shift=BASIS):
    c = copy.deepcopy(curve)
    c["id"] = new_id
    for p in c["points"]:
        p["point"]["rate"] += shift
    return c


def _curve(request, curve_id):
    return next(c for c in request["pricing"]["rates"]["curves"] if c["id"] == curve_id)


def _hw_explicit(model_id="hw_explicit", a=0.03, sigma=0.01, steps=100):
    return {"id": model_id, "payload_type": "SwaptionModelSpec",
            "payload": {"model_type": "HullWhiteLattice", "lattice_steps": steps,
                        "param_mode": "Explicit", "hw_a": a, "hw_sigma": sigma}}


# --------------------------------------------------------------------------- #
# Requests built from the shipped examples
# --------------------------------------------------------------------------- #

def _ois_bermudan(data_dir, lookback_days=0):
    """The ESTR OIS swaption example as a physically settled Bermudan on an
    explicit Hull-White model, ESTR projected 20bp over the discount curve."""
    req = load_json(data_dir / "swaption/swpt_eur_ois_1y5y_payer_estr.json")
    curves = req["pricing"]["rates"]["curves"]
    curves.append(_shifted_curve(_curve(req, "EUR_OIS"), "EUR_OIS_FWD"))
    req["pricing"]["volatility"]["models"] = [_hw_explicit()]
    entry = req["swaptions"][0]
    entry["model"] = "hw_explicit"
    sw = entry["swaption"]
    sw["exercise_type"] = "Bermudan"
    sw.pop("exercise_date")
    sw["exercise_dates"] = ["2026-01-15", "2027-01-15", "2028-01-14"]
    sw["settlement_type"] = "Physical"
    sw["settlement_method"] = "PhysicalOTC"
    leg = sw["underlying"]["overnight_leg"]
    leg["forwarding_curve"] = "EUR_OIS_FWD"
    leg["coupons"]["lookback_days"] = lookback_days
    return req


def _calibrated_bermudan(data_dir):
    """The calibrated Bermudan example with Euribor projected 20bp over the
    discount curve and a sigma-only fit (a well-conditioned calibration)."""
    req = load_json(data_dir / "swaption/swaption_bermudan_hw_calibrated.json")
    req["pricing"]["rates"]["curves"].append(_shifted_curve(_curve(req, "discount"), FWD))
    calib = req["pricing"]["volatility"]["models"][0]["payload"]["hw_calibration"]
    calib["forwarding_curve_id"] = FWD
    calib["calibrate_a"] = False
    calib["a_init"] = 0.03
    req["swaptions"][0]["swaption"]["underlying"]["floating_leg"]["forwarding_curve"] = FWD
    return req


def _with_explicit_model(request, model_id, a, sigma):
    """`request` with model `model_id` replaced by explicit (a, sigma) on the
    same lattice_steps."""
    out = copy.deepcopy(request)
    models = out["pricing"]["volatility"]["models"]
    i = next(k for k, m in enumerate(models) if m["id"] == model_id)
    steps = models[i]["payload"].get("lattice_steps", 50)
    models[i] = _hw_explicit(model_id, a, sigma, steps)
    return out


# --------------------------------------------------------------------------- #
# Priced on the FD engine, equal to QuantLib-Python
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("lookback_days", [0, 2])
def test_ois_bermudan_matches_quantlib_fd_engine(client, data_dir, lookback_days):
    """OIS underlyings go through QuantLib's FD engine too (it rebuilds the
    OvernightIndexedSwap on the forwarding curve). A 2-day lookback still
    observes on or after each exercise date, so it prices."""
    req = _ois_bermudan(data_dir, lookback_days)
    npv = _row(client, req)["npv"]
    expected = ql_reference.price_swaption_ql(req)
    assert npv == pytest.approx(expected, abs=NPV_TOL), (npv, expected)


def test_inline_calibration_and_pricing_match_quantlib_fd_engine(client, data_dir):
    """Param_mode Calibrate with a forwarding curve: the helpers and the
    Bermudan are both priced on the FD engine. Sigma equals QuantLib-Python's
    FD calibration of the same helpers; the NPV equals the FD price at it."""
    req = _calibrated_bermudan(data_dir)
    row = _row(client, req)
    model_id = req["swaptions"][0]["model"]
    ref = ql_reference.calibrate_swaption_model_ql(
        {"pricing": req["pricing"], "model_id": model_id})
    assert row["used_hw_a"] == 0.03
    assert row["used_hw_sigma"] == pytest.approx(ref["hw_sigma"], abs=SIGMA_TOL)
    assert row["used_hw_rmse"] == pytest.approx(ref["rmse"], abs=SIGMA_TOL)
    expected = ql_reference.price_swaption_ql(
        _with_explicit_model(req, model_id, ref["hw_a"], ref["hw_sigma"]))
    # Sigma agrees to ~1e-12; at a vega of ~1e6 per unit sigma that is ~1e-6.
    assert row["npv"] == pytest.approx(expected, abs=1e-4), (row["npv"], expected)


def test_callable_bond_calibration_projects_on_forwarding_curve(client, data_dir):
    """A callable bond's coupons are fixed, so its tree pricing is unchanged;
    only a calibration whose helpers project on a forwarding curve moves (FD
    helpers). The bond equals QuantLib-Python's tree price at QuantLib-Python's
    FD-calibrated parameters."""
    cal = load_json(data_dir / CALIB_FWD)
    bond = load_json(data_dir / "callable_bonds/cfrb_eur_8y_5pct_call_100_itm.json")
    req = {"pricing": copy.deepcopy(cal["pricing"]), "bonds": copy.deepcopy(bond["bonds"])}
    model = req["pricing"]["volatility"]["models"][0]
    model["payload"]["hw_calibration"] = copy.deepcopy(cal["calibration"])
    req["bonds"][0]["model"] = model["id"]
    npv = client.price("callable_fixed_rate_bond", req)["bonds"][0]["npv"]

    ref = ql_reference.calibrate_swaption_model_ql(
        {"pricing": req["pricing"], "model_id": model["id"]})
    expected = ql_reference.price_callable_fixed_rate_bond_ql(
        _with_explicit_model(req, model["id"], ref["hw_a"], ref["hw_sigma"]))
    assert npv == pytest.approx(expected, abs=1e-3), (npv, expected)


# --------------------------------------------------------------------------- #
# Requests the FD engine cannot value: 400 naming the field
# --------------------------------------------------------------------------- #

def test_forwarding_curve_day_counter_must_match_discount_curve(client, data_dir):
    req = load_json(data_dir / BERMUDAN_FWD)
    _curve(req, FWD)["day_counter"] = "Actual360"
    msg = _error(client, "swaption", req)
    for needle in (f"forwarding_curve '{FWD}'", "discounting_curve 'discount'",
                   "day counter"):
        assert needle in msg, msg


def test_forwarding_curve_reference_date_must_match_discount_curve(client, data_dir):
    req = load_json(data_dir / BERMUDAN_FWD)
    _curve(req, FWD)["reference_date"] = "2025-01-17"
    msg = _error(client, "swaption", req)
    for needle in (f"forwarding_curve '{FWD}'", "discounting_curve 'discount'",
                   "reference date"):
        assert needle in msg, msg


@pytest.mark.parametrize("field,value", [("day_counter", "Actual360"),
                                         ("reference_date", "2025-01-17")])
def test_calibration_curves_must_share_day_counter_and_reference_date(
        client, data_dir, field, value):
    req = load_json(data_dir / CALIB_FWD)
    _curve(req, FWD)[field] = value
    msg = _error(client, "calibrate_swaption_model", req)
    for needle in (f"forwarding_curve_id '{FWD}'", "discount_curve_id 'discount'",
                   field.replace("_", " ")):
        assert needle in msg, msg


def test_american_exercise_with_forwarding_curve_is_400(client, data_dir):
    """QuantLib's FD engine maps only the exercise dates themselves, so it
    cannot value American exercise. Never falls back to the basis-free tree."""
    req = load_json(data_dir / "swaption/swpt_eur_american_2y_window_hw_explicit.json")
    req["pricing"]["rates"]["curves"].append(_shifted_curve(_curve(req, "discount"), FWD))
    req["swaptions"][0]["swaption"]["underlying"]["floating_leg"]["forwarding_curve"] = FWD
    msg = _error(client, "swaption", req)
    assert "Swaption.exercise_type" in msg and "American" in msg, msg


def test_par_yield_cash_settlement_with_forwarding_curve_is_400(client, data_dir):
    """The FD engine values the swap at exercise and ignores the settlement
    method; a par-yield cash settlement would be priced as physical."""
    req = load_json(data_dir / BERMUDAN_FWD)
    sw = req["swaptions"][0]["swaption"]
    sw["settlement_type"] = "Cash"
    sw["settlement_method"] = "ParYieldCurve"
    msg = _error(client, "swaption", req)
    assert "Swaption.settlement_method" in msg and "ParYieldCurve" in msg, msg


def test_ois_lookback_before_exercise_date_is_400(client, data_dir):
    """A 5-day lookback makes the coupon starting two business days after an
    exercise date observe rates before it, which QuantLib's FD engine cannot
    project from that exercise date."""
    msg = _error(client, "swaption", _ois_bermudan(data_dir, lookback_days=5))
    assert "Swaption.underlying.overnight_leg.coupons.lookback_days" in msg, msg
