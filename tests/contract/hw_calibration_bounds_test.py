"""Joint Hull-White calibration: bounds on "a" and the flatness diagnostic.

A joint (a, sigma) Levenberg-Marquardt fit on a market ATM matrix is flat in
"a" below ~1e-3: the fitted value there is decided by engine noise (it moved
2.3x between two QuantLib builds on the same request). The engine now keeps a
joint fit inside [a_lower, a_upper] (defaults 1e-3 / 1.0, overridable on
SwaptionHwCalibrationSpec) and reports how well the surface pins "a":
`a_flatness` = 1 - rmse / max(rmse(2a), rmse(a/2)) at the calibrated sigma and
`a_poorly_identified` = a_flatness < 0.01. Both are present iff calibrate_a,
on calibrate-swaption-model and as `used_hw_a_*` on an inline-calibrated
swaption.
"""

import copy
import json

from ql_reference import load_json


def _post(client, product, request):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _calib_spec(req):
    for m in req["pricing"]["volatility"]["models"]:
        if m["id"] == req["model_id"]:
            return m["payload"]["hw_calibration"]
    raise AssertionError("model not found")


def test_market_matrix_joint_fit_is_bounded_and_flagged(client, data_dir):
    req = load_json(data_dir / "vol/calibrate_swaption_model_request.json")
    r = _post(client, "calibrate_swaption_model", req)
    assert r.status_code == 200, r.text[:300]
    out = r.json()
    # The unconstrained optimum of this market-style matrix sat at a ~ 2.8e-4
    # on the flat plateau; the fit now stops at the default lower bound.
    assert out["hw_a"] >= 0.001 - 1e-12
    assert out["hw_a"] <= 1.0
    assert "a_flatness" in out and "a_poorly_identified" in out
    assert 0.0 <= out["a_flatness"] <= 1.0
    assert out["a_poorly_identified"] is True
    assert out["a_flatness"] < 0.01


def test_bounds_override_through_request_calibration(client, data_dir):
    req = load_json(data_dir / "vol/calibrate_swaption_model_request.json")
    req = copy.deepcopy(req)
    spec = copy.deepcopy(_calib_spec(req))
    spec["a_lower"] = 0.05
    spec["a_upper"] = 0.20
    req["calibration"] = spec
    r = _post(client, "calibrate_swaption_model", req)
    assert r.status_code == 200, r.text[:300]
    out = r.json()
    assert 0.05 - 1e-12 <= out["hw_a"] <= 0.20 + 1e-12


def test_invalid_bounds_are_400(client, data_dir):
    req = load_json(data_dir / "vol/calibrate_swaption_model_request.json")
    for lower, upper, needle in ((0.0, 1.0, "a_lower"), (0.5, 0.1, "a_upper")):
        rr = copy.deepcopy(req)
        spec = copy.deepcopy(_calib_spec(rr))
        spec["a_lower"], spec["a_upper"] = lower, upper
        rr["calibration"] = spec
        r = _post(client, "calibrate_swaption_model", rr)
        assert r.status_code == 400, r.text[:300]
        assert f"SwaptionHwCalibrationSpec.{needle}" in r.json()["error"]


def test_sigma_only_fit_has_no_flatness_fields_and_ignores_bounds(client, data_dir):
    req = load_json(data_dir / "vol/calibrate_swaption_model_request.json")
    req = copy.deepcopy(req)
    spec = copy.deepcopy(_calib_spec(req))
    spec["calibrate_a"] = False
    spec["a_init"] = 0.0002  # below a_lower: allowed, the bounds do not apply
    req["calibration"] = spec
    r = _post(client, "calibrate_swaption_model", req)
    assert r.status_code == 200, r.text[:300]
    out = r.json()
    assert out["hw_a"] == 0.0002
    assert "a_flatness" not in out
    assert "a_poorly_identified" not in out


def test_well_identified_surface_is_not_flagged(client, data_dir):
    # Market vols generated from HullWhite(a=0.05, sigma=0.008): the fit
    # round-trips inside the box and the diagnostic says "a" is pinned.
    req = load_json(data_dir / "vol" / "hwcal_eur_atm_matrix_roundtrip.json")
    r = _post(client, "calibrate_swaption_model", req)
    assert r.status_code == 200, r.text[:300]
    out = r.json()
    assert abs(out["hw_a"] - 0.05) < 1e-4
    assert out["a_poorly_identified"] is False
    assert out["a_flatness"] > 0.5


def test_inline_calibrated_swaption_carries_used_hw_flatness(client, data_dir):
    req = load_json(data_dir / "swaption/swaption_bermudan_hw_calibrated.json")
    r = _post(client, "swaption", req)
    assert r.status_code == 200, r.text[:300]
    row = r.json()["swaptions"][0]
    assert row["used_hw_a"] >= 0.001 - 1e-12
    assert "used_hw_a_flatness" in row
    assert row["used_hw_a_poorly_identified"] is True
