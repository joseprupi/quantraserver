"""Unit tests for tools/migrate_swap_requests (0.9.0 -> 0.10.0 flat SwapLeg).

Pure Python: no server, no QuantLib. The corpus under examples/data is already
in the 0.10.0 shape, so every vanilla / OIS / basis / cross-currency swap and
swaption request there must be reported as already converted (converts to
itself, passes the structural 0.10.0 check). Its 0.9.0 form is rebuilt in
this test by the exact inverse of the documented mapping (`_to_old_shape`),
and converting that form must give the fixture back, field for field. Literal
0.9.0 requests of every kind (`OLD_*_0_9_0`) pin the old shapes in plain
sight and are mapped field by field. The `--check` mode is exercised against
a fake HTTP server.
"""

from __future__ import annotations

import copy
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO / "tools" / "migrate_swap_requests"
DATA_DIR = REPO / "examples" / "data"
sys.path.insert(0, str(TOOL_DIR))

import migrate_swap_requests as msr  # noqa: E402

OLD_ONLY_KEYS = {"swap_type", "cms_leg", "forwarding_curve_leg1", "forwarding_curve_leg2"}
SHARED = ("notional", "notionals", "schedule", "day_counter", "payment_convention",
          "payment_calendar", "payment_lag")
KIND_FIELDS = {
    "FixedCoupons": ("rate",),
    "IborCoupons": ("index", "spread", "fixing_days", "in_arrears"),
    "OvernightCoupons": ("index", "spread", "averaging_method", "lookback_days",
                         "lockout_days", "apply_observation_shift", "telescopic_value_dates"),
    "CmsCoupons": ("swap_index_id", "swap_tenor", "swaption_vol_id", "fixing_days",
                   "gear", "spread", "pricer", "cap", "floor"),
}
LEG_ORDER = ("pay_receive", "currency") + SHARED + (
    "coupons_type", "coupons", "discounting_curve", "forwarding_curve",
    "initial_exchange", "final_exchange")


def _load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _corpus() -> list[Path]:
    files = []
    for f in sorted(DATA_DIR.rglob("*.json")):
        try:
            msr.detect_endpoint(_load(f))
        except (msr.NotASwapRequest, msr.ConversionError, json.JSONDecodeError):
            continue
        files.append(f)
    return files


CORPUS = _corpus()


