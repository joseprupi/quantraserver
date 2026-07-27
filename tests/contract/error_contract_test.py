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
    (TermStructure). day_counter, interpolator and bootstrap_trait are
    presence-required: an omitted convention is an error, never an
    alphabetical-0 default."""
    def f(req):
        req["pricing"]["rates"]["curves"][0].pop(field, None)
        return req
    return f


def _ec_del_credit_field(field):
    """Drop a convention field from the first credit curve (CreditCurveSpec).
    recovery_rate / curve_interpolator / calendar / day_counter are
    presence-required conventions: an omission is a named 400, never a silent
    market-default assumption."""
    def f(req):
        req["pricing"]["credit"]["credit_curves"][0].pop(field, None)
        return req
    return f


def _ec_del_credit_helper_field(field):
    """Drop a convention field from the first credit curve's CdsHelperConventions
    (e.g. settlement_days). Presence-required: an omission is a named 400."""
    def f(req):
        req["pricing"]["credit"]["credit_curves"][0]["helper_conventions"].pop(field, None)
        return req
    return f


def _ec_del_index_field(field):
    """Drop a convention field from the first registered IndexDef
    (fixing_days / calendar / business_day_convention / day_counter /
    end_of_month / index_type). Presence-required: an omission is a named 400."""
    def f(req):
        req["pricing"]["rates"]["indices"][0].pop(field, None)
        return req
    return f


def _ec_del_index_tenor_unit():
    """Drop `unit` from the first IndexDef's tenor Period. An omitted period unit
    would otherwise silently mean Months, so it must be a named 400."""
    def f(req):
        req["pricing"]["rates"]["indices"][0]["tenor"].pop("unit", None)
        return req
    return f


def _ec_dup_curve_id():
    """Append a second curve reusing the first curve's id. Defining the same id
    twice silently kept the first and dropped the second (first-wins), which can
    change the price without warning; it must now be a named 400."""
    def f(req):
        curves = req["pricing"]["rates"]["curves"]
        curves.append(json.loads(json.dumps(curves[0])))
        return req
    return f


def _ec_dup_index_id():
    """Append a second index reusing the first index's id. A duplicate index id
    must be a named 400 rather than silently keeping the first definition."""
    def f(req):
        indices = req["pricing"]["rates"]["indices"]
        indices.append(json.loads(json.dumps(indices[0])))
        return req
    return f


def _ec_del_cds_schedule_field(field):
    """Drop a field from the CDS trade schedule (e.g. end_of_month). Presence-
    required: absent-vs-false silently changes schedule dates."""
    def f(req):
        req["cds_list"][0]["cds"]["schedule"].pop(field, None)
        return req
    return f


def _ec_set_curve_field(field, value):
    """Set a top-level field on the first pricing curve (TermStructure). Used to
    drive bootstrap_trait to a value that conflicts with the curve's points."""
    def f(req):
        req["pricing"]["rates"]["curves"][0][field] = value
        return req
    return f


def _ec_curve_inject_zero_point():
    """Replace the first curve point with a ZeroRatePoint while leaving the
    curve's bootstrap_trait unchanged. A bootstrap trait builds from rate
    helpers, so a ZeroRatePoint must be rejected by name."""
    def f(req):
        req["pricing"]["rates"]["curves"][0]["points"][0] = {
            "point_type": "ZeroRatePoint",
            "point": {
                "date": "2030-01-15",
                "zero_rate": 0.03,
                "compounding": "Continuous",
                "frequency": "Annual",
                "calendar": "TARGET",
                "business_day_convention": "ModifiedFollowing",
            },
        }
        return req
    return f


def _ec_curve_mix_zero_point_compounding():
    """Change the second ZeroRatePoint's compounding so the curve carries two
    different compounding conventions. A single InterpolatedZeroCurve has ONE
    compounding, so mixed conventions must be rejected."""
    def f(req):
        pts = req["pricing"]["rates"]["curves"][0]["points"]
        pts[1]["point"]["compounding"] = "Compounded"
        pts[1]["point"]["frequency"] = "Annual"
        return req
    return f


def _ec_curve_make_discount_factor(trait="InterpolatedDiscount", mutate=None):
    """Replace the first pricing curve with a discount-factor curve
    (DiscountFactorPoints) under the given bootstrap_trait, then optionally
    mutate the points list. Used to drive DiscountFactorPoint validation: the
    trait must be InterpolatedDiscount, every discount_factor must be present
    and in (0, 1], and a bootstrap trait must reject a DiscountFactorPoint."""
    def f(req):
        curve = req["pricing"]["rates"]["curves"][0]
        curve["interpolator"] = "LogLinear"
        curve["bootstrap_trait"] = trait
        curve["reference_date"] = "2008-09-18"
        pts = [
            {"point_type": "DiscountFactorPoint",
             "point": {"date": "2008-09-18", "discount_factor": 1.0,
                       "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing"}},
            {"point_type": "DiscountFactorPoint",
             "point": {"date": "2009-09-18", "discount_factor": 0.97,
                       "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing"}},
            {"point_type": "DiscountFactorPoint",
             "point": {"date": "2013-09-18", "discount_factor": 0.90,
                       "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing"}},
        ]
        curve["points"] = pts
        if mutate is not None:
            mutate(pts)
        return req
    return f


