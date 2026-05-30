"""Error-contract suite (HTTP status per error-type policy).

Locks the error-type policy at the HTTP boundary: every throw site is one of
QuantraNotFound / QuantraInvalidArgument / QuantraNotImplemented /
QuantraError, which CallDataGeneric maps to gRPC NOT_FOUND / INVALID_ARGUMENT /
UNIMPLEMENTED / ABORTED and the JSON gateway maps to HTTP 404 / 400 / 501 / 500.
These scenarios drive product-pricing endpoints with malformed, missing-
reference, or unsupported-feature requests and assert the resulting HTTP status:

  404 (QuantraNotFound)        - a referenced id (curve, model, vol, credit
                                 curve) is well-formed but absent from pricing.
  400 (QuantraInvalidArgument) - the request is malformed (empty required
                                 list, missing required field) OR references a
                                 thing of the WRONG KIND (a model id that
                                 resolves to the wrong variant).
  501 (QuantraNotImplemented)  - a valid, well-formed request for a feature
                                 that is not built yet (e.g. a schema-ready but
                                 unimplemented curve helper).

Most scenarios drive product-pricing endpoints on purpose: their throws
propagate to the gRPC status. The sample-vol-surfaces query endpoint instead
follows a per-item error contract: a malformed TOP-LEVEL request (empty
queries) still hard-fails with an HTTP status, but a per-query failure (a
missing referenced id) is folded into that item's own error field and returned
at HTTP 200. The two sample-vol scenarios lock exactly that split — the first
asserts the 400 on empty top-level input, the second asserts a 200 carrying a
per-item error entry (validated by an optional body checker).
"""

import copy
import json

import pytest

from ql_reference import _normalize_period_fields_for_api


def _ec_post_status(client, product, request):
    endpoint = client.ENDPOINTS[product]
    r = client.session.post(
        f"{client.base_url}/{endpoint}",
        json=_normalize_period_fields_for_api(copy.deepcopy(request)),
    )
    return r.status_code, r.text


def _ec_set_field(arr_key, field, value):
    def f(req):
        req[arr_key][0][field] = value
        return req
    return f


def _ec_del_field(arr_key, field):
    def f(req):
        req[arr_key][0].pop(field, None)
        return req
    return f


def _ec_empty_list(arr_key):
    def f(req):
        req[arr_key] = []
        return req
    return f


def _ec_body_has_per_item_error(item_id_field, item_id, error_field="error"):
    """Body validator for the per-item error contract (list/query endpoints).

    Returns None when the response body is HTTP-200 JSON whose results list
    carries an entry identified by item_id_field == item_id with a non-empty
    error string; otherwise returns a human-readable failure reason. The error
    table is serialized by the JSON gateway as {"error_message": "..."}.
    """
    def check(body_text):
        try:
            body = json.loads(body_text)
        except Exception as e:
            return f"response body is not JSON: {e} :: {body_text[:160]}"
        results = body.get("results")
        if not isinstance(results, list) or not results:
            return f"expected non-empty 'results' list, got: {body_text[:160]}"
        for entry in results:
            if entry.get(item_id_field) == item_id:
                err = entry.get(error_field) or {}
                msg = err.get("error_message")
                if isinstance(msg, str) and msg.strip():
                    return None
                return (f"result for {item_id_field}={item_id!r} has no non-empty "
                        f"error_message: {json.dumps(entry)[:200]}")
        return f"no result entry for {item_id_field}={item_id!r} in: {body_text[:200]}"
    return check


def _ec_flip_model_type(new_type):
    def f(req):
        for m in req["pricing"].get("models", []):
            m["payload_type"] = new_type
        return req
    return f


def _ec_unimplemented_curve_point():
    # Replace the first curve's first point with a schema-ready-but-unbuilt
    # helper (FxSwapHelper). The point parses into a valid FlatBuffer, then the
    # term-structure point parser throws QuantraNotImplemented when the curve is
    # built during pricing -> gRPC UNIMPLEMENTED -> HTTP 501. FxSwapHelper is
    # used because it has no FlatBuffer-required fields, so an empty payload is
    # accepted by the JSON parser and the throw is reached deterministically.
    def f(req):
        req["pricing"]["curves"][0]["points"][0] = {
            "point_type": "FxSwapHelper",
            "point": {},
        }
        return req
    return f


