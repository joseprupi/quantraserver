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
from pathlib import Path

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
    and positive (above 1 is valid: negative rates), and a bootstrap trait
    must reject a DiscountFactorPoint."""
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
    """Drop a kind-specific field from a swap leg's coupon body
    (`<swap>.<leg>.coupons.<field>`: rate, index, spread, averaging_method,
    apply_observation_shift, ...). The presence-required ones are an error
    when omitted, never an alphabetical-0 default. The fields every coupon
    kind shares (notional, schedule, day_counter, payment_*) live on the leg
    itself: use `_ec_del_leg_attr` for those."""
    def f(req):
        req[arr_key][0][swap_key][leg_key]["coupons"].pop(field, None)
        return req
    return f


def _ec_set_swap_leg_field(arr_key, swap_key, leg_key, field, value):
    """Set a kind-specific field of a swap leg's coupon body
    (`<swap>.<leg>.coupons.<field>`) to an invalid value (e.g. a negative
    day count that would wrap into QuantLib's unsigned Natural)."""
    def f(req):
        req[arr_key][0][swap_key][leg_key]["coupons"][field] = value
        return req
    return f


def _ec_set_leg_attr(arr_key, swap_key, leg_key, field, value):
    """Set a SwapLeg-level field on one leg of the first swap: the fields
    every coupon kind shares (notional, notionals, schedule, day_counter,
    payment_convention, payment_calendar, payment_lag) plus pay_receive,
    currency, coupons_type, discounting_curve, forwarding_curve and the
    initial/final_exchange flags. Not the kind-specific coupon body."""
    def f(req):
        req[arr_key][0][swap_key][leg_key][field] = value
        return req
    return f


def _ec_del_leg_attr(arr_key, swap_key, leg_key, field):
    """Drop a SwapLeg-level field (shared coupon field, side, currency,
    curve id, ...) from one leg of the first swap. The shared convention
    enums (day_counter, payment_convention, OIS payment_calendar /
    payment_lag) are presence-required: an omitted value is a 400 naming
    `<Swap>.<leg>.<field>`, never an alphabetical-0 default."""
    def f(req):
        req[arr_key][0][swap_key][leg_key].pop(field, None)
        return req
    return f


def _ec_set_swaption_leg_attr(leg_key, field, value):
    """Set a SwapLeg-level field on one leg of the swaption's underlying."""
    def f(req):
        req["swaptions"][0]["swaption"]["underlying"][leg_key][field] = value
        return req
    return f


def _ec_set_swaption_leg_coupon_field(leg_key, field, value):
    """Set a kind-specific coupon-body field (`coupons.<field>`) on one leg
    of the swaption's underlying."""
    def f(req):
        req["swaptions"][0]["swaption"]["underlying"][leg_key]["coupons"][field] = value
        return req
    return f


def _ec_ibor_body(coupons):
    """The IborCoupons body equivalent of another floating coupon body (same
    index and spread; the shared fields - notional, schedule, conventions -
    stay on the leg and need no change). The JSON parser reads a leg's
    `coupons` as the table its `coupons_type` names, so a leg whose kind is
    changed needs a body that table can parse; the role / index-kind checks
    of the mapper and pricer then do the rejecting."""
    return {k: coupons[k] for k in ("index", "spread")}


def _ec_vanilla_fixed_leg_as_ibor():
    """Give the vanilla swap's fixed leg IborCoupons (a copy of the floating
    leg's body). The fixed-leg role accepts FixedCoupons only."""
    def f(req):
        swap = req["swaps"][0]["vanilla_swap"]
        swap["fixed_leg"]["coupons_type"] = "IborCoupons"
        swap["fixed_leg"]["coupons"] = _ec_ibor_body(swap["floating_leg"]["coupons"])
        return req
    return f


def _ec_ois_overnight_leg_as_ibor():
    """Give the OIS overnight leg IborCoupons on its overnight index. The
    overnight-leg role accepts OvernightCoupons only (rejected before any
    market data is looked at)."""
    def f(req):
        leg = req["swaps"][0]["ois_swap"]["overnight_leg"]
        leg["coupons_type"] = "IborCoupons"
        leg["coupons"] = _ec_ibor_body(leg["coupons"])
        return req
    return f


def _ec_vanilla_with_ibor_coupons_on_overnight_index():
    """Build a price-vanilla-swap request out of the OIS fixture: the floating
    leg carries IborCoupons but names the overnight index (ESTR). The role
    accepts IborCoupons, so this is caught only once the index is resolved:
    an IBOR coupon on an overnight index is a 400 naming OvernightCoupons."""
    def f(req):
        ois = req["swaps"][0].pop("ois_swap")
        floating = ois["overnight_leg"]
        floating["coupons_type"] = "IborCoupons"
        floating["coupons"] = _ec_ibor_body(floating["coupons"])
        req["swaps"][0]["vanilla_swap"] = {
            "fixed_leg": ois["fixed_leg"], "floating_leg": floating}
        return req
    return f


def _ec_basis_leg_as_fixed():
    """Give basis leg1 FixedCoupons (a basis swap is floating vs floating).
    The leg keeps its notional / schedule / conventions; only the coupon
    body changes to the one FixedCoupons parses (a rate)."""
    def f(req):
        leg = req["swaps"][0]["basis_swap"]["leg1"]
        leg["coupons_type"] = "FixedCoupons"
        leg["coupons"] = {"rate": 0.03}
        leg.pop("forwarding_curve", None)
        return req
    return f


def _ec_swaption_floating_leg_as_cms():
    """Give the swaption underlying's floating leg CmsCoupons (the leg keeps
    its shared fields; the body is the CmsCoupons table's required ids). A
    swaption is priced from a QuantLib::VanillaSwap, whose floating leg is
    an IBOR leg."""
    def f(req):
        leg = req["swaptions"][0]["swaption"]["underlying"]["floating_leg"]
        leg["coupons_type"] = "CmsCoupons"
        leg["coupons"] = {
            "swap_index_id": "EUR_SWAP_6M", "swap_tenor": {"n": 10, "unit": "Years"},
            "swaption_vol_id": "swaption_vol"}
        return req
    return f


def _ec_set_curve_point_field(point_type, field, value):
    """Set `field` on the first curve helper of `point_type` to an invalid
    value (e.g. a negative day count that would wrap into QuantLib's unsigned
    Natural)."""
    def f(req):
        for curve in req["pricing"]["rates"]["curves"]:
            for p in curve.get("points", []):
                if p.get("point_type") == point_type:
                    p["point"][field] = value
                    return req
        raise AssertionError(f"no {point_type} point found in request")
    return f


def _ec_set_instrument_fields(arr_key, product_key, fields):
    """Set several fields of the product instrument block at once (e.g. turn
    the base cap into a Collar with or without its floor strike)."""
    def f(req):
        req[arr_key][0][product_key].update(fields)
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


def _ec_add_ratio_yoy_specs(*links):
    """Append ratio YoY index specs, one per (id, underlying_zero_index_id),
    each a copy of the request's first index spec without fixings."""
    def f(req):
        indices = req["pricing"]["inflation"]["inflation_indices"]
        template = indices[0]
        for spec_id, underlying_id in links:
            spec = {k: v for k, v in template.items() if k != "fixings"}
            spec["id"] = spec_id
            spec["underlying_zero_index_id"] = underlying_id
            indices.append(spec)
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
    """Set the optional per-period `notionals` vector on a swap leg (a
    leg-level field, next to the scalar `notional`)."""
    def f(req):
        req[arr_key][0][product_key][leg_key]["notionals"] = values
        return req
    return f


def _ec_set_cms_cap_floor(cap, floor):
    """Set cap/floor on the first swap's CMS coupons (the floating leg of a
    vanilla swap with coupons_type CmsCoupons; presence-based fields)."""
    def f(req):
        cms = req["swaps"][0]["vanilla_swap"]["floating_leg"]["coupons"]
        cms["cap"] = cap
        cms["floor"] = floor
        return req
    return f


def _ec_set_swaption_underlying_notionals(values):
    """Set the per-period notionals vector on the swaption underlying's fixed
    leg (a leg-level field)."""
    def f(req):
        underlying = req["swaptions"][0]["swaption"]["underlying"]
        underlying["fixed_leg"]["notionals"] = values
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


def _ec_body_contains_all(*substrs):
    """Body validator: every substring must appear in the (error) body. Used
    where the field path and the reason are both pinned but the exact text
    between them is not (e.g. the amortizing-notionals 400s)."""
    def check(body_text):
        for substr in substrs:
            if substr not in body_text:
                return f"expected body to contain {substr!r}, got: {body_text[:200]}"
        return None
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


def _ec_load_probe(relpath):
    """Mutator that REPLACES the base request with a version-diff probe from
    tests/version_diff/probes/<relpath> (the base filename only has to exist).
    Used where the scenario is a probe written for the QuantLib upgrade notes
    and should stay byte-identical to the one the notes cite."""
    def f(_req):
        probe = (Path(__file__).resolve().parents[1] / "version_diff" / "probes" / relpath)
        with open(probe) as fh:
            return json.load(fh)
    return f


def _ec_overflowing_zero_curve():
    """Set every ZeroRatePoint of the request's curves to a continuously
    compounded zero rate of -1e4 (-1,000,000 %). The request is valid on the
    wire (finite rate, well-formed points) but the discount factors overflow
    to +inf, so the priced NPV is non-finite: the gateway must answer 500
    naming the field rather than a 200 body with a bare `inf` token."""
    def f(req):
        for curve in req["pricing"]["rates"]["curves"]:
            for pt in curve["points"]:
                if pt.get("point_type") == "ZeroRatePoint":
                    pt["point"]["zero_rate"] = -1.0e4
        return req
    return f


def _ec_bootstrap_overflowing_zero_curve():
    """Rebuild the bootstrap-curves request around the bond example's
    InterpolatedZero curve with every zero rate at -1e4 and sample only the
    discount factors, so the second grid point already carries +inf."""
    def f(req):
        bond_file = (Path(__file__).resolve().parents[2] / "examples" / "data" / "bonds"
                     / "frb_usd_10y_treasury_semiannual_zero_curve.json")
        with open(bond_file) as fh:
            bond = json.load(fh)
        curve = bond["pricing"]["rates"]["curves"][0]
        for pt in curve["points"]:
            pt["point"]["zero_rate"] = -1.0e4
        req["pricing"]["as_of_date"] = bond["pricing"]["as_of_date"]
        req["pricing"]["rates"] = {"curves": [curve]}
        req["queries"] = [req["queries"][0]]
        req["queries"][0]["curve_id"] = curve["id"]
        req["queries"][0]["measures"] = ["DF"]
        return req
    return f


def _ec_body_is_error_json(expected_error):
    """Body validator for the gateway's own 500s: the body must be valid JSON
    whose `error` equals expected_error exactly (no bare non-finite tokens)."""
    def check(body_text):
        try:
            body = json.loads(body_text)
        except Exception as e:
            return f"response body is not valid JSON: {e} :: {body_text[:200]}"
        if body.get("error") != expected_error:
            return f"expected error {expected_error!r}, got {body.get('error')!r}"
        return None
    return check


def _ec_unimplemented_curve_point():
    # Replace the first curve's first point with a schema-ready-but-unbuilt
    # helper (TenorBasisSwapHelper). The point parses into a valid FlatBuffer
    # (every required field present), then the term-structure point parser
    # throws QuantraNotImplemented when the curve is built during pricing ->
    # gRPC UNIMPLEMENTED -> HTTP 501. No deps are given so the dependency
    # ordering has nothing to resolve before the throw, and the indices it
    # names exist in the request so no 404 can mask the 501.
    def f(req):
        req["pricing"]["rates"]["curves"][0]["points"][0] = {
            "point_type": "TenorBasisSwapHelper",
            "point": {
                "spread": 0.0005,
                "tenor": {"n": 2, "unit": "Years"},
                "index_short": {"id": "EUR_6M"},
                "index_long": {"id": "EUR_6M"},
                "calendar": "TARGET",
            },
        }
        return req
    return f


# (label, product, filename, expected_http_status, mutate_fn[, body_check])
def _ec_correlations(matrices):
    """Append a pricing.correlations block to any request.

    Correlation matrices are shared market data: the registry parses and
    PSD-checks every matrix on every request that carries the block, whether
    or not the product references it, so a malformed block is a 400 on an
    endpoint that has no use for correlations (here: vanilla swap)."""
    def f(req):
        req["pricing"]["correlations"] = {"matrices": matrices}
        return req
    return f


def _ec_correlation_matrix(matrix_id, labels, values):
    n = len(labels)
    return {"id": matrix_id, "labels": labels,
            "values": {"n_rows": n, "n_cols": n, "values": values}}


_EC_QUAD_LABELS = ["SMI", "SPX", "SX5E", "NKY"]
_EC_QUAD_VALUES = [1.00, 0.55, 0.80, 0.45,
                   0.55, 1.00, 0.60, 0.50,
                   0.80, 0.60, 1.00, 0.50,
                   0.45, 0.50, 0.50, 1.00]
# (SPX, SX5E) = 0.61 while (SX5E, SPX) = 0.60.
_EC_QUAD_ASYMMETRIC = _EC_QUAD_VALUES[:6] + [0.61] + _EC_QUAD_VALUES[7:]
# A at 0.9 with both B and C while B and C are at -0.9: eigenvalue -0.8.
_EC_NON_PSD = [1.0, 0.9, 0.9,
               0.9, 1.0, -0.9,
               0.9, -0.9, 1.0]



# ---------------------------------------------------------------------------
# Structured notes (price-structured-note): the mapper's schema 400s, the
# evaluator's market-dependent 400s, 404s and the 501 of the opt-in blocks.
# Every mutation starts from the BNP example (4 underlyings, 3 quanto, one
# past observation, constant vols, European barrier).
# ---------------------------------------------------------------------------

_EC_CORR = "correlation/ndx_rut_closes_2025-09-29_to_2026-09-30.json"


def _ec_corr(mutate):
    """Mutate a calibrate-correlation request in place (the mutator may
    return nothing)."""
    def f(req):
        mutate(req)
        return req
    return f


_EC_SN = "structured_note/bnp_quad_index_classic_autocall_xs2741509801.json"


def _ec_sn(mutate):
    """Mutate notes[0] in place (mutate receives the note dict)."""
    def f(req):
        mutate(req["notes"][0])
        return req
    return f


def _ec_sn_set(field, value):
    return _ec_sn(lambda n: n.__setitem__(field, value))


def _ec_sn_del(field):
    return _ec_sn(lambda n: n.pop(field, None))


def _ec_sn_event(index, mutate):
    return _ec_sn(lambda n: mutate(n["events"][index]))


def _ec_sn_underlying(index, mutate):
    return _ec_sn(lambda n: mutate(n["underlyings"][index]))


def _ec_sn_swap_events(i, j):
    def mut(n):
        n["events"][i], n["events"][j] = n["events"][j], n["events"][i]
    return _ec_sn(mut)


def _ec_sn_correlation_labels(labels):
    def f(req):
        req["pricing"]["correlations"]["matrices"][0]["labels"] = labels
        return req
    return f


def _ec_sn_top(field, value):
    def f(req):
        req[field] = value
        return req
    return f


def _ec_sn_surface_vol(vol_id):
    """Replace the BlackVolSpec `vol_id` by a strike x expiry surface."""
    def f(req):
        for v in req["pricing"]["volatility"]["vol_surfaces"]:
            if v["id"] == vol_id:
                base = v["payload"]["base"]
                base["shape"] = "SmileCube3D"
                base.pop("constant_vol", None)
                v["payload"] = {
                    "base": base,
                    "expiries": [{"n": 1, "unit": "Years"}, {"n": 2, "unit": "Years"}],
                    "strikes": [1.0, 1.25, 1.5],
                    "surface_vols": {"n_rows": 2, "n_cols": 3,
                                     "values": [0.09, 0.08, 0.085, 0.09, 0.08, 0.085]},
                    "surface_interpolator": "Bilinear",
                    "allow_extrapolation": True,
                }
        return req
    return f


def _ec_sn_drop_equity_currency(underlying_id):
    def f(req):
        for u in req["pricing"]["equity"]["equity_underlyings"]:
            if u["id"] == underlying_id:
                u.pop("currency")
        return req
    return f


def _ec_sn_greeks_on_called():
    """Every historical close above its initial level: the past observation
    calls the note; then ask for Greeks."""
    def f(req):
        for u in req["notes"][0]["underlyings"]:
            u["historical_levels"][0]["value"] = 2.0 * u["initial_level"]
        req["greeks"] = {}
        return req
    return f


def _ec_sn_paths_and_samples(paths, sample_paths):
    def f(req):
        req["notes"][0]["monte_carlo"]["paths"] = paths
        req["diagnostics"] = {"distribution": True, "sample_paths": sample_paths}
        return req
    return f

SCENARIOS = [
    # ---- 404 NOT_FOUND: referenced id absent from pricing ----
    ("ec:404 fixed_rate_bond discounting_curve missing", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 404, _ec_set_field("bonds", "discounting_curve", "no_such_curve")),
    ("ec:404 floating_rate_bond forwarding_curve missing", "floating_rate_bond",
     "bonds/floating_rate_bond_request.json", 404, _ec_set_field("bonds", "forwarding_curve", "no_such_curve")),
    # The curve ids live on the legs (SwapLeg.discounting_curve /
    # forwarding_curve); an unknown id is a 404 naming the leg and the role.
    ("ec:404 vanilla_swap fixed leg discounting_curve missing", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 404,
     _ec_set_leg_attr("swaps", "vanilla_swap", "fixed_leg", "discounting_curve",
                      "no_such_curve"),
     _ec_body_contains("VanillaSwap.fixed_leg discounting curve not found: no_such_curve")),
    ("ec:404 vanilla_swap floating leg forwarding_curve missing", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 404,
     _ec_set_leg_attr("swaps", "vanilla_swap", "floating_leg", "forwarding_curve",
                      "no_such_curve"),
     _ec_body_contains("VanillaSwap.floating_leg forwarding curve not found: no_such_curve")),
    ("ec:404 vanilla_swap floating leg index missing", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 404,
     _ec_set_swap_leg_field("swaps", "vanilla_swap", "floating_leg", "index",
                            {"id": "NO_SUCH_INDEX"}),
     _ec_body_contains("Index not found: NO_SUCH_INDEX")),
    ("ec:404 ois_swap overnight leg discounting_curve missing", "ois_swap",
     "ir_swaps/ois_swap_request.json", 404,
     _ec_set_leg_attr("swaps", "ois_swap", "overnight_leg", "discounting_curve",
                      "no_such_curve"),
     _ec_body_contains("OisSwap.overnight_leg discounting curve not found: no_such_curve")),
    ("ec:404 basis_swap leg2 forwarding_curve missing", "basis_swap",
     "ir_swaps/basis_swap_request.json", 404,
     _ec_set_leg_attr("swaps", "basis_swap", "leg2", "forwarding_curve", "no_such_curve"),
     _ec_body_contains("BasisSwap.leg2 forwarding curve not found: no_such_curve")),
    ("ec:404 swaption underlying discounting_curve missing", "swaption",
     "swaption/swaption_request.json", 404,
     lambda req: _ec_set_swaption_leg_attr("floating_leg", "discounting_curve", "no_such_curve")(
         _ec_set_swaption_leg_attr("fixed_leg", "discounting_curve", "no_such_curve")(req))),
    ("ec:404 fra forwarding_curve missing", "fra",
     "fra/fra_request.json", 404, _ec_set_field("fras", "forwarding_curve", "no_such_curve")),
    ("ec:404 swaption model missing", "swaption",
     "swaption/swaption_request.json", 404, _ec_set_field("swaptions", "model", "no_such_model")),
    ("ec:404 swaption volatility missing", "swaption",
     "swaption/swaption_request.json", 404, _ec_set_field("swaptions", "volatility", "no_such_vol")),
    ("ec:404 cap_floor model missing", "cap_floor",
     "cap_floor/cap_floor_request.json", 404, _ec_set_field("cap_floors", "model", "no_such_model")),
    ("ec:404 cds credit_curve missing", "cds",
     "cds/cds_request.json", 404, _ec_set_field("cds_list", "credit_curve_id", "no_such_credit")),
    # ---- 400 INVALID_ARGUMENT: wrong-kind reference (id resolves to wrong variant) ----
    ("ec:400 swaption model is wrong kind", "swaption",
     "swaption/swaption_request.json", 400, _ec_flip_model_type("CapFloorModelSpec")),
    ("ec:400 cap_floor model is wrong kind", "cap_floor",
     "cap_floor/cap_floor_request.json", 400, _ec_flip_model_type("SwaptionModelSpec")),
    # ---- 400 INVALID_ARGUMENT: malformed request (empty list / missing required field) ----
    ("ec:400 fixed_rate_bond empty bonds list", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400, _ec_empty_list("bonds")),
    ("ec:400 vanilla_swap empty swaps list", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400, _ec_empty_list("swaps")),
    ("ec:400 swaption missing required model field", "swaption",
     "swaption/swaption_request.json", 400, _ec_del_field("swaptions", "model")),
    ("ec:400 fixed_rate_bond missing discounting_curve field", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400, _ec_del_field("bonds", "discounting_curve")),
    # ---- 400 INVALID_ARGUMENT: duplicate ids in a request list ----
    # Defining the same id twice in a curves/indices list previously kept the
    # first definition and silently dropped the rest, which can change the
    # priced result. A duplicate id must be a named 400.
    ("ec:400 vanilla_swap duplicate curve id", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_dup_curve_id(),
     _ec_body_contains("duplicate curve id")),
    ("ec:400 vanilla_swap duplicate index id", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_dup_index_id(),
     _ec_body_contains("duplicate index id")),
    # ---- 400 INVALID_ARGUMENT: swap-leg convention presence ----
    # A swap-leg convention enum omitted from the request must be rejected, not
    # silently defaulted to the alphabetical-0 value.
    # The conventions every coupon kind shares live on the leg itself
    # (`<Swap>.<leg>.day_counter`, not under `coupons`).
    ("ec:400 vanilla_swap fixed leg missing day_counter", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "fixed_leg", "day_counter"),
     _ec_body_contains("VanillaSwap.fixed_leg.day_counter is required")),
    ("ec:400 vanilla_swap fixed leg missing payment_convention", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "fixed_leg", "payment_convention"),
     _ec_body_contains("VanillaSwap.fixed_leg.payment_convention is required")),
    ("ec:400 vanilla_swap floating leg missing day_counter", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "floating_leg", "day_counter"),
     _ec_body_contains("VanillaSwap.floating_leg.day_counter is required")),
    # The notional is a `= null` scalar on the leg: absent is a named 400,
    # never a silently-priced zero (a bare double would default to 0).
    ("ec:400 vanilla_swap fixed leg missing notional", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "fixed_leg", "notional"),
     _ec_body_contains("VanillaSwap.fixed_leg.notional is required")),
    # SwapLeg.schedule is a `(required)` table in the schema: a leg without
    # one never reaches the mapper, the JSON parser rejects the request
    # (a 400 naming the missing field and the table).
    ("ec:400 vanilla_swap fixed leg missing schedule", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "fixed_leg", "schedule"),
     _ec_body_contains("required field is missing: schedule in SwapLeg")),
    # ---- 400 INVALID_ARGUMENT: hard-coded convention-default presence ----
    # Conventions that previously carried a hard-coded default (recovery 0.4,
    # fixing_days 2, calendar TARGET, period unit Months, schedule end-of-month,
    # a helper settlement lag) are now presence-required: an omission is a named
    # 400 rather than a silent market-default assumption.
    ("ec:400 cds credit curve missing recovery_rate", "cds",
     "cds/cds_request.json", 400,
     _ec_del_credit_field("recovery_rate"),
     _ec_body_contains("CreditCurveSpec.recovery_rate is required")),
    ("ec:400 cds credit curve missing curve_interpolator", "cds",
     "cds/cds_request.json", 400,
     _ec_del_credit_field("curve_interpolator"),
     _ec_body_contains("CreditCurveSpec.curve_interpolator is required")),
    ("ec:400 cds credit helper missing settlement_days", "cds",
     "cds/cds_request.json", 400,
     _ec_del_credit_helper_field("settlement_days"),
     _ec_body_contains("CdsHelperConventions.settlement_days is required")),
    ("ec:400 cds index missing fixing_days", "cds",
     "cds/cds_request.json", 400,
     _ec_del_index_field("fixing_days"),
     _ec_body_contains("IndexDef.fixing_days")),
    ("ec:400 cds index missing calendar", "cds",
     "cds/cds_request.json", 400,
     _ec_del_index_field("calendar"),
     _ec_body_contains("IndexDef.calendar")),
    ("ec:400 cds index tenor missing period unit", "cds",
     "cds/cds_request.json", 400,
     _ec_del_index_tenor_unit(),
     _ec_body_contains("unit is required")),
    ("ec:400 cds schedule missing end_of_month", "cds",
     "cds/cds_request.json", 400,
     _ec_del_cds_schedule_field("end_of_month"),
     _ec_body_contains("Schedule.end_of_month is required")),
    # ---- 400 INVALID_ARGUMENT: bond / yield convention presence ----
    # A bond or yield convention enum omitted from the request must be rejected,
    # not silently defaulted to the alphabetical-0 value.
    ("ec:400 fixed_rate_bond missing accrual_day_counter", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_bond_field("bonds", "fixed_rate_bond", "accrual_day_counter"),
     _ec_body_contains("FixedRateBond.accrual_day_counter is required")),
    # issue_date is optional on the wire; without the guard the parser
    # dereferenced the null string and crashed the worker.
    ("ec:400 fixed_rate_bond missing issue_date", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_bond_field("bonds", "fixed_rate_bond", "issue_date"),
     _ec_body_contains("FixedRateBond.issue_date is required")),
    ("ec:400 fixed_rate_bond yield missing compounding", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_yield_field("bonds", "compounding"),
     _ec_body_contains("Yield.compounding is required")),
    ("ec:400 fixed_rate_bond yield missing frequency", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_yield_field("bonds", "frequency"),
     _ec_body_contains("Yield.frequency is required")),
    # ---- 400 INVALID_ARGUMENT: null-deref guards (optional date fields
    #      omitted on curve helpers; pre-guard these crashed the worker) ----
    ("ec:400 fixed_rate_bond curve BondHelper missing issue_date", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400, _ec_del_curve_point_field("BondHelper", "issue_date")),
    ("ec:400 fixed_rate_bond curve FutureHelper missing future_start_date", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400, _ec_future_helper_missing_start_date()),
    # ---- 400 INVALID_ARGUMENT: identity / structural field presence ----
    # Structural sub-tables and identity strings a product cannot be built
    # without must fail loudly with a named message, never be silently omitted.
    ("ec:400 fixed_rate_bond curve BondHelper missing schedule", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_curve_point_field("BondHelper", "schedule"),
     _ec_body_contains("BondHelper.schedule is required")),
    # SwapLeg.discounting_curve is a `(required)` string in the schema: a leg
    # without one never reaches the mapper, the JSON parser rejects the request.
    ("ec:400 vanilla_swap fixed leg missing discounting_curve", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "fixed_leg", "discounting_curve"),
     _ec_body_contains("discounting_curve")),
    ("ec:400 cds missing credit_curve_id reference", "cds",
     "cds/cds_request.json", 400,
     _ec_del_field("cds_list", "credit_curve_id"),
     _ec_body_contains("PriceCDS entry requires credit_curve_id")),
    # ---- 400 INVALID_ARGUMENT: unbounded schedule generation guard ----
    ("ec:400 fixed_rate_bond 300y daily schedule rejected", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400, _ec_abusive_schedule("bonds", "fixed_rate_bond")),
    # ---- 400 INVALID_ARGUMENT: wire date format is strictly ISO-8601 ----
    # Dates on the wire must be YYYY-MM-DD. A slash format, an impossible
    # calendar date, or free text must all be rejected with a named 400 that
    # names the offending value, never silently normalized or 500'd.
    ("ec:400 fixed_rate_bond slash date rejected", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_date("bonds", "fixed_rate_bond", "effective_date", "2024/06/15"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 fixed_rate_bond impossible date rejected", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_date("bonds", "fixed_rate_bond", "effective_date", "2024-02-30"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 fixed_rate_bond textual date rejected", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_date("bonds", "fixed_rate_bond", "effective_date", "June 5th"),
     _ec_body_contains("expected YYYY-MM-DD")),
    # ---- 400 INVALID_ARGUMENT: stub-period control (first_date / next_to_last_date) ----
    # The optional stub dates must lie strictly inside (effective_date,
    # termination_date), first_date must be on or before next_to_last_date, be
    # ISO-8601, and be compatible with the date-generation rule. Each violation
    # is a named 400 (never an opaque 500), even when QuantLib is the one that
    # rejects the stub/rule combination.
    ("ec:400 fixed_rate_bond first_date after termination", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", first_date="2020-05-15"),
     _ec_body_contains("Schedule.first_date")),
    ("ec:400 fixed_rate_bond first_date before effective", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", first_date="2000-05-15"),
     _ec_body_contains("Schedule.first_date")),
    ("ec:400 fixed_rate_bond next_to_last_date after termination", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", next_to_last_date="2020-05-15"),
     _ec_body_contains("Schedule.next_to_last_date")),
    ("ec:400 fixed_rate_bond first_date after next_to_last_date", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond",
                             first_date="2015-05-15", next_to_last_date="2009-05-15"),
     _ec_body_contains("must be on or before next_to_last_date")),
    ("ec:400 fixed_rate_bond non-ISO first_date", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond", first_date="2010/05/15"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 fixed_rate_bond Zero rule rejects stub date", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_schedule_fields("bonds", "fixed_rate_bond",
                             date_generation_rule="Zero", first_date="2010-05-15"),
     _ec_body_contains("Schedule")),
    # ---- 400 INVALID_ARGUMENT: schedule convention presence ----
    # A Schedule convention enum omitted from the request must be rejected, not
    # silently defaulted to the alphabetical-0 value (calendar Argentina, etc.).
    ("ec:400 fixed_rate_bond schedule missing calendar", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_schedule_field("bonds", "fixed_rate_bond", "calendar"),
     _ec_body_contains("Schedule.calendar is required")),
    ("ec:400 fixed_rate_bond schedule missing convention", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_schedule_field("bonds", "fixed_rate_bond", "convention"),
     _ec_body_contains("Schedule.convention is required")),
    # ---- 400 INVALID_ARGUMENT: curve-helper / term-structure convention presence ----
    # A curve-helper convention enum omitted from the request must be rejected,
    # not silently defaulted to the alphabetical-0 value. Same for the
    # TermStructure's own day_counter / interpolator.
    ("ec:400 fixed_rate_bond curve DepositHelper missing day_counter", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_curve_point_field("DepositHelper", "day_counter"),
     _ec_body_contains("DepositHelper.day_counter is required")),
    ("ec:400 fixed_rate_bond curve TermStructure missing interpolator", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_curve_field("interpolator"),
     _ec_body_contains("TermStructure.interpolator is required")),
    # ---- 400 INVALID_ARGUMENT: volatility base convention presence ----
    # An IrVol/BlackVol base convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value.
    ("ec:400 swaption vol base missing day_counter", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_del_vol_base_field("day_counter"),
     _ec_body_contains("IrVolBaseSpec.day_counter is required")),
    ("ec:400 swaption vol base missing calendar", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_del_vol_base_field("calendar"),
     _ec_body_contains("IrVolBaseSpec.calendar is required")),
    # ---- 400 INVALID_ARGUMENT: vol quotation convention presence ----
    # volatility_type says whether the supplied numbers are normal or lognormal
    # vols. An omitted value is indistinguishable from the alphabetical-0
    # default (Normal), which would price a lognormal-quoted surface as normal
    # vols, so it must be rejected instead.
    ("ec:400 swaption vol base missing volatility_type", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_del_vol_base_field("volatility_type"),
     _ec_body_contains("IrVolBaseSpec.volatility_type is required")),
    ("ec:400 cap_floor vol base missing volatility_type", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_del_vol_base_field("volatility_type"),
     _ec_body_contains("IrVolBaseSpec.volatility_type is required")),
    # ---- 400 INVALID_ARGUMENT: pricing-model discriminator presence ----
    # model_type selects the pricing engine. An omitted value is
    # indistinguishable from the alphabetical-0 default (Black for the rate
    # models, BlackScholesAnalytic for equity), so it must be rejected rather
    # than silently pricing on an engine the caller never asked for.
    ("ec:400 swaption model missing model_type", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_del_model_payload_field("model_type"),
     _ec_body_contains("SwaptionModelSpec.model_type is required")),
    ("ec:400 cap_floor model missing model_type", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
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
     "swaption/swaption_request.json", 400,
     _ec_del_instrument_field("swaptions", "swaption", "exercise_type"),
     _ec_body_contains("Swaption.exercise_type is required")),
    ("ec:400 swaption missing settlement_type", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_del_instrument_field("swaptions", "swaption", "settlement_type"),
     _ec_body_contains("Swaption.settlement_type is required")),
    # ---- 400 INVALID_ARGUMENT: cap-floor / CDS convention presence ----
    # A cap/floor or CDS convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value. (The FRA's
    # own day_counter/calendar/business_day_convention are accepted-but-unused —
    # QuantLib builds the FRA from the index — so they are NOT presence-required;
    # see test_fra.py::test_fra_conventions_optional_and_ignored.)
    ("ec:400 cap_floor missing business_day_convention", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "cap_floor", "business_day_convention"),
     _ec_body_contains("CapFloor.business_day_convention is required")),
    ("ec:400 cds missing day_counter", "cds",
     "cds/cds_request.json", 400,
     _ec_del_instrument_field("cds_list", "cds", "day_counter"),
     _ec_body_contains("CDS.day_counter is required")),
    # ---- 400 INVALID_ARGUMENT: FRA / cap-floor / CDS discriminator presence ----
    # The product-discriminator enums are presence-required: an omitted
    # discriminator must be rejected, not silently defaulted to the
    # alphabetical-0 value (Long FRA, Cap, or CDS protection Buyer).
    ("ec:400 fra missing fra_type", "fra",
     "fra/fra_request.json", 400,
     _ec_del_instrument_field("fras", "fra", "fra_type"),
     _ec_body_contains("FRA.fra_type is required")),
    ("ec:400 cap_floor missing cap_floor_type", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "cap_floor", "cap_floor_type"),
     _ec_body_contains("CapFloor.cap_floor_type is required")),
    ("ec:400 cds missing side", "cds",
     "cds/cds_request.json", 400,
     _ec_del_instrument_field("cds_list", "cds", "side"),
     _ec_body_contains("CDS.side is required")),
    # ---- 400 INVALID_ARGUMENT: SwapLeg shape (vanilla / OIS / basis / swaption) ----
    # Every leg is a SwapLeg: pay_receive, currency, coupons_type + coupons,
    # discounting_curve, forwarding_curve (floating kinds only). The mapper
    # rejects a malformed leg with a 400 naming `<Swap>.<leg>.<field>`.
    # pay_receive decides which way round the trade is booked: an omitted
    # value is a 400, never a default side; one leg is paid, the other received.
    ("ec:400 vanilla_swap fixed leg missing pay_receive", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "fixed_leg", "pay_receive"),
     _ec_body_contains("VanillaSwap.fixed_leg.pay_receive is required (Pay or Receive)")),
    ("ec:400 vanilla_swap both legs Pay", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_attr("swaps", "vanilla_swap", "floating_leg", "pay_receive", "Pay"),
     _ec_body_contains("VanillaSwap.fixed_leg.pay_receive and VanillaSwap.floating_leg."
                       "pay_receive are both Pay: one leg must be paid and the other "
                       "received")),
    ("ec:400 ois_swap fixed leg missing pay_receive", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "ois_swap", "fixed_leg", "pay_receive"),
     _ec_body_contains("OisSwap.fixed_leg.pay_receive is required (Pay or Receive)")),
    ("ec:400 ois_swap both legs Receive", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_set_leg_attr("swaps", "ois_swap", "fixed_leg", "pay_receive", "Receive"),
     _ec_body_contains("OisSwap.fixed_leg.pay_receive and OisSwap.overnight_leg.pay_receive "
                       "are both Receive")),
    ("ec:400 basis_swap leg1 missing pay_receive", "basis_swap",
     "ir_swaps/basis_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "basis_swap", "leg1", "pay_receive"),
     _ec_body_contains("BasisSwap.leg1.pay_receive is required (Pay or Receive)")),
    ("ec:400 swaption underlying missing pay_receive", "swaption",
     "swaption/swaption_request.json", 400,
     lambda req: (req["swaptions"][0]["swaption"]["underlying"]["fixed_leg"]
                  .pop("pay_receive") and req) or req,
     _ec_body_contains("Swaption.underlying.fixed_leg.pay_receive is required")),
    # The role decides the coupon kind: a fixed leg carries FixedCoupons, an
    # OIS overnight leg OvernightCoupons, a basis leg IborCoupons or
    # OvernightCoupons. The 400 names the role's allowed kinds and the one sent.
    ("ec:400 vanilla_swap fixed leg with IborCoupons", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_vanilla_fixed_leg_as_ibor(),
     _ec_body_contains("VanillaSwap.fixed_leg.coupons must be FixedCoupons (got IborCoupons)")),
    ("ec:400 ois_swap overnight leg with IborCoupons", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_ois_overnight_leg_as_ibor(),
     _ec_body_contains(
         "OisSwap.overnight_leg.coupons must be OvernightCoupons (got IborCoupons)")),
    ("ec:400 basis_swap leg with FixedCoupons", "basis_swap",
     "ir_swaps/basis_swap_request.json", 400,
     _ec_basis_leg_as_fixed(),
     _ec_body_contains(
         "BasisSwap.leg1.coupons must be IborCoupons or OvernightCoupons (got FixedCoupons)")),
    ("ec:400 swaption underlying floating leg with CmsCoupons", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_swaption_floating_leg_as_cms(),
     _ec_body_contains(
         "Swaption.underlying.floating_leg.coupons must be IborCoupons (got CmsCoupons)")),
    # The coupon kind must match the index kind (checked once the index is
    # resolved, so after market data; still a 400 naming the other kind).
    ("ec:400 vanilla_swap IborCoupons on an overnight index", "vanilla_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_vanilla_with_ibor_coupons_on_overnight_index(),
     _ec_body_contains("VanillaSwap.floating_leg.coupons is IborCoupons but its index 'ESTR' "
                       "is an overnight index: use OvernightCoupons")),
    ("ec:400 ois_swap OvernightCoupons on an IBOR index", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_set_swap_leg_field("swaps", "ois_swap", "overnight_leg", "index", {"id": "EUR_6M"}),
     _ec_body_contains("OisSwap.overnight_leg.coupons is OvernightCoupons but its index "
                       "'EUR_6M' is not an overnight index: use IborCoupons")),
    # Curves: a floating leg must say what it projects on; a fixed leg
    # projects nothing, so a forwarding_curve there is a mistake, not noise.
    ("ec:400 vanilla_swap forwarding_curve on the fixed leg", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_attr("swaps", "vanilla_swap", "fixed_leg", "forwarding_curve", "discount"),
     _ec_body_contains("VanillaSwap.fixed_leg.forwarding_curve must be absent with "
                       "FixedCoupons")),
    ("ec:400 vanilla_swap floating leg missing forwarding_curve", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "vanilla_swap", "floating_leg", "forwarding_curve"),
     _ec_body_contains("VanillaSwap.floating_leg.forwarding_curve is required with "
                       "IborCoupons")),
    ("ec:400 ois_swap overnight leg missing forwarding_curve", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "ois_swap", "overnight_leg", "forwarding_curve"),
     _ec_body_contains("OisSwap.overnight_leg.forwarding_curve is required with "
                       "OvernightCoupons")),
    # Currency: explicit ISO code on every leg, one currency per
    # single-currency endpoint, and the index's own currency on a floating leg
    # (the engine never infers a currency from an index id).
    ("ec:400 vanilla_swap currency not ISO", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_attr("swaps", "vanilla_swap", "fixed_leg", "currency", "eur"),
     _ec_body_contains("VanillaSwap.fixed_leg.currency must be a three-letter uppercase ISO "
                       "currency code, got 'eur'")),
    ("ec:400 vanilla_swap two currencies", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_attr("swaps", "vanilla_swap", "floating_leg", "currency", "USD"),
     _ec_body_contains("VanillaSwap.fixed_leg.currency 'EUR' and VanillaSwap.floating_leg."
                       "currency 'USD' must be equal on price-vanilla-swap")),
    ("ec:400 basis_swap two currencies", "basis_swap",
     "ir_swaps/basis_swap_request.json", 400,
     _ec_set_leg_attr("swaps", "basis_swap", "leg2", "currency", "USD"),
     _ec_body_contains("BasisSwap.leg1.currency 'EUR' and BasisSwap.leg2.currency 'USD' "
                       "must be equal on price-basis-swap")),
    ("ec:400 vanilla_swap leg currency differs from the index currency", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     lambda req: _ec_set_leg_attr("swaps", "vanilla_swap", "floating_leg", "currency", "USD")(
         _ec_set_leg_attr("swaps", "vanilla_swap", "fixed_leg", "currency", "USD")(req)),
     _ec_body_contains("VanillaSwap.floating_leg.currency 'USD' does not match the currency "
                       "EUR of its index 'EUR_6M' (IndexDef.currency)")),
    # A swaption underlying is a QuantLib::VanillaSwap: one discount curve,
    # no principal exchange, in-advance fixings.
    ("ec:400 swaption underlying legs on different discounting curves", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_set_swaption_leg_attr("floating_leg", "discounting_curve", "other_discount"),
     _ec_body_contains("Swaption.underlying.fixed_leg.discounting_curve 'discount' and "
                       "Swaption.underlying.floating_leg.discounting_curve 'other_discount' "
                       "must be equal: a swaption is priced on one discount curve")),
    ("ec:400 swaption underlying final_exchange true", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_set_swaption_leg_attr("fixed_leg", "final_exchange", True),
     _ec_body_contains("Swaption.underlying.fixed_leg.final_exchange must be absent or false: "
                       "a swaption underlying leg carries no principal exchange")),
    ("ec:400 swaption underlying floating leg in_arrears", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_set_swaption_leg_coupon_field("floating_leg", "in_arrears", True),
     _ec_body_contains("Swaption.underlying.floating_leg.coupons.in_arrears is not supported "
                       "on a swaption underlying")),
    # ---- 400 INVALID_ARGUMENT: swap-direction discriminator presence ----
    # swap_type decides which way round an inflation swap is booked. An omitted value is
    # indistinguishable from the alphabetical-0 default (Payer), so it must be
    # rejected rather than silently pricing the trade in the wrong direction.
    # ---- 400 INVALID_ARGUMENT: OIS payment-lag / overnight-coupon presence ----
    # The payment-lag and overnight-coupon parameters shift every payment /
    # fixing date, so an omitted value must be a named 400, never a silent
    # default; a negative day count would wrap into QuantLib's unsigned
    # Natural, so it is rejected explicitly. Locked on both the priced leg
    # (OvernightCoupons) and the curve helper (OISHelper). payment_calendar /
    # payment_lag are leg-level fields, presence-required on BOTH legs of
    # price-ois-swap (both legs pay on the lagged dates).
    ("ec:400 ois_swap overnight leg missing payment_lag", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "ois_swap", "overnight_leg", "payment_lag"),
     _ec_body_contains("OisSwap.overnight_leg.payment_lag is required")),
    ("ec:400 ois_swap overnight leg missing payment_calendar", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "ois_swap", "overnight_leg", "payment_calendar"),
     _ec_body_contains("OisSwap.overnight_leg.payment_calendar is required")),
    ("ec:400 ois_swap fixed leg missing payment_lag", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "ois_swap", "fixed_leg", "payment_lag"),
     _ec_body_contains("OisSwap.fixed_leg.payment_lag is required")),
    ("ec:400 ois_swap fixed leg missing payment_calendar", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_leg_attr("swaps", "ois_swap", "fixed_leg", "payment_calendar"),
     _ec_body_contains("OisSwap.fixed_leg.payment_calendar is required")),
    ("ec:400 ois_swap leg missing averaging_method", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_swap_leg_field("swaps", "ois_swap", "overnight_leg", "averaging_method"),
     _ec_body_contains("OisSwap.overnight_leg.coupons.averaging_method is required")),
    ("ec:400 ois_swap leg missing apply_observation_shift", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_del_swap_leg_field("swaps", "ois_swap", "overnight_leg",
                            "apply_observation_shift"),
     _ec_body_contains("OisSwap.overnight_leg.coupons.apply_observation_shift is required")),
    ("ec:400 ois_swap leg negative lockout_days", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_set_swap_leg_field("swaps", "ois_swap", "overnight_leg",
                            "lockout_days", -1),
     _ec_body_contains("OisSwap.overnight_leg.coupons.lockout_days must be non-negative")),
    ("ec:400 ois_swap leg negative lookback_days", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_set_swap_leg_field("swaps", "ois_swap", "overnight_leg",
                            "lookback_days", -1),
     _ec_body_contains("OisSwap.overnight_leg.coupons.lookback_days must be non-negative")),
    ("ec:400 ois helper missing payment_lag", "ois_swap",
     "ir_swaps/ois_eur_5y_payer_estr.json", 400,
     _ec_del_curve_point_field("OISHelper", "payment_lag"),
     _ec_body_contains("OISHelper.payment_lag is required")),
    ("ec:400 ois helper missing averaging_method", "ois_swap",
     "ir_swaps/ois_eur_5y_payer_estr.json", 400,
     _ec_del_curve_point_field("OISHelper", "averaging_method"),
     _ec_body_contains("OISHelper.averaging_method is required")),
    ("ec:400 ois helper missing apply_observation_shift", "ois_swap",
     "ir_swaps/ois_eur_5y_payer_estr.json", 400,
     _ec_del_curve_point_field("OISHelper", "apply_observation_shift"),
     _ec_body_contains("OISHelper.apply_observation_shift is required")),
    ("ec:400 ois helper negative lockout_days", "ois_swap",
     "ir_swaps/ois_eur_5y_payer_estr.json", 400,
     _ec_set_curve_point_field("OISHelper", "lockout_days", -2),
     _ec_body_contains("OISHelper.lockout_days must be non-negative")),
    ("ec:400 ois helper negative lookback_days", "ois_swap",
     "ir_swaps/ois_eur_5y_payer_estr.json", 400,
     _ec_set_curve_point_field("OISHelper", "lookback_days", -1),
     _ec_body_contains("OISHelper.lookback_days must be non-negative")),
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
    # ---- 400: a ratio YoY index's underlying must be a zero inflation index ----
    # The kind is checked before the underlying is built: a YoY spec naming
    # itself, or two YoY specs naming each other, used to recurse until the
    # worker's stack overflowed. The valid request after them checks the
    # worker is still serving.
    ("ec:400 yoy inflation index naming itself as underlying",
     "year_on_year_inflation_swap",
     "inflation/yyiis_eur_5y_payer_annual.json", 400,
     _ec_add_ratio_yoy_specs(("YY_SELF", "YY_SELF")),
     _ec_body_contains("must reference a zero inflation index for index id 'YY_SELF': "
                       "YY_SELF")),
    ("ec:400 yoy inflation indices naming each other as underlying",
     "year_on_year_inflation_swap",
     "inflation/yyiis_eur_5y_payer_annual.json", 400,
     _ec_add_ratio_yoy_specs(("YY_CYCLE_A", "YY_CYCLE_B"), ("YY_CYCLE_B", "YY_CYCLE_A")),
     _ec_body_contains("must reference a zero inflation index for index id 'YY_CYCLE_A': "
                       "YY_CYCLE_B")),
    ("ec:200 year_on_year_inflation_swap after the cyclic index specs",
     "year_on_year_inflation_swap",
     "inflation/yyiis_eur_5y_payer_annual.json", 200, _ec_identity),
    # ---- 400 INVALID_ARGUMENT: coupon-pricer optionlet-vol convention presence ----
    # A ConstantOptionletVolatility convention enum omitted from a coupon pricer
    # must be rejected, not silently defaulted to the alphabetical-0 value.
    ("ec:400 coupon pricer optionlet vol missing calendar", "floating_rate_bond",
     "bonds/floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("calendar"),
     _ec_body_contains("ConstantOptionletVolatility.calendar is required")),
    ("ec:400 coupon pricer optionlet vol missing business_day_convention",
     "floating_rate_bond", "bonds/floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("business_day_convention"),
     _ec_body_contains("ConstantOptionletVolatility.business_day_convention is required")),
    ("ec:400 coupon pricer optionlet vol missing day_counter", "floating_rate_bond",
     "bonds/floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("day_counter"),
     _ec_body_contains("ConstantOptionletVolatility.day_counter is required")),
    # ---- 400 INVALID_ARGUMENT: request value scalars are presence-required ----
    # A bare double/enum on the wire defaults to 0/first-enum when omitted, so a
    # forgotten value would silently price as zero instead of erroring. Each of
    # these must now be a named 400.
    ("ec:400 coupon pricer optionlet vol missing volatility", "floating_rate_bond",
     "bonds/floating_rate_bond_request.json", 400,
     _ec_del_optionlet_vol_field("volatility"),
     _ec_body_contains("ConstantOptionletVolatility.volatility is required")),
    ("ec:400 quote spec missing value", "cds",
     "cds/cds_complex_request.json", 400,
     _ec_del_quote_value(),
     _ec_body_contains("QuoteSpec.value is required")),
    ("ec:400 cds quote missing quote_type", "cds",
     "cds/cds_complex_request.json", 400,
     _ec_del_cds_quote_type(),
     _ec_body_contains("CdsQuote.quote_type is required")),
    ("ec:400 bond helper missing redemption", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_curve_point_field("BondHelper", "redemption"),
     _ec_body_contains("BondHelper.redemption is required")),
    ("ec:400 fra missing strike", "fra",
     "fra/fra_request.json", 400,
     _ec_del_instrument_field("fras", "fra", "strike"),
     _ec_body_contains("FRA.strike is required")),
    ("ec:400 cap_floor missing strike", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "cap_floor", "strike"),
     _ec_body_contains("CapFloor.strike is required")),
    # Collar: floor_strike required, <= the cap strike, and absent otherwise.
    ("ec:400 cap_floor collar missing floor_strike", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_set_instrument_fields("cap_floors", "cap_floor", {"cap_floor_type": "Collar"}),
     _ec_body_contains("CapFloor.floor_strike is required for a Collar")),
    ("ec:400 cap_floor collar floor_strike above the cap strike", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_set_instrument_fields("cap_floors", "cap_floor",
                               {"cap_floor_type": "Collar", "floor_strike": 0.05}),
     _ec_body_contains("CapFloor.floor_strike must be <= strike (the cap strike) for a Collar")),
    ("ec:400 cap_floor floor_strike on a cap", "cap_floor",
     "cap_floor/cap_floor_request.json", 400,
     _ec_set_instrument_fields("cap_floors", "cap_floor", {"floor_strike": 0.02}),
     _ec_body_contains("CapFloor.floor_strike must be absent unless cap_floor_type is Collar")),
    # ---- 400 INVALID_ARGUMENT: fail-closed enum handling ----
    # The CDS credit-curve bootstrap supports LogLinear only; ForwardFlat
    # and LogCubic used to be silently priced as LogLinear. The complex request
    # is used because it carries real par-spread quotes (the simple request
    # takes the flat-hazard branch and never reaches the interpolator switch).
    ("ec:400 cds credit interpolator ForwardFlat rejected", "cds",
     "cds/cds_complex_request.json", 400,
     _ec_set_credit_curve_field("curve_interpolator", "ForwardFlat"),
     _ec_body_contains("Unsupported credit curve interpolator")),
    ("ec:400 cds credit interpolator LogCubic rejected", "cds",
     "cds/cds_complex_request.json", 400,
     _ec_set_credit_curve_field("curve_interpolator", "LogCubic"),
     _ec_body_contains("Unsupported credit curve interpolator")),
    # An out-of-range CDS engine enum (raw integers are accepted by the
    # FlatBuffers JSON parser) used to silently price with the MidPoint engine.
    ("ec:400 cds engine type out of range", "cds",
     "cds/cds_request.json", 400,
     _ec_set_model_payload_field("engine_type", 99),
     _ec_body_contains("Unsupported CDS engine type")),
    # A credit curve with no flat_hazard_rate and no quotes used to silently
    # price off an invented 1% flat hazard. It must now be rejected: an empty
    # credit curve is an error, never a fabricated default hazard rate. The
    # simple request carries an empty quotes list plus flat_hazard_rate;
    # dropping the hazard leaves nothing to build the curve from.
    ("ec:400 cds empty credit curve rejected", "cds",
     "cds/cds_request.json", 400,
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
     "swaption/swaption_request.json", 400,
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
     "swaption/swaption_request.json", 400,
     _ec_swaption_rebump_without_rates(),
     _ec_body_contains("Swaption rebump pricing requires pricing.rates.curves")),
    # ---- 400 INVALID_ARGUMENT: a notional must be present and strictly positive ----
    ("ec:400 basis_swap negative notional rejected", "basis_swap",
     "ir_swaps/basis_swap_request.json", 400,
     _ec_basis_swap_negative_notional(),
     _ec_body_contains("BasisSwap.leg1.notional must be positive")),
    # ---- 501 UNIMPLEMENTED: valid request, feature not built yet ----
    ("ec:501 swaption curve uses unimplemented helper", "swaption",
     "swaption/swaption_request.json", 501, _ec_unimplemented_curve_point()),
    # ---- sample-vol-surfaces per-item error contract (list/query endpoint) ----
    # (A) Malformed TOP-LEVEL input -> transport error. Empty queries trips
    #     toInputs validation, which propagates as INVALID_ARGUMENT -> HTTP 400.
    ("ec:400 sample_vol_surfaces empty queries list", "sample_vol_surfaces",
     "vol/sample_vol_surfaces_request.json", 400, _ec_empty_list("queries")),
    # (B) Per-item failure -> HTTP 200 with a per-item error entry. Pointing a
    #     query at a non-existent vol-surface id resolves the top-level request
    #     fine but fails when that query is sampled, so the error is folded into
    #     that result's error field rather than failing the whole batch.
    ("ec:200 sample_vol_surfaces per-item error on missing vol id", "sample_vol_surfaces",
     "vol/sample_vol_surfaces_request.json", 200,
     _ec_set_field("queries", "vol_id", "no_such_vol_surface_xyz"),
     _ec_body_has_per_item_error("vol_id", "no_such_vol_surface_xyz")),
    # ---- bootstrap-curves A5 guard: the curve build and the mapper's
    #      pillar-date extraction both deref FutureHelper.future_start_date;
    #      pre-guard this request SIGSEGV'd the worker. Post-guard it must be
    #      a clean INVALID_ARGUMENT (curve-parse guard fires at the transport
    #      level before the per-query error fold). ----
    ("ec:400 bootstrap_curves FutureHelper missing future_start_date",
     "bootstrap_curves", "curves/bootstrap_curves_tenor_grid.json", 400,
     _ec_future_helper_missing_start_date()),
    # ---- 400 INVALID_ARGUMENT: amortizing notionals validation ----
    # When present the per-period notionals vector must be non-empty, carry
    # exactly one entry per coupon period, and hold only positive amounts. The
    # vanilla fixed leg schedule is annual 5Y (5 periods).
    ("ec:400 vanilla_swap notionals too short", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg",
                           [1e7, 1e7, 1e7, 1e7]),
     _ec_body_contains_all("VanillaSwap.fixed_leg.notionals",
                           "has 4 entries but the schedule has 5 periods")),
    ("ec:400 vanilla_swap notionals too long", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg",
                           [1e7, 1e7, 1e7, 1e7, 1e7, 1e7]),
     _ec_body_contains_all("VanillaSwap.fixed_leg.notionals",
                           "has 6 entries but the schedule has 5 periods")),
    ("ec:400 vanilla_swap notionals empty", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg", []),
     _ec_body_contains_all("VanillaSwap.fixed_leg.notionals",
                           "must be non-empty when present")),
    ("ec:400 vanilla_swap notionals non-positive entry", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "vanilla_swap", "fixed_leg",
                           [1e7, 1e7, 1e7, 1e7, 0.0]),
     _ec_body_contains_all("VanillaSwap.fixed_leg.notionals",
                           "entries must be positive")),
    # ---- 400 INVALID_ARGUMENT: only the vanilla swap takes amortizing notionals ----
    # The same SwapLeg table serves the OIS, basis and swaption-underlying
    # legs, which do not support amortizing notionals yet.
    # A present notionals vector must be rejected naming the endpoint, never
    # silently ignored.
    ("ec:400 ois_swap fixed leg notionals rejected", "ois_swap",
     "ir_swaps/ois_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "ois_swap", "fixed_leg", [1e7]),
     _ec_body_contains("OisSwap.fixed_leg.notionals (price-ois-swap) does not support "
                       "amortizing notionals yet")),
    ("ec:400 basis_swap leg notionals rejected", "basis_swap",
     "ir_swaps/basis_swap_request.json", 400,
     _ec_set_leg_notionals("swaps", "basis_swap", "leg1", [1e7]),
     _ec_body_contains("BasisSwap.leg1.notionals (price-basis-swap) does not support "
                       "amortizing notionals yet")),
    ("ec:400 swaption underlying notionals rejected", "swaption",
     "swaption/swaption_request.json", 400,
     _ec_set_swaption_underlying_notionals([1e7]),
     _ec_body_contains("Swaption.underlying.fixed_leg.notionals (swaption underlying) "
                       "does not support amortizing notionals yet")),
    # ---- 400 INVALID_ARGUMENT: zero-coupon bond convention / date presence ----
    # A zero-coupon bond convention enum omitted from the request must be
    # rejected, not silently defaulted to the alphabetical-0 value; wire dates
    # must be strictly ISO-8601 YYYY-MM-DD.
    ("ec:400 zero_coupon_bond missing calendar", "zero_coupon_bond",
     "bonds/zero_coupon_bond_request.json", 400,
     _ec_del_bond_field("bonds", "zero_coupon_bond", "calendar"),
     _ec_body_contains("ZeroCouponBond.calendar is required")),
    ("ec:400 zero_coupon_bond missing payment_convention", "zero_coupon_bond",
     "bonds/zero_coupon_bond_request.json", 400,
     _ec_del_bond_field("bonds", "zero_coupon_bond", "payment_convention"),
     _ec_body_contains("ZeroCouponBond.payment_convention is required")),
    ("ec:400 zero_coupon_bond non-ISO maturity_date rejected", "zero_coupon_bond",
     "bonds/zero_coupon_bond_request.json", 400,
     _ec_set_nested_field("bonds", "zero_coupon_bond", "maturity_date", "2026/01/15"),
     _ec_body_contains("expected YYYY-MM-DD")),
    # ---- 400 INVALID_ARGUMENT: zero-coupon swap convention / form / date presence ----
    # The zero-coupon swap payment conventions are presence-required; the fixed
    # side must carry exactly one of the fixed_payment / fixed_rate forms; the
    # maturity must be strictly after the start; wire dates must be ISO-8601.
    ("ec:400 zero_coupon_swap missing payment_calendar", "zero_coupon_swap",
     "zero_coupon_swap/zero_coupon_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_swap", "payment_calendar"),
     _ec_body_contains("ZeroCouponSwap.payment_calendar is required")),
    ("ec:400 zero_coupon_swap missing payment_convention", "zero_coupon_swap",
     "zero_coupon_swap/zero_coupon_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_swap", "payment_convention"),
     _ec_body_contains("ZeroCouponSwap.payment_convention is required")),
    ("ec:400 zero_coupon_swap neither fixed_payment nor fixed_rate", "zero_coupon_swap",
     "zero_coupon_swap/zero_coupon_swap_request.json", 400,
     _ec_del_instrument_field("swaps", "zero_coupon_swap", "fixed_payment"),
     _ec_body_contains(
         "ZeroCouponSwap must contain exactly one of fixed_payment or fixed_rate")),
    ("ec:400 zero_coupon_swap both fixed_payment and fixed_rate", "zero_coupon_swap",
     "zero_coupon_swap/zero_coupon_swap_request.json", 400,
     _ec_set_nested_field("swaps", "zero_coupon_swap", "fixed_rate", 0.032),
     _ec_body_contains(
         "ZeroCouponSwap must contain exactly one of fixed_payment or fixed_rate")),
    ("ec:400 zero_coupon_swap maturity not after start", "zero_coupon_swap",
     "zero_coupon_swap/zero_coupon_swap_request.json", 400,
     _ec_set_nested_field("swaps", "zero_coupon_swap", "maturity_date", "2025-01-17"),
     _ec_body_contains("ZeroCouponSwap.maturity_date must be after start_date")),
    ("ec:400 zero_coupon_swap non-ISO maturity_date rejected", "zero_coupon_swap",
     "zero_coupon_swap/zero_coupon_swap_request.json", 400,
     _ec_set_nested_field("swaps", "zero_coupon_swap", "maturity_date", "2030/01/17"),
     _ec_body_contains("expected YYYY-MM-DD")),
    ("ec:400 vanilla_swap CMS cap below floor rejected", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_collared_lineartsr.json", 400,
     _ec_set_cms_cap_floor(0.02, 0.03),
     _ec_body_contains("VanillaSwap.floating_leg.coupons.cap must be >= floor")),
    # A CMS leg pays on its schedule dates: the leg-level payment_calendar /
    # payment_lag (optional on the other vanilla legs) are rejected rather
    # than silently ignored.
    ("ec:400 vanilla_swap CMS leg with payment_lag rejected", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json", 400,
     _ec_set_leg_attr("swaps", "vanilla_swap", "floating_leg", "payment_lag", 2),
     _ec_body_contains("VanillaSwap.floating_leg.payment_lag is not supported with "
                       "CmsCoupons")),

    # The year-on-year inflation cap/floor: cap_floor_type / day_counter /
    # payment_convention are presence-required; the strike set must match the
    # type (Cap => cap_rate only, Floor => floor_rate only, Collar => both with
    # cap_rate >= floor_rate); referenced ids must resolve; dates must be ISO.
    ("ec:400 yoy_inflation_cap_floor missing cap_floor_type",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "year_on_year_inflation_cap_floor",
                              "cap_floor_type"),
     _ec_body_contains("YoYInflationCapFloor.cap_floor_type is required")),
    ("ec:400 yoy_inflation_cap_floor missing day_counter",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "year_on_year_inflation_cap_floor",
                              "day_counter"),
     _ec_body_contains("YoYInflationCapFloor.day_counter is required")),
    ("ec:400 yoy_inflation_cap_floor missing payment_convention",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_del_instrument_field("cap_floors", "year_on_year_inflation_cap_floor",
                              "payment_convention"),
     _ec_body_contains("YoYInflationCapFloor.payment_convention is required")),
    ("ec:400 yoy_inflation_cap_floor Cap carrying floor_rate",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_set_nested_field("cap_floors", "year_on_year_inflation_cap_floor",
                          "floor_rate", 0.01),
     _ec_body_contains("YoYInflationCapFloor of type Cap must not carry floor_rate")),
    ("ec:400 yoy_inflation_cap_floor Collar missing floor_rate",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_set_nested_field("cap_floors", "year_on_year_inflation_cap_floor",
                          "cap_floor_type", "Collar"),
     _ec_body_contains(
         "YoYInflationCapFloor of type Collar requires both cap_rate and floor_rate")),
    ("ec:400 yoy_inflation_cap_floor Collar cap below floor",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_yoy_cf_collar_cap_below_floor(),
     _ec_body_contains("YoYInflationCapFloor Collar cap_rate must be >= floor_rate")),
    ("ec:400 yoy_inflation_cap_floor index id not matching curve",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_set_nested_field("cap_floors", "year_on_year_inflation_cap_floor",
                          "inflation_index_id", "NOPE_YY"),
     _ec_body_contains(
         "inflation_index_id does not match inflation_curve")),
    ("ec:404 yoy_inflation_cap_floor unknown volatility id",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 404,
     _ec_set_field("cap_floors", "volatility", "NOPE_VOL"),
     _ec_body_contains("YoY optionlet vol not found")),
    ("ec:400 yoy_inflation_cap_floor non-ISO schedule date rejected",
     "year_on_year_inflation_cap_floor",
     "inflation_cap_floor/year_on_year_inflation_cap_floor_request.json", 400,
     _ec_yoy_cf_bad_schedule_date(),
     _ec_body_contains("expected YYYY-MM-DD")),

    # The callable fixed-rate bond: the call schedule must be present and
    # non-empty, with strictly increasing dates, positive clean prices and a
    # present callability_type; the bond conventions follow the presence
    # discipline; the model and discounting curve must resolve; dates are ISO.
    ("ec:400 callable_fixed_rate_bond empty call_schedule",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_callable_empty_call_schedule(),
     _ec_body_contains("call_schedule is required and must be non-empty")),
    ("ec:400 callable_fixed_rate_bond non-increasing call dates",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_callable_non_increasing_dates(),
     _ec_body_contains("call_schedule dates must be strictly increasing")),
    ("ec:400 callable_fixed_rate_bond non-positive call price",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_callable_nonpositive_price(),
     _ec_body_contains("CallabilityEntry.price must be > 0")),
    ("ec:400 callable_fixed_rate_bond missing callability_type",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_callable_del_callability_type(),
     _ec_body_contains("CallabilityEntry.callability_type is required")),
    ("ec:400 callable_fixed_rate_bond missing accrual_day_counter",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_del_instrument_field("bonds", "callable_fixed_rate_bond",
                              "accrual_day_counter"),
     _ec_body_contains("CallableFixedRateBond.accrual_day_counter is required")),
    ("ec:400 callable_fixed_rate_bond missing payment_convention",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_del_instrument_field("bonds", "callable_fixed_rate_bond",
                              "payment_convention"),
     _ec_body_contains("CallableFixedRateBond.payment_convention is required")),
    ("ec:400 callable_fixed_rate_bond missing discounting_curve",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_del_field("bonds", "discounting_curve"),
     _ec_body_contains("PriceCallableFixedRateBond.discounting_curve is required")),
    ("ec:404 callable_fixed_rate_bond unknown model id",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 404,
     _ec_set_field("bonds", "model", "NOPE_MODEL"),
     _ec_body_contains("Model not found")),
    ("ec:400 callable_fixed_rate_bond non-ISO call date rejected",
     "callable_fixed_rate_bond",
     "callable_bonds/callable_fixed_rate_bond_request.json", 400,
     _ec_callable_bad_call_date(),
     _ec_body_contains("expected YYYY-MM-DD")),
    # ---- 400 INVALID_ARGUMENT: bootstrap_trait is the explicit curve-family
    #      selector — required, and validated against the curve's point types ----
    ("ec:400 curve missing bootstrap_trait", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_del_curve_field("bootstrap_trait"),
     _ec_body_contains("TermStructure.bootstrap_trait is required")),
    ("ec:400 curve Discount trait with a ZeroRatePoint", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_inject_zero_point(),
     _ec_body_contains("builds from rate helpers but received a ZeroRatePoint")),
    ("ec:400 curve InterpolatedZero trait with a rate helper", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_curve_field("bootstrap_trait", "InterpolatedZero"),
     _ec_body_contains("builds from zero-rate points but received a")),
    ("ec:400 curve InterpolatedDiscount trait with a rate helper", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_curve_field("bootstrap_trait", "InterpolatedDiscount"),
     _ec_body_contains("builds from discount-factor points but received a")),
    ("ec:400 curve DiscountFactorPoint missing discount_factor", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(
         mutate=lambda pts: pts[1]["point"].pop("discount_factor", None)),
     _ec_body_contains("DiscountFactorPoint.discount_factor is required")),
    ("ec:400 curve DiscountFactorPoint discount_factor not positive", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(
         mutate=lambda pts: pts[1]["point"].__setitem__("discount_factor", -0.1)),
     _ec_body_contains("DiscountFactorPoint.discount_factor must be > 0")),
    ("ec:400 curve DiscountFactorPoint discount_factor zero", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(
         mutate=lambda pts: pts[1]["point"].__setitem__("discount_factor", 0.0)),
     _ec_body_contains("DiscountFactorPoint.discount_factor must be > 0")),
    ("ec:400 curve Discount trait with a DiscountFactorPoint", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_discount_factor(trait="Discount"),
     _ec_body_contains("builds from rate helpers but received a DiscountFactorPoint")),
    ("ec:400 curve InterpolatedFwd trait with a rate helper", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_set_curve_field("bootstrap_trait", "InterpolatedFwd"),
     _ec_body_contains("builds from forward-rate points but received a")),
    ("ec:400 curve ForwardRatePoint missing forward_rate", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(
         mutate=lambda pts: pts[1]["point"].pop("forward_rate", None)),
     _ec_body_contains("ForwardRatePoint.forward_rate is required")),
    # Non-finite forward_rate is rejected by the parser's std::isfinite guard,
    # but that cannot be exercised over the JSON wire: JSON has no inf/nan
    # literal and the client's serializer refuses to emit one (same reason the
    # DiscountFactorPoint.discount_factor finite guard has no HTTP case). The
    # guard is defense-in-depth for the binary gRPC path.
    ("ec:400 curve Discount trait with a ForwardRatePoint", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(trait="Discount"),
     _ec_body_contains("builds from rate helpers but received a ForwardRatePoint")),
    ("ec:400 curve InterpolatedZero trait with a ForwardRatePoint", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(trait="InterpolatedZero"),
     _ec_body_contains("builds from zero-rate points but received a ForwardRatePoint")),
    ("ec:400 curve InterpolatedFwd rejects log interpolators", "fixed_rate_bond",
     "bonds/fixed_rate_bond_request.json", 400,
     _ec_curve_make_forward_rate(interpolator="LogLinear"),
     _ec_body_contains("InterpolatedFwd does not support log interpolators")),
    ("ec:400 curve InterpolatedZero mixed compounding across points",
     "fixed_rate_bond",
     "bonds/fixed_rate_bond_zerorate_request.json", 400,
     _ec_curve_mix_zero_point_compounding(),
     _ec_body_contains(
         "ZeroRatePoint.compounding must be identical across all points")),
    # ---- 400 INVALID_ARGUMENT: pricing.correlations is validated on every request ----
    ("ec:400 correlations not positive semi-definite", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([_ec_correlation_matrix("IMPOSSIBLE", ["A", "B", "C"], _EC_NON_PSD)]),
     _ec_body_contains_all("correlations[IMPOSSIBLE]", "not positive semi-definite",
                           "smallest eigenvalue -0.8")),
    ("ec:400 correlations asymmetric names the pair", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([_ec_correlation_matrix("QUAD", _EC_QUAD_LABELS, _EC_QUAD_ASYMMETRIC)]),
     _ec_body_contains_all("correlations[0].values: not symmetric",
                           "(SPX, SX5E) = 0.61", "(SX5E, SPX) = 0.6")),
    ("ec:400 correlations diagonal not one", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([_ec_correlation_matrix(
         "QUAD", _EC_QUAD_LABELS, _EC_QUAD_VALUES[:10] + [0.99] + _EC_QUAD_VALUES[11:])]),
     _ec_body_contains("correlations[0].values: diagonal entry (SX5E, SX5E) must be 1, got 0.99")),
    ("ec:400 correlations entry outside unit interval", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([_ec_correlation_matrix(
         "QUAD", _EC_QUAD_LABELS,
         [1.2 if i in (3, 12) else v for i, v in enumerate(_EC_QUAD_VALUES)])]),
     _ec_body_contains("correlations[0].values: entry (SMI, NKY) must be in [-1, 1], got 1.2")),
    ("ec:400 correlations label count mismatch", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([{"id": "QUAD", "labels": ["SMI", "SPX", "SX5E"],
                        "values": {"n_rows": 4, "n_cols": 4, "values": _EC_QUAD_VALUES}}]),
     _ec_body_contains("correlations[0].values: n_rows and n_cols must both equal "
                       "len(labels) = 3, got 4x4")),
    ("ec:400 correlations duplicate label", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([_ec_correlation_matrix(
         "QUAD", ["SMI", "SPX", "SMI", "NKY"], _EC_QUAD_VALUES)]),
     _ec_body_contains("correlations[0].labels: duplicate label 'SMI'")),
    ("ec:400 correlations duplicate id", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_correlations([_ec_correlation_matrix("QUAD", _EC_QUAD_LABELS, _EC_QUAD_VALUES),
                       _ec_correlation_matrix("QUAD", _EC_QUAD_LABELS, _EC_QUAD_VALUES)]),
     _ec_body_contains("duplicate correlation matrix id in pricing.correlations.matrices: QUAD")),

    # ---- price-structured-note ----
    ("ec:400 structured note empty notes list", "structured_note", _EC_SN, 400,
     _ec_sn_top("notes", []),
     _ec_body_contains("PriceStructuredNoteRequest.notes: required")),
    ("ec:400 structured note notional missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("notional"), _ec_body_contains("notes[0].notional: required")),
    ("ec:400 structured note notional non-positive", "structured_note", _EC_SN, 400,
     _ec_sn_set("notional", 0.0), _ec_body_contains("notes[0].notional: must be > 0")),
    ("ec:400 structured note initial level missing", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u.pop("initial_level")),
     _ec_body_contains("notes[0].underlyings[1].initial_level: required")),
    ("ec:400 structured note initial level non-positive", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u.__setitem__("initial_level", -1.0)),
     _ec_body_contains("notes[0].underlyings[1].initial_level: must be > 0")),
    ("ec:400 structured note duplicate underlying", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u.__setitem__("underlying_id", "SMI")),
     _ec_body_contains("notes[0].underlyings[1].underlying_id: duplicate 'SMI'")),
    ("ec:400 structured note performance rule missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("performance_rule"), _ec_body_contains("notes[0].performance_rule: required")),
    ("ec:400 structured note events not increasing", "structured_note", _EC_SN, 400,
     _ec_sn_swap_events(3, 4),
     _ec_body_contains("notes[0].events[4].observation_date: 2027-06-28 must be after events[3]"
                       ".observation_date 2027-12-27 (strictly increasing)")),
    ("ec:400 structured note final event without trigger", "structured_note", _EC_SN, 400,
     _ec_sn_event(-1, lambda e: (e.pop("autocall_trigger"), e.pop("payment_date"))),
     _ec_body_contains("notes[0].events[11]: the final event must carry an autocall_trigger or a "
                       "coupon_trigger")),
    # `is_final` (maturity-only observation) rules, on the BNP example's last
    # event (events[11]: autocall_trigger 1.0, coupon 0.07, paid 2031-07-07 =
    # maturity_payment_date).
    ("ec:400 structured note is_final with autocall_trigger", "structured_note", _EC_SN, 400,
     _ec_sn_event(-1, lambda e: (e.__setitem__("is_final", True), e.pop("coupon"))),
     _ec_body_contains("notes[0].events[11].autocall_trigger: must be absent on an is_final "
                       "event")),
    ("ec:400 structured note is_final with coupon_trigger", "structured_note", _EC_SN, 400,
     _ec_sn_event(-1, lambda e: (e.__setitem__("is_final", True), e.pop("autocall_trigger"),
                                 e.pop("coupon"), e.__setitem__("coupon_trigger", 0.8))),
     _ec_body_contains("notes[0].events[11].coupon_trigger: must be absent on an is_final "
                       "event")),
    ("ec:400 structured note is_final with coupon", "structured_note", _EC_SN, 400,
     _ec_sn_event(-1, lambda e: (e.__setitem__("is_final", True), e.pop("autocall_trigger"))),
     _ec_body_contains("notes[0].events[11].coupon: must be absent on an is_final event")),
    ("ec:400 structured note is_final without payment date", "structured_note", _EC_SN, 400,
     _ec_sn_event(-1, lambda e: (e.__setitem__("is_final", True), e.pop("autocall_trigger"),
                                 e.pop("coupon"), e.pop("payment_date"))),
     _ec_body_contains("notes[0].events[11].payment_date: required on an is_final event")),
    ("ec:400 structured note is_final payment date is not the maturity", "structured_note",
     _EC_SN, 400,
     _ec_sn_event(-1, lambda e: (e.__setitem__("is_final", True), e.pop("autocall_trigger"),
                                 e.pop("coupon"), e.__setitem__("payment_date", "2031-07-01"))),
     _ec_body_contains("notes[0].events[11].payment_date: 2031-07-01 must equal "
                       "maturity_payment_date 2031-07-07 on an is_final event")),
    ("ec:400 structured note is_final not the last event", "structured_note", _EC_SN, 400,
     _ec_sn_event(10, lambda e: (e.__setitem__("is_final", True), e.pop("autocall_trigger"),
                                 e.pop("coupon"))),
     _ec_body_contains("notes[0].events[10].is_final: must be the last event")),
    ("ec:400 structured note accrual-only event needs memory", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["coupon_rule"].__setitem__("memory", False)),
     _ec_body_contains("notes[0].events[0]: an accrual-only event (no trigger) requires "
                       "coupon_rule.memory = true")),
    ("ec:400 structured note accrual-only event with payment date", "structured_note", _EC_SN, 400,
     _ec_sn_event(0, lambda e: e.__setitem__("payment_date", "2026-01-05")),
     _ec_body_contains("notes[0].events[0].payment_date: must be absent on an accrual-only event")),
    ("ec:400 structured note accrual-only event without coupon", "structured_note", _EC_SN, 400,
     _ec_sn_event(0, lambda e: e.pop("coupon")),
     _ec_body_contains("notes[0].events[0].coupon: required on an accrual-only event")),
    ("ec:400 structured note coupon trigger without coupon", "structured_note", _EC_SN, 400,
     _ec_sn_event(5, lambda e: (e.__setitem__("coupon_trigger", 0.8), e.pop("coupon"))),
     _ec_body_contains("notes[0].events[5].coupon: required when coupon_trigger is present")),
    ("ec:400 structured note payment date missing with trigger", "structured_note", _EC_SN, 400,
     _ec_sn_event(5, lambda e: e.pop("payment_date")),
     _ec_body_contains("notes[0].events[5].payment_date: required when a trigger is present")),
    ("ec:400 structured note payment date before observation", "structured_note", _EC_SN, 400,
     _ec_sn_event(5, lambda e: e.__setitem__("payment_date", "2028-06-01")),
     _ec_body_contains("notes[0].events[5].payment_date: 2028-06-01 is before observation_date "
                       "2028-06-27")),
    ("ec:400 structured note coupon rule memory missing", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["coupon_rule"].pop("memory")),
     _ec_body_contains("notes[0].coupon_rule.memory: required")),
    ("ec:400 structured note barrier outside (0, 1]", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["capital"].__setitem__("barrier", 1.2)),
     _ec_body_contains("notes[0].capital.barrier: must be in (0, 1]")),
    ("ec:400 structured note monitoring missing", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["capital"].pop("monitoring")),
     _ec_body_contains("notes[0].capital.monitoring: required")),
    ("ec:400 structured note daily barrier needs steps_per_year", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: (n["capital"].__setitem__("monitoring", "DailyClose"),
                       n["capital"].__setitem__("knocked_in", False))),
     _ec_body_contains("notes[0].monte_carlo.steps_per_year: required, the capital barrier is "
                       "monitored DailyClose")),
    ("ec:400 structured note daily barrier with a past needs knocked_in", "structured_note",
     _EC_SN, 400,
     _ec_sn(lambda n: (n["capital"].__setitem__("monitoring", "DailyClose"),
                       n["monte_carlo"].__setitem__("steps_per_year", 252))),
     _ec_body_contains("notes[0].capital.knocked_in: required when monitoring is DailyClose and "
                       "an event is on or before the valuation date")),
    ("ec:400 structured note knocked_in on a European barrier", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["capital"].__setitem__("knocked_in", False)),
     _ec_body_contains("notes[0].capital.knocked_in: must be absent")),
    ("ec:400 structured note steps_per_year with constant vols", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["monte_carlo"].__setitem__("steps_per_year", 12)),
     _ec_body_contains("notes[0].monte_carlo.steps_per_year: must be absent, every volatility is "
                       "constant or term")),
    ("ec:400 structured note maturity before final observation", "structured_note", _EC_SN, 400,
     _ec_sn_set("maturity_payment_date", "2031-06-01"),
     _ec_body_contains("notes[0].maturity_payment_date: 2031-06-01 is before the final "
                       "observation_date 2031-06-27")),
    ("ec:400 structured note quanto on a note-currency underlying", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u.__setitem__("quanto", {
         "discounting_curve": "USD_SOFR_OIS", "fx_spot_id": "EURUSD_SPOT",
         "fx_volatility_id": "EURUSD_VOL", "fx_underlying_correlation": 0.0})),
     _ec_body_contains("notes[0].underlyings[1].quanto: must be absent, the underlying 'SPX' is "
                       "in the note currency USD")),
    ("ec:400 structured note quanto missing on a foreign underlying", "structured_note", _EC_SN,
     400, _ec_sn_underlying(0, lambda u: u.pop("quanto")),
     _ec_body_contains("notes[0].underlyings[0].quanto: required, the underlying 'SMI' is in CHF "
                       "and the note pays in USD")),
    ("ec:400 structured note quanto correlation missing", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(0, lambda u: u["quanto"].pop("fx_underlying_correlation")),
     _ec_body_contains("notes[0].underlyings[0].quanto.fx_underlying_correlation: required")),
    ("ec:400 structured note quanto correlation outside [-1, 1]", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(0, lambda u: u["quanto"].__setitem__("fx_underlying_correlation", 1.5)),
     _ec_body_contains("notes[0].underlyings[0].quanto.fx_underlying_correlation: must be in "
                       "[-1, 1]")),
    ("ec:400 structured note quanto fx spot of another pair", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(0, lambda u: u["quanto"].__setitem__("fx_spot_id", "EURUSD_SPOT")),
     _ec_body_contains("notes[0].underlyings[0].quanto.fx_spot_id 'EURUSD_SPOT' (EUR/USD) does "
                       "not quote the underlying currency CHF against the note currency USD")),
    ("ec:400 structured note correlation labels are not the basket", "structured_note", _EC_SN,
     400, _ec_sn_correlation_labels(["SMI", "SPX", "SX5E", "DAX"]),
     _ec_body_contains("notes[0].correlation_id 'QUAD_INDEX_CORR': labels [SMI, SPX, SX5E, DAX] "
                       "must be exactly the note's underlying ids [SMI, SPX, SX5E, NKY]")),
    ("ec:400 structured note past observation without a level", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(2, lambda u: u.pop("historical_levels")),
     _ec_body_contains("notes[0].underlyings[2].historical_levels: no level of 'SX5E' for the "
                       "past observation date 2026-06-29 (events[1])")),
    ("ec:400 structured note historical level after the valuation date", "structured_note",
     _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u["historical_levels"].append(
         {"date": "2026-12-28", "value": 6100.0})),
     _ec_body_contains("notes[0].underlyings[1].historical_levels[1].date: 2026-12-28 is after "
                       "the valuation date 2026-10-21")),
    ("ec:400 structured note odd paths with antithetic", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["monte_carlo"].__setitem__("paths", 1001)),
     _ec_body_contains("notes[0].monte_carlo.paths: must be even with antithetic = true")),
    ("ec:400 structured note paths missing", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["monte_carlo"].pop("paths")),
     _ec_body_contains("notes[0].monte_carlo.paths: required")),
    ("ec:400 structured note sobol seed 0", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["monte_carlo"].__setitem__("seed", 0)),
     _ec_body_contains("notes[0].monte_carlo.seed: must be > 0 with the Sobol sequence")),
    ("ec:400 structured note brownian bridge with pseudo-random", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["monte_carlo"].__setitem__("sequence", "PseudoRandom")),
     _ec_body_contains("notes[0].monte_carlo.brownian_bridge: not supported with the "
                       "PseudoRandom sequence")),
    ("ec:400 structured note upside strike missing", "structured_note", _EC_SN, 400,
     _ec_sn_set("upside", {"participation": 1.0}),
     _ec_body_contains("notes[0].upside.strike: required")),
    ("ec:400 structured note upside cap non-positive", "structured_note", _EC_SN, 400,
     _ec_sn_set("upside", {"strike": 1.0, "participation": 1.0, "cap": 0.0}),
     _ec_body_contains("notes[0].upside.cap: must be > 0")),
    ("ec:404 structured note unknown underlying", "structured_note", _EC_SN, 404,
     _ec_sn_underlying(1, lambda u: u.__setitem__("underlying_id", "NDX")),
     _ec_body_contains("Equity underlying not found: NDX")),
    ("ec:404 structured note unknown vol", "structured_note", _EC_SN, 404,
     _ec_sn_underlying(1, lambda u: u.__setitem__("volatility_id", "NDX_VOL")),
     _ec_body_contains("Black vol not found: NDX_VOL")),
    ("ec:404 structured note unknown discounting curve", "structured_note", _EC_SN, 404,
     _ec_sn_set("discounting_curve", "USD_LIBOR"),
     _ec_body_contains("Discounting curve not found: USD_LIBOR")),
    ("ec:404 structured note unknown correlation", "structured_note", _EC_SN, 404,
     _ec_sn_set("correlation_id", "NOPE")),
    # Required strings and tables of the fbs: the JSON parser rejects the
    # request before the mapper runs.
    ("ec:400 structured note trade_id missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("trade_id"), _ec_body_contains("required field is missing: trade_id")),
    ("ec:400 structured note currency missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("currency"), _ec_body_contains("required field is missing: currency")),
    ("ec:400 structured note volatility_id missing", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u.pop("volatility_id")),
     _ec_body_contains("required field is missing: volatility_id")),
    ("ec:400 structured note discounting_curve missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("discounting_curve"),
     _ec_body_contains("required field is missing: discounting_curve")),
    ("ec:400 structured note coupon_rule missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("coupon_rule"), _ec_body_contains("required field is missing: coupon_rule")),
    ("ec:400 structured note capital missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("capital"), _ec_body_contains("required field is missing: capital")),
    ("ec:400 structured note maturity_payment_date missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("maturity_payment_date"),
     _ec_body_contains("required field is missing: maturity_payment_date")),
    ("ec:400 structured note monte_carlo missing", "structured_note", _EC_SN, 400,
     _ec_sn_del("monte_carlo"), _ec_body_contains("required field is missing: monte_carlo")),
    ("ec:400 structured note empty underlyings", "structured_note", _EC_SN, 400,
     _ec_sn_set("underlyings", []),
     _ec_body_contains("notes[0].underlyings: required, at least one entry")),
    ("ec:400 structured note empty events", "structured_note", _EC_SN, 400,
     _ec_sn_set("events", []), _ec_body_contains("notes[0].events: required, at least one entry")),
    # correlation_id: required with two or more underlyings, absent with one.
    ("ec:400 structured note correlation_id missing with four underlyings", "structured_note",
     _EC_SN, 400, _ec_sn_del("correlation_id"),
     _ec_body_contains("notes[0].correlation_id: required with 4 underlyings")),
    ("ec:400 structured note correlation_id with a single underlying", "structured_note",
     _EC_SN, 400, _ec_sn(lambda n: n.__setitem__("underlyings", [n["underlyings"][1]])),
     _ec_body_contains("notes[0].correlation_id: must be absent with a single underlying")),
    ("ec:400 structured note duplicate historical date", "structured_note", _EC_SN, 400,
     _ec_sn_underlying(1, lambda u: u["historical_levels"].append(
         {"date": "2026-06-29", "value": 6200.0})),
     _ec_body_contains("notes[0].underlyings[1].historical_levels[1].date: duplicate date "
                       "2026-06-29")),
    ("ec:400 structured note negative autocall trigger", "structured_note", _EC_SN, 400,
     _ec_sn_event(5, lambda e: e.__setitem__("autocall_trigger", -1.0)),
     _ec_body_contains("notes[0].events[5].autocall_trigger: must be >= 0")),
    ("ec:400 structured note negative coupon", "structured_note", _EC_SN, 400,
     _ec_sn_event(5, lambda e: e.__setitem__("coupon", -0.07)),
     _ec_body_contains("notes[0].events[5].coupon: must be >= 0")),
    ("ec:400 structured note steps_per_year zero", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["monte_carlo"].__setitem__("steps_per_year", 0)),
     _ec_body_contains("notes[0].monte_carlo.steps_per_year: must be > 0 when present")),
    ("ec:400 structured note negative put_strike", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["capital"].__setitem__("put_strike", -1.0)),
     _ec_body_contains("notes[0].capital.put_strike: must be >= 0")),
    ("ec:400 structured note negative participation", "structured_note", _EC_SN, 400,
     _ec_sn(lambda n: n["capital"].__setitem__("participation", -1.0)),
     _ec_body_contains("notes[0].capital.participation: must be >= 0")),
    ("ec:400 structured note fx vol as a surface", "structured_note", _EC_SN, 400,
     _ec_sn_surface_vol("CHFUSD_VOL"),
     _ec_body_contains("notes[0].underlyings[0].quanto.fx_volatility_id 'CHFUSD_VOL' must be a "
                       "Constant or term (AtmMatrix2D) BlackVolSpec")),
    ("ec:400 structured note underlying spec without currency", "structured_note", _EC_SN, 400,
     _ec_sn_drop_equity_currency("SPX"),
     _ec_body_contains("notes[0].underlyings[1].underlying_id 'SPX': "
                       "pricing.equity.equity_underlyings[].currency is required")),
    # diagnostics
    ("ec:400 structured note sample_paths above the cap", "structured_note", _EC_SN, 400,
     _ec_sn_top("diagnostics", {"distribution": True, "sample_paths": 101}),
     _ec_body_contains("PriceStructuredNoteRequest.diagnostics.sample_paths: must be <= 100, "
                       "got 101")),
    ("ec:400 structured note sample_paths without distribution", "structured_note", _EC_SN, 400,
     _ec_sn_top("diagnostics", {"market": True, "sample_paths": 3}),
     _ec_body_contains("PriceStructuredNoteRequest.diagnostics.sample_paths: requires "
                       "distribution = true")),
    ("ec:400 structured note sample_paths above paths", "structured_note", _EC_SN, 400,
     _ec_sn_paths_and_samples(16, 32),
     _ec_body_contains("PriceStructuredNoteRequest.diagnostics.sample_paths: 32 exceeds "
                       "notes[0].monte_carlo.paths (16)")),
    # greeks
    ("ec:400 structured note greeks spot_bump zero", "structured_note", _EC_SN, 400,
     _ec_sn_top("greeks", {"spot_bump": 0.0}),
     _ec_body_contains("PriceStructuredNoteRequest.greeks.spot_bump: must be finite and > 0")),
    ("ec:400 structured note greeks negative vol_bump", "structured_note", _EC_SN, 400,
     _ec_sn_top("greeks", {"vol_bump": -0.01}),
     _ec_body_contains("PriceStructuredNoteRequest.greeks.vol_bump: must be finite and > 0")),
    ("ec:400 structured note greeks negative correlation_bump", "structured_note", _EC_SN, 400,
     _ec_sn_top("greeks", {"correlation_bump": -0.5}),
     _ec_body_contains("PriceStructuredNoteRequest.greeks.correlation_bump: must be finite "
                       "and > 0")),
    ("ec:400 structured note greeks funding_bump_bp zero", "structured_note", _EC_SN, 400,
     _ec_sn_top("greeks", {"funding_bump_bp": 0.0}),
     _ec_body_contains("PriceStructuredNoteRequest.greeks.funding_bump_bp: must be finite "
                       "and > 0")),
    ("ec:400 structured note greeks negative rate_bump_bp", "structured_note", _EC_SN, 400,
     _ec_sn_top("greeks", {"rate_bump_bp": -1.0}),
     _ec_body_contains("PriceStructuredNoteRequest.greeks.rate_bump_bp: must be finite "
                       "and > 0")),
    ("ec:400 structured note greeks on a called note", "structured_note", _EC_SN, 400,
     _ec_sn_greeks_on_called(),
     _ec_body_contains("notes[0]: no Greeks for a note that is not alive (status Called")),
    # ---- 400: deposit helper on a valuation date that is a holiday of its calendar ----
    # Since QuantLib 1.44 the helper's spot date is counted from the actual
    # evaluation date even when it is a holiday; with fixing_days > 0 the
    # deposit's fixing date then precedes the evaluation date and the bootstrap
    # would ask for a fixing of the helper's internal index, which no request
    # field can supply (was an opaque 422 "Missing no-fix3M ... fixing"). The
    # engine now reports it early as a 400 naming the pillar, the date, the
    # calendar and both remedies. The probe is the one the upgrade notes cite.
    ("ec:400 vanilla_swap deposit helper as_of on TARGET holiday fixing_days 2", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", 400,
     _ec_load_probe("price-vanilla-swap/vanilla_as_of_on_target_holiday.json"),
     _ec_body_contains_all("rates.curves['discount'].points[0] (DepositHelper)",
                           "2025-05-01", "TARGET", "fixing_days is 2",
                           "Value on a business day of TARGET",
                           "set fixing_days: 0")),
    # ---- 500: non-finite value in a response ----
    # FlatBuffers prints a non-finite double as a bare `inf` / `nan`, which is
    # not JSON. The gateway scans every generated response and turns the first
    # offender into a 500 naming its path. The requests are valid on the wire
    # (-1e4 continuously compounded zero rate): the discount factors overflow.
    ("ec:500 fixed_rate_bond npv non-finite (overflowing zero curve)", "fixed_rate_bond",
     "bonds/frb_usd_10y_treasury_semiannual_zero_curve.json", 500,
     _ec_overflowing_zero_curve(),
     _ec_body_is_error_json("non-finite value in response at bonds[0].npv")),
    ("ec:500 bootstrap_curves sampled DF non-finite (overflowing zero curve)", "bootstrap_curves",
     "curves/bootstrap_curves_forward.json", 500,
     _ec_bootstrap_overflowing_zero_curve(),
     _ec_body_is_error_json(
         "non-finite value in response at results[0].series[0].values[1]")),
    # ---- calibrate-correlation ----
    ("ec:400 calibrate_correlation one series", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["series"].pop()),
     _ec_body_contains("series: at least two price series are needed")),
    ("ec:400 calibrate_correlation duplicate id", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["series"][1].__setitem__("underlying_id", "NDX")),
     _ec_body_contains("series[1].underlying_id: duplicate 'NDX'")),
    ("ec:400 calibrate_correlation non-increasing dates", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["series"][0]["closes"].__setitem__(5, dict(q["series"][0]["closes"][4]))),
     _ec_body_contains("series[0].closes[5].date")),
    ("ec:400 calibrate_correlation non-positive close", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["series"][1]["closes"][7].__setitem__("value", 0.0)),
     _ec_body_contains("series[1].closes[7].value must be finite and > 0")),
    ("ec:400 calibrate_correlation overlap below min_overlap", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["estimator"].__setitem__("min_overlap", 300)),
     _ec_body_contains("is below estimator.min_overlap (300)")),
    ("ec:400 calibrate_correlation labels not a permutation", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q.__setitem__("labels", ["NDX", "SPX"])),
     _ec_body_contains("labels: 'SPX' is not a series id")),
    ("ec:400 calibrate_correlation both window forms", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["estimator"].update({"window_days": 30, "end_date": "2026-09-30"})),
     _ec_body_contains("estimator.window_days: give either window_days or start_date / end_date")),
    ("ec:400 calibrate_correlation Fixed without intensity", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["estimator"].__setitem__("shrinkage", "Fixed")),
     _ec_body_contains("estimator.shrinkage_intensity is required with shrinkage = Fixed")),
    ("ec:400 calibrate_correlation intensity out of range", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["estimator"].update({"shrinkage": "Fixed", "shrinkage_intensity": -0.1})),
     _ec_body_contains("estimator.shrinkage_intensity must be in [0, 1]")),
    ("ec:400 calibrate_correlation empty window", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q["estimator"].__setitem__("start_date", "2027-01-01")),
     _ec_body_contains("contains no date common to every series")),
    ("ec:400 calibrate_correlation correlation_id missing", "calibrate_correlation", _EC_CORR, 400,
     _ec_corr(lambda q: q.pop("correlation_id"))),

    # ---- price-structured-note: correlation_markup ----
    ("ec:400 structured note correlation_markup above 1", "structured_note", _EC_SN, 400,
     _ec_sn_set("correlation_markup", 1.5),
     _ec_body_contains("notes[0].correlation_markup: must be finite and in [-1, 1]")),
    ("ec:400 structured note correlation_markup on a single underlying", "structured_note",
     "structured_note/single_underlying_reverse_convertible.json", 400,
     _ec_sn_set("correlation_markup", 0.05),
     _ec_body_contains("notes[0].correlation_markup: must be absent with a single underlying")),

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