def _ec_curve_make_forward_rate(trait="InterpolatedFwd", interpolator="Linear",
                                mutate=None):
    """Replace the first pricing curve with a forward-rate curve
    (ForwardRatePoints) under the given bootstrap_trait, then optionally mutate
    the points list. Used to drive ForwardRatePoint validation: the trait must
    be InterpolatedFwd, every forward_rate must be present and finite, and both
    the bootstrap traits and the other interpolated traits must reject a
    ForwardRatePoint by name."""
    def f(req):
        curve = req["pricing"]["rates"]["curves"][0]
        curve["interpolator"] = interpolator
        curve["bootstrap_trait"] = trait
        curve["reference_date"] = "2025-01-15"
        pts = [
            {"point_type": "ForwardRatePoint",
             "point": {"date": "2025-01-15", "forward_rate": 0.030,
                       "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing"}},
            {"point_type": "ForwardRatePoint",
             "point": {"date": "2027-01-15", "forward_rate": 0.032,
                       "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing"}},
            {"point_type": "ForwardRatePoint",
             "point": {"date": "2031-01-15", "forward_rate": 0.034,
                       "calendar": "TARGET",
                       "business_day_convention": "ModifiedFollowing"}},
        ]
        curve["points"] = pts
        if mutate is not None:
            mutate(pts)
        return req
    return f


def _ec_equity_set_payoff(payoff_type, payoff):
    """Replace the equity option's payoff union (payoff_type + payoff)."""
    def f(req):
        opt = req["options"][0]["option"]
        opt["payoff_type"] = payoff_type
        opt["payoff"] = payoff
        return req
    return f


def _ec_equity_set_exercise(exercise_type, exercise):
    """Replace the equity option's exercise union (exercise_type + exercise)."""
    def f(req):
        opt = req["options"][0]["option"]
        opt["exercise_type"] = exercise_type
        opt["exercise"] = exercise
        return req
    return f


def _ec_equity_set_barrier(barrier):
    """Attach a barrier feature to the equity option."""
    def f(req):
        req["options"][0]["option"]["barrier"] = barrier
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


def _ec_set_schedule_fields(arr_key, product_key, **fields):
    """Merge one or more fields into the product's schedule. Used to drive the
    stub-period control (first_date / next_to_last_date) and its validation:
    both stub dates must lie strictly inside (effective_date, termination_date),
    first_date must be on or before next_to_last_date, be ISO-8601, and be
    compatible with the date-generation rule (e.g. Zero rejects any stub)."""
    def f(req):
        req[arr_key][0][product_key]["schedule"].update(fields)
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
    """Drop a convention or discriminator field from the product instrument
    block. The instrument-level enums (day_counter, calendar,
    business_day_convention, swap_type, ...) are presence-required: an omitted
    value is an error, never an alphabetical-0 default."""
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


def _ec_del_model_payload_field(field):
    """Drop a field from every model payload in the request. model_type selects
    the pricing engine, so it is presence-required: an omitted value is an
    error, never an alphabetical-0 default."""
    def f(req):
        for m in req["pricing"].get("volatility", {}).get("models", []):
            m["payload"].pop(field, None)
        return req
    return f


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


def _ec_yoy_cf_collar_cap_below_floor():
    """Turn the base YoY cap into a Collar whose cap strike is below its floor
    strike — rejected 400 (Collar requires cap_rate >= floor_rate)."""
    def f(req):
        cf = req["cap_floors"][0]["year_on_year_inflation_cap_floor"]
        cf["cap_floor_type"] = "Collar"
        cf["cap_rate"] = 0.01
        cf["floor_rate"] = 0.02
        return req
    return f


def _ec_yoy_cf_bad_schedule_date():
    """Corrupt the YoY cap schedule termination date to a non-ISO form —
    rejected 400 by the date parser."""
    def f(req):
        cf = req["cap_floors"][0]["year_on_year_inflation_cap_floor"]
        cf["schedule"]["termination_date"] = "2030/01/15"
        return req
    return f


def _ec_callable_empty_call_schedule():
    """Empty the callable bond's call_schedule — rejected 400 (a present-yet-empty
    call schedule is a plain fixed-rate bond, not a callable one)."""
    def f(req):
        req["bonds"][0]["callable_fixed_rate_bond"]["call_schedule"] = []
        return req
    return f


def _ec_callable_non_increasing_dates():
    """Give the callable bond two call entries with non-increasing dates —
    rejected 400 (dates must be strictly increasing)."""
    def f(req):
        req["bonds"][0]["callable_fixed_rate_bond"]["call_schedule"] = [
            {"date": "2030-01-17", "price": 101.0, "callability_type": "Call"},
            {"date": "2029-01-17", "price": 100.0, "callability_type": "Call"},
        ]
        return req
    return f


def _ec_callable_nonpositive_price():
    """Set the first call entry's clean price to zero — rejected 400."""
    def f(req):
        req["bonds"][0]["callable_fixed_rate_bond"]["call_schedule"][0]["price"] = 0.0
        return req
    return f


