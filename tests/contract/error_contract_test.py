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

import json

import pytest


def _ec_post_status(client, product, request):
    endpoint = client.ENDPOINTS[product]
    # Example payloads are stored in the canonical nested schema; POST verbatim.
    r = client.session.post(
        f"{client.base_url}/{endpoint}",
        json=request,
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
        for m in req["pricing"].get("volatility", {}).get("models", []):
            m["payload_type"] = new_type
        return req
    return f


def _ec_del_curve_point_field(point_type, field):
    """Drop `field` from the first curve helper of `point_type`.

    Pre-fix these requests SIGSEGV'd the worker (null flatbuffers::String
    deref); post-fix they must return a clean INVALID_ARGUMENT and leave the
    server alive for the rest of the suite.
    """
    def f(req):
        for curve in req["pricing"]["rates"]["curves"]:
            for p in curve.get("points", []):
                if p.get("point_type") == point_type:
                    p["point"].pop(field, None)
                    return req
        raise AssertionError(f"no {point_type} point found in request")
    return f


def _ec_del_curve_field(field):
    """Drop a top-level convention field from the first pricing curve
    (TermStructure). day_counter and interpolator are presence-required: an
    omitted convention is an error, never an alphabetical-0 default."""
    def f(req):
        req["pricing"]["rates"]["curves"][0].pop(field, None)
        return req
    return f


def _ec_future_helper_missing_start_date():
    """Replace the first curve point with a FutureHelper that omits
    future_start_date (optional in the schema, deref'd by the parser)."""
    def f(req):
        req["pricing"]["rates"]["curves"][0]["points"][0] = {
            "point_type": "FutureHelper",
            "point": {
                "rate": 0.02,
                "future_months": 3,
                "calendar": "TARGET",
                "business_day_convention": "ModifiedFollowing",
                "day_counter": "Actual360",
            },
        }
        return req
    return f


def _ec_abusive_schedule(arr_key, product_key):
    """Stretch the product schedule to ~300 years Daily (~108k
    implied periods). Must be rejected fast with INVALID_ARGUMENT instead of
    pinning the worker for the full gRPC deadline."""
    def f(req):
        sch = req[arr_key][0][product_key]["schedule"]
        sch["effective_date"] = "1901-01-02"
        sch["termination_date"] = "2199-12-30"
        sch["frequency"] = "Daily"
        return req
    return f
def _ec_set_schedule_date(arr_key, product_key, field, value):
    """Set a schedule date field to a specific (possibly malformed) value.
    Used to check that the wire date format is strictly ISO-8601 YYYY-MM-DD."""
    def f(req):
        req[arr_key][0][product_key]["schedule"][field] = value
        return req
    return f


def _ec_del_schedule_field(arr_key, product_key, field):
    """Drop a convention field from the product's schedule. The Schedule
    convention enums are presence-required: an omitted convention is an error,
    never an alphabetical-0 default (e.g. calendar Argentina)."""
    def f(req):
        req[arr_key][0][product_key]["schedule"].pop(field, None)
        return req
    return f


def _ec_del_swap_leg_field(arr_key, swap_key, leg_key, field):
    """Drop a convention field from a swap leg. The swap-leg convention enums
    (day_counter, payment_convention) are presence-required: an omitted
    convention is an error, never an alphabetical-0 default."""
    def f(req):
        req[arr_key][0][swap_key][leg_key].pop(field, None)
        return req
    return f


def _ec_del_instrument_field(arr_key, product_key, field):
    """Drop a convention field from the product instrument block. The FRA,
    cap/floor and CDS convention enums (day_counter, calendar,
    business_day_convention) are presence-required: an omitted convention is an
    error, never an alphabetical-0 default."""
    def f(req):
        req[arr_key][0][product_key].pop(field, None)
        return req
    return f


def _ec_del_bond_field(arr_key, bond_key, field):
    """Drop a convention field from the bond block. The bond convention enums
    (accrual_day_counter, payment_convention) are presence-required: an omitted
    convention is an error, never an alphabetical-0 default."""
    def f(req):
        req[arr_key][0][bond_key].pop(field, None)
        return req
    return f


def _ec_del_yield_field(arr_key, field):
    """Drop a convention field from the yield block carried by a bond request.
    The Yield convention enums (day_counter, frequency) are presence-required:
    an omitted convention is an error, never an alphabetical-0 default."""
    def f(req):
        req[arr_key][0]["yield"].pop(field, None)
        return req
    return f


def _ec_del_vol_base_field(field):
    """Drop a convention field from the first vol surface's base spec. The
    IrVol/BlackVol base convention enums are presence-required: an omitted
    convention is an error, never an alphabetical-0 default."""
    def f(req):
        surfaces = req["pricing"]["volatility"]["vol_surfaces"]
        payload = surfaces[0]["payload"]
        # Descend through nested payload wrappers until the base spec is found.
        while "base" not in payload and "payload" in payload:
            payload = payload["payload"]
        payload["base"].pop(field, None)
        return req
    return f


def _ec_del_optionlet_vol_field(field):
    """Drop a convention field from the first coupon pricer's constant optionlet
    volatility block. The ConstantOptionletVolatility convention enums (calendar,
    business_day_convention, day_counter) are presence-required: an omitted
    convention is an error, never an alphabetical-0 default."""
    def f(req):
        pricers = req["pricing"]["rates"]["coupon_pricers"]
        ov = pricers[0]["black_ibor_coupon_pricer"]["optionlet_volatility"]
        ov.pop(field, None)
        return req
    return f


def _ec_set_credit_curve_field(field, value):
    def f(req):
        req["pricing"]["credit"]["credit_curves"][0][field] = value
        return req
    return f


def _ec_del_credit_curve_field(field):
    def f(req):
        req["pricing"]["credit"]["credit_curves"][0].pop(field, None)
        return req
    return f


def _ec_identity(req):
    return req


def _ec_set_model_payload_field(field, value):
    def f(req):
        for m in req["pricing"].get("volatility", {}).get("models", []):
            m["payload"][field] = value
        return req
    return f


def _ec_set_nested_field(arr_key, obj_key, field, value):
    def f(req):
        req[arr_key][0][obj_key][field] = value
        return req
    return f


def _ec_body_contains(substr):
    """Body validator: the (error) response body must carry substr somewhere.

    Backend errors surface the real exception text in the JSON envelope's
    `message` field; a plain substring check on the raw body asserts the
    fail-closed throw was actually reached (not e.g. a parse-layer 400).
    """
    def check(body_text):
        if substr in body_text:
            return None
        return f"expected body to contain {substr!r}, got: {body_text[:200]}"
    return check


def _ec_unimplemented_curve_point():
    # Replace the first curve's first point with a schema-ready-but-unbuilt
    # helper (FxSwapHelper). The point parses into a valid FlatBuffer, then the
    # term-structure point parser throws QuantraNotImplemented when the curve is
    # built during pricing -> gRPC UNIMPLEMENTED -> HTTP 501. FxSwapHelper is
    # used because it has no FlatBuffer-required fields, so an empty payload is
    # accepted by the JSON parser and the throw is reached deterministically.
    def f(req):
        req["pricing"]["rates"]["curves"][0]["points"][0] = {
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
    # ---- 400 INVALID_ARGUMENT: swap-leg convention presence ----
    # A swap-leg convention enum omitted from the request must be rejected, not
    # silently defaulted to the alphabetical-0 value.
    ("ec:400 vanilla_swap fixed leg missing day_counter", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_del_swap_leg_field("swaps", "vanilla_swap", "fixed_leg", "day_counter"),
     _ec_body_contains("SwapFixedLeg.day_counter is required")),
    ("ec:400 vanilla_swap fixed leg missing payment_convention", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_del_swap_leg_field("swaps", "vanilla_swap", "fixed_leg", "payment_convention"),
     _ec_body_contains("SwapFixedLeg.payment_convention is required")),
    # ---- 400 INVALID_ARGUMENT: bond / yield convention presence ----
    # A bond or yield convention enum omitted from the request must be rejected,
    # not silently defaulted to the alphabetical-0 value.
    ("ec:400 fixed_rate_bond missing accrual_day_counter", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_bond_field("bonds", "fixed_rate_bond", "accrual_day_counter"),
     _ec_body_contains("FixedRateBond.accrual_day_counter is required")),
    ("ec:400 fixed_rate_bond yield missing compounding", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_yield_field("bonds", "compounding"),
     _ec_body_contains("Yield.compounding is required")),
    ("ec:400 fixed_rate_bond yield missing frequency", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_yield_field("bonds", "frequency"),
     _ec_body_contains("Yield.frequency is required")),
    # ---- 400 INVALID_ARGUMENT: null-deref guards (optional date fields
    #      omitted on curve helpers; pre-guard these crashed the worker) ----
    ("ec:400 fixed_rate_bond curve BondHelper missing issue_date", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400, _ec_del_curve_point_field("BondHelper", "issue_date")),
    ("ec:400 fixed_rate_bond curve FutureHelper missing future_start_date", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400, _ec_future_helper_missing_start_date()),
    # ---- 400 INVALID_ARGUMENT: identity / structural field presence ----
    # Structural sub-tables and identity strings a product cannot be built
    # without must fail loudly with a named message, never be silently omitted.
    ("ec:400 fixed_rate_bond curve BondHelper missing schedule", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_curve_point_field("BondHelper", "schedule"),
     _ec_body_contains("BondHelper.schedule is required")),
    ("ec:400 vanilla_swap missing discounting_curve reference", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_del_field("swaps", "discounting_curve"),
     _ec_body_contains("PriceVanillaSwap.discounting_curve is required")),
    ("ec:400 cds missing credit_curve_id reference", "cds",
     "cds_request.json", 400,
     _ec_del_field("cds_list", "credit_curve_id"),
     _ec_body_contains("PriceCDS entry requires credit_curve_id")),
    # ---- 400 INVALID_ARGUMENT: unbounded schedule generation guard ----
    ("ec:400 fixed_rate_bond 300y daily schedule rejected", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400, _ec_abusive_schedule("bonds", "fixed_rate_bond")),
    # ---- 400 INVALID_ARGUMENT: wire date format is strictly ISO-8601 ----
    # Dates on the wire must be YYYY-MM-DD. A slash format, an impossible
    # calendar date, or free text must all be rejected with a named 400 that
    # names the offending value, never silently normalized or 500'd.
    ("ec:400 fixed_rate_bond slash date rejected", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_date("bonds", "fixed_rate_bond", "effective_date", "2024/06/15"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 fixed_rate_bond impossible date rejected", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_date("bonds", "fixed_rate_bond", "effective_date", "2024-02-30"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 fixed_rate_bond textual date rejected", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_date("bonds", "fixed_rate_bond", "effective_date", "June 5th"),
     _ec_body_contains("expected YYYY-MM-DD")),
    # ---- 400 INVALID_ARGUMENT: schedule convention presence ----
    # A Schedule convention enum omitted from the request must be rejected, not
    # silently defaulted to the alphabetical-0 value (calendar Argentina, etc.).
    ("ec:400 fixed_rate_bond schedule missing calendar", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_schedule_field("bonds", "fixed_rate_bond", "calendar"),
     _ec_body_contains("Schedule.calendar is required")),
    ("ec:400 fixed_rate_bond schedule missing convention", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_schedule_field("bonds", "fixed_rate_bond", "convention"),
     _ec_body_contains("Schedule.convention is required")),
    # ---- 400 INVALID_ARGUMENT: curve-helper / term-structure convention presence ----
    # A curve-helper convention enum omitted from the request must be rejected,
    # not silently defaulted to the alphabetical-0 value. Same for the
    # TermStructure's own day_counter / interpolator.
    ("ec:400 fixed_rate_bond curve DepositHelper missing day_counter", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_curve_point_field("DepositHelper", "day_counter"),
     _ec_body_contains("DepositHelper.day_counter is required")),
    ("ec:400 fixed_rate_bond curve TermStructure missing interpolator", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_curve_field("interpolator"),
     _ec_body_contains("TermStructure.interpolator is required")),
    # ---- 400 INVALID_ARGUMENT: volatility base convention presence ----
    # An IrVol/BlackVol base convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value.
    ("ec:400 swaption vol base missing day_counter", "swaption",
     "swaption_request.json", 400,
     _ec_del_vol_base_field("day_counter"),
     _ec_body_contains("IrVolBaseSpec.day_counter is required")),
    ("ec:400 swaption vol base missing calendar", "swaption",
     "swaption_request.json", 400,
     _ec_del_vol_base_field("calendar"),
     _ec_body_contains("IrVolBaseSpec.calendar is required")),
    # ---- 400 INVALID_ARGUMENT: FRA / cap-floor / CDS convention presence ----
    # An FRA, cap/floor or CDS convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value.
    ("ec:400 fra missing calendar", "fra",
     "fra_request.json", 400,
     _ec_del_instrument_field("fras", "fra", "calendar"),
     _ec_body_contains("FRA.calendar is required")),
    ("ec:400 cap_floor missing business_day_convention", "cap_floor",
     "cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "cap_floor", "business_day_convention"),
     _ec_body_contains("CapFloor.business_day_convention is required")),
    ("ec:400 cds missing day_counter", "cds",
     "cds_request.json", 400,
     _ec_del_instrument_field("cds_list", "cds", "day_counter"),
     _ec_body_contains("CDS.day_counter is required")),
    # ---- 400 INVALID_ARGUMENT: FRA / cap-floor / CDS discriminator presence ----
    # The product-discriminator enums are presence-required: an omitted
    # discriminator must be rejected, not silently defaulted to the
    # alphabetical-0 value (Long FRA, Cap, or CDS protection Buyer).
    ("ec:400 fra missing fra_type", "fra",
     "fra_request.json", 400,
     _ec_del_instrument_field("fras", "fra", "fra_type"),
     _ec_body_contains("FRA.fra_type is required")),
    ("ec:400 cap_floor missing cap_floor_type", "cap_floor",
     "cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "cap_floor", "cap_floor_type"),
     _ec_body_contains("CapFloor.cap_floor_type is required")),
    ("ec:400 cds missing side", "cds",
     "cds_request.json", 400,
     _ec_del_instrument_field("cds_list", "cds", "side"),
     _ec_body_contains("CDS.side is required")),
    # ---- 400 INVALID_ARGUMENT: coupon-pricer optionlet-vol convention presence ----
    # A ConstantOptionletVolatility convention enum omitted from a coupon pricer
    # must be rejected, not silently defaulted to the alphabetical-0 value.
    ("ec:400 coupon pricer optionlet vol missing calendar", "floating_rate_bond",
     "floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("calendar"),
     _ec_body_contains("ConstantOptionletVolatility.calendar is required")),
    ("ec:400 coupon pricer optionlet vol missing business_day_convention",
     "floating_rate_bond", "floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("business_day_convention"),
     _ec_body_contains("ConstantOptionletVolatility.business_day_convention is required")),
    ("ec:400 coupon pricer optionlet vol missing day_counter", "floating_rate_bond",
     "floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("day_counter"),
     _ec_body_contains("ConstantOptionletVolatility.day_counter is required")),
    # ---- 400 INVALID_ARGUMENT: fail-closed enum handling ----
    # The CDS credit-curve bootstrap supports LogLinear only; ForwardFlat
    # and LogCubic used to be silently priced as LogLinear. The complex request
    # is used because it carries real par-spread quotes (the simple request
    # takes the flat-hazard branch and never reaches the interpolator switch).
    ("ec:400 cds credit interpolator ForwardFlat rejected", "cds",
     "cds_complex_request.json", 400,
     _ec_set_credit_curve_field("curve_interpolator", "ForwardFlat"),
     _ec_body_contains("Unsupported credit curve interpolator")),
    ("ec:400 cds credit interpolator LogCubic rejected", "cds",
     "cds_complex_request.json", 400,
     _ec_set_credit_curve_field("curve_interpolator", "LogCubic"),
     _ec_body_contains("Unsupported credit curve interpolator")),
    # An out-of-range CDS engine enum (raw integers are accepted by the
    # FlatBuffers JSON parser) used to silently price with the MidPoint engine.
    ("ec:400 cds engine type out of range", "cds",
     "cds_request.json", 400,
     _ec_set_model_payload_field("engine_type", 99),
     _ec_body_contains("Unsupported CDS engine type")),
    # A credit curve with no flat_hazard_rate and no quotes used to silently
    # price off an invented 1% flat hazard. It must now be rejected: an empty
    # credit curve is an error, never a fabricated default hazard rate. The
    # simple request carries an empty quotes list plus flat_hazard_rate;
    # dropping the hazard leaves nothing to build the curve from.
    ("ec:400 cds empty credit curve rejected", "cds",
     "cds_request.json", 400,
     _ec_del_credit_curve_field("flat_hazard_rate"),
     _ec_body_contains("neither flat_hazard_rate nor quotes")),
    # A genuine 0 running coupon on an upfront quote (and a 0 running coupon /
    # present upfront on the trade) is a valid CDS, not an error. It used to be
    # rejected by a running_coupon == 0.0 value test; presence-driven handling
    # now prices it. Posted verbatim; a clean 200 proves it is representable.
    ("ec:200 cds upfront quote with zero running coupon prices", "cds",
     "cds/cds_upfront_zero_running_coupon.json", 200, _ec_identity),
    # An out-of-range swaption settlement type used to fail open to
    # Physical settlement.
    ("ec:400 swaption settlement type out of range", "swaption",
     "swaption_request.json", 400,
     _ec_set_nested_field("swaptions", "swaption", "settlement_type", 99),
     _ec_body_contains("Invalid settlement type")),
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
    # ---- bootstrap-curves A5 guard: the curve build and the mapper's
    #      pillar-date extraction both deref FutureHelper.future_start_date;
    #      pre-guard this request SIGSEGV'd the worker. Post-guard it must be
    #      a clean INVALID_ARGUMENT (curve-parse guard fires at the transport
    #      level before the per-query error fold). ----
    ("ec:400 bootstrap_curves FutureHelper missing future_start_date",
     "bootstrap_curves", "bootstrap_curves_tenor_grid.json", 400,
     _ec_future_helper_missing_start_date()),
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
