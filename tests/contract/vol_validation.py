"""Helpers shared by the contract suites: build a `/validate-vol-surface`
request on a surface `/calibrate-vol-surface` returned and post it.

The fit statistics, the local vol check and the Monte Carlo repricing of a
calibrated surface live on `/validate-vol-surface` since 0.16.7; the suites
that pinned them on the calibrate response read them from here (same
quotes, same filter, the calibrate response's forwards pasted, so the
numbers are the calibrate-time ones).
"""

import copy

PAYLOAD_KEYS = {"grid": "vol_surface_spec", "svi": "vol_surface_spec_svi",
                "ssvi": "vol_surface_spec_ssvi"}


def forwards_of(calibrate_response):
    """The calibrate response's forwards as pasted `forwards` entries."""
    return [{"expiry": f["expiry"], "usable": True, "forward": f["forward"],
             "implied_discount_factor": f["implied_discount_factor"]}
            for f in calibrate_response["forwards"]]


def validation_request(calibrate_request, calibrate_response, payload="grid", checks=None,
                       simulation=None, sheet_forwards=True, bands=None):
    """`payload` picks the returned surface (grid / svi / ssvi); with
    `sheet_forwards` the calibrate response's forwards are pasted (the
    market the surface was fitted on), otherwise the checks read the curves
    of `pricing`."""
    spec = calibrate_response[PAYLOAD_KEYS[payload]]
    pricing = copy.deepcopy(calibrate_request["pricing"])
    vol = pricing.setdefault("volatility", {})
    vol["vol_surfaces"] = [s for s in vol.get("vol_surfaces", []) if s["id"] != spec["id"]]
    vol["vol_surfaces"].append(spec)
    req = {
        "pricing": pricing,
        "volatility_id": spec["id"],
        "underlying_id": calibrate_request["sheet"]["underlying_id"],
        "discounting_curve": calibrate_request["discounting_curve"],
        "quotes": calibrate_request["sheet"],
    }
    if calibrate_request.get("filter") is not None:
        req["filter"] = calibrate_request["filter"]
    if sheet_forwards:
        req["forwards"] = forwards_of(calibrate_response)
    if checks is not None:
        req["checks"] = checks
    if simulation is not None:
        req["simulation"] = simulation
    if bands is not None:
        req["bands"] = bands
    return req


def post_validation(client, request):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS['validate_vol_surface']}", json=request)


def validate(client, calibrate_request, calibrate_response, payload="grid", **kwargs):
    r = post_validation(
        client, validation_request(calibrate_request, calibrate_response, payload, **kwargs))
    assert r.status_code == 200, r.text[:600]
    return r.json()