def _ec_callable_del_callability_type():
    """Drop the presence-required callability_type from the first call entry —
    rejected 400 (Call/Put has no alphabetical-0 default)."""
    def f(req):
        req["bonds"][0]["callable_fixed_rate_bond"]["call_schedule"][0].pop(
            "callability_type", None)
        return req
    return f


def _ec_callable_bad_call_date():
    """Corrupt the first call entry's date to a non-ISO form — rejected 400 by
    the date parser."""
    def f(req):
        req["bonds"][0]["callable_fixed_rate_bond"]["call_schedule"][0]["date"] = \
            "2029/01/17"
        return req
    return f


def _ec_set_leg_notionals(arr_key, product_key, leg_key, values):
    """Set the optional per-period notionals vector on a swap leg (or bond)."""
    def f(req):
        req[arr_key][0][product_key][leg_key]["notionals"] = values
        return req
    return f


def _ec_set_cms_cap_floor(cap, floor):
    """Set cap/floor on the first swap's CMS leg (presence-based fields)."""
    def f(req):
        cms = req["swaps"][0]["vanilla_swap"]["cms_leg"]
        cms["cap"] = cap
        cms["floor"] = floor
        return req
    return f


def _ec_set_swaption_underlying_notionals(values):
    """Set the notionals vector on the swaption underlying swap's fixed leg."""
    def f(req):
        req["swaptions"][0]["swaption"]["underlying"]["fixed_leg"]["notionals"] = values
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


def _ec_swaption_rebump_without_rates():
    """Ask for rebump greeks while omitting pricing.rates entirely.

    The rebump snapshots are built in the mapper, which runs before the pricing
    registry (owner of the usual rates-presence guard). Pre-guard this
    dereferenced a null pricing.rates and SIGSEGV'd the worker; it must now be a
    clean INVALID_ARGUMENT that names the requirement."""
    def f(req):
        req["pricing"]["options"] = {"swaption_pricing_rebump": True}
        req["pricing"].pop("rates", None)
        return req
    return f


def _ec_basis_swap_negative_notional():
    """Set a basis-swap leg notional to a negative value.

    A notional is a strictly-positive quantity; an omitted-or-nonpositive value
    must be a named 400, never a silently-priced zero (a bare double on the wire
    would default to 0)."""
    def f(req):
        bs = req["swaps"][0]["basis_swap"]
        bs["leg1"]["notional"] = -1000000.0
        return req
    return f


def _ec_del_quote_value():
    """Drop the inline `value` from the first shared market quote (QuoteSpec).

    A bare double on the wire defaults to 0, so an omitted quote value would
    silently bootstrap every referencing helper off a zero rate. It must be a
    named 400 instead."""
    def f(req):
        req["pricing"]["quotes"][0].pop("value", None)
        return req
    return f


def _ec_del_cds_quote_type():
    """Drop the presence-required `quote_type` from the first CDS market quote.

    Without it a bare enum defaults to the alphabetical-0 value (ParSpread);
    an omitted discriminator must be a named 400."""
    def f(req):
        req["pricing"]["credit"]["credit_curves"][0]["quotes"][0].pop(
            "quote_type", None)
        return req
    return f