# (label, product, filename, expected_http_status, mutate_fn[, body_check])
SCENARIOS = [
    # ---- 404 NOT_FOUND: referenced id absent from pricing ----
    ("ec:404 fixed_rate_bond discounting_curve missing", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 404, _ec_set_field("bonds", "discounting_curve", "no_such_curve")),
    ("ec:404 floating_rate_bond forwarding_curve missing", "floating_rate_bond",
     "floating_rate_bond_request.json", 404, _ec_set_field("bonds", "forwarding_curve", "no_such_curve")),
    ("ec:404 vanilla_swap discounting_curve missing", "vanilla_swap",
     "vanilla_swap_request.json", 404, _ec_set_field("swaps", "discounting_curve", "no_such_curve")),
    ("ec:404 fra forwarding_curve missing", "fra",
     "fra_request.json", 404, _ec_set_field("fras", "forwarding_curve", "no_such_curve")),
    ("ec:404 swaption model missing", "swaption",
     "swaption_request.json", 404, _ec_set_field("swaptions", "model", "no_such_model")),
    ("ec:404 swaption volatility missing", "swaption",
     "swaption_request.json", 404, _ec_set_field("swaptions", "volatility", "no_such_vol")),
    ("ec:404 cap_floor model missing", "cap_floor",
     "cap_floor_request.json", 404, _ec_set_field("cap_floors", "model", "no_such_model")),
    ("ec:404 cds credit_curve missing", "cds",
     "cds_request.json", 404, _ec_set_field("cds_list", "credit_curve_id", "no_such_credit")),
    # ---- 400 INVALID_ARGUMENT: wrong-kind reference (id resolves to wrong variant) ----
    ("ec:400 swaption model is wrong kind", "swaption",
     "swaption_request.json", 400, _ec_flip_model_type("CapFloorModelSpec")),
    ("ec:400 cap_floor model is wrong kind", "cap_floor",
     "cap_floor_request.json", 400, _ec_flip_model_type("SwaptionModelSpec")),
    # ---- 400 INVALID_ARGUMENT: malformed request (empty list / missing required field) ----
    ("ec:400 fixed_rate_bond empty bonds list", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400, _ec_empty_list("bonds")),
    ("ec:400 vanilla_swap empty swaps list", "vanilla_swap",
     "vanilla_swap_request.json", 400, _ec_empty_list("swaps")),
    ("ec:400 swaption missing required model field", "swaption",
     "swaption_request.json", 400, _ec_del_field("swaptions", "model")),
    ("ec:400 fixed_rate_bond missing discounting_curve field", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400, _ec_del_field("bonds", "discounting_curve")),
    # ---- 501 UNIMPLEMENTED: valid request, feature not built yet ----
    ("ec:501 swaption curve uses unimplemented helper", "swaption",
     "swaption_request.json", 501, _ec_unimplemented_curve_point()),
    # ---- sample-vol-surfaces per-item error contract (list/query endpoint) ----
    # (A) Malformed TOP-LEVEL input -> transport error. Empty queries trips
    #     toInputs validation, which propagates as INVALID_ARGUMENT -> HTTP 400.
    ("ec:400 sample_vol_surfaces empty queries list", "sample_vol_surfaces",
     "sample_vol_surfaces_request.json", 400, _ec_empty_list("queries")),
    # (B) Per-item failure -> HTTP 200 with a per-item error entry. Pointing a
    #     query at a non-existent vol-surface id resolves the top-level request
    #     fine but fails when that query is sampled, so the error is folded into
    #     that result's error field rather than failing the whole batch.
    ("ec:200 sample_vol_surfaces per-item error on missing vol id", "sample_vol_surfaces",
     "sample_vol_surfaces_request.json", 200,
     _ec_set_field("queries", "vol_id", "no_such_vol_surface_xyz"),
     _ec_body_has_per_item_error("vol_id", "no_such_vol_surface_xyz")),
]


@pytest.mark.parametrize(
    "scenario",
    SCENARIOS,
    ids=[s[0] for s in SCENARIOS],
)
def test_error_contract(client, data_dir, scenario):
    label, product, filename, expected, mutate = scenario[:5]
    # Optional 6th element: a body validator returning None on success or a
    # failure reason string. Used by the per-item (HTTP 200) error scenarios.
    body_check = scenario[5] if len(scenario) > 5 else None

    filepath = data_dir / filename
    assert filepath.exists(), f"{filepath} not found"

    with open(filepath) as fh:
        req = json.load(fh)
    req = mutate(req)

    status, body = _ec_post_status(client, product, req)
    assert status == expected, (
        f"{label}: expected HTTP {expected}, got {status} :: {body[:160]}"
    )

    if body_check is not None:
        detail = body_check(body)
        assert detail is None, (
            f"{label}: HTTP {status} as expected but body check failed: {detail}"
        )
