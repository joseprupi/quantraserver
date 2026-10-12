#!/usr/bin/env python3
"""Rewrite 0.9.0 swap and swaption requests into the 0.10.0 `SwapLeg` shape.

0.10.0 changes the request shape of `price-vanilla-swap`, `price-ois-swap`,
`price-basis-swap`, `price-cross-currency-swap` and, through its underlying
swap, `price-swaption`: every leg becomes a flat `SwapLeg` that carries the
fields every coupon kind shares (`notional` / `notionals`, `schedule`,
`day_counter`, `payment_convention`, `payment_calendar`, `payment_lag`) next
to `pay_receive`, `currency`, the curves and the exchange flags, while
`coupons_type` / `coupons` hold only what is specific to the coupon kind
(`rate`; `index`, `spread`...). On the single-currency endpoints the wrapper
no longer carries curve ids and `swap_type` disappears; on the cross-currency
endpoint the shared fields move out of `coupons` onto the leg. See README.md
next to this file for the field-by-field mapping.

The module is importable (`convert_request`, `detect_endpoint`,
`validate_converted`) and a CLI:

    migrate_swap_requests.py INPUT [INPUT...] [--in-place | --out DIR]
                             [--check OLD_URL NEW_URL]

Standard library only. The conversion is idempotent: a request already in the
0.10.0 shape converts to itself. A request field that has no place in the new
shape is an error, never a silent drop.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

__all__ = [
    "ConversionError",
    "NotASwapRequest",
    "convert_request",
    "detect_endpoint",
    "validate_converted",
    "compare_responses",
    "main",
]

# Endpoint names returned by detect_endpoint(); the HTTP route is
# "/price-" + name.replace("_", "-").
VANILLA = "vanilla_swap"
OIS = "ois_swap"
BASIS = "basis_swap"
XCCY = "cross_currency_swap"
SWAPTION = "swaption"
SWAP_ENDPOINTS = (VANILLA, OIS, BASIS, XCCY)
ENDPOINTS = SWAP_ENDPOINTS + (SWAPTION,)

# Swap table key -> leg role names in field order.
_SWAP_ROLES = {
    VANILLA: ("fixed_leg", "floating_leg"),
    OIS: ("fixed_leg", "overnight_leg"),
    BASIS: ("leg1", "leg2"),
    XCCY: ("leg1", "leg2"),
}
# Coupon union tags allowed per leg role (swap_leg.fbs, `SwapCoupons`).
_ALLOWED_COUPONS = {
    (VANILLA, "fixed_leg"): ("FixedCoupons",),
    (VANILLA, "floating_leg"): ("IborCoupons", "CmsCoupons"),
    (OIS, "fixed_leg"): ("FixedCoupons",),
    (OIS, "overnight_leg"): ("OvernightCoupons",),
    (BASIS, "leg1"): ("IborCoupons", "OvernightCoupons"),
    (BASIS, "leg2"): ("IborCoupons", "OvernightCoupons"),
    (XCCY, "leg1"): ("FixedCoupons", "IborCoupons", "OvernightCoupons"),
    (XCCY, "leg2"): ("FixedCoupons", "IborCoupons", "OvernightCoupons"),
}
# 0.9.0 wrapper curve fields per single-currency endpoint -> receiving roles.
_WRAPPER_CURVES = {
    VANILLA: {"discounting_curve": ("fixed_leg", "floating_leg"),
              "forwarding_curve": ("floating_leg",)},
    OIS: {"discounting_curve": ("fixed_leg", "overnight_leg"),
          "forwarding_curve": ("overnight_leg",)},
    BASIS: {"discounting_curve": ("leg1", "leg2"),
            "forwarding_curve_leg1": ("leg1",),
            "forwarding_curve_leg2": ("leg2",)},
}
_SWAPTION_UNDERLYING = {"VanillaSwap": VANILLA, "OisSwap": OIS}

# The flat SwapLeg (swap_leg.fbs), in schema order. The shared fields are the
# ones every 0.9.0 coupon body carried; in 0.10.0 they live on the leg.
_SHARED_FIELDS = ("notional", "notionals", "schedule", "day_counter",
                  "payment_convention", "payment_calendar", "payment_lag")
_LEG_KEYS = ("pay_receive", "currency") + _SHARED_FIELDS + (
    "coupons_type", "coupons", "discounting_curve", "forwarding_curve",
    "initial_exchange", "final_exchange")
# Kind-specific fields per coupon table (swap_coupons.fbs), in schema order.
_KIND_FIELDS = {
    "FixedCoupons": ("rate",),
    "IborCoupons": ("index", "spread", "fixing_days", "in_arrears"),
    "OvernightCoupons": ("index", "spread", "averaging_method", "lookback_days",
                         "lockout_days", "apply_observation_shift",
                         "telescopic_value_dates"),
    "CmsCoupons": ("swap_index_id", "swap_tenor", "swaption_vol_id", "fixing_days",
                   "gear", "spread", "pricer", "cap", "floor"),
}
_SWAP_TYPES = {"Payer": ("Pay", "Receive"), "Receiver": ("Receive", "Pay")}
_CCY_RE = re.compile(r"^[A-Z]{3}$")

# --check tolerances: money fields at 1e-9 * notional, rates at 1e-12 abs,
# everything also at 1e-12 relative.
CHECK_ABS_PER_NOTIONAL = 1e-9
CHECK_ABS_RATE = 1e-12
CHECK_REL = 1e-12
CHECK_TIMEOUT = 120.0


class ConversionError(Exception):
    """A request (or one field of it) cannot be converted. `field` is the
    JSON path of the offending field, e.g. `swaps[0].vanilla_swap.swap_type`."""

    def __init__(self, field: str, message: str):
        super().__init__(f"{field}: {message}")
        self.field = field
        self.message = message


class NotASwapRequest(Exception):
    """The document is not a vanilla / OIS / basis / cross-currency swap or
    swaption request (nothing to convert; the CLI reports it as skipped)."""


# --------------------------------------------------------------------------- #
# Detection
# --------------------------------------------------------------------------- #

def detect_endpoint(request: dict) -> str:
    """Return VANILLA / OIS / BASIS / XCCY / SWAPTION for a request in the
    0.9.0 or the 0.10.0 shape. Raises NotASwapRequest for anything else (other
    products, other swap kinds, empty lists) and ConversionError when one
    `swaps` list mixes swap kinds."""
    if not isinstance(request, dict):
        raise NotASwapRequest("top level is not a JSON object")
    if isinstance(request.get("swaptions"), list) and request["swaptions"]:
        return SWAPTION
    swaps = request.get("swaps")
    if not isinstance(swaps, list) or not swaps:
        raise NotASwapRequest("no non-empty `swaps` or `swaptions` list")
    kinds = []
    for i, item in enumerate(swaps):
        if not isinstance(item, dict):
            raise ConversionError(f"swaps[{i}]", "not a JSON object")
        present = [k for k in SWAP_ENDPOINTS if k in item]
        if len(present) > 1:
            raise ConversionError(f"swaps[{i}]", f"more than one swap table: {present}")
        kinds.append(present[0] if present else None)
    if all(k is None for k in kinds):
        raise NotASwapRequest("`swaps` holds no vanilla_swap / ois_swap / basis_swap / "
                              "cross_currency_swap")
    distinct = sorted({k for k in kinds if k is not None})
    if len(distinct) > 1 or any(k is None for k in kinds):
        raise ConversionError("swaps", f"mixed swap kinds in one request: {kinds}")
    return distinct[0]


# --------------------------------------------------------------------------- #
# Conversion
# --------------------------------------------------------------------------- #

def convert_request(request: dict) -> dict:
    """Return the 0.10.0 form of a 0.9.0 (or already 0.10.0) request. The input
    is not modified. Raises ConversionError / NotASwapRequest."""
    endpoint = detect_endpoint(request)
    out = copy.deepcopy(request)
    pricing = request.get("pricing") if isinstance(request.get("pricing"), dict) else {}
    if endpoint == SWAPTION:
        out["swaptions"] = [
            _convert_swaption_item(item, pricing, f"swaptions[{i}]")
            for i, item in enumerate(request["swaptions"])
        ]
    elif endpoint == XCCY:
        out["swaps"] = [
            _convert_xccy_item(item, f"swaps[{i}]")
            for i, item in enumerate(request["swaps"])
        ]
    else:
        out["swaps"] = [
            _convert_swap_item(endpoint, item, pricing, f"swaps[{i}]")
            for i, item in enumerate(request["swaps"])
        ]
    return out


def _convert_swap_item(endpoint: str, item: dict, pricing: dict, path: str) -> dict:
    """One element of `swaps`: {<swap>, discounting_curve, forwarding_curve...}
    -> {<swap>} with the curves on the legs."""
    table = item.get(endpoint)
    if not isinstance(table, dict):
        raise ConversionError(f"{path}.{endpoint}", "missing or not an object")
    curves = {}
    for key, value in item.items():
        if key == endpoint:
            continue
        if key in _WRAPPER_CURVES[endpoint]:
            curves[key] = value
        else:
            raise ConversionError(f"{path}.{key}", "unknown field on the swap wrapper "
                                  "(not part of the 0.9.0 or the 0.10.0 shape)")
    new_table = _convert_swap_table(endpoint, table, curves, pricing,
                                    f"{path}.{endpoint}", path)
    out = {}
    for key, value in item.items():          # key order preserved; curves gone
        if key == endpoint:
            out[key] = new_table
    return out


def _convert_swaption_item(item: dict, pricing: dict, path: str) -> dict:
    swaption = item.get("swaption")
    if not isinstance(swaption, dict):
        raise ConversionError(f"{path}.swaption", "missing or not an object")
    curves = {k: item[k] for k in ("discounting_curve", "forwarding_curve") if k in item}
    kind_tag = swaption.get("underlying_type")
    if kind_tag not in _SWAPTION_UNDERLYING:
        raise ConversionError(f"{path}.swaption.underlying_type",
                              f"expected VanillaSwap or OisSwap, got {kind_tag!r}")
    endpoint = _SWAPTION_UNDERLYING[kind_tag]
    underlying = swaption.get("underlying")
    if not isinstance(underlying, dict):
        raise ConversionError(f"{path}.swaption.underlying", "missing or not an object")
    new_underlying = _convert_swap_table(endpoint, underlying, curves, pricing,
                                         f"{path}.swaption.underlying", path)
    new_swaption = {k: (new_underlying if k == "underlying" else v)
                    for k, v in swaption.items()}
    out = {}
    for key, value in item.items():
        if key == "swaption":
            out[key] = new_swaption
        elif key in curves:
            continue                          # moved onto the underlying's legs
        else:
            out[key] = value                  # volatility, model, anything swaption-specific
    return out


def _convert_xccy_item(item: dict, path: str) -> dict:
    """One element of a `price-cross-currency-swap` request. The wrapper
    (`fx_spot`, `reporting_currency`) is verbatim; each leg of the table is
    flattened (the shared fields hoisted out of `coupons`)."""
    table = item.get(XCCY)
    if not isinstance(table, dict):
        raise ConversionError(f"{path}.{XCCY}", "missing or not an object")
    tpath = f"{path}.{XCCY}"
    roles = _SWAP_ROLES[XCCY]
    for k in table:
        if k not in roles:
            raise ConversionError(f"{tpath}.{k}", "unknown field on a cross_currency_swap table")
    new_table = {}
    for role in roles:
        leg = table.get(role)
        if not isinstance(leg, dict):
            raise ConversionError(f"{tpath}.{role}", "missing or not a SwapLeg")
        new_table[role] = _flatten_leg(leg, f"{tpath}.{role}", hoist=True)
    return {k: (new_table if k == XCCY else copy.deepcopy(v)) for k, v in item.items()}


def _is_leg(value: Any) -> bool:
    return isinstance(value, dict) and ("coupons" in value or "coupons_type" in value)


def _split_body(tag: str, body: dict, path: str, old_name: str) -> tuple[dict, dict]:
    """Split a 0.9.0 coupon body into (shared SwapLeg fields, kind-specific
    `coupons` fields), both in schema order. An unknown key is an error."""
    kind = _KIND_FIELDS[tag]
    for k in body:
        if k not in _SHARED_FIELDS and k not in kind:
            raise ConversionError(f"{path}.{k}", f"unknown field on a 0.9.0 {old_name} body "
                                  f"(neither a shared SwapLeg field nor a {tag} field)")
    shared = {k: copy.deepcopy(body[k]) for k in _SHARED_FIELDS if k in body}
    specific = {k: copy.deepcopy(body[k]) for k in kind if k in body}
    return shared, specific


def _flatten_leg(leg: dict, path: str, hoist: bool) -> dict:
    """Return the flat form of a `SwapLeg` at `path`. With `hoist` (the
    cross-currency endpoint, whose 0.9.0 legs nested the shared fields under
    `coupons`) the shared fields found inside `coupons` move onto the leg;
    without it (every other endpoint: that nested form never shipped) a
    shared field inside `coupons` is an error. Unknown keys anywhere are
    errors; the result is in schema key order."""
    for k in leg:
        if k not in _LEG_KEYS:
            raise ConversionError(f"{path}.{k}", "not a SwapLeg field")
    tag = leg.get("coupons_type")
    if tag not in _KIND_FIELDS:
        raise ConversionError(f"{path}.coupons_type",
                              f"expected one of {tuple(_KIND_FIELDS)}, got {tag!r}")
    coupons = leg.get("coupons")
    if not isinstance(coupons, dict):
        raise ConversionError(f"{path}.coupons", "missing or not an object")
    kind = _KIND_FIELDS[tag]
    hoisted: dict = {}
    for k in coupons:
        if k in _SHARED_FIELDS:
            if not hoist:
                raise ConversionError(f"{path}.coupons.{k}", "shared SwapLeg field inside "
                                      "`coupons`; the 0.10.0 shape carries it on the leg")
            if k in leg:
                raise ConversionError(f"{path}.coupons.{k}", "also present on the leg")
            hoisted[k] = coupons[k]
        elif k not in kind:
            raise ConversionError(f"{path}.coupons.{k}", f"unknown field on {tag}")
    if hoisted and any(k in leg for k in _SHARED_FIELDS):
        first = next(k for k in coupons if k in _SHARED_FIELDS)
        raise ConversionError(f"{path}.coupons.{first}", "shared fields both on the leg "
                              "and inside `coupons`: the two shapes are mixed")
    merged = dict(leg)
    merged.update(hoisted)
    out = {}
    for k in _LEG_KEYS:
        if k == "coupons":
            out[k] = {f: copy.deepcopy(coupons[f]) for f in kind if f in coupons}
        elif k in merged:
            out[k] = copy.deepcopy(merged[k])
    return out


def _convert_swap_table(endpoint: str, table: dict, curves: dict, pricing: dict,
                        path: str, wrapper_path: str) -> dict:
    """`table` is the 0.9.0 (or 0.10.0) swap table at `path`; `curves` the
    curve ids found on its wrapper at `wrapper_path`."""
    roles = _SWAP_ROLES[endpoint]
    old_leg_keys = roles + (("cms_leg",) if endpoint == VANILLA else ())
    new_legs = [k for k in roles if _is_leg(table.get(k))]
    if new_legs:
        # Already 0.10.0 (idempotent path). Everything old must be gone.
        if "swap_type" in table:
            raise ConversionError(f"{path}.swap_type",
                                  "present next to SwapLeg legs; the 0.10.0 shape uses "
                                  "pay_receive on each leg")
        for k in curves:
            raise ConversionError(f"{wrapper_path}.{k}",
                                  "wrapper curve next to SwapLeg legs; curves live on the "
                                  "legs in 0.10.0")
        for k in table:
            if k not in roles:
                raise ConversionError(f"{path}.{k}", "unknown field on a 0.10.0 swap table")
        out = {}
        for role in roles:
            if not _is_leg(table.get(role)):
                raise ConversionError(f"{path}.{role}", "missing or not a SwapLeg while the "
                                      "other leg already is one")
            out[role] = _flatten_leg(table[role], f"{path}.{role}", hoist=False)
        return out

    for k in table:
        if k not in old_leg_keys and k != "swap_type":
            raise ConversionError(f"{path}.{k}", "unknown field on the 0.9.0 swap table")
    if "swap_type" not in table:
        raise ConversionError(f"{path}.swap_type", "missing (presence-required in 0.9.0)")
    swap_type = table["swap_type"]
    if swap_type not in _SWAP_TYPES:
        raise ConversionError(f"{path}.swap_type", f"expected Payer or Receiver, got {swap_type!r}")
    first_side, second_side = _SWAP_TYPES[swap_type]

    # Resolve which old leg feeds which role; CMS rides in floating_leg.
    sources = {}
    for role in roles:
        if endpoint == VANILLA and role == "floating_leg":
            has_float = "floating_leg" in table
            has_cms = "cms_leg" in table
            if has_float == has_cms:
                raise ConversionError(f"{path}.floating_leg" if not has_float else f"{path}.cms_leg",
                                      "exactly one of floating_leg / cms_leg must be present")
            src_key, tag = ("floating_leg", "IborCoupons") if has_float else ("cms_leg", "CmsCoupons")
        else:
            src_key = role
            tag = {"fixed_leg": "FixedCoupons", "overnight_leg": "OvernightCoupons",
                   "leg1": "IborCoupons", "leg2": "IborCoupons"}[role]
        body = table.get(src_key)
        if not isinstance(body, dict):
            raise ConversionError(f"{path}.{src_key}", "missing or not an object")
        sources[role] = (src_key, tag, body)

    # Currencies: floating legs from their index, the fixed leg from the other.
    currencies = {}
    for role, (src_key, tag, body) in sources.items():
        if tag != "FixedCoupons":
            currencies[role] = _leg_currency(tag, body, pricing, f"{path}.{src_key}")
    for role in roles:
        if role not in currencies:
            other = [c for r, c in currencies.items() if r != role]
            if not other:
                raise ConversionError(f"{path}.{role}", "no floating leg to take the currency from")
            currencies[role] = other[0]
    if len(set(currencies.values())) != 1:
        raise ConversionError(path, "the two legs resolve to different currencies "
                              f"{currencies}; a single-currency endpoint needs one")

    # Required curves.
    def curve(key: str, role: str) -> str:
        if key not in curves:
            raise ConversionError(f"{wrapper_path}.{key}",
                                  f"missing; 0.10.0 requires it on {role}")
        value = curves[key]
        if not isinstance(value, str) or not value:
            raise ConversionError(f"{wrapper_path}.{key}", "must be a non-empty string")
        return value

    out = {}
    for i, role in enumerate(roles):
        src_key, tag, body = sources[role]
        old_name = {"FixedCoupons": "SwapFixedLeg", "IborCoupons": "SwapFloatingLeg",
                    "OvernightCoupons": "OisFloatingLeg", "CmsCoupons": "SwapCmsLeg"}[tag]
        shared, specific = _split_body(tag, body, f"{path}.{src_key}", old_name)
        if endpoint == OIS and role == "fixed_leg":
            overnight = sources["overnight_leg"][2]
            for k in ("payment_calendar", "payment_lag"):
                if k in body:
                    raise ConversionError(f"{path}.fixed_leg.{k}",
                                          "not a 0.9.0 fixed-leg field; cannot tell it "
                                          "apart from the overnight leg's value")
                if k in overnight:
                    shared[k] = copy.deepcopy(overnight[k])
        leg = {
            "pay_receive": first_side if i == 0 else second_side,
            "currency": currencies[role],
        }
        for k in _SHARED_FIELDS:
            if k in shared:
                leg[k] = shared[k]
        leg["coupons_type"] = tag
        leg["coupons"] = specific
        disc_key = "discounting_curve"
        leg["discounting_curve"] = curve(disc_key, role)
        fwd_keys = [k for k, targets in _WRAPPER_CURVES[endpoint].items()
                    if k != disc_key and role in targets]
        if tag != "FixedCoupons":
            leg["forwarding_curve"] = curve(fwd_keys[0], role)
        out[role] = leg
    return out


def _leg_currency(tag: str, body: dict, pricing: dict, path: str) -> str:
    rates = pricing.get("rates") if isinstance(pricing.get("rates"), dict) else {}
    indices = {d.get("id"): d for d in rates.get("indices") or [] if isinstance(d, dict)}
    if tag == "CmsCoupons":
        swap_index_id = body.get("swap_index_id")
        if not isinstance(swap_index_id, str):
            raise ConversionError(f"{path}.swap_index_id", "missing")
        swap_indices = {d.get("id"): d for d in rates.get("swap_indices") or []
                        if isinstance(d, dict)}
        sdef = swap_indices.get(swap_index_id)
        if sdef is None:
            raise ConversionError(f"{path}.swap_index_id",
                                  f"{swap_index_id!r} not found in pricing.rates.swap_indices")
        index_id = sdef.get("float_index_id")
        if not isinstance(index_id, str):
            raise ConversionError(f"pricing.rates.swap_indices[{swap_index_id}].float_index_id",
                                  "missing")
        where = f"pricing.rates.swap_indices[{swap_index_id}].float_index_id"
    else:
        ref = body.get("index")
        index_id = ref.get("id") if isinstance(ref, dict) else None
        if not isinstance(index_id, str):
            raise ConversionError(f"{path}.index.id", "missing")
        where = f"{path}.index.id"
    idef = indices.get(index_id)
    if idef is None:
        raise ConversionError(where, f"index {index_id!r} not found in pricing.rates.indices")
    ccy = idef.get("currency")
    if not isinstance(ccy, str) or not _CCY_RE.match(ccy):
        raise ConversionError(f"pricing.rates.indices[{index_id}].currency",
                              "missing or not an ISO code; the leg currency is taken from it")
    return ccy


# --------------------------------------------------------------------------- #
# Structural validation of the 0.10.0 shape (mirrors swap_leg.fbs)
# --------------------------------------------------------------------------- #

def validate_converted(request: dict) -> list[str]:
    """Return the list of structural problems of a request that should be in
    the 0.10.0 shape (empty when it conforms). Checks keys, union tags, sides,
    currencies, the leg-level shared fields, the kind-specific `coupons` and
    the absence of every 0.9.0-only field; it does not validate field values
    beyond their identifying shape."""
    problems: list[str] = []
    try:
        endpoint = detect_endpoint(request)
    except (NotASwapRequest, ConversionError) as exc:
        return [str(exc)]
    if endpoint == SWAPTION:
        for i, item in enumerate(request["swaptions"]):
            p = f"swaptions[{i}]"
            for k in ("discounting_curve", "forwarding_curve"):
                if k in item:
                    problems.append(f"{p}.{k}: 0.9.0 wrapper curve still present")
            sw = item.get("swaption")
            if not isinstance(sw, dict):
                problems.append(f"{p}.swaption: missing")
                continue
            kind = _SWAPTION_UNDERLYING.get(sw.get("underlying_type"))
            if kind is None:
                problems.append(f"{p}.swaption.underlying_type: invalid")
                continue
            _validate_table(kind, sw.get("underlying"), f"{p}.swaption.underlying", problems,
                            underlying=True)
    elif endpoint == XCCY:
        for i, item in enumerate(request["swaps"]):
            p = f"swaps[{i}]"
            for k in item:
                if k not in (XCCY, "fx_spot", "reporting_currency"):
                    problems.append(f"{p}.{k}: not allowed on a price-cross-currency-swap wrapper")
            for k in ("fx_spot", "reporting_currency"):
                if not isinstance(item.get(k), str):
                    problems.append(f"{p}.{k}: missing")
            _validate_table(XCCY, item.get(XCCY), f"{p}.{XCCY}", problems)
    else:
        for i, item in enumerate(request["swaps"]):
            p = f"swaps[{i}]"
            for k in item:
                if k != endpoint:
                    problems.append(f"{p}.{k}: not allowed on a 0.10.0 wrapper")
            _validate_table(endpoint, item.get(endpoint), f"{p}.{endpoint}", problems)
    return problems


def _validate_table(endpoint: str, table: Any, path: str, problems: list[str],
                    underlying: bool = False) -> None:
    if not isinstance(table, dict):
        problems.append(f"{path}: missing or not an object")
        return
    roles = _SWAP_ROLES[endpoint]
    for k in table:
        if k not in roles:
            problems.append(f"{path}.{k}: not a 0.10.0 field (old shape?)")
    sides = []
    for role in roles:
        leg = table.get(role)
        lp = f"{path}.{role}"
        if not isinstance(leg, dict):
            problems.append(f"{lp}: missing SwapLeg")
            continue
        for k in leg:
            if k not in _LEG_KEYS:
                problems.append(f"{lp}.{k}: not a SwapLeg field")
        side = leg.get("pay_receive")
        if side not in ("Pay", "Receive"):
            problems.append(f"{lp}.pay_receive: expected Pay or Receive, got {side!r}")
        sides.append(side)
        ccy = leg.get("currency")
        if not isinstance(ccy, str) or not _CCY_RE.match(ccy):
            problems.append(f"{lp}.currency: expected an ISO code, got {ccy!r}")
        tag = leg.get("coupons_type")
        allowed = _ALLOWED_COUPONS[(endpoint, role)]
        if tag not in allowed:
            problems.append(f"{lp}.coupons_type: expected one of {allowed}, got {tag!r}")
        _validate_shared(endpoint, tag, leg, lp, problems, underlying)
        coupons = leg.get("coupons")
        if not isinstance(coupons, dict):
            problems.append(f"{lp}.coupons: missing or not an object")
        elif tag in _KIND_FIELDS:
            for k in coupons:
                if k in _SHARED_FIELDS:
                    problems.append(f"{lp}.coupons.{k}: shared field inside coupons "
                                    "(nested shape?)")
                elif k not in _KIND_FIELDS[tag]:
                    problems.append(f"{lp}.coupons.{k}: not a {tag} field")
            if tag == "FixedCoupons" and "rate" not in coupons:
                problems.append(f"{lp}.coupons.rate: missing on FixedCoupons")
            if tag in ("IborCoupons", "OvernightCoupons") and not isinstance(
                    coupons.get("index"), dict):
                problems.append(f"{lp}.coupons.index: missing on {tag}")
            if tag == "CmsCoupons" and "swap_index_id" not in coupons:
                problems.append(f"{lp}.coupons.swap_index_id: missing on CmsCoupons")
        if not isinstance(leg.get("discounting_curve"), str):
            problems.append(f"{lp}.discounting_curve: missing")
        has_fwd = "forwarding_curve" in leg
        if tag == "FixedCoupons" and has_fwd:
            problems.append(f"{lp}.forwarding_curve: present on a fixed leg")
        if tag in ("IborCoupons", "OvernightCoupons", "CmsCoupons") and not has_fwd:
            problems.append(f"{lp}.forwarding_curve: missing on {tag}")
        for k in ("initial_exchange", "final_exchange"):
            if k in leg and not isinstance(leg[k], bool):
                problems.append(f"{lp}.{k}: not a bool")
            elif k not in leg and endpoint == XCCY:
                problems.append(f"{lp}.{k}: missing (presence-required on a cross-currency leg)")
    if len(sides) == 2 and sides[0] == sides[1] and sides[0] in ("Pay", "Receive"):
        problems.append(f"{path}: both legs are {sides[0]}")
    ccys = [table[r].get("currency") for r in roles if isinstance(table.get(r), dict)]
    if endpoint == XCCY:
        if len(ccys) == 2 and ccys[0] == ccys[1]:
            problems.append(f"{path}: both legs in {ccys[0]}; a cross-currency swap needs two")
    elif len(set(ccys)) > 1:
        problems.append(f"{path}: legs in different currencies {sorted(map(str, ccys))}")


def _validate_shared(endpoint: str, tag: Any, leg: dict, lp: str, problems: list[str],
                     underlying: bool) -> None:
    """The leg-level shared fields: presence rules of swap_leg.fbs."""
    has_notionals = isinstance(leg.get("notionals"), list) and leg["notionals"]
    if "notional" not in leg and not has_notionals:
        problems.append(f"{lp}.notional: missing (and no notionals vector)")
    if has_notionals and (endpoint != VANILLA or underlying):
        problems.append(f"{lp}.notionals: only accepted on price-vanilla-swap legs")
    if not isinstance(leg.get("schedule"), dict):
        problems.append(f"{lp}.schedule: missing")
    if tag in ("FixedCoupons", "IborCoupons", "CmsCoupons") and "day_counter" not in leg:
        problems.append(f"{lp}.day_counter: missing on {tag}")
    if "payment_convention" not in leg:
        problems.append(f"{lp}.payment_convention: missing")
    for k in ("payment_calendar", "payment_lag"):
        if endpoint == OIS and k not in leg:
            problems.append(f"{lp}.{k}: missing (presence-required on both OIS legs)")
        if tag == "CmsCoupons" and k in leg:
            problems.append(f"{lp}.{k}: not accepted on a CMS leg")


# --------------------------------------------------------------------------- #
# --check: price old and new, compare
# --------------------------------------------------------------------------- #

def _post_json(base_url: str, endpoint: str, body: dict, timeout: float = CHECK_TIMEOUT) -> dict:
    url = f"{base_url.rstrip('/')}/price-{endpoint.replace('_', '-')}"
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8", "replace")[:300]
        raise RuntimeError(f"POST {url} -> HTTP {exc.code}: {text}") from None
    except urllib.error.URLError as exc:
        raise RuntimeError(f"POST {url} failed: {exc.reason}") from None


def _leg_notional(leg: dict) -> float:
    if isinstance(leg.get("notionals"), list) and leg["notionals"]:
        return max(abs(float(x)) for x in leg["notionals"])
    return abs(float(leg.get("notional") or 0.0))


def _close(a: float, b: float, abs_tol: float) -> bool:
    return abs(a - b) <= max(abs_tol, CHECK_REL * max(abs(a), abs(b)))


def compare_responses(endpoint: str, new_request: dict, old_response: dict,
                      new_response: dict) -> list[tuple[str, Any, Any, bool]]:
    """Compare a 0.9.0 and a 0.10.0 response of the same trades.
    Returns rows (field, old_value, new_value, ok). A field that is missing on
    either side is reported as not ok (except old per-leg fields the 0.9.0
    endpoint does not produce)."""
    rows: list[tuple[str, Any, Any, bool]] = []
    list_key = "swaptions" if endpoint == SWAPTION else "swaps"
    old_items = old_response.get(list_key) or []
    new_items = new_response.get(list_key) or []
    if len(old_items) != len(new_items):
        rows.append((f"{list_key}.length", len(old_items), len(new_items), False))
        return rows
    for i, (o, n) in enumerate(zip(old_items, new_items)):
        p = f"{list_key}[{i}]"
        req_item = new_request[list_key][i]
        if endpoint == SWAPTION:
            table = req_item["swaption"]["underlying"]
            kind = _SWAPTION_UNDERLYING[req_item["swaption"]["underlying_type"]]
        else:
            table = req_item[endpoint]
            kind = endpoint
        roles = _SWAP_ROLES[kind]
        notional = max(_leg_notional(table[r]) for r in roles) or 1.0
        money_tol = CHECK_ABS_PER_NOTIONAL * notional

        def row(field: str, ov: Any, nv: Any, tol: float) -> None:
            ok = isinstance(ov, (int, float)) and isinstance(nv, (int, float)) \
                and _close(float(ov), float(nv), tol)
            rows.append((f"{p}.{field}", ov, nv, ok))

        row("npv", o.get("npv"), n.get("npv"), money_tol)
        if endpoint == SWAPTION:
            continue
        legs = n.get("legs") or []
        if kind == XCCY:
            # 0.9.0 already reported legs[]; same fields on both sides.
            old_legs = o.get("legs") or []
            for j in range(len(roles)):
                ol = old_legs[j] if j < len(old_legs) and isinstance(old_legs[j], dict) else {}
                nl = legs[j] if j < len(legs) and isinstance(legs[j], dict) else {}
                row(f"legs[{j}].npv", ol.get("npv"), nl.get("npv"), money_tol)
            continue
        if kind in (VANILLA, OIS):
            row("fair_rate", o.get("fair_rate"), n.get("fair_rate"), CHECK_ABS_RATE)
            row("fair_spread", o.get("fair_spread"), n.get("fair_spread"), CHECK_ABS_RATE)
        else:
            # 0.9.0 reports fair_spread_leg1/leg2; 0.10.0 one fair_spread on
            # the leg carrying the request's spread (both zero -> leg2).
            spreads = [float((table[r].get("coupons") or {}).get("spread") or 0.0) for r in roles]
            which = "leg1" if spreads[0] != 0.0 and spreads[1] == 0.0 else "leg2"
            rows.append((f"{p}.fair_spread (old {which})",
                         o.get(f"fair_spread_{which}"), n.get("fair_spread"),
                         isinstance(o.get(f"fair_spread_{which}"), (int, float))
                         and isinstance(n.get("fair_spread"), (int, float))
                         and _close(float(o[f"fair_spread_{which}"]), float(n["fair_spread"]),
                                    CHECK_ABS_RATE)))
        for j, role in enumerate(roles):
            old_key = f"{role}_npv"
            if old_key not in o:
                continue
            new_leg = legs[j] if j < len(legs) and isinstance(legs[j], dict) else {}
            row(f"legs[{j}].npv (old {old_key})", o.get(old_key), new_leg.get("npv"), money_tol)
    return rows


def _fmt(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.12g}"
    return "-" if v is None else str(v)


def run_check(files: list[Path], old_url: str, new_url: str, out=None) -> int:
    """Convert each file, price the original on OLD and the converted on NEW,
    print a comparison table. Returns the number of files with a mismatch or
    an error."""
    out = out or sys.stdout
    failures = 0
    for f in files:
        try:
            original = json.loads(f.read_text(encoding="utf-8"))
            endpoint = detect_endpoint(original)
            converted = convert_request(original)
        except NotASwapRequest as exc:
            print(f"SKIP  {f}: {exc}", file=out)
            continue
        except (ConversionError, json.JSONDecodeError) as exc:
            print(f"ERROR {f}: {exc}", file=out)
            failures += 1
            continue
        if converted == original:
            print(f"SKIP  {f}: already in the 0.10.0 shape; the 0.9.0 server cannot price it",
                  file=out)
            continue
        try:
            old_resp = _post_json(old_url, endpoint, original)
            new_resp = _post_json(new_url, endpoint, converted)
        except RuntimeError as exc:
            print(f"ERROR {f}: {exc}", file=out)
            failures += 1
            continue
        rows = compare_responses(endpoint, converted, old_resp, new_resp)
        bad = [r for r in rows if not r[3]]
        print(f"{'FAIL ' if bad else 'OK   '} {f} ({endpoint}, {len(rows)} fields, "
              f"{len(bad)} mismatches)", file=out)
        width = max((len(r[0]) for r in rows), default=5)
        for field, ov, nv, ok in rows:
            print(f"    {'ok ' if ok else 'BAD'} {field:<{width}}  old={_fmt(ov)}  new={_fmt(nv)}",
                  file=out)
        if bad:
            failures += 1
    return failures


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def _collect(inputs: list[str]) -> list[tuple[Path, Path]]:
    """Expand files and directories (recursively, *.json) into
    (file, base_dir) pairs; base_dir keeps the relative layout under --out."""
    files: list[tuple[Path, Path]] = []
    for raw in inputs:
        p = Path(raw)
        if p.is_dir():
            files.extend((f, p) for f in sorted(p.rglob("*.json")) if f.is_file())
        elif p.is_file():
            files.append((p, p.parent))
        else:
            raise FileNotFoundError(raw)
    return files


def _dump(doc: dict) -> str:
    return json.dumps(doc, indent=2) + "\n"


def main(argv: list[str] | None = None, out=None) -> int:
    parser = argparse.ArgumentParser(
        prog="migrate_swap_requests.py",
        description="Rewrite 0.9.0 vanilla / OIS / basis / cross-currency swap and swaption "
                    "request JSON into the 0.10.0 SwapLeg shape. Without --in-place / --out "
                    "the files are only converted in memory and a summary is printed.")
    parser.add_argument("inputs", nargs="+", metavar="INPUT",
                        help="request JSON file or directory (searched recursively)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--in-place", action="store_true", help="overwrite the input files")
    mode.add_argument("--out", metavar="DIR", help="write converted files under DIR "
                      "(directory inputs keep their relative layout)")
    parser.add_argument("--check", nargs=2, metavar=("OLD_URL", "NEW_URL"),
                        help="price every original on the 0.9.0 server at OLD_URL and its "
                             "conversion on the 0.10.0 server at NEW_URL and compare")
    parser.add_argument("--quiet", action="store_true", help="only print the summary line")
    args = parser.parse_args(argv)
    out = out or sys.stdout

    try:
        files = _collect(args.inputs)
    except FileNotFoundError as exc:
        print(f"error: no such file or directory: {exc}", file=sys.stderr)
        return 2
    if not files:
        print("error: no .json files found", file=sys.stderr)
        return 2

    if args.check:
        failures = run_check([f for f, _ in files], args.check[0], args.check[1], out=out)
        print(f"check: {len(files)} files, {failures} with mismatches or errors", file=out)
        return 1 if failures else 0

    counts = {e: 0 for e in ENDPOINTS}
    skipped = errors = unchanged = 0
    for f, base in files:
        try:
            original = json.loads(f.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            print(f"ERROR {f}: not valid JSON ({exc})", file=out)
            errors += 1
            continue
        try:
            endpoint = detect_endpoint(original)
            converted = convert_request(original)
        except NotASwapRequest as exc:
            skipped += 1
            if not args.quiet:
                print(f"SKIP  {f}: {exc}", file=out)
            continue
        except ConversionError as exc:
            print(f"ERROR {f}: {exc}", file=out)
            errors += 1
            continue
        problems = validate_converted(converted)
        if problems:
            print(f"ERROR {f}: converted request fails the 0.10.0 shape check: "
                  + "; ".join(problems), file=out)
            errors += 1
            continue
        counts[endpoint] += 1
        same = converted == original
        unchanged += same
        if args.in_place:
            if not same:
                f.write_text(_dump(converted), encoding="utf-8")
            target = f
        elif args.out:
            target = Path(args.out) / f.relative_to(base)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(_dump(converted), encoding="utf-8")
        else:
            target = None
        if not args.quiet:
            status = "SAME  " if same else "OK    "
            print(f"{status}{f}: {endpoint}" + (f" -> {target}" if target else ""), file=out)
    total = sum(counts.values())
    per = ", ".join(f"{e}={counts[e]}" for e in ENDPOINTS)
    print(f"converted {total} ({per}; {unchanged} already in the 0.10.0 shape), "
          f"skipped {skipped} (not a vanilla/OIS/basis/cross-currency swap or swaption "
          f"request), errors {errors}", file=out)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