def _ec_body_valid_json_without(*absent_keys):
    """Body validator: the 200 body must parse as JSON, carry no bare `nan`
    token, and none of the swaps may carry any of `absent_keys` (the fields
    that would have serialized a non-finite double)."""
    def check(body_text):
        if "nan" in body_text.lower():
            return f"response body carries a non-finite token: {body_text[:200]}"
        try:
            body = json.loads(body_text)
        except Exception as e:
            return f"response body is not valid JSON: {e} :: {body_text[:200]}"
        for swap in body.get("swaps", []) or []:
            for k in absent_keys:
                if k in swap:
                    return (f"expected field {k!r} to be omitted (non-finite), "
                            f"but it is present: {json.dumps(swap)[:200]}")
        return None
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
    # ---- 400 INVALID_ARGUMENT: duplicate ids in a request list ----
    # Defining the same id twice in a curves/indices list previously kept the
    # first definition and silently dropped the rest, which can change the
    # priced result. A duplicate id must be a named 400.
    ("ec:400 vanilla_swap duplicate curve id", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_dup_curve_id(),
     _ec_body_contains("duplicate curve id")),
    ("ec:400 vanilla_swap duplicate index id", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_dup_index_id(),
     _ec_body_contains("duplicate index id")),
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
    # ---- 400 INVALID_ARGUMENT: hard-coded convention-default presence ----
    # Conventions that previously carried a hard-coded default (recovery 0.4,
    # fixing_days 2, calendar TARGET, period unit Months, schedule end-of-month,
    # a helper settlement lag) are now presence-required: an omission is a named
    # 400 rather than a silent market-default assumption.
    ("ec:400 cds credit curve missing recovery_rate", "cds",
     "cds_request.json", 400,
     _ec_del_credit_field("recovery_rate"),
     _ec_body_contains("CreditCurveSpec.recovery_rate is required")),
    ("ec:400 cds credit curve missing curve_interpolator", "cds",
     "cds_request.json", 400,
     _ec_del_credit_field("curve_interpolator"),
     _ec_body_contains("CreditCurveSpec.curve_interpolator is required")),
    ("ec:400 cds credit helper missing settlement_days", "cds",
     "cds_request.json", 400,
     _ec_del_credit_helper_field("settlement_days"),
     _ec_body_contains("CdsHelperConventions.settlement_days is required")),
    ("ec:400 cds index missing fixing_days", "cds",
     "cds_request.json", 400,
     _ec_del_index_field("fixing_days"),
     _ec_body_contains("IndexDef.fixing_days")),
    ("ec:400 cds index missing calendar", "cds",
     "cds_request.json", 400,
     _ec_del_index_field("calendar"),
     _ec_body_contains("IndexDef.calendar")),
    ("ec:400 cds index tenor missing period unit", "cds",
     "cds_request.json", 400,
     _ec_del_index_tenor_unit(),
     _ec_body_contains("unit is required")),
    ("ec:400 cds schedule missing end_of_month", "cds",
     "cds_request.json", 400,
     _ec_del_cds_schedule_field("end_of_month"),
     _ec_body_contains("Schedule.end_of_month is required")),
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
    # ---- 400 INVALID_ARGUMENT: stub-period control (first_date / next_to_last_date) ----
    # The optional stub dates must lie strictly inside (effective_date,
    # termination_date), first_date must be on or before next_to_last_date, be
    # ISO-8601, and be compatible with the date-generation rule. Each violation
    # is a named 400 (never an opaque 500), even when QuantLib is the one that
    # rejects the stub/rule combination.
    ("ec:400 fixed_rate_bond first_date after termination", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", first_date="2020-05-15"),
     _ec_body_contains("Schedule.first_date")),
    ("ec:400 fixed_rate_bond first_date before effective", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", first_date="2000-05-15"),
     _ec_body_contains("Schedule.first_date")),
    ("ec:400 fixed_rate_bond next_to_last_date after termination", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", next_to_last_date="2020-05-15"),
     _ec_body_contains("Schedule.next_to_last_date")),
    ("ec:400 fixed_rate_bond first_date after next_to_last_date", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond",
                             first_date="2015-05-15", next_to_last_date="2009-05-15"),
     _ec_body_contains("must be on or before next_to_last_date")),
    ("ec:400 fixed_rate_bond non-ISO first_date", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", first_date="2010/05/15"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 fixed_rate_bond Zero rule rejects stub date", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond",
                             date_generation_rule="Zero", first_date="2010-05-15"),
     _ec_body_contains("Schedule")),
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
    # ---- 400 INVALID_ARGUMENT: vol quotation convention presence ----
    # volatility_type says whether the supplied numbers are normal or lognormal
    # vols. An omitted value is indistinguishable from the alphabetical-0
    # default (Normal), which would price a lognormal-quoted surface as normal
    # vols, so it must be rejected instead.
    ("ec:400 swaption vol base missing volatility_type", "swaption",
     "swaption_request.json", 400,
     _ec_del_vol_base_field("volatility_type"),
     _ec_body_contains("IrVolBaseSpec.volatility_type is required")),
    ("ec:400 cap_floor vol base missing volatility_type", "cap_floor",
     "cap_floor_request.json", 400,
     _ec_del_vol_base_field("volatility_type"),
     _ec_body_contains("IrVolBaseSpec.volatility_type is required")),
    # ---- 400 INVALID_ARGUMENT: pricing-model discriminator presence ----
    # model_type selects the pricing engine. An omitted value is
    # indistinguishable from the alphabetical-0 default (Black for the rate
    # models, BlackScholesAnalytic for equity), so it must be rejected rather
    # than silently pricing on an engine the caller never asked for.
    ("ec:400 swaption model missing model_type", "swaption",
     "swaption_request.json", 400,
     _ec_del_model_payload_field("model_type"),
     _ec_body_contains("SwaptionModelSpec.model_type is required")),
    ("ec:400 cap_floor model missing model_type", "cap_floor",
     "cap_floor_request.json", 400,
     _ec_del_model_payload_field("model_type"),
     _ec_body_contains("CapFloorModelSpec.model_type is required")),
    ("ec:400 equity_option model missing model_type", "equity_option",
     "equity/eqopt_eur_call_atm_1y.json", 400,
     _ec_del_model_payload_field("model_type"),
     _ec_body_contains("EquityVanillaModelSpec.model_type is required")),
    # ---- 400 INVALID_ARGUMENT: swaption exercise/settlement discriminators ----
    # Both decide the payoff. An omitted value is indistinguishable from the
    # alphabetical-0 default (European / Physical), so it must be rejected.
    ("ec:400 swaption missing exercise_type", "swaption",
     "swaption_request.json", 400,
     _ec_del_instrument_field("swaptions", "swaption", "exercise_type"),
     _ec_body_contains("Swaption.exercise_type is required")),
    ("ec:400 swaption missing settlement_type", "swaption",
     "swaption_request.json", 400,
     _ec_del_instrument_field("swaptions", "swaption", "settlement_type"),
     _ec_body_contains("Swaption.settlement_type is required")),
    # ---- 400 INVALID_ARGUMENT: cap-floor / CDS convention presence ----
    # A cap/floor or CDS convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value. (The FRA's
    # own day_counter/calendar/business_day_convention are accepted-but-unused —
    # QuantLib builds the FRA from the index — so they are NOT presence-required;
    # see test_fra.py::test_fra_conventions_optional_and_ignored.)
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
    # ---- 400 INVALID_ARGUMENT: swap-direction discriminator presence ----
    # swap_type decides which way round the trade is booked. An omitted value is
    # indistinguishable from the alphabetical-0 default (Payer), so it must be
    # rejected rather than silently pricing the trade in the wrong direction.
    ("ec:400 vanilla_swap missing swap_type", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "vanilla_swap", "swap_type"),
     _ec_body_contains("VanillaSwap.swap_type is required")),
    ("ec:400 ois_swap missing swap_type", "ois_swap",
     "ois_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "ois_swap", "swap_type"),
     _ec_body_contains("OisSwap.swap_type is required")),
    ("ec:400 basis_swap missing swap_type", "basis_swap",
     "basis_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "basis_swap", "swap_type"),
     _ec_body_contains("BasisSwap.swap_type is required")),
    ("ec:400 zero_coupon_inflation_swap missing swap_type",
     "zero_coupon_inflation_swap",
     "inflation/zciis_eur_5y_payer_linear_obs.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_inflation_swap", "swap_type"),
     _ec_body_contains("ZeroCouponInflationSwap.swap_type is required")),
    ("ec:400 year_on_year_inflation_swap missing swap_type",
     "year_on_year_inflation_swap",
     "inflation/yyiis_eur_5y_payer_annual.json", 400,
     _ec_del_instrument_field("swaps", "year_on_year_inflation_swap", "swap_type"),
     _ec_body_contains("YearOnYearInflationSwap.swap_type is required")),
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
    # ---- 400 INVALID_ARGUMENT: request value scalars are presence-required ----
    # A bare double/enum on the wire defaults to 0/first-enum when omitted, so a
    # forgotten value would silently price as zero instead of erroring. Each of
    # these must now be a named 400.
    ("ec:400 coupon pricer optionlet vol missing volatility", "floating_rate_bond",
     "floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("volatility"),
     _ec_body_contains("ConstantOptionletVolatility.volatility is required")),
    ("ec:400 quote spec missing value", "cds",
     "cds_complex_request.json", 400,
     _ec_del_quote_value(),
     _ec_body_contains("QuoteSpec.value is required")),
    ("ec:400 cds quote missing quote_type", "cds",
     "cds_complex_request.json", 400,
     _ec_del_cds_quote_type(),
     _ec_body_contains("CdsQuote.quote_type is required")),
    ("ec:400 bond helper missing redemption", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_curve_point_field("BondHelper", "redemption"),
     _ec_body_contains("BondHelper.redemption is required")),
    ("ec:400 fra missing strike", "fra",
     "fra_request.json", 400,
     _ec_del_instrument_field("fras", "fra", "strike"),
     _ec_body_contains("FRA.strike is required")),
    ("ec:400 cap_floor missing strike", "cap_floor",
     "cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "cap_floor", "strike"),
     _ec_body_contains("CapFloor.strike is required")),
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
     _ec_body_contains("Swaption.settlement_type is not a known settlement type")),
    # ---- 400 INVALID_ARGUMENT: equity exercise-style / payoff combinations
    #      QuantLib does not natively support are rejected, never silently
    #      re-routed to a different product. ----
    # A digital payoff combined with discrete cash dividends has no clean
    # native engine, so it must be rejected. The base request already carries
    # discrete dividends on the underlying; swapping the payoff to a digital
    # trips the guard.
    ("ec:400 equity digital payoff with discrete dividends rejected", "equity_option",
     "equity/eqopt_eur_call_atm_1y_discrete_div.json", 400,
     _ec_equity_set_payoff("EquityCashOrNothingPayoff",
                           {"option_type": "Call", "strike": 100.0, "cash": 10.0}),
     _ec_body_contains("Discrete cash dividends are not supported for digital")),
    # A Bermudan exercise combined with a digital payoff has no native engine.
    ("ec:400 equity Bermudan digital payoff rejected", "equity_option",
     "equity/eqopt_eur_call_atm_1y.json", 400,
     lambda req: _ec_equity_set_exercise(
         "EquityBermudanExercise",
         {"exercise_dates": ["2025-07-15", "2025-10-15", "2026-01-15"]})(
         _ec_equity_set_payoff("EquityCashOrNothingPayoff",
                               {"option_type": "Call", "strike": 100.0, "cash": 10.0})(req)),
     _ec_body_contains("Bermudan exercise is not supported for digital")),
    # A barrier monitored on a discrete date schedule has no native QuantLib
    # engine (all barrier engines monitor continuously). Rather than hand-roll a
    # continuity correction, the request is rejected with a message naming the
    # limitation. monitoring_dates is supplied to prove the schedule is not read.
    ("ec:400 equity barrier discrete monitoring rejected", "equity_option",
     "equity/eqopt_eur_call_atm_1y.json", 400,
     _ec_equity_set_barrier({"barrier_type": "DownOut", "level": 80.0,
                             "rebate": 0.0, "monitoring": "Discrete",
                             "monitoring_dates": ["2025-07-15", "2026-01-15"]}),
     _ec_body_contains("Discrete monitoring")),
    # ---- 400 INVALID_ARGUMENT: swaption rebump requires market data ----
    # Rebump snapshots are built in the mapper (before the registry's usual
    # rates-presence guard); omitting pricing.rates used to crash the worker.
    ("ec:400 swaption rebump without pricing.rates", "swaption",
     "swaption_request.json", 400,
     _ec_swaption_rebump_without_rates(),
     _ec_body_contains("Swaption rebump pricing requires pricing.rates.curves")),
    # ---- 400 INVALID_ARGUMENT: a notional must be present and strictly positive ----
    ("ec:400 basis_swap negative notional rejected", "basis_swap",
     "basis_swap_request.json", 400,
     _ec_basis_swap_negative_notional(),
     _ec_body_contains("SwapFloatingLeg.notional must be positive")),
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
    # ---- 400 INVALID_ARGUMENT: amortizing notionals validation ----
    # When present the per-period notionals vector must be non-empty, carry
    # exactly one entry per coupon period, and hold only positive amounts. The
    # vanilla fixed leg schedule is annual 5Y (5 periods).
    ("ec:400 vanilla_swap notionals too short", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg",
                           [1e7, 1e7, 1e7, 1e7]),
     _ec_body_contains("SwapFixedLeg.notionals has 4 entries but the schedule has 5 periods")),
    ("ec:400 vanilla_swap notionals too long", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg",
                           [1e7, 1e7, 1e7, 1e7, 1e7, 1e7]),
     _ec_body_contains("SwapFixedLeg.notionals has 6 entries but the schedule has 5 periods")),
    ("ec:400 vanilla_swap notionals empty", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg", []),
     _ec_body_contains("SwapFixedLeg.notionals must be non-empty when present")),
    ("ec:400 vanilla_swap notionals non-positive entry", "vanilla_swap",
     "vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg",
                           [1e7, 1e7, 1e7, 1e7, 0.0]),
     _ec_body_contains("SwapFixedLeg.notionals entries must be positive")),
    # ---- 400 INVALID_ARGUMENT: shared-table paths reject amortizing notionals ----
    # SwapFixedLeg / SwapFloatingLeg are shared with the OIS, basis and
    # swaption-underlying paths, which do not support amortizing notionals yet.
    # A present notionals vector must be rejected, never silently ignored.
    ("ec:400 ois_swap fixed leg notionals rejected", "ois_swap",
     "ois_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "ois_swap", "fixed_leg", [1e7]),
     _ec_body_contains("OisSwap fixed leg does not support amortizing notionals yet")),
    ("ec:400 basis_swap leg notionals rejected", "basis_swap",
     "basis_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "basis_swap", "leg1", [1e7]),
     _ec_body_contains("BasisSwap leg1 does not support amortizing notionals yet")),
    ("ec:400 swaption underlying notionals rejected", "swaption",
     "swaption_request.json", 400,
     _ec_set_swaption_underlying_notionals([1e7]),
     _ec_body_contains(
         "Swaption underlying VanillaSwap fixed leg does not support amortizing notionals yet")),
    # ---- 400 INVALID_ARGUMENT: zero-coupon bond convention / date presence ----
    # A zero-coupon bond convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value; wire dates
    # must be strictly ISO-8601 YYYY-MM-DD.
    ("ec:400 zero_coupon_bond missing calendar", "zero_coupon_bond",
     "zero_coupon_bond_request.json", 400,
     _ec_del_bond_field("bonds", "zero_coupon_bond", "calendar"),
     _ec_body_contains("ZeroCouponBond.calendar is required")),
    ("ec:400 zero_coupon_bond missing payment_convention", "zero_coupon_bond",
     "zero_coupon_bond_request.json", 400,
     _ec_del_bond_field("bonds", "zero_coupon_bond", "payment_convention"),
     _ec_body_contains("ZeroCouponBond.payment_convention is required")),
    ("ec:400 zero_coupon_bond non-ISO maturity_date rejected", "zero_coupon_bond",
     "zero_coupon_bond_request.json", 400,
     _ec_set_nested_field("bonds", "zero_coupon_bond", "maturity_date", "2026/01/15"),
     _ec_body_contains("expected YYYY-MM-DD")),
    # ---- 400 INVALID_ARGUMENT: zero-coupon swap convention / form / date presence ----
    # The zero-coupon swap payment conventions are presence-required; the fixed
    # side must carry exactly one of the fixed_payment / fixed_rate forms; the
    # maturity must be strictly after the start; wire dates must be ISO-8601.
    ("ec:400 zero_coupon_swap missing payment_calendar", "zero_coupon_swap",
     "zero_coupon_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_swap", "payment_calendar"),
     _ec_body_contains("ZeroCouponSwap.payment_calendar is required")),
    ("ec:400 zero_coupon_swap missing payment_convention", "zero_coupon_swap",
     "zero_coupon_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_swap", "payment_convention"),
     _ec_body_contains("ZeroCouponSwap.payment_convention is required")),
    ("ec:400 zero_coupon_swap neither fixed_payment nor fixed_rate", "zero_coupon_swap",
     "zero_coupon_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_swap", "fixed_payment"),
     _ec_body_contains(
         "ZeroCouponSwap must contain exactly one of fixed_payment or fixed_rate")),
    ("ec:400 zero_coupon_swap both fixed_payment and fixed_rate", "zero_coupon_swap",
     "zero_coupon_swap_request.json", 400,
     _ec_set_nested_field("swaps", "zero_coupon_swap", "fixed_rate", 0.032),
     _ec_body_contains(
         "ZeroCouponSwap must contain exactly one of fixed_payment or fixed_rate")),
    ("ec:400 zero_coupon_swap maturity not after start", "zero_coupon_swap",
     "zero_coupon_swap_request.json", 400,
     _ec_set_nested_field("swaps", "zero_coupon_swap", "maturity_date", "2025-01-17"),
     _ec_body_contains("ZeroCouponSwap.maturity_date must be after start_date")),
    ("ec:400 zero_coupon_swap non-ISO maturity_date rejected", "zero_coupon_swap",
     "zero_coupon_swap_request.json", 400,
     _ec_set_nested_field("swaps", "zero_coupon_swap", "maturity_date", "2030/01/17"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 vanilla_swap CMS cap below floor rejected", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_collared_lineartsr.json", 400,
     _ec_set_cms_cap_floor(0.02, 0.03),
     _ec_body_contains("SwapCmsLeg.cap must be >= floor")),

    # The year-on-year inflation cap/floor: cap_floor_type / day_counter /
    # payment_convention are presence-required; the strike set must match the
    # type (Cap => cap_rate only, Floor => floor_rate only, Collar => both with
    # cap_rate >= floor_rate); referenced ids must resolve; dates must be ISO.
    ("ec:400 yoy_inflation_cap_floor missing cap_floor_type",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "year_on_year_inflation_cap_floor",
                              "cap_floor_type"),
     _ec_body_contains("YoYInflationCapFloor.cap_floor_type is required")),
    ("ec:400 yoy_inflation_cap_floor missing day_counter",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "year_on_year_inflation_cap_floor",
                              "day_counter"),
     _ec_body_contains("YoYInflationCapFloor.day_counter is required")),
    ("ec:400 yoy_inflation_cap_floor missing payment_convention",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "year_on_year_inflation_cap_floor",
                              "payment_convention"),
     _ec_body_contains("YoYInflationCapFloor.payment_convention is required")),
    ("ec:400 yoy_inflation_cap_floor Cap carrying floor_rate",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_set_nested_field("cap_floors", "year_on_year_inflation_cap_floor",
                          "floor_rate", 0.01),
     _ec_body_contains("YoYInflationCapFloor of type Cap must not carry floor_rate")),
    ("ec:400 yoy_inflation_cap_floor Collar missing floor_rate",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_set_nested_field("cap_floors", "year_on_year_inflation_cap_floor",
                          "cap_floor_type", "Collar"),
     _ec_body_contains(
         "YoYInflationCapFloor of type Collar requires both cap_rate and floor_rate")),
    ("ec:400 yoy_inflation_cap_floor Collar cap below floor",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_yoy_cf_collar_cap_below_floor(),
     _ec_body_contains("YoYInflationCapFloor Collar cap_rate must be >= floor_rate")),
    ("ec:400 yoy_inflation_cap_floor index id not matching curve",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_set_nested_field("cap_floors", "year_on_year_inflation_cap_floor",
                          "inflation_index_id", "NOPE_YY"),
     _ec_body_contains(
         "inflation_index_id does not match inflation_curve")),
    ("ec:404 yoy_inflation_cap_floor unknown volatility id",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 404,
     _ec_set_field("cap_floors", "volatility", "NOPE_VOL"),
     _ec_body_contains("YoY optionlet vol not found")),
    ("ec:400 yoy_inflation_cap_floor non-ISO schedule date rejected",
     "year_on_year_inflation_cap_floor",
     "year_on_year_inflation_cap_floor_request.json", 400,
     _ec_yoy_cf_bad_schedule_date(),
     _ec_body_contains("expected YYYY-MM-DD")),

    # The callable fixed-rate bond: the call schedule must be present and
    # non-empty, with strictly increasing dates, positive clean prices and a
    # present callability_type; the bond conventions follow the presence
    # discipline; the model and discounting curve must resolve; dates are ISO.
    ("ec:400 callable_fixed_rate_bond empty call_schedule",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_callable_empty_call_schedule(),
     _ec_body_contains("call_schedule is required and must be non-empty")),
    ("ec:400 callable_fixed_rate_bond non-increasing call dates",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_callable_non_increasing_dates(),
     _ec_body_contains("call_schedule dates must be strictly increasing")),
    ("ec:400 callable_fixed_rate_bond non-positive call price",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_callable_nonpositive_price(),
     _ec_body_contains("CallabilityEntry.price must be > 0")),
    ("ec:400 callable_fixed_rate_bond missing callability_type",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_callable_del_callability_type(),
     _ec_body_contains("CallabilityEntry.callability_type is required")),
    ("ec:400 callable_fixed_rate_bond missing accrual_day_counter",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_del_instrument_field("bonds", "callable_fixed_rate_bond",
                              "accrual_day_counter"),
     _ec_body_contains("CallableFixedRateBond.accrual_day_counter is required")),
    ("ec:400 callable_fixed_rate_bond missing payment_convention",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_del_instrument_field("bonds", "callable_fixed_rate_bond",
                              "payment_convention"),
     _ec_body_contains("CallableFixedRateBond.payment_convention is required")),
    ("ec:400 callable_fixed_rate_bond missing discounting_curve",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_del_field("bonds", "discounting_curve"),
     _ec_body_contains("PriceCallableFixedRateBond.discounting_curve is required")),
    ("ec:404 callable_fixed_rate_bond unknown model id",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 404,
     _ec_set_field("bonds", "model", "NOPE_MODEL"),
     _ec_body_contains("Model not found")),
    ("ec:400 callable_fixed_rate_bond non-ISO call date rejected",
     "callable_fixed_rate_bond",
     "callable_fixed_rate_bond_request.json", 400,
     _ec_callable_bad_call_date(),
     _ec_body_contains("expected YYYY-MM-DD")),
    # ---- 400 INVALID_ARGUMENT: bootstrap_trait is the explicit curve-family
    #      selector — required, and validated against the curve's point types ----
    ("ec:400 curve missing bootstrap_trait", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_del_curve_field("bootstrap_trait"),
     _ec_body_contains("TermStructure.bootstrap_trait is required")),
    ("ec:400 curve Discount trait with a ZeroRatePoint", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_inject_zero_point(),
     _ec_body_contains("builds from rate helpers but received a ZeroRatePoint")),
    ("ec:400 curve InterpolatedZero trait with a rate helper", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_curve_field("bootstrap_trait", "InterpolatedZero"),
     _ec_body_contains("builds from zero-rate points but received a")),
    ("ec:400 curve InterpolatedDiscount trait with a rate helper", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_curve_field("bootstrap_trait", "InterpolatedDiscount"),
     _ec_body_contains("builds from discount-factor points but received a")),
    ("ec:400 curve DiscountFactorPoint missing discount_factor", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(
         mutate=lambda pts: pts[1]["point"].pop("discount_factor", None)),
     _ec_body_contains("DiscountFactorPoint.discount_factor is required")),
    ("ec:400 curve DiscountFactorPoint discount_factor above one", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(
         mutate=lambda pts: pts[1]["point"].__setitem__("discount_factor", 1.5)),
     _ec_body_contains("DiscountFactorPoint.discount_factor must be in (0, 1]")),
    ("ec:400 curve DiscountFactorPoint discount_factor not positive", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(
         mutate=lambda pts: pts[1]["point"].__setitem__("discount_factor", -0.1)),
     _ec_body_contains("DiscountFactorPoint.discount_factor must be in (0, 1]")),
    ("ec:400 curve Discount trait with a DiscountFactorPoint", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(trait="Discount"),
     _ec_body_contains("builds from rate helpers but received a DiscountFactorPoint")),
    ("ec:400 curve InterpolatedFwd trait with a rate helper", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_set_curve_field("bootstrap_trait", "InterpolatedFwd"),
     _ec_body_contains("builds from forward-rate points but received a")),
    ("ec:400 curve ForwardRatePoint missing forward_rate", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(
         mutate=lambda pts: pts[1]["point"].pop("forward_rate", None)),
     _ec_body_contains("ForwardRatePoint.forward_rate is required")),
    # Non-finite forward_rate is rejected by the parser's std::isfinite guard,
    # but that cannot be exercised over the JSON wire: JSON has no inf/nan
    # literal and the client's serializer refuses to emit one (same reason the
    # DiscountFactorPoint.discount_factor finite guard has no HTTP case). The
    # guard is defense-in-depth for the binary gRPC path.
    ("ec:400 curve Discount trait with a ForwardRatePoint", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(trait="Discount"),
     _ec_body_contains("builds from rate helpers but received a ForwardRatePoint")),
    ("ec:400 curve InterpolatedZero trait with a ForwardRatePoint", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(trait="InterpolatedZero"),
     _ec_body_contains("builds from zero-rate points but received a ForwardRatePoint")),
    ("ec:400 curve InterpolatedFwd rejects log interpolators", "fixed_rate_bond",
     "fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(interpolator="LogLinear"),
     _ec_body_contains("InterpolatedFwd does not support log interpolators")),
    ("ec:400 curve InterpolatedZero mixed compounding across points",
     "fixed_rate_bond",
     "fixed_rate_bond_zerorate_request.json", 400,
     _ec_curve_mix_zero_point_compounding(),
     _ec_body_contains(
         "ZeroRatePoint.compounding must be identical across all points")),
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