def _walk_keys(node, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            yield f"{path}.{k}" if path else k
            yield from _walk_keys(v, f"{path}.{k}" if path else k)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _walk_keys(v, f"{path}[{i}]")


def _fixture(name: str) -> dict:
    """A corpus request, in the 0.10.0 shape."""
    return _load(DATA_DIR / name)


_ROLES = {"vanilla_swap": ("fixed_leg", "floating_leg"),
          "ois_swap": ("fixed_leg", "overnight_leg"),
          "basis_swap": ("leg1", "leg2"),
          "cross_currency_swap": ("leg1", "leg2")}
_UNDERLYING = {"VanillaSwap": "vanilla_swap", "OisSwap": "ois_swap"}


def _old_body(leg: dict) -> dict:
    """The 0.9.0 coupon body of a flat leg: the shared fields merged back with
    the kind-specific `coupons` fields."""
    body = {k: copy.deepcopy(leg[k]) for k in SHARED if k in leg}
    body.update(copy.deepcopy(leg["coupons"]))
    return body


def _old_table_and_curves(endpoint: str, table: dict) -> tuple[dict, dict]:
    """Inverse of the leg mapping for one 0.10.0 single-currency swap table:
    the bare coupon bodies keyed by role (a CmsCoupons floating leg becomes
    `cms_leg`), the 0.9.0 `swap_type` (Payer = the first role is paid), and
    the wrapper curve ids (`discounting_curve`, `forwarding_curve` /
    `forwarding_curve_leg1/2`). The OIS fixed leg loses the
    `payment_calendar` / `payment_lag` the converter copied from the overnight
    leg (not 0.9.0 fixed-leg fields)."""
    roles = _ROLES[endpoint]
    first = table[roles[0]]
    old_table = {"swap_type": "Payer" if first["pay_receive"] == "Pay" else "Receiver"}
    curves = {"discounting_curve": first["discounting_curve"]}
    for role in roles:
        leg = table[role]
        body = _old_body(leg)
        if endpoint == "ois_swap" and role == "fixed_leg":
            body.pop("payment_calendar", None)
            body.pop("payment_lag", None)
        old_table["cms_leg" if leg["coupons_type"] == "CmsCoupons" else role] = body
        if "forwarding_curve" in leg:
            key = f"forwarding_curve_{role}" if endpoint == "basis_swap" else "forwarding_curve"
            curves[key] = leg["forwarding_curve"]
    return old_table, curves


def _nest_xccy_leg(leg: dict) -> dict:
    """Inverse of the cross-currency flattening: the 0.9.0 nested SwapLeg,
    with the whole body under `coupons` (0.9.0 key order)."""
    out = {k: copy.deepcopy(leg[k]) for k in ("pay_receive", "currency", "coupons_type")}
    out["coupons"] = _old_body(leg)
    for k in ("initial_exchange", "final_exchange", "discounting_curve", "forwarding_curve"):
        if k in leg:
            out[k] = copy.deepcopy(leg[k])
    return out


def _to_old_shape(new: dict) -> dict:
    """The 0.9.0 form of a 0.10.0 request (inverse of convert_request, per the
    tool README's field-by-field tables). A request that is not one of the
    five swap kinds is returned as a copy."""
    old = copy.deepcopy(new)
    try:
        endpoint = msr.detect_endpoint(new)
    except msr.NotASwapRequest:
        return old
    if endpoint == "swaption":
        for item in old["swaptions"]:
            sw = item["swaption"]
            table, curves = _old_table_and_curves(_UNDERLYING[sw["underlying_type"]],
                                                  sw["underlying"])
            sw["underlying"] = table
            rebuilt = {"swaption": sw, **curves}
            rebuilt.update({k: v for k, v in item.items() if k != "swaption"})
            item.clear()
            item.update(rebuilt)
        return old
    if endpoint == "cross_currency_swap":
        for item in old["swaps"]:
            table = item["cross_currency_swap"]
            for role in _ROLES[endpoint]:
                table[role] = _nest_xccy_leg(table[role])
        return old
    for item in old["swaps"]:
        table, curves = _old_table_and_curves(endpoint, item[endpoint])
        item.clear()
        item[endpoint] = table
        item.update(curves)
    return old


def _old_fixture(name: str) -> dict:
    """A corpus request rebuilt in the 0.9.0 shape (what the converter reads)."""
    return _to_old_shape(_fixture(name))


# --------------------------------------------------------------------------- #
# Literal 0.9.0 requests, one per kind
# --------------------------------------------------------------------------- #

_SCHEDULE = {"calendar": "TARGET", "effective_date": "2025-01-17",
             "termination_date": "2030-01-17", "frequency": "Annual",
             "convention": "ModifiedFollowing",
             "termination_date_convention": "ModifiedFollowing",
             "date_generation_rule": "Forward", "end_of_month": False}
_SEMI = dict(_SCHEDULE, frequency="Semiannual")
_QUART = dict(_SCHEDULE, frequency="Quarterly")
_PRICING = {"rates": {"indices": [{"id": "EUR_6M", "currency": "EUR"},
                                  {"id": "EUR_3M", "currency": "EUR"},
                                  {"id": "ESTR", "currency": "EUR"},
                                  {"id": "USD_SOFR", "currency": "USD"}],
                      "curves": []}}

# A literal 0.9.0 price-vanilla-swap request: swap_type on the swap, bare
# coupon bodies as legs (shared and kind-specific fields side by side), curve
# ids on the wrapper. The pricing block is the minimum the converter reads
# (the index currency).
OLD_VANILLA_0_9_0 = {
    "pricing": _PRICING,
    "swaps": [{
        "vanilla_swap": {
            "swap_type": "Payer",
            "fixed_leg": {"notional": 1e6, "schedule": _SCHEDULE, "rate": 0.035,
                          "day_counter": "Thirty360",
                          "payment_convention": "ModifiedFollowing"},
            "floating_leg": {"notional": 1e6, "schedule": _SEMI,
                             "index": {"id": "EUR_6M"}, "spread": 0.0005,
                             "day_counter": "Actual360",
                             "payment_convention": "ModifiedFollowing",
                             "fixing_days": 2, "in_arrears": True},
        },
        "discounting_curve": "discount",
        "forwarding_curve": "fwd",
    }],
    "include_flows": True,
}

OLD_OIS_0_9_0 = {
    "pricing": _PRICING,
    "swaps": [{
        "ois_swap": {
            "swap_type": "Receiver",
            "fixed_leg": {"notional": 2e6, "schedule": _SCHEDULE, "rate": 0.03,
                          "day_counter": "Actual360",
                          "payment_convention": "ModifiedFollowing"},
            # 0.9.0 OisFloatingLeg key order (payment_lag after the flags).
            "overnight_leg": {"notional": 2e6, "schedule": _SCHEDULE,
                              "index": {"id": "ESTR"}, "spread": 0.001,
                              "day_counter": "Actual360",
                              "payment_convention": "Following",
                              "payment_calendar": "TARGET",
                              "averaging_method": "Compound", "lockout_days": 0,
                              "apply_observation_shift": False,
                              "telescopic_value_dates": False,
                              "payment_lag": 2, "lookback_days": 0},
        },
        "discounting_curve": "ois",
        "forwarding_curve": "ois",
    }],
}

OLD_BASIS_0_9_0 = {
    "pricing": _PRICING,
    "swaps": [{
        "basis_swap": {
            "swap_type": "Payer",
            "leg1": {"notional": 1e6, "schedule": _QUART, "index": {"id": "EUR_3M"},
                     "spread": 0.0005, "day_counter": "Actual360",
                     "payment_convention": "ModifiedFollowing"},
            "leg2": {"notional": 1e6, "schedule": _SEMI, "index": {"id": "EUR_6M"},
                     "spread": 0.0, "day_counter": "Actual360",
                     "payment_convention": "ModifiedFollowing"},
        },
        "discounting_curve": "ois",
        "forwarding_curve_leg1": "fwd3m",
        "forwarding_curve_leg2": "fwd6m",
    }],
}

OLD_CMS_0_9_0 = {
    "pricing": {"rates": {"indices": [{"id": "EUR_6M", "currency": "EUR"}],
                          "swap_indices": [{"id": "EUR_SWAP_6M", "float_index_id": "EUR_6M"}],
                          "curves": []}},
    "swaps": [{
        "vanilla_swap": {
            "swap_type": "Payer",
            "fixed_leg": {"notional": 1e6, "schedule": _SCHEDULE, "rate": 0.03,
                          "day_counter": "Thirty360",
                          "payment_convention": "ModifiedFollowing"},
            "cms_leg": {"notional": 1e6, "schedule": _SCHEDULE,
                        "swap_index_id": "EUR_SWAP_6M", "swap_tenor": {"n": 10, "unit": "Years"},
                        "swaption_vol_id": "vol", "day_counter": "Actual360",
                        "payment_convention": "ModifiedFollowing",
                        "pricer": {"pricer_type": "LinearTsr", "mean_reversion": 0.03},
                        "cap": 0.05, "gear": 1.0, "spread": 0.001},
        },
        "discounting_curve": "ois",
        "forwarding_curve": "fwd",
    }],
}

OLD_SWAPTION_0_9_0 = {
    "pricing": _PRICING,
    "swaptions": [{
        "swaption": {
            "exercise_type": "European", "settlement_type": "Physical",
            "exercise_date": "2026-01-15",
            "underlying_type": "VanillaSwap",
            "underlying": OLD_VANILLA_0_9_0["swaps"][0]["vanilla_swap"],
        },
        "discounting_curve": "ois",
        "forwarding_curve": "fwd",
        "volatility": "vol", "model": "black",
    }],
}

OLD_SWAPTION_OIS_0_9_0 = {
    "pricing": _PRICING,
    "swaptions": [{
        "swaption": {
            "exercise_type": "European", "settlement_type": "Cash",
            "settlement_method": "ParYieldCurve", "exercise_date": "2026-01-15",
            "underlying_type": "OisSwap",
            "underlying": OLD_OIS_0_9_0["swaps"][0]["ois_swap"],
        },
        "discounting_curve": "ois",
        "forwarding_curve": "ois",
        "volatility": "vol", "model": "bachelier",
    }],
}

# A literal 0.9.0 price-cross-currency-swap request: SwapLegs already (side,
# currency, curves, exchange flags on the leg) but the whole coupon body
# nested under `coupons`.
OLD_XCCY_0_9_0 = {
    "pricing": _PRICING,
    "swaps": [{
        "cross_currency_swap": {
            "leg1": {
                "pay_receive": "Pay", "currency": "USD",
                "coupons_type": "OvernightCoupons",
                "coupons": {"notional": 1.085e7, "schedule": _QUART,
                            "index": {"id": "USD_SOFR"}, "spread": 0.0,
                            "payment_convention": "ModifiedFollowing",
                            "payment_calendar": "TARGET", "payment_lag": 0,
                            "averaging_method": "Compound", "lookback_days": 0,
                            "lockout_days": 0, "apply_observation_shift": False,
                            "telescopic_value_dates": False},
                "initial_exchange": True, "final_exchange": True,
                "discounting_curve": "USD_SOFR_OIS", "forwarding_curve": "USD_SOFR_OIS",
            },
            "leg2": {
                "pay_receive": "Receive", "currency": "EUR",
                "coupons_type": "FixedCoupons",
                "coupons": {"notional": 1e7, "schedule": _SCHEDULE, "rate": 0.025,
                            "day_counter": "Thirty360",
                            "payment_convention": "ModifiedFollowing"},
                "initial_exchange": True, "final_exchange": True,
                "discounting_curve": "EUR_USD_COLLATERAL",
            },
        },
        "fx_spot": "EURUSD_SPOT",
        "reporting_currency": "USD",
    }],
    "include_flows": False,
}


def _flat(side, ccy, body, tag, disc, fwd=None, extra=None):
    """Expected flat leg of a 0.9.0 body (schema key order)."""
    leg = {"pay_receive": side, "currency": ccy}
    leg.update({k: body[k] for k in SHARED if k in body})
    if extra:
        leg.update(extra)
    leg["coupons_type"] = tag
    leg["coupons"] = {k: body[k] for k in KIND_FIELDS[tag] if k in body}
    leg["discounting_curve"] = disc
    if fwd is not None:
        leg["forwarding_curve"] = fwd
    return leg


# --------------------------------------------------------------------------- #
# Corpus
# --------------------------------------------------------------------------- #

def test_corpus_is_what_we_expect():
    counts = {}
    for f in CORPUS:
        endpoint = msr.detect_endpoint(_load(f))
        counts[endpoint] = counts.get(endpoint, 0) + 1
    # 71 files when the corpus was converted: 35 vanilla, 6 OIS, 3 basis,
    # 6 cross-currency, 21 swaption. New fixtures may be added, none of these
    # may disappear.
    assert counts["vanilla_swap"] >= 35 and counts["ois_swap"] >= 6
    assert counts["basis_swap"] >= 3 and counts["cross_currency_swap"] >= 6
    assert counts["swaption"] >= 21
    assert len(CORPUS) >= 71


_IDS = [str(p.relative_to(DATA_DIR)) for p in CORPUS]


@pytest.mark.parametrize("path", CORPUS, ids=_IDS)
def test_corpus_fixture_is_already_in_the_0_10_0_shape(path):
    """Every corpus fixture is reported SAME: it converts to itself, passes
    the structural check, carries no 0.9.0 leftovers and every leg is flat
    (shared fields on the leg, kind-specific ones only under `coupons`)."""
    fixture = _load(path)
    snapshot = copy.deepcopy(fixture)
    assert msr.convert_request(fixture) == fixture, "not idempotent"
    assert fixture == snapshot, "convert_request must not mutate its input"
    assert msr.validate_converted(fixture) == []
    for k in _walk_keys(fixture):
        assert k.rsplit(".", 1)[-1] not in OLD_ONLY_KEYS, k
    endpoint = msr.detect_endpoint(fixture)
    list_key = "swaptions" if endpoint == "swaption" else "swaps"
    for i, item in enumerate(fixture[list_key]):
        if endpoint != "cross_currency_swap":
            assert "discounting_curve" not in item and "forwarding_curve" not in item, i
        table = item["swaption"]["underlying"] if endpoint == "swaption" else item[endpoint]
        kind = _UNDERLYING[item["swaption"]["underlying_type"]] if endpoint == "swaption" \
            else endpoint
        for role in _ROLES[kind]:
            leg = table[role]
            assert "schedule" in leg and ("notional" in leg or "notionals" in leg), role
            assert set(leg["coupons"]) <= set(KIND_FIELDS[leg["coupons_type"]]), role
            assert not set(leg["coupons"]) & set(SHARED), role
            keys = list(leg.keys())
            assert keys == [k for k in LEG_ORDER if k in keys], "schema key order"


@pytest.mark.parametrize("path", CORPUS, ids=_IDS)
def test_corpus_fixture_is_the_conversion_of_its_0_9_0_form(path):
    """Rebuilding the 0.9.0 form of a fixture and converting it gives the
    fixture back: the mapping is what the README documents, in both
    directions, and nothing is dropped. Non-swap content stays verbatim and
    the top-level key order is kept."""
    fixture = _load(path)
    old = _to_old_shape(fixture)
    assert old != fixture
    endpoint = msr.detect_endpoint(fixture)
    if endpoint == "cross_currency_swap":
        for item in old["swaps"]:
            for role in ("leg1", "leg2"):
                assert "notional" in item["cross_currency_swap"][role]["coupons"]
    else:
        for k in ("swap_type", "discounting_curve"):
            assert any(key.rsplit(".", 1)[-1] == k for key in _walk_keys(old)), k
    snapshot = copy.deepcopy(old)
    converted = msr.convert_request(old)
    assert old == snapshot, "convert_request must not mutate its input"
    assert converted == fixture
    assert converted.get("pricing") == old.get("pricing")
    for k in ("include_flows", "include_diagnostics"):
        assert converted.get(k) == old.get(k)
    assert list(converted.keys()) == list(old.keys())


# --------------------------------------------------------------------------- #
# Literal 0.9.0 requests map field by field
# --------------------------------------------------------------------------- #

def test_inline_vanilla_maps_field_by_field():
    old = OLD_VANILLA_0_9_0
    swap = old["swaps"][0]["vanilla_swap"]
    out = msr.convert_request(old)
    assert out == {
        "pricing": old["pricing"],
        "swaps": [{"vanilla_swap": {
            "fixed_leg": _flat("Pay", "EUR", swap["fixed_leg"], "FixedCoupons", "discount"),
            "floating_leg": _flat("Receive", "EUR", swap["floating_leg"], "IborCoupons",
                                  "discount", "fwd"),
        }}],
        "include_flows": True,
    }
    fixed = out["swaps"][0]["vanilla_swap"]["fixed_leg"]
    floating = out["swaps"][0]["vanilla_swap"]["floating_leg"]
    assert list(fixed.keys()) == ["pay_receive", "currency", "notional", "schedule",
                                  "day_counter", "payment_convention", "coupons_type",
                                  "coupons", "discounting_curve"]
    assert fixed["coupons"] == {"rate": 0.035}
    assert floating["coupons"] == {"index": {"id": "EUR_6M"}, "spread": 0.0005,
                                   "fixing_days": 2, "in_arrears": True}
    assert list(floating["coupons"].keys()) == ["index", "spread", "fixing_days", "in_arrears"]
    assert "forwarding_curve" not in fixed
    assert msr.validate_converted(out) == []
    assert msr.validate_converted(old), "the 0.9.0 shape fails the structural check"
    assert _to_old_shape(out) == old
    assert msr.convert_request(out) == out


def test_inline_ois_maps_field_by_field():
    old = OLD_OIS_0_9_0
    swap = old["swaps"][0]["ois_swap"]
    out = msr.convert_request(old)
    on = swap["overnight_leg"]
    assert out == {
        "pricing": old["pricing"],
        "swaps": [{"ois_swap": {
            "fixed_leg": _flat("Receive", "EUR", swap["fixed_leg"], "FixedCoupons", "ois",
                               extra={"payment_calendar": "TARGET", "payment_lag": 2}),
            "overnight_leg": _flat("Pay", "EUR", on, "OvernightCoupons", "ois", "ois"),
        }}],
    }
    legs = out["swaps"][0]["ois_swap"]
    assert list(legs["fixed_leg"].keys()) == [
        "pay_receive", "currency", "notional", "schedule", "day_counter",
        "payment_convention", "payment_calendar", "payment_lag", "coupons_type", "coupons",
        "discounting_curve"]
    assert list(legs["overnight_leg"].keys()) == [
        "pay_receive", "currency", "notional", "schedule", "day_counter",
        "payment_convention", "payment_calendar", "payment_lag", "coupons_type", "coupons",
        "discounting_curve", "forwarding_curve"]
    assert legs["overnight_leg"]["day_counter"] == "Actual360"     # accepted-but-unused, kept
    assert legs["overnight_leg"]["payment_lag"] == 2 and legs["fixed_leg"]["payment_lag"] == 2
    assert legs["fixed_leg"]["payment_convention"] == "ModifiedFollowing"   # its own, not copied
    assert list(legs["overnight_leg"]["coupons"].keys()) == list(KIND_FIELDS["OvernightCoupons"])
    assert msr.validate_converted(out) == []
    assert _to_old_shape(out) == old


def test_inline_basis_maps_field_by_field():
    old = OLD_BASIS_0_9_0
    swap = old["swaps"][0]["basis_swap"]
    out = msr.convert_request(old)
    assert out == {
        "pricing": old["pricing"],
        "swaps": [{"basis_swap": {
            "leg1": _flat("Pay", "EUR", swap["leg1"], "IborCoupons", "ois", "fwd3m"),
            "leg2": _flat("Receive", "EUR", swap["leg2"], "IborCoupons", "ois", "fwd6m"),
        }}],
    }
    assert out["swaps"][0]["basis_swap"]["leg1"]["coupons"] == {"index": {"id": "EUR_3M"},
                                                                 "spread": 0.0005}
    assert msr.validate_converted(out) == []
    assert _to_old_shape(out) == old


def test_inline_cms_leg_maps_to_floating_leg_with_cms_coupons():
    old = OLD_CMS_0_9_0
    swap = old["swaps"][0]["vanilla_swap"]
    out = msr.convert_request(old)
    legs = out["swaps"][0]["vanilla_swap"]
    assert list(legs.keys()) == ["fixed_leg", "floating_leg"]
    assert legs["floating_leg"] == _flat("Receive", "EUR", swap["cms_leg"], "CmsCoupons",
                                         "ois", "fwd")
    assert legs["floating_leg"]["coupons"] == {
        "swap_index_id": "EUR_SWAP_6M", "swap_tenor": {"n": 10, "unit": "Years"},
        "swaption_vol_id": "vol", "gear": 1.0, "spread": 0.001,
        "pricer": {"pricer_type": "LinearTsr", "mean_reversion": 0.03}, "cap": 0.05}
    assert "floor" not in legs["floating_leg"]["coupons"]       # nothing invented
    assert legs["floating_leg"]["day_counter"] == "Actual360"
    assert msr.validate_converted(out) == []
    assert _to_old_shape(out) == old


def test_inline_swaption_vanilla_underlying():
    old = OLD_SWAPTION_0_9_0
    item = old["swaptions"][0]
    out = msr.convert_request(old)["swaptions"][0]
    assert list(out.keys()) == ["swaption", "volatility", "model"]
    assert out["volatility"] == "vol" and out["model"] == "black"
    assert list(out["swaption"].keys()) == list(item["swaption"].keys())
    for k in item["swaption"]:
        if k != "underlying":
            assert out["swaption"][k] == item["swaption"][k]
    swap = item["swaption"]["underlying"]
    assert out["swaption"]["underlying"] == {
        "fixed_leg": _flat("Pay", "EUR", swap["fixed_leg"], "FixedCoupons", "ois"),
        "floating_leg": _flat("Receive", "EUR", swap["floating_leg"], "IborCoupons",
                              "ois", "fwd"),
    }
    assert msr.validate_converted(msr.convert_request(old)) == []
    assert _to_old_shape(msr.convert_request(old)) == old


def test_inline_swaption_ois_underlying():
    old = OLD_SWAPTION_OIS_0_9_0
    out = msr.convert_request(old)
    legs = out["swaptions"][0]["swaption"]["underlying"]
    swap = old["swaptions"][0]["swaption"]["underlying"]
    assert out["swaptions"][0]["swaption"]["underlying_type"] == "OisSwap"
    assert legs["fixed_leg"] == _flat("Receive", "EUR", swap["fixed_leg"], "FixedCoupons",
                                      "ois", extra={"payment_calendar": "TARGET",
                                                    "payment_lag": 2})
    assert legs["overnight_leg"] == _flat("Pay", "EUR", swap["overnight_leg"],
                                          "OvernightCoupons", "ois", "ois")
    assert "discounting_curve" not in out["swaptions"][0]
    assert msr.validate_converted(out) == []
    assert _to_old_shape(out) == old


def test_inline_xccy_hoists_shared_fields_onto_the_legs():
    old = OLD_XCCY_0_9_0
    out = msr.convert_request(old)
    item = out["swaps"][0]
    assert list(item.keys()) == ["cross_currency_swap", "fx_spot", "reporting_currency"]
    assert item["fx_spot"] == "EURUSD_SPOT" and item["reporting_currency"] == "USD"
    assert out["include_flows"] is False and out["pricing"] == old["pricing"]
    legs = item["cross_currency_swap"]
    l1 = old["swaps"][0]["cross_currency_swap"]["leg1"]["coupons"]
    l2 = old["swaps"][0]["cross_currency_swap"]["leg2"]["coupons"]
    assert legs["leg1"] == {
        "pay_receive": "Pay", "currency": "USD", "notional": 1.085e7, "schedule": _QUART,
        "payment_convention": "ModifiedFollowing", "payment_calendar": "TARGET",
        "payment_lag": 0, "coupons_type": "OvernightCoupons",
        "coupons": {k: l1[k] for k in KIND_FIELDS["OvernightCoupons"]},
        "discounting_curve": "USD_SOFR_OIS", "forwarding_curve": "USD_SOFR_OIS",
        "initial_exchange": True, "final_exchange": True}
    assert list(legs["leg1"].keys()) == [k for k in LEG_ORDER if k in legs["leg1"]]
    assert legs["leg2"] == {
        "pay_receive": "Receive", "currency": "EUR", "notional": 1e7, "schedule": _SCHEDULE,
        "day_counter": "Thirty360", "payment_convention": "ModifiedFollowing",
        "coupons_type": "FixedCoupons", "coupons": {"rate": 0.025},
        "discounting_curve": "EUR_USD_COLLATERAL",
        "initial_exchange": True, "final_exchange": True}
    assert "day_counter" not in legs["leg1"]           # the body had none: nothing invented
    assert "forwarding_curve" not in legs["leg2"]
    assert msr.validate_converted(out) == []
    assert msr.validate_converted(old), "the nested 0.9.0 shape fails the structural check"
    assert msr.convert_request(out) == out
    assert _to_old_shape(out) == old
    assert l2 == _old_body(legs["leg2"])


def test_detect_endpoint_old_and_new_shapes():
    for name, expected in (("vanilla_swap_request.json", "vanilla_swap"),
                           ("ois_swap_request.json", "ois_swap"),
                           ("basis_swap_request.json", "basis_swap"),
                           ("swaption_request.json", "swaption"),
                           ("cross_currency_swap/xccy_eur_fixed_vs_usd_sofr_5y_payer_eur.json",
                            "cross_currency_swap")):
        assert msr.detect_endpoint(_old_fixture(name)) == expected
        assert msr.detect_endpoint(_fixture(name)) == expected
    for old, expected in ((OLD_VANILLA_0_9_0, "vanilla_swap"), (OLD_OIS_0_9_0, "ois_swap"),
                          (OLD_BASIS_0_9_0, "basis_swap"), (OLD_XCCY_0_9_0, "cross_currency_swap"),
                          (OLD_SWAPTION_0_9_0, "swaption")):
        assert msr.detect_endpoint(old) == expected


# --------------------------------------------------------------------------- #
# Mapping rules on the corpus
# --------------------------------------------------------------------------- #

def _legs(converted: dict, table: str) -> dict:
    return converted["swaps"][0][table]


def test_payer_and_receiver_sides():
    req = _old_fixture("ir_swaps/irs_eur_5y_payer_fixed_30360_vs_euribor6m.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert list(legs.keys()) == ["fixed_leg", "floating_leg"]
    assert legs["fixed_leg"]["pay_receive"] == "Pay"
    assert legs["floating_leg"]["pay_receive"] == "Receive"
    req = _old_fixture("ir_swaps/irs_eur_5y_receiver_fixed_30360_vs_euribor6m.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["fixed_leg"]["pay_receive"] == "Receive"
    assert legs["floating_leg"]["pay_receive"] == "Pay"
    req = _old_fixture("ir_swaps/ois_eur_2y_receiver_estr_spread10bp_quarterly.json")
    legs = _legs(msr.convert_request(req), "ois_swap")
    assert legs["fixed_leg"]["pay_receive"] == "Receive"
    assert legs["overnight_leg"]["pay_receive"] == "Pay"


def test_vanilla_curves_currency_and_split_bodies():
    req = _old_fixture("ir_swaps/irs_eur_5y_payer_ois_discounted_multicurve.json")
    item = req["swaps"][0]
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["fixed_leg"]["discounting_curve"] == item["discounting_curve"]
    assert legs["floating_leg"]["discounting_curve"] == item["discounting_curve"]
    assert legs["floating_leg"]["forwarding_curve"] == item["forwarding_curve"]
    assert "forwarding_curve" not in legs["fixed_leg"]
    for role in ("fixed_leg", "floating_leg"):
        assert _old_body(legs[role]) == item["vanilla_swap"][role]
    assert legs["fixed_leg"]["coupons_type"] == "FixedCoupons"
    assert legs["floating_leg"]["coupons_type"] == "IborCoupons"
    index_id = item["vanilla_swap"]["floating_leg"]["index"]["id"]
    ccy = next(i["currency"] for i in req["pricing"]["rates"]["indices"] if i["id"] == index_id)
    assert legs["fixed_leg"]["currency"] == ccy == legs["floating_leg"]["currency"]


def test_amortizing_and_in_arrears_fields_land_where_they_belong():
    req = _old_fixture("ir_swaps/irs_eur_5y_amortizing_payer.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["fixed_leg"]["notionals"] == req["swaps"][0]["vanilla_swap"]["fixed_leg"]["notionals"]
    assert "notionals" not in legs["fixed_leg"]["coupons"]
    req = _old_fixture("ir_swaps/irs_eur_5y_payer_euribor6m_in_arrears.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert legs["floating_leg"]["coupons"]["in_arrears"] is True
    assert "in_arrears" not in legs["floating_leg"]


def test_ois_fixed_leg_copies_payment_lag_and_calendar_to_leg_level():
    req = _old_fixture("ir_swaps/ois_eur_5y_payer_estr_payment_lag2.json")
    legs = _legs(msr.convert_request(req), "ois_swap")
    assert list(legs.keys()) == ["fixed_leg", "overnight_leg"]
    fixed = legs["fixed_leg"]
    assert fixed["payment_lag"] == 2 and fixed["payment_calendar"] == "TARGET"
    assert fixed["coupons"] == {"rate": req["swaps"][0]["ois_swap"]["fixed_leg"]["rate"]}
    old_fixed = req["swaps"][0]["ois_swap"]["fixed_leg"]
    assert {k: v for k, v in _old_body(fixed).items()
            if k not in ("payment_lag", "payment_calendar")} == old_fixed
    on = legs["overnight_leg"]
    assert on["coupons_type"] == "OvernightCoupons"
    assert on["payment_lag"] == 2 and on["payment_calendar"] == "TARGET"
    assert _old_body(on) == req["swaps"][0]["ois_swap"]["overnight_leg"]
    assert on["forwarding_curve"] == req["swaps"][0]["forwarding_curve"]
    assert "forwarding_curve" not in fixed
    assert fixed["currency"] == on["currency"] == "EUR"


def test_cms_leg_becomes_floating_leg_with_cms_coupons():
    req = _old_fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    legs = _legs(msr.convert_request(req), "vanilla_swap")
    assert list(legs.keys()) == ["fixed_leg", "floating_leg"]
    assert legs["floating_leg"]["coupons_type"] == "CmsCoupons"
    assert _old_body(legs["floating_leg"]) == req["swaps"][0]["vanilla_swap"]["cms_leg"]
    assert "pricer" in legs["floating_leg"]["coupons"]
    assert legs["floating_leg"]["forwarding_curve"] == req["swaps"][0]["forwarding_curve"]
    assert legs["floating_leg"]["currency"] == "EUR"   # via EUR_SWAP_6M -> EUR_6M
    assert legs["floating_leg"]["pay_receive"] == "Receive"


def test_basis_forwarding_curves_per_leg():
    req = _old_fixture("ir_swaps/basis_eur_5y_3m_vs_6m_ois_discounted_multicurve.json")
    item = req["swaps"][0]
    legs = _legs(msr.convert_request(req), "basis_swap")
    assert list(legs.keys()) == ["leg1", "leg2"]
    assert legs["leg1"]["forwarding_curve"] == item["forwarding_curve_leg1"]
    assert legs["leg2"]["forwarding_curve"] == item["forwarding_curve_leg2"]
    assert legs["leg1"]["discounting_curve"] == legs["leg2"]["discounting_curve"] \
        == item["discounting_curve"]
    assert legs["leg1"]["coupons_type"] == legs["leg2"]["coupons_type"] == "IborCoupons"
    assert legs["leg1"]["pay_receive"] == "Pay" and legs["leg2"]["pay_receive"] == "Receive"
    assert set(msr.convert_request(req)["swaps"][0].keys()) == {"basis_swap"}


def test_swaption_underlying_takes_the_wrapper_curves():
    req = _old_fixture("swaption_request.json")
    item = req["swaptions"][0]
    out = msr.convert_request(req)["swaptions"][0]
    assert list(out.keys()) == ["swaption", "volatility", "model"]
    assert out["volatility"] == item["volatility"] and out["model"] == item["model"]
    legs = out["swaption"]["underlying"]
    assert legs["fixed_leg"]["pay_receive"] == "Pay"
    assert legs["fixed_leg"]["discounting_curve"] == item["discounting_curve"]
    assert legs["floating_leg"]["forwarding_curve"] == item["forwarding_curve"]
    assert legs["floating_leg"]["coupons_type"] == "IborCoupons"
    req = _old_fixture("swaption_ois_request.json")
    item = req["swaptions"][0]
    legs = msr.convert_request(req)["swaptions"][0]["swaption"]["underlying"]
    assert legs["overnight_leg"]["coupons_type"] == "OvernightCoupons"
    assert legs["fixed_leg"]["payment_lag"] == \
        item["swaption"]["underlying"]["overnight_leg"]["payment_lag"]
    assert legs["fixed_leg"]["currency"] == "USD"
    req = _old_fixture("swaption_bermudan_hw_calibrated.json")
    out = msr.convert_request(req)["swaptions"][0]["swaption"]
    assert out["exercise_dates"] == req["swaptions"][0]["swaption"]["exercise_dates"]


def test_xccy_corpus_wrapper_and_exchange_flags_verbatim():
    name = "cross_currency_swap/xccy_usd_sofr_vs_eur_estr_minus15bp_5y_final_exchange_only.json"
    req = _old_fixture(name)
    out = msr.convert_request(req)
    old_item, new_item = req["swaps"][0], out["swaps"][0]
    assert list(new_item.keys()) == list(old_item.keys())
    assert new_item["fx_spot"] == old_item["fx_spot"]
    assert new_item["reporting_currency"] == old_item["reporting_currency"]
    for role in ("leg1", "leg2"):
        o, n = old_item["cross_currency_swap"][role], new_item["cross_currency_swap"][role]
        for k in ("pay_receive", "currency", "coupons_type", "initial_exchange",
                  "final_exchange", "discounting_curve", "forwarding_curve"):
            assert n.get(k) == o.get(k), (role, k)
        assert _old_body(n) == o["coupons"]
    assert out == _fixture(name)


# --------------------------------------------------------------------------- #
# Error cases
# --------------------------------------------------------------------------- #

def _err(req: dict) -> msr.ConversionError:
    with pytest.raises(msr.ConversionError) as info:
        msr.convert_request(req)
    return info.value


def test_unknown_body_field_is_an_error_naming_it():
    req = copy.deepcopy(OLD_VANILLA_0_9_0)
    req["swaps"][0]["vanilla_swap"]["fixed_leg"]["spread"] = 0.0   # an IBOR field on a fixed body
    assert _err(req).field == "swaps[0].vanilla_swap.fixed_leg.spread"
    req = copy.deepcopy(OLD_VANILLA_0_9_0)
    req["swaps"][0]["vanilla_swap"]["floating_leg"]["lookback_days"] = 0
    assert _err(req).field == "swaps[0].vanilla_swap.floating_leg.lookback_days"
    req = copy.deepcopy(OLD_OIS_0_9_0)
    req["swaps"][0]["ois_swap"]["overnight_leg"]["in_arrears"] = False
    assert _err(req).field == "swaps[0].ois_swap.overnight_leg.in_arrears"
    req = copy.deepcopy(OLD_CMS_0_9_0)
    req["swaps"][0]["vanilla_swap"]["cms_leg"]["typo"] = 1
    assert _err(req).field == "swaps[0].vanilla_swap.cms_leg.typo"
    req = copy.deepcopy(OLD_SWAPTION_0_9_0)
    req["swaptions"][0]["swaption"]["underlying"]["floating_leg"]["rate"] = 0.1
    assert _err(req).field == "swaptions[0].swaption.underlying.floating_leg.rate"


def test_nested_intermediate_shape_is_rejected_for_single_currency_swaps():
    """A vanilla / OIS / basis / swaption leg with `coupons` carrying shared
    fields (the nested form that never shipped) is an error naming the
    field, never hoisted."""
    new = _fixture("vanilla_swap_request.json")
    bad = copy.deepcopy(new)
    leg = bad["swaps"][0]["vanilla_swap"]["fixed_leg"]
    leg["coupons"]["notional"] = leg.pop("notional")
    assert _err(bad).field == "swaps[0].vanilla_swap.fixed_leg.coupons.notional"
    bad = copy.deepcopy(new)
    leg = bad["swaps"][0]["vanilla_swap"]["floating_leg"]
    leg["coupons"] = {**{k: leg.pop(k) for k in ("notional", "schedule")}, **leg["coupons"]}
    assert _err(bad).field == "swaps[0].vanilla_swap.floating_leg.coupons.notional"
    # Fully nested legs (every shared field under coupons), every kind.
    for name in ("ois_swap_request.json", "basis_swap_request.json"):
        bad = _fixture(name)
        endpoint = msr.detect_endpoint(bad)
        table = bad["swaps"][0][endpoint]
        role = _ROLES[endpoint][1]
        table[role] = _nest_xccy_leg(table[role])
        assert _err(bad).field.startswith(f"swaps[0].{endpoint}.{role}.coupons.")
    bad = _fixture("swaption_request.json")
    legs = bad["swaptions"][0]["swaption"]["underlying"]
    legs["fixed_leg"] = _nest_xccy_leg(legs["fixed_leg"])
    assert _err(bad).field == "swaptions[0].swaption.underlying.fixed_leg.coupons.notional"


def test_xccy_mixed_levels_and_unknown_fields_are_errors():
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    leg = old["swaps"][0]["cross_currency_swap"]["leg1"]
    leg["notional"] = leg["coupons"]["notional"]            # on both levels
    assert _err(old).field == "swaps[0].cross_currency_swap.leg1.coupons.notional"
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    leg = old["swaps"][0]["cross_currency_swap"]["leg2"]
    leg["schedule"] = leg["coupons"].pop("schedule")        # half hoisted: mixed shapes
    assert _err(old).field == "swaps[0].cross_currency_swap.leg2.coupons.notional"
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    old["swaps"][0]["cross_currency_swap"]["leg2"]["coupons"]["spread"] = 0.0
    assert _err(old).field == "swaps[0].cross_currency_swap.leg2.coupons.spread"
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    old["swaps"][0]["cross_currency_swap"]["leg1"]["swap_type"] = "Payer"
    assert _err(old).field == "swaps[0].cross_currency_swap.leg1.swap_type"
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    old["swaps"][0]["cross_currency_swap"]["leg3"] = {}
    assert _err(old).field == "swaps[0].cross_currency_swap.leg3"
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    old["swaps"][0]["cross_currency_swap"]["leg1"]["coupons_type"] = "Coupons"
    assert _err(old).field == "swaps[0].cross_currency_swap.leg1.coupons_type"
    old = copy.deepcopy(OLD_XCCY_0_9_0)
    del old["swaps"][0]["cross_currency_swap"]["leg2"]
    assert _err(old).field == "swaps[0].cross_currency_swap.leg2"
    new = msr.convert_request(OLD_XCCY_0_9_0)
    new["swaps"][0]["cross_currency_swap"]["leg1"]["coupons"]["rate"] = 0.01
    assert _err(new).field == "swaps[0].cross_currency_swap.leg1.coupons.rate"


def test_unknown_coupons_field_on_a_flat_leg_is_an_error():
    new = _fixture("ois_swap_request.json")
    new["swaps"][0]["ois_swap"]["overnight_leg"]["coupons"]["in_arrears"] = True
    assert _err(new).field == "swaps[0].ois_swap.overnight_leg.coupons.in_arrears"
    new = _fixture("ois_swap_request.json")
    new["swaps"][0]["ois_swap"]["fixed_leg"]["extra"] = 1
    assert _err(new).field == "swaps[0].ois_swap.fixed_leg.extra"


def test_missing_index_currency_is_an_error_naming_the_index():
    req = _old_fixture("vanilla_swap_request.json")
    idx = req["swaps"][0]["vanilla_swap"]["floating_leg"]["index"]["id"]
    for d in req["pricing"]["rates"]["indices"]:
        d.pop("currency", None)
    err = _err(req)
    assert err.field == f"pricing.rates.indices[{idx}].currency"


def test_missing_swap_index_currency_path_for_cms():
    req = _old_fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    for d in req["pricing"]["rates"]["indices"]:
        d.pop("currency", None)
    err = _err(req)
    assert err.field == "pricing.rates.indices[EUR_6M].currency"
    req = _old_fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    req["swaps"][0]["vanilla_swap"]["cms_leg"]["swap_index_id"] = "NOPE"
    assert _err(req).field == "swaps[0].vanilla_swap.cms_leg.swap_index_id"


def test_unknown_index_is_an_error():
    req = _old_fixture("vanilla_swap_request.json")
    req["swaps"][0]["vanilla_swap"]["floating_leg"]["index"]["id"] = "NO_SUCH"
    assert _err(req).field == "swaps[0].vanilla_swap.floating_leg.index.id"


def test_other_products_are_not_swap_requests():
    for name in ("zero_coupon_swap_request.json", "fixed_rate_bond_request.json",
                 "fx_forward/fx_forward_eurusd_6m_buy_eur.json"):
        with pytest.raises(msr.NotASwapRequest):
            msr.detect_endpoint(_fixture(name))
        with pytest.raises(msr.NotASwapRequest):
            msr.convert_request(_fixture(name))
    with pytest.raises(msr.NotASwapRequest):
        msr.detect_endpoint({"swaps": []})
    with pytest.raises(msr.NotASwapRequest):
        msr.detect_endpoint([])


def test_mixed_swap_kinds_in_one_list_is_an_error():
    a = _old_fixture("vanilla_swap_request.json")
    b = _old_fixture("ois_swap_request.json")
    a["swaps"].append(b["swaps"][0])
    with pytest.raises(msr.ConversionError):
        msr.detect_endpoint(a)
    a = copy.deepcopy(OLD_XCCY_0_9_0)
    a["swaps"].append(OLD_BASIS_0_9_0["swaps"][0])
    with pytest.raises(msr.ConversionError):
        msr.detect_endpoint(a)


def test_leftover_unknown_field_on_wrapper_is_an_error():
    req = _old_fixture("vanilla_swap_request.json")
    req["swaps"][0]["extra_thing"] = 1
    assert _err(req).field == "swaps[0].extra_thing"


def test_unknown_field_on_swap_table_is_an_error():
    req = _old_fixture("ois_swap_request.json")
    req["swaps"][0]["ois_swap"]["spread_leg"] = {}
    assert _err(req).field == "swaps[0].ois_swap.spread_leg"
    req = _fixture("ois_swap_request.json")      # same on a 0.10.0 table
    req["swaps"][0]["ois_swap"]["spread_leg"] = {}
    assert _err(req).field == "swaps[0].ois_swap.spread_leg"


def test_missing_or_invalid_swap_type():
    req = _old_fixture("basis_swap_request.json")
    del req["swaps"][0]["basis_swap"]["swap_type"]
    assert _err(req).field == "swaps[0].basis_swap.swap_type"
    req = _old_fixture("basis_swap_request.json")
    req["swaps"][0]["basis_swap"]["swap_type"] = "Long"
    assert _err(req).field == "swaps[0].basis_swap.swap_type"


def test_missing_wrapper_curve_is_an_error():
    req = _old_fixture("vanilla_swap_request.json")
    del req["swaps"][0]["forwarding_curve"]
    assert _err(req).field == "swaps[0].forwarding_curve"
    req = _old_fixture("swaption_request.json")
    del req["swaptions"][0]["discounting_curve"]
    assert _err(req).field == "swaptions[0].discounting_curve"
    req = _old_fixture("basis_swap_request.json")
    req["swaps"][0]["forwarding_curve_leg2"] = ""
    assert _err(req).field == "swaps[0].forwarding_curve_leg2"


def test_both_floating_and_cms_leg_is_an_error():
    req = _old_fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    req["swaps"][0]["vanilla_swap"]["floating_leg"] = \
        _old_fixture("vanilla_swap_request.json")["swaps"][0]["vanilla_swap"]["floating_leg"]
    assert _err(req).field == "swaps[0].vanilla_swap.cms_leg"
    req = _old_fixture("vanilla_swap_request.json")
    del req["swaps"][0]["vanilla_swap"]["floating_leg"]
    assert _err(req).field == "swaps[0].vanilla_swap.floating_leg"


def test_mixed_old_and_new_shape_is_an_error():
    new = _fixture("vanilla_swap_request.json")
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["swap_type"] = "Payer"
    assert _err(bad).field == "swaps[0].vanilla_swap.swap_type"
    bad = copy.deepcopy(new)
    bad["swaps"][0]["discounting_curve"] = "discount"
    assert _err(bad).field == "swaps[0].discounting_curve"
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["cms_leg"] = {}
    assert _err(bad).field == "swaps[0].vanilla_swap.cms_leg"
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["floating_leg"] = \
        OLD_VANILLA_0_9_0["swaps"][0]["vanilla_swap"]["floating_leg"]
    assert _err(bad).field == "swaps[0].vanilla_swap.floating_leg"
    new_sw = _fixture("swaption_request.json")
    bad = copy.deepcopy(new_sw)
    bad["swaptions"][0]["forwarding_curve"] = "discount"
    assert _err(bad).field == "swaptions[0].forwarding_curve"


def test_ois_fixed_leg_with_its_own_lag_in_the_old_shape_is_an_error():
    req = _old_fixture("ois_swap_request.json")
    req["swaps"][0]["ois_swap"]["fixed_leg"]["payment_lag"] = 1
    assert _err(req).field == "swaps[0].ois_swap.fixed_leg.payment_lag"
    req = copy.deepcopy(OLD_SWAPTION_OIS_0_9_0)
    req["swaptions"][0]["swaption"]["underlying"]["fixed_leg"]["payment_calendar"] = "TARGET"
    assert _err(req).field == "swaptions[0].swaption.underlying.fixed_leg.payment_calendar"


def test_bad_swaption_underlying_type():
    req = _old_fixture("swaption_request.json")
    req["swaptions"][0]["swaption"]["underlying_type"] = "BasisSwap"
    assert _err(req).field == "swaptions[0].swaption.underlying_type"


def test_basis_legs_in_different_currencies_is_an_error():
    req = _old_fixture("basis_swap_request.json")
    leg2_idx = req["swaps"][0]["basis_swap"]["leg2"]["index"]["id"]
    for d in req["pricing"]["rates"]["indices"]:
        if d["id"] == leg2_idx:
            d["currency"] = "USD"
    assert _err(req).field == "swaps[0].basis_swap"


# --------------------------------------------------------------------------- #
# Structural validator
# --------------------------------------------------------------------------- #

def _problems(req: dict) -> str:
    return "\n".join(msr.validate_converted(req))


def test_validator_flags_old_shape_and_bad_unions():
    old = _old_fixture("vanilla_swap_request.json")
    assert msr.validate_converted(old)
    new = _fixture("vanilla_swap_request.json")
    assert msr.validate_converted(new) == []
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["floating_leg"]["coupons_type"] = "OvernightCoupons"
    assert "coupons_type" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["floating_leg"]["pay_receive"] = "Pay"
    assert "both legs are Pay" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["forwarding_curve"] = "x"
    assert "forwarding_curve: present on a fixed leg" in _problems(bad)
    bad = copy.deepcopy(new)
    del bad["swaps"][0]["vanilla_swap"]["floating_leg"]["forwarding_curve"]
    assert "forwarding_curve: missing" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["currency"] = "eur"
    assert "currency" in _problems(bad)
    ois = _fixture("ois_swap_request.json")
    bad = copy.deepcopy(ois)
    bad["swaps"][0]["ois_swap"]["overnight_leg"]["coupons_type"] = "IborCoupons"
    assert "coupons_type" in _problems(bad)


def test_validator_checks_the_flat_structure():
    new = _fixture("vanilla_swap_request.json")
    leg = new["swaps"][0]["vanilla_swap"]["fixed_leg"]
    bad = copy.deepcopy(new)
    b = bad["swaps"][0]["vanilla_swap"]["fixed_leg"]
    b["coupons"]["notional"] = b.pop("notional")
    text = _problems(bad)
    assert "coupons.notional: shared field inside coupons" in text and "notional: missing" in text
    bad = copy.deepcopy(new)
    del bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["schedule"]
    assert "fixed_leg.schedule: missing" in _problems(bad)
    bad = copy.deepcopy(new)
    del bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["day_counter"]
    assert "day_counter: missing on FixedCoupons" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["coupons"]["spread"] = 0.0
    assert "fixed_leg.coupons.spread: not a FixedCoupons field" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["vanilla_swap"]["fixed_leg"]["coupons"] = {}
    assert "coupons.rate: missing" in _problems(bad)
    assert "notional" in leg and "schedule" in leg     # the fixture itself is flat
    ois = _fixture("ois_swap_request.json")
    bad = copy.deepcopy(ois)
    del bad["swaps"][0]["ois_swap"]["fixed_leg"]["payment_lag"]
    assert "fixed_leg.payment_lag: missing (presence-required on both OIS legs)" in _problems(bad)
    bad = copy.deepcopy(ois)
    bad["swaps"][0]["ois_swap"]["fixed_leg"]["notionals"] = [1.0]
    assert "notionals: only accepted on price-vanilla-swap" in _problems(bad)
    cms = _fixture("ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json")
    bad = copy.deepcopy(cms)
    bad["swaps"][0]["vanilla_swap"]["floating_leg"]["payment_lag"] = 0
    assert "payment_lag: not accepted on a CMS leg" in _problems(bad)
    sw = _fixture("swaption_request.json")
    bad = copy.deepcopy(sw)
    bad["swaptions"][0]["swaption"]["underlying"]["fixed_leg"]["notionals"] = [1.0]
    assert "notionals: only accepted on price-vanilla-swap" in _problems(bad)


def test_validator_cross_currency():
    new = msr.convert_request(OLD_XCCY_0_9_0)
    assert msr.validate_converted(new) == []
    assert "nested" in _problems(OLD_XCCY_0_9_0)
    bad = copy.deepcopy(new)
    del bad["swaps"][0]["cross_currency_swap"]["leg1"]["initial_exchange"]
    assert "leg1.initial_exchange: missing" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["cross_currency_swap"]["leg2"]["currency"] = "USD"
    assert "both legs in USD" in _problems(bad)
    bad = copy.deepcopy(new)
    del bad["swaps"][0]["fx_spot"]
    assert "fx_spot: missing" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["discounting_curve"] = "x"
    assert "discounting_curve: not allowed on a price-cross-currency-swap wrapper" in _problems(bad)
    bad = copy.deepcopy(new)
    bad["swaps"][0]["cross_currency_swap"]["leg2"]["coupons_type"] = "CmsCoupons"
    assert "leg2.coupons_type" in _problems(bad)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def test_cli_out_dir_writes_converted_files_and_skips_others(tmp_path, capsys):
    src = tmp_path / "in"
    (src / "sub").mkdir(parents=True)
    (src / "sub" / "v.json").write_text(json.dumps(OLD_VANILLA_0_9_0))
    (src / "x.json").write_text(json.dumps(OLD_XCCY_0_9_0))
    (src / "bond.json").write_text(json.dumps(_fixture("fixed_rate_bond_request.json")))
    out = tmp_path / "out"
    rc = msr.main([str(src), "--out", str(out)])
    text = capsys.readouterr().out
    assert rc == 0
    assert (out / "sub" / "v.json").exists() and (out / "x.json").exists()
    assert not (out / "bond.json").exists()
    for name in ("sub/v.json", "x.json"):
        written = json.loads((out / name).read_text())
        assert msr.validate_converted(written) == []
        assert (out / name).read_text().endswith("}\n")
    assert json.loads((out / "x.json").read_text()) == msr.convert_request(OLD_XCCY_0_9_0)
    assert "converted 2 (vanilla_swap=1, ois_swap=0, basis_swap=0, cross_currency_swap=1" in text
    assert "skipped 1" in text and "errors 0" in text


def test_cli_in_place_and_idempotent_rerun(tmp_path, capsys):
    f = tmp_path / "o.json"
    f.write_text(json.dumps(OLD_OIS_0_9_0, indent=2) + "\n")
    assert msr.main([str(f), "--in-place"]) == 0
    first = f.read_text()
    assert "swap_type" not in first and '"coupons_type": "OvernightCoupons"' in first
    doc = json.loads(first)
    assert "notional" in doc["swaps"][0]["ois_swap"]["fixed_leg"]
    assert "notional" not in doc["swaps"][0]["ois_swap"]["fixed_leg"]["coupons"]
    assert msr.main([str(f), "--in-place"]) == 0
    assert f.read_text() == first
    assert "1 already in the 0.10.0 shape" in capsys.readouterr().out
    g = tmp_path / "x.json"
    g.write_text(json.dumps(OLD_XCCY_0_9_0))
    assert msr.main([str(g), "--in-place"]) == 0
    once = g.read_text()
    assert msr.main([str(g), "--in-place"]) == 0 and g.read_text() == once


def test_cli_error_names_file_and_field(tmp_path, capsys):
    req = copy.deepcopy(OLD_VANILLA_0_9_0)
    req["swaps"][0]["leftover"] = True
    f = tmp_path / "bad.json"
    f.write_text(json.dumps(req))
    nested = tmp_path / "nested.json"
    doc = _fixture("vanilla_swap_request.json")
    doc["swaps"][0]["vanilla_swap"]["fixed_leg"] = \
        _nest_xccy_leg(doc["swaps"][0]["vanilla_swap"]["fixed_leg"])
    nested.write_text(json.dumps(doc))
    good = tmp_path / "good.json"
    good.write_text(json.dumps(OLD_BASIS_0_9_0))
    rc = msr.main([str(f), str(nested), str(good), "--out", str(tmp_path / "out")])
    text = capsys.readouterr().out
    assert rc == 1
    assert f"ERROR {f}: swaps[0].leftover" in text
    assert f"ERROR {nested}: swaps[0].vanilla_swap.fixed_leg.coupons.notional" in text
    assert (tmp_path / "out" / "good.json").exists()
    assert "errors 2" in text


def test_cli_invalid_json_and_missing_input(tmp_path, capsys):
    f = tmp_path / "x.json"
    f.write_text("{not json")
    assert msr.main([str(f)]) == 1
    assert "not valid JSON" in capsys.readouterr().out
    assert msr.main([str(tmp_path / "nope.json")]) == 2


# --------------------------------------------------------------------------- #
# --check against a fake server
# --------------------------------------------------------------------------- #

class _Fake:
    """Minimal HTTP server returning canned JSON per path; records bodies."""

    def __init__(self, responses: dict):
        self.responses = responses
        self.requests: list[tuple[str, dict]] = []
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n))
                fake.requests.append((self.path, body))
                resp = fake.responses.get(self.path)
                if resp is None:
                    self.send_response(404)
                    self.end_headers()
                    self.wfile.write(b"no route")
                    return
                data = json.dumps(resp).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def fake_servers():
    made = []

    def make(responses):
        s = _Fake(responses)
        made.append(s)
        return s

    yield make
    for s in made:
        s.close()


def test_check_matching_responses_pass(tmp_path, fake_servers, capsys):
    old = fake_servers({"/price-vanilla-swap": {"swaps": [{
        "npv": 1234.5, "fair_rate": 0.0312, "fair_spread": 0.0011,
        "fixed_leg_npv": -5000.25, "floating_leg_npv": 6234.75}]}})
    new = fake_servers({"/price-vanilla-swap": {"swaps": [{
        "npv": 1234.5 + 1e-6, "fair_rate": 0.0312, "fair_spread": 0.0011,
        "legs": [{"npv": -5000.25}, {"npv": 6234.75}]}]}})
    f = tmp_path / "v.json"
    f.write_text(json.dumps(_old_fixture("vanilla_swap_request.json")))
    rc = msr.main([str(f), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 0, text
    assert "OK" in text and "BAD" not in text
    assert old.requests[0][0] == "/price-vanilla-swap" and "swap_type" in json.dumps(old.requests[0][1])
    assert "swap_type" not in json.dumps(new.requests[0][1])
    assert msr.validate_converted(new.requests[0][1]) == []


def test_check_mismatch_fails_and_prints_table(tmp_path, fake_servers, capsys):
    old = fake_servers({"/price-ois-swap": {"swaps": [{
        "npv": 100.0, "fair_rate": 0.03, "fair_spread": 0.0,
        "fixed_leg_npv": -10.0, "overnight_leg_npv": 110.0}]}})
    new = fake_servers({"/price-ois-swap": {"swaps": [{
        "npv": 100.5, "fair_rate": 0.03, "fair_spread": 0.0,
        "legs": [{"npv": -10.0}, {"npv": 110.5}]}]}})
    f = tmp_path / "o.json"
    f.write_text(json.dumps(_old_fixture("ois_swap_request.json")))
    rc = msr.main([str(f), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 1
    assert "FAIL" in text and "BAD swaps[0].npv" in text
    assert "BAD swaps[0].legs[1].npv (old overnight_leg_npv)" in text
    assert "ok  swaps[0].legs[0].npv (old fixed_leg_npv)" in text


def test_check_cross_currency_posts_both_shapes(tmp_path, fake_servers, capsys):
    resp = {"swaps": [{"npv": 12.5, "legs": [{"npv": -1e6}, {"npv": 1e6 + 12.5}],
                       "fx_spot_used": 1.1}]}
    old = fake_servers({"/price-cross-currency-swap": resp})
    new = fake_servers({"/price-cross-currency-swap": resp})
    f = tmp_path / "x.json"
    f.write_text(json.dumps(OLD_XCCY_0_9_0))
    rc = msr.main([str(f), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 0, text
    assert "(cross_currency_swap, 3 fields, 0 mismatches)" in text
    assert "notional" in old.requests[0][1]["swaps"][0]["cross_currency_swap"]["leg1"]["coupons"]
    assert "notional" in new.requests[0][1]["swaps"][0]["cross_currency_swap"]["leg1"]
    assert msr.validate_converted(new.requests[0][1]) == []


def test_check_http_error_and_already_converted(tmp_path, fake_servers, capsys):
    old = fake_servers({})
    new = fake_servers({})
    f = tmp_path / "v.json"
    f.write_text(json.dumps(_old_fixture("vanilla_swap_request.json")))
    g = tmp_path / "already.json"
    g.write_text(json.dumps(_fixture("vanilla_swap_request.json")))
    rc = msr.main([str(f), str(g), "--check", old.url, new.url])
    text = capsys.readouterr().out
    assert rc == 1
    assert "HTTP 404" in text and "already in the 0.10.0 shape" in text


def test_compare_responses_basis_swaption_xccy_and_tolerance():
    basis = _fixture("ir_swaps/basis_eur_5y_euribor3m_plus5bp_vs_euribor6m.json")
    spreads = [basis["swaps"][0]["basis_swap"][r]["coupons"].get("spread", 0.0)
               for r in ("leg1", "leg2")]
    assert spreads[0] != 0.0 and spreads[1] == 0.0
    old = {"swaps": [{"npv": 10.0, "leg1_npv": 1.0, "leg2_npv": 9.0,
                      "fair_spread_leg1": 0.0007, "fair_spread_leg2": -0.0006}]}
    new = {"swaps": [{"npv": 10.0, "fair_spread": 0.0007, "legs": [{"npv": 1.0}, {"npv": 9.0}]}]}
    rows = msr.compare_responses("basis_swap", basis, old, new)
    assert all(ok for _, _, _, ok in rows), rows
    assert any("fair_spread (old leg1)" in r[0] for r in rows)
    new["swaps"][0]["fair_spread"] = -0.0006
    rows = msr.compare_responses("basis_swap", basis, old, new)
    assert [r for r in rows if "fair_spread" in r[0]][0][3] is False

    sw = _fixture("swaption_request.json")
    rows = msr.compare_responses("swaption", sw, {"swaptions": [{"npv": 5.0}]},
                                 {"swaptions": [{"npv": 5.0}]})
    assert rows == [("swaptions[0].npv", 5.0, 5.0, True)]

    xccy = msr.convert_request(OLD_XCCY_0_9_0)
    a = {"swaps": [{"npv": 1.0, "legs": [{"npv": 2.0}, {"npv": -1.0}]}]}
    b = {"swaps": [{"npv": 1.0, "legs": [{"npv": 2.0}, {"npv": -1.0 + 0.5e-9 * 1.085e7}]}]}
    rows = msr.compare_responses("cross_currency_swap", xccy, a, b)
    assert [r[0] for r in rows] == ["swaps[0].npv", "swaps[0].legs[0].npv", "swaps[0].legs[1].npv"]
    assert all(r[3] for r in rows)
    b["swaps"][0]["legs"][1]["npv"] = -1.0 + 2e-9 * 1.085e7
    assert not all(r[3] for r in msr.compare_responses("cross_currency_swap", xccy, a, b))

    # Money tolerance scales with the leg-level notional (1e6 here -> 1e-3 abs).
    van = _fixture("vanilla_swap_request.json")
    notional = van["swaps"][0]["vanilla_swap"]["fixed_leg"]["notional"]
    base = {"npv": 1.0, "fair_rate": 0.03, "fair_spread": 0.0}
    ok = dict(base, npv=1.0 + 0.5e-9 * notional)
    bad = dict(base, npv=1.0 + 2e-9 * notional)
    assert all(r[3] for r in msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [ok]}))
    assert not all(r[3] for r in msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [bad]}))
    # Rates do not scale with the notional.
    bad_rate = dict(base, fair_rate=0.03 + 1e-9)
    assert not all(r[3] for r in msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [bad_rate]}))
    # A missing field on either side is a mismatch, never a silent pass.
    rows = msr.compare_responses("vanilla_swap", van, {"swaps": [base]}, {"swaps": [{"npv": 1.0}]})
    assert [r[3] for r in rows] == [True, False, False]
