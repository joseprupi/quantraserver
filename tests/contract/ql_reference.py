#!/usr/bin/env python3
"""
Quantra JSON API vs QuantLib — shared reference harness.

This module is the correctness anchor for the contract/parity pytest suite
(tests/contract/). It holds:

  * ApiClient, which POSTs the example requests to the JSON API verbatim
    (they are stored in the canonical nested schema). A read-only
    _reference_pricing_view() flattens the nested groups for the QuantLib
    reference pricers only — it is never used for an API POST.
  * The get_*(...) QuantLib-enum converters and index/curve builders
    (build_ibor_index, build_overnight_index, build_curve_from_json, ...).
  * The independent per-product QuantLib reference pricers
    (price_fixed_rate_bond_ql ... price_cds_ql) whose NPVs the API is
    compared against — these mirror the legacy monolith (the vanilla swap
    pricer additionally honours the request's floating IndexDef and a
    separate forwarding curve, matching the server) and MUST NOT be
    weakened.
  * _make_multicurve_exogenous_request(), the inline 2-curve bootstrap
    request reused by the bootstrap-curves contract tests.

The pytest test modules (e.g. bond_test.py, swaption_test.py) import the
pricers/helpers from here; conftest.py exposes the client/data-dir fixtures.
"""

import json
import argparse
import requests
import re
from pathlib import Path
from typing import Tuple, Optional, Dict, Any

try:
    import QuantLib as ql
except ImportError:
    print("ERROR: QuantLib not found. Install with: pip install QuantLib")
    exit(1)


# =============================================================================
# API Client
# =============================================================================

class ApiClient:
    @staticmethod
    def _load_endpoints() -> Dict[str, str]:
        # This module lives at <repo>/tests/contract/ql_reference.py, so the
        # repo root is parents[2].
        header = Path(__file__).resolve().parents[2] / "src" / "common" / "product_catalog.h"
        pattern = re.compile(
            r'^\s*X\(\s*\w+,\s*"(?P<key>[^"]+)",\s*"(?P<route>[^"]+)",\s*"[^"]+",\s*"[^"]+"\s*\)'
        )
        endpoints: Dict[str, str] = {}
        with open(header, "r") as f:
            for raw_line in f:
                line = raw_line.strip().rstrip("\\")
                match = pattern.match(line)
                if match:
                    entry = match.groupdict()
                    endpoints[entry["key"]] = entry["route"]
        if not endpoints:
            raise RuntimeError(f"Could not parse endpoint catalog from {header}")
        return endpoints

    ENDPOINTS = _load_endpoints.__func__()
    
    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip('/')
        self.session = requests.Session()
        self.session.headers.update({'Content-Type': 'application/json'})
    
    def health(self) -> bool:
        try:
            return self.session.get(f"{self.base_url}/health").status_code == 200
        except:
            return False
    
    def price(self, product: str, request: dict) -> dict:
        endpoint = self.ENDPOINTS[product]
        # Example payloads are stored in the canonical nested schema, so they are
        # POSTed verbatim — exactly what a real client sends.
        r = self.session.post(f"{self.base_url}/{endpoint}", json=request)
        if r.status_code != 200:
            raise Exception(f"API error ({r.status_code}): {r.text[:200]}")
        return r.json()


def _reference_pricing_view(request: dict) -> dict:
    """Flat read-only view of a request's pricing for the QuantLib reference pricers.

    The example payloads are stored (and POSTed) in the canonical nested shape
    (pricing.rates.*, pricing.volatility.*, pricing.credit.*, ...). The
    independent QuantLib reference pricers below read the legacy flat keys
    (pricing["curves"], pricing.get("vol_surfaces"), ...). This merges the
    nested domain groups back up to the top level so the reference pricers see
    exactly the collections they expect. It is NEVER used for an API POST — the
    server only ever receives the verbatim nested request.
    """
    pricing = request.get("pricing", request)
    if not isinstance(pricing, dict):
        return pricing
    flat = dict(pricing)
    for group in ("rates", "credit", "volatility", "equity", "inflation", "options"):
        section = pricing.get(group)
        if isinstance(section, dict):
            for key, value in section.items():
                flat.setdefault(key, value)
    return flat


def _period_n_unit(container: dict, key: str = "tenor", default_n: int = 0, default_unit: str = "Days"):
    # Canonical Period object
    if "n" in container and "unit" in container:
        return int(container.get("n", default_n)), container.get("unit", default_unit)
    p = container.get(key)
    if isinstance(p, dict):
        return int(p.get("n", default_n)), p.get("unit", default_unit)
    return int(container.get(f"{key}_number", default_n)), container.get(f"{key}_time_unit", default_unit)


# =============================================================================
# JSON Helpers
# =============================================================================

def load_json(filepath: Path) -> dict:
    with open(filepath) as f:
        return json.load(f)


def parse_date(date_str: str) -> ql.Date:
    """Parse date string like '2025-01-15' or '2025/01/15' to QuantLib Date."""
    date_str = date_str.replace('/', '-')
    parts = date_str.split('-')
    return ql.Date(int(parts[2]), int(parts[1]), int(parts[0]))


def get_day_counter(name: str) -> ql.DayCounter:
    mapping = {
        "Actual360": ql.Actual360(),
        "Actual365Fixed": ql.Actual365Fixed(),
        "ActualActualISDA": ql.ActualActual(ql.ActualActual.ISDA),
        "ActualActualBond": ql.ActualActual(ql.ActualActual.Bond),
        "Thirty360": ql.Thirty360(ql.Thirty360.BondBasis),
    }
    return mapping.get(name, ql.Actual365Fixed())


def get_frequency(name: str):
    mapping = {
        "Annual": ql.Annual,
        "Semiannual": ql.Semiannual,
        "Quarterly": ql.Quarterly,
        "Monthly": ql.Monthly,
    }
    return mapping.get(name, ql.Annual)


def get_convention(name: str):
    mapping = {
        "ModifiedFollowing": ql.ModifiedFollowing,
        "Following": ql.Following,
        "Preceding": ql.Preceding,
        "Unadjusted": ql.Unadjusted,
    }
    return mapping.get(name, ql.ModifiedFollowing)


def get_calendar(name: str):
    mapping = {
        "TARGET": ql.TARGET(),
        "UnitedStates": ql.UnitedStates(ql.UnitedStates.NYSE),
        "UnitedStatesNYSE": ql.UnitedStates(ql.UnitedStates.NYSE),
        "UnitedStatesGovernmentBond": ql.UnitedStates(ql.UnitedStates.GovernmentBond),
    }
    return mapping.get(name, ql.TARGET())


def get_date_generation(name: str):
    mapping = {
        "Forward": ql.DateGeneration.Forward,
        "Backward": ql.DateGeneration.Backward,
        "TwentiethIMM": ql.DateGeneration.TwentiethIMM,
    }
    return mapping.get(name, ql.DateGeneration.Forward)


def get_time_unit(name: str):
    mapping = {
        "Days": ql.Days,
        "Weeks": ql.Weeks,
        "Months": ql.Months,
        "Years": ql.Years,
    }
    return mapping.get(name, ql.Days)


def get_rate_averaging(name: str):
    mapping = {
        "Compound": ql.RateAveraging.Compound,
        "Simple": ql.RateAveraging.Simple,
    }
    return mapping.get(name, ql.RateAveraging.Compound)


def get_settlement_method(name: str):
    mapping = {
        "PhysicalOTC": ql.Settlement.PhysicalOTC,
        "PhysicalCleared": ql.Settlement.PhysicalCleared,
        "CollateralizedCashPrice": ql.Settlement.CollateralizedCashPrice,
        "ParYieldCurve": ql.Settlement.ParYieldCurve,
    }
    return mapping.get(name, ql.Settlement.PhysicalOTC)


def get_volatility_type(name: str):
    mapping = {
        "Normal": ql.Normal,
        "Lognormal": ql.ShiftedLognormal,
        "ShiftedLognormal": ql.ShiftedLognormal,
    }
    return mapping.get(name, ql.ShiftedLognormal)


def get_compounding(name: str):
    mapping = {
        "Compounded": ql.Compounded,
        "Continuous": ql.Continuous,
        "Simple": ql.Simple,
        "SimpleThenCompounded": ql.SimpleThenCompounded,
    }
    return mapping.get(name, ql.Continuous)


def get_interpolator(name: str):
    mapping = {
        "Linear": ql.Linear(),
        "LogLinear": ql.LogLinear(),
        "BackwardFlat": ql.BackwardFlat(),
        "ForwardFlat": ql.ForwardFlat(),
        "LogCubic": ql.MonotonicLogCubic(),
    }
    return mapping.get(name, ql.Linear())


def get_currency(code: str):
    mapping = {
        "USD": ql.USDCurrency(),
        "EUR": ql.EURCurrency(),
        "GBP": ql.GBPCurrency(),
        "JPY": ql.JPYCurrency(),
        "CHF": ql.CHFCurrency(),
    }
    return mapping.get(code, ql.USDCurrency())


def get_cds_engine_type(name: str):
    mapping = {
        "MidPoint": "MidPoint",
        "ISDA": "ISDA",
    }
    return mapping.get(name, "MidPoint")


def get_cds_helper_model(name: str):
    mapping = {
        "MidPoint": ql.CreditDefaultSwap.Midpoint,
        "ISDA": ql.CreditDefaultSwap.ISDA,
    }
    return mapping.get(name, ql.CreditDefaultSwap.Midpoint)


def get_cds_curve_interpolator(name: str):
    mapping = {
        "LogLinear": ql.LogLinear(),
        "Linear": ql.Linear(),
        "BackwardFlat": ql.BackwardFlat(),
    }
    return mapping.get(name, ql.LogLinear())


def get_isda_numerical_fix(name: str):
    none_fix = getattr(ql.IsdaCdsEngine, "None", ql.IsdaCdsEngine.Taylor)
    mapping = {
        "None": none_fix,
        "Taylor": ql.IsdaCdsEngine.Taylor,
    }
    return mapping.get(name, ql.IsdaCdsEngine.Taylor)


def get_isda_accrual_bias(name: str):
    mapping = {
        "HalfDayBias": ql.IsdaCdsEngine.HalfDayBias,
        "NoBias": ql.IsdaCdsEngine.NoBias,
    }
    return mapping.get(name, ql.IsdaCdsEngine.HalfDayBias)


def get_isda_forwards_in_coupon_period(name: str):
    mapping = {
        "Flat": ql.IsdaCdsEngine.Flat,
        "Piecewise": ql.IsdaCdsEngine.Piecewise,
    }
    return mapping.get(name, ql.IsdaCdsEngine.Piecewise)


def build_ibor_index(idx_def: dict, curve_handle=None):
    tenor_n, tenor_u = _period_n_unit(idx_def, "tenor", 6, "Months")
    period = ql.Period(tenor_n, get_time_unit(tenor_u))
    fixing_days = idx_def.get("fixing_days", 2)
    calendar = get_calendar(idx_def.get("calendar", "TARGET"))
    bdc = get_convention(idx_def.get("business_day_convention", "ModifiedFollowing"))
    eom = idx_def.get("end_of_month", False)
    day_counter = get_day_counter(idx_def.get("day_counter", "Actual360"))
    ccy = get_currency(idx_def.get("currency", "USD"))
    name = idx_def.get("name", idx_def.get("id", "Ibor"))
    handle = curve_handle if curve_handle else ql.YieldTermStructureHandle()
    return ql.IborIndex(name, period, fixing_days, ccy, calendar, bdc, eom, day_counter, handle)


def build_overnight_index(idx_def: dict, curve_handle=None):
    fixing_days = idx_def.get("fixing_days", 0)
    calendar = get_calendar(idx_def.get("calendar", "TARGET"))
    day_counter = get_day_counter(idx_def.get("day_counter", "Actual360"))
    ccy = get_currency(idx_def.get("currency", "USD"))
    name = idx_def.get("name", idx_def.get("id", "ON"))
    handle = curve_handle if curve_handle else ql.YieldTermStructureHandle()
    return ql.OvernightIndex(name, fixing_days, ccy, calendar, day_counter, handle)


def get_ibor_index(name: str):
    if "Euribor6M" in name:
        return ql.Euribor6M()
    elif "Euribor3M" in name:
        return ql.Euribor3M()
    return ql.Euribor6M()


# =============================================================================
# Curve Building
# =============================================================================


def find_index_def(idx_id, request_data):
    """Find an IndexDef in the request's indices array by id."""
    pricing = _reference_pricing_view(request_data)
    indices = pricing.get("indices", [])
    for idef in indices:
        if idef.get("id") == idx_id:
            return idef
    # Also check top-level indices (for bootstrap requests)
    for idef in request_data.get("indices", []):
        if idef.get("id") == idx_id:
            return idef
    return None


def resolve_index_from_id(idx_id, request_data, curve_handle=None):
    """Resolve an IndexRef id to a QuantLib index for QuantLib-side comparison."""
    idx_def = find_index_def(idx_id, request_data)
    if idx_def:
        if idx_def.get("index_type") == "Overnight":
            return build_overnight_index(idx_def, curve_handle)
        return build_ibor_index(idx_def, curve_handle)

    # Fallback for legacy ids
    if "3M" in idx_id:
        return ql.Euribor3M()
    if "6M" in idx_id:
        return ql.Euribor6M()
    return ql.Euribor6M()


def build_curve_from_json(curve_json: dict, eval_date: ql.Date, request_data: dict = None) -> ql.YieldTermStructureHandle:
    """Build QuantLib curve from JSON curve definition."""
    points = curve_json.get("points", [])
    quote_values = {}
    quote_types = {}
    if request_data:
        pricing = request_data.get("pricing", {})
        for q in pricing.get("quotes", []):
            if "id" in q:
                quote_values[q["id"]] = q.get("value", 0.0)
                quote_types[q["id"]] = q.get("quote_type", "Curve")

    def resolve_point_rate(point: dict) -> float:
        quote_id = point.get("quote_id")
        if quote_id:
            if quote_types.get(quote_id, "Curve") != "Curve":
                raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
            return quote_values.get(quote_id, 0.0)
        return point["rate"]

    def resolve_bond_quote(point: dict) -> float:
        # Mirrors the server's presence-based selection: price wins over rate.
        quote_id = point.get("quote_id")
        if quote_id:
            if quote_types.get(quote_id, "Curve") != "Curve":
                raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
            return quote_values.get(quote_id, 0.0)
        if "price" in point:
            return point["price"]
        return point["rate"]
    if any(p.get("point_type") == "ZeroRatePoint" for p in points):
        dates = []
        rates = []
        compounding = ql.Continuous
        frequency = ql.Annual
        comp_set = False

        for point_wrapper in points:
            if point_wrapper["point_type"] != "ZeroRatePoint":
                raise ValueError("ZeroRatePoint cannot be mixed with bootstrap helpers")
            point = point_wrapper["point"]
            if "date" in point and point["date"]:
                d = parse_date(point["date"])
            else:
                tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
                tenor_unit = get_time_unit(tenor_unit_s)
                cal = get_calendar(point.get("calendar", "TARGET"))
                bdc = get_convention(point.get("business_day_convention", "ModifiedFollowing"))
                d = cal.advance(eval_date, ql.Period(tenor_num, tenor_unit), bdc)
            dates.append(d)
            rates.append(point["zero_rate"])

            if not comp_set:
                compounding = get_compounding(point.get("compounding", "Continuous"))
                frequency = get_frequency(point.get("frequency", "Annual"))
                comp_set = True

        day_counter = get_day_counter(curve_json.get("day_counter", "Actual365Fixed"))
        interpolator = get_interpolator(curve_json.get("interpolator", "Linear"))
        curve = ql.ZeroCurve(dates, rates, day_counter, get_calendar(curve_json.get("calendar", "TARGET")),
                             interpolator, compounding, frequency)
        curve.enableExtrapolation()
        return ql.YieldTermStructureHandle(curve)

    helpers = []
    seen_pillars = set()  # Track pillar dates to avoid duplicates

    for point_wrapper in points:
        point_type = point_wrapper["point_type"]
        point = point_wrapper["point"]
        
        if point_type == "DepositHelper":
            tenor_num, tenor_str = _period_n_unit(point, "tenor", 0, "Days")
            if tenor_str == "Weeks":
                tenor_unit = ql.Weeks
            elif tenor_str == "Months":
                tenor_unit = ql.Months
            else:
                tenor_unit = ql.Years
            
            helper = ql.DepositRateHelper(
                resolve_point_rate(point),
                ql.Period(tenor_num, tenor_unit),
                point.get("fixing_days", 2),
                get_calendar(point.get("calendar", "TARGET")),
                get_convention(point.get("business_day_convention", "ModifiedFollowing")),
                True,
                get_day_counter(point.get("day_counter", "Actual365Fixed"))
            )
            
            # Check for duplicate pillar
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
        
        elif point_type == "SwapHelper":
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Years")
            tenor_unit = ql.Years if tenor_unit_s == "Years" else ql.Months
            index = ql.Euribor6M()
            if request_data and point.get("float_index", {}).get("id"):
                index = resolve_index_from_id(point["float_index"]["id"], request_data)
            
            helper = ql.SwapRateHelper(
                resolve_point_rate(point),
                ql.Period(tenor_num, tenor_unit),
                get_calendar(point.get("calendar", "TARGET")),
                get_frequency(point.get("sw_fixed_leg_frequency", "Annual")),
                get_convention(point.get("sw_fixed_leg_convention", "ModifiedFollowing")),
                get_day_counter(point.get("sw_fixed_leg_day_counter", "Thirty360")),
                index
            )
            
            # Check for duplicate pillar
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
        
        elif point_type == "OISHelper":
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
            tenor_unit = get_time_unit(tenor_unit_s)
            overnight_idx = None
            if request_data and point.get("overnight_index", {}).get("id"):
                overnight_idx = resolve_index_from_id(point["overnight_index"]["id"], request_data)
            if overnight_idx is None:
                overnight_idx = ql.OvernightIndex(
                    "ON", 0, ql.USDCurrency(), ql.TARGET(), ql.Actual360()
                )
            helper = ql.OISRateHelper(
                point.get("settlement_days", 2),
                ql.Period(tenor_num, tenor_unit),
                resolve_point_rate(point),
                overnight_idx
            )
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
        
        elif point_type == "BondHelper":
            # Build bond schedule
            sch = point["schedule"]
            schedule = ql.Schedule(
                parse_date(sch["effective_date"]),
                parse_date(sch["termination_date"]),
                ql.Period(get_frequency(sch["frequency"])),
                get_calendar(sch.get("calendar", "TARGET")),
                get_convention(sch.get("convention", "Unadjusted")),
                get_convention(sch.get("termination_date_convention", "Unadjusted")),
                get_date_generation(sch.get("date_generation_rule", "Backward")),
                False
            )
            
            # Create fixed rate bond helper
            helper = ql.FixedRateBondHelper(
                ql.QuoteHandle(ql.SimpleQuote(resolve_bond_quote(point))),  # Clean price
                point.get("settlement_days", 3),
                point.get("face_amount", 100.0),
                schedule,
                [point["coupon_rate"]],
                get_day_counter(point.get("day_counter", "ActualActualBond")),
                get_convention(point.get("business_day_convention", "Unadjusted")),
                point.get("redemption", 100.0),
                parse_date(point.get("issue_date", sch["effective_date"]))
            )
            
            # Check for duplicate pillar
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
    
    if not helpers:
        # Fallback to flat curve
        return ql.YieldTermStructureHandle(
            ql.FlatForward(eval_date, 0.03, ql.Actual365Fixed())
        )
    
    curve = ql.PiecewiseLogLinearDiscount(eval_date, helpers, get_day_counter(curve_json.get("day_counter", "Actual365Fixed")))
    curve.enableExtrapolation()
    return ql.YieldTermStructureHandle(curve)


# =============================================================================
# QuantLib Pricing Functions
# =============================================================================

def price_fixed_rate_bond_ql(request: dict) -> float:
    """Price fixed rate bond using QuantLib."""
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["fixed_rate_bond"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build curve
    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Build schedule
    sch = bond["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        False
    )
    
    # Build bond
    ql_bond = ql.FixedRateBond(
        bond.get("settlement_days", 2),
        bond.get("face_amount", 100.0),
        schedule,
        [bond["rate"]],
        get_day_counter(bond.get("accrual_day_counter", "Thirty360"))
    )
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(curve))
    
    return ql_bond.NPV()


def price_floating_rate_bond_ql(request: dict) -> float:
    """Price floating rate bond using QuantLib."""
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["floating_rate_bond"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build discount curve
    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    discount_curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Build forwarding curve (may be different)
    forward_id = bond_data.get("forwarding_curve", curve_id)
    forward_json = next((c for c in pricing["curves"] if c["id"] == forward_id), curve_json)
    forward_curve = build_curve_from_json(forward_json, eval_date, request)
    
    # Build schedule
    sch = bond["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        False
    )
    
    # Get index info
    idx_ref = bond.get("index", {})
    idx_id = idx_ref.get("id", "EUR_6M") if isinstance(idx_ref, dict) else "EUR_6M"
    
    # Resolve index from definitions
    idx_def = find_index_def(idx_id, request)
    period_months = (_period_n_unit(idx_def, "tenor", 6, "Months")[0] if idx_def else 6)
    
    # Create index with forecasting curve
    if period_months == 3:
        index = ql.Euribor3M(forward_curve)
    elif period_months == 6:
        index = ql.Euribor6M(forward_curve)
    else:
        index = ql.Euribor6M(forward_curve)
    
    # Add any fixings from the IndexDef
    if idx_def:
        for fixing in idx_def.get("fixings", []):
            fixing_date = parse_date(fixing["date"])
            index.addFixing(fixing_date, fixing["value"])
    
    # Build floating rate bond
    ql_bond = ql.FloatingRateBond(
        bond.get("settlement_days", 2),
        bond.get("face_amount", 100.0),
        schedule,
        index,
        get_day_counter(bond.get("accrual_day_counter", "Actual360")),
        get_convention(bond.get("payment_convention", "ModifiedFollowing")),
        bond.get("fixing_days", 2),
        [1.0],  # gearings
        [bond.get("spread", 0.0)],  # spreads
        [],  # caps
        [],  # floors
        bond.get("in_arrears", False),
        bond.get("redemption", 100.0),
        parse_date(bond.get("issue_date", sch["effective_date"]))
    )
    
    # Set up coupon pricer for floating rate coupons
    pricer = ql.BlackIborCouponPricer()
    volatility = ql.ConstantOptionletVolatility(
        bond.get("settlement_days", 2),
        get_calendar((idx_def or {}).get("calendar", "TARGET")),
        get_convention((idx_def or {}).get("business_day_convention", "ModifiedFollowing")),
        0.0,  # zero volatility for simple pricing
        ql.Actual365Fixed()
    )
    pricer.setCapletVolatility(ql.OptionletVolatilityStructureHandle(volatility))
    ql.setCouponPricer(ql_bond.cashflows(), pricer)
    
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(discount_curve))
    
    return ql_bond.NPV()


def price_vanilla_swap_ql(request: dict) -> float:
    """Price vanilla swap using QuantLib."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    swap = swap_data["vanilla_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Build discounting curve
    curve_id = swap_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)

    # Forwarding curve for the floating index. Multicurve requests discount on
    # a separate (typically OIS) curve; single-curve requests point both ids at
    # the same curve, which keeps this identical to the single-curve behaviour.
    forward_id = swap_data.get("forwarding_curve", curve_id)
    if forward_id == curve_id:
        forward_curve = curve
    else:
        forward_json = next((c for c in pricing["curves"] if c["id"] == forward_id), curve_json)
        forward_curve = build_curve_from_json(forward_json, eval_date, request)

    # Fixed leg
    fixed_leg = swap["fixed_leg"]
    fixed_sch = fixed_leg["schedule"]
    fixed_schedule = ql.Schedule(
        parse_date(fixed_sch["effective_date"]),
        parse_date(fixed_sch["termination_date"]),
        ql.Period(get_frequency(fixed_sch["frequency"])),
        get_calendar(fixed_sch.get("calendar", "TARGET")),
        get_convention(fixed_sch.get("convention", "ModifiedFollowing")),
        get_convention(fixed_sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(fixed_sch.get("date_generation_rule", "Forward")),
        False
    )
    
    # Float leg
    float_leg = swap["floating_leg"]
    float_sch = float_leg["schedule"]
    float_schedule = ql.Schedule(
        parse_date(float_sch["effective_date"]),
        parse_date(float_sch["termination_date"]),
        ql.Period(get_frequency(float_sch["frequency"])),
        get_calendar(float_sch.get("calendar", "TARGET")),
        get_convention(float_sch.get("convention", "ModifiedFollowing")),
        get_convention(float_sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(float_sch.get("date_generation_rule", "Forward")),
        False
    )
    
    # Resolve the floating index exactly as the server does: from the request's
    # IndexDef (tenor / calendar / conventions), projecting off the forwarding
    # curve. Falls back to Euribor6M when the request carries no index def.
    idx_ref = float_leg.get("index", {})
    idx_id = idx_ref.get("id") if isinstance(idx_ref, dict) else None
    if idx_id and find_index_def(idx_id, request):
        index = resolve_index_from_id(idx_id, request, forward_curve)
    else:
        index = ql.Euribor6M(forward_curve)
    swap_type = ql.VanillaSwap.Payer if swap["swap_type"] == "Payer" else ql.VanillaSwap.Receiver
    
    ql_swap = ql.VanillaSwap(
        swap_type,
        fixed_leg["notional"],
        fixed_schedule,
        fixed_leg["rate"],
        get_day_counter(fixed_leg.get("day_counter", "Thirty360")),
        float_schedule,
        index,
        float_leg.get("spread", 0.0),
        get_day_counter(float_leg.get("day_counter", "Actual360"))
    )
    ql_swap.setPricingEngine(ql.DiscountingSwapEngine(curve))
    
    return ql_swap.NPV()


def _build_overnight_index(idx_def: dict, curve):
    idx_name = (idx_def or {}).get("name", "ON")
    fixing_days = int((idx_def or {}).get("fixing_days", 0))
    ccy = get_currency((idx_def or {}).get("currency", "USD"))
    cal = get_calendar((idx_def or {}).get("calendar", "TARGET"))
    dc = get_day_counter((idx_def or {}).get("day_counter", "Actual360"))
    return ql.OvernightIndex(idx_name, fixing_days, ccy, cal, dc, curve)


def price_ois_swap_ql(request: dict) -> float:
    """Price OIS swap using QuantLib."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    swap = swap_data["ois_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    disc_id = swap_data.get("discounting_curve", "discount")
    disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id), pricing["curves"][0])
    discount_curve = build_curve_from_json(disc_json, eval_date, request)

    fwd_id = swap_data.get("forwarding_curve", disc_id)
    fwd_json = next((c for c in pricing["curves"] if c["id"] == fwd_id), disc_json)
    forward_curve = build_curve_from_json(fwd_json, eval_date, request)

    fixed = swap["fixed_leg"]
    fs = fixed["schedule"]
    fixed_schedule = ql.Schedule(
        parse_date(fs["effective_date"]),
        parse_date(fs["termination_date"]),
        ql.Period(get_frequency(fs["frequency"])),
        get_calendar(fs.get("calendar", "TARGET")),
        get_convention(fs.get("convention", "ModifiedFollowing")),
        get_convention(fs.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(fs.get("date_generation_rule", "Forward")),
        False,
    )

    overnight = swap["overnight_leg"]
    os = overnight["schedule"]
    overnight_schedule = ql.Schedule(
        parse_date(os["effective_date"]),
        parse_date(os["termination_date"]),
        ql.Period(get_frequency(os["frequency"])),
        get_calendar(os.get("calendar", "TARGET")),
        get_convention(os.get("convention", "ModifiedFollowing")),
        get_convention(os.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(os.get("date_generation_rule", "Forward")),
        False,
    )

    idx_def = find_index_def(overnight["index"]["id"], request)
    overnight_index = _build_overnight_index(idx_def, forward_curve)

    swap_type = ql.OvernightIndexedSwap.Payer if swap["swap_type"] == "Payer" else ql.OvernightIndexedSwap.Receiver
    lookback = overnight.get("lookback_days", -1)
    lookback = 0 if lookback < 0 else int(lookback)
    # Python bindings in our QuantLib wheel expose the one-schedule overload.
    # For this request shape fixed and overnight schedules are aligned.
    ql_swap = ql.OvernightIndexedSwap(
        swap_type,
        fixed["notional"],
        overnight_schedule,
        fixed["rate"],
        get_day_counter(fixed.get("day_counter", "Actual360")),
        overnight_index,
        overnight.get("spread", 0.0),
        overnight.get("payment_lag", 0),
        get_convention(overnight.get("payment_convention", "Following")),
        get_calendar(overnight.get("payment_calendar", "TARGET")),
        overnight.get("telescopic_value_dates", False),
        get_rate_averaging(overnight.get("averaging_method", "Compound")),
        lookback,
        overnight.get("lockout_days", 0),
        overnight.get("apply_observation_shift", False),
    )
    ql_swap.setPricingEngine(ql.DiscountingSwapEngine(discount_curve))
    return ql_swap.NPV()


def price_basis_swap_ql(request: dict) -> float:
    """Price basis swap as two floating legs using QuantLib::Swap."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    swap = swap_data["basis_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    disc_id = swap_data.get("discounting_curve", "discount")
    disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id), pricing["curves"][0])
    discount_curve = build_curve_from_json(disc_json, eval_date, request)

    fwd1_id = swap_data.get("forwarding_curve_leg1", disc_id)
    fwd2_id = swap_data.get("forwarding_curve_leg2", disc_id)
    fwd1_json = next((c for c in pricing["curves"] if c["id"] == fwd1_id), disc_json)
    fwd2_json = next((c for c in pricing["curves"] if c["id"] == fwd2_id), disc_json)
    fwd1_curve = build_curve_from_json(fwd1_json, eval_date, request)
    fwd2_curve = build_curve_from_json(fwd2_json, eval_date, request)

    leg1 = swap["leg1"]
    leg2 = swap["leg2"]

    def _build_leg(leg: dict, curve):
        sch = leg["schedule"]
        schedule = ql.Schedule(
            parse_date(sch["effective_date"]),
            parse_date(sch["termination_date"]),
            ql.Period(get_frequency(sch["frequency"])),
            get_calendar(sch.get("calendar", "TARGET")),
            get_convention(sch.get("convention", "ModifiedFollowing")),
            get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
            get_date_generation(sch.get("date_generation_rule", "Forward")),
            False,
        )
        idx_def = find_index_def(leg["index"]["id"], request)
        n, _ = _period_n_unit(idx_def, "tenor", 6, "Months")
        index = ql.Euribor3M(curve) if n <= 3 else ql.Euribor6M(curve)
        return ql.IborLeg([leg["notional"]], schedule, index, get_day_counter(leg.get("day_counter", "Actual360")), get_convention(leg.get("payment_convention", "ModifiedFollowing")), [leg.get("fixing_days", 2)], [1.0], [leg.get("spread", 0.0)], [], [], leg.get("in_arrears", False))

    ql_leg1 = _build_leg(leg1, fwd1_curve)
    ql_leg2 = _build_leg(leg2, fwd2_curve)
    payer = [True, False] if swap["swap_type"] == "Payer" else [False, True]
    ql_swap = ql.Swap([ql_leg1, ql_leg2], payer)
    ql_swap.setPricingEngine(ql.DiscountingSwapEngine(discount_curve))
    return ql_swap.NPV()


def price_fra_ql(request: dict) -> float:
    """Price FRA using QuantLib."""
    pricing = _reference_pricing_view(request)
    fra_data = request["fras"][0]
    fra = fra_data["fra"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build curve
    curve_id = fra_data.get("forwarding_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Get index period from JSON
    idx = fra.get("index", {})
    # New schema: index is IndexRef with just id
    if isinstance(idx, dict) and "id" in idx:
        idx_def = find_index_def(idx["id"], request)
        period_months = (_period_n_unit(idx_def, "tenor", 3, "Months")[0] if idx_def else 3)
    else:
        period_months = idx.get("period_number", 3)
    
    if period_months == 3:
        index = ql.Euribor3M(curve)
    else:
        index = ql.Euribor6M(curve)
    
    start_date = parse_date(fra["start_date"])
    position = ql.Position.Long if fra["fra_type"] == "Long" else ql.Position.Short
    
    ql_fra = ql.ForwardRateAgreement(
        index, start_date, position, fra["strike"], fra["notional"], curve
    )
    
    return ql_fra.NPV()


def price_cap_floor_ql(request: dict) -> float:
    """Price cap/floor using QuantLib."""
    pricing = _reference_pricing_view(request)
    cf_data = request["cap_floors"][0]
    cf = cf_data["cap_floor"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build curve
    curve_id = cf_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Build schedule
    sch = cf["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        False
    )
    
    # Get index
    idx = cf.get("index", {})
    # New schema: index is IndexRef with just id
    if isinstance(idx, dict) and "id" in idx:
        idx_def = find_index_def(idx["id"], request)
        period_months = (_period_n_unit(idx_def, "tenor", 3, "Months")[0] if idx_def else 3)
    else:
        period_months = idx.get("period_number", 3)
    index = ql.Euribor3M(curve) if period_months == 3 else ql.Euribor6M(curve)
    
    # Build cap or floor
    if cf["cap_floor_type"] == "Cap":
        ql_cf = ql.Cap(ql.IborLeg([cf["notional"]], schedule, index), [cf["strike"]])
    else:
        ql_cf = ql.Floor(ql.IborLeg([cf["notional"]], schedule, index), [cf["strike"]])
    
    # Get volatility
    vol = 0.20  # Default
    for v in pricing.get("volatilities", []):
        if v["id"] == cf_data.get("volatility"):
            vol = v.get("constant_vol", 0.20)
            break
    
    vol_handle = ql.OptionletVolatilityStructureHandle(
        ql.ConstantOptionletVolatility(eval_date, ql.TARGET(), ql.ModifiedFollowing, vol, ql.Actual365Fixed())
    )
    ql_cf.setPricingEngine(ql.BlackCapFloorEngine(curve, vol_handle))
    
    return ql_cf.NPV()


def price_swaption_ql(request: dict) -> float:
    """Price swaption using QuantLib."""
    pricing = _reference_pricing_view(request)
    sw_data = request["swaptions"][0]
    sw = sw_data["swaption"]
    underlying_type = sw.get("underlying_type")
    underlying = sw.get("underlying")
    if not underlying_type or underlying is None:
        raise ValueError("Swaption underlying_type and underlying are required")
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build curve
    curve_id = sw_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)
    
    if underlying_type == "OisSwap":
        fixed_leg = underlying["fixed_leg"]
        fixed_sch = fixed_leg["schedule"]
        fixed_schedule = ql.Schedule(
            parse_date(fixed_sch["effective_date"]),
            parse_date(fixed_sch["termination_date"]),
            ql.Period(get_frequency(fixed_sch["frequency"])),
            get_calendar(fixed_sch.get("calendar", "TARGET")),
            get_convention(fixed_sch.get("convention", "ModifiedFollowing")),
            get_convention(fixed_sch.get("termination_date_convention", "ModifiedFollowing")),
            get_date_generation(fixed_sch.get("date_generation_rule", "Forward")),
            False
        )

        on_leg = underlying["overnight_leg"]
        on_sch = on_leg["schedule"]
        on_schedule = ql.Schedule(
            parse_date(on_sch["effective_date"]),
            parse_date(on_sch["termination_date"]),
            ql.Period(get_frequency(on_sch["frequency"])),
            get_calendar(on_sch.get("calendar", "TARGET")),
            get_convention(on_sch.get("convention", "ModifiedFollowing")),
            get_convention(on_sch.get("termination_date_convention", "ModifiedFollowing")),
            get_date_generation(on_sch.get("date_generation_rule", "Forward")),
            False
        )

        on_index = resolve_index_from_id(on_leg["index"]["id"], request, curve)
        swap_type = ql.OvernightIndexedSwap.Payer if underlying["swap_type"] == "Payer" else ql.OvernightIndexedSwap.Receiver
        payment_calendar = get_calendar(on_leg.get("payment_calendar", on_sch.get("calendar", "TARGET")))
        payment_lag = on_leg.get("payment_lag", 0)
        averaging = get_rate_averaging(on_leg.get("averaging_method", "Compound"))
        lookback = on_leg.get("lookback_days", -1)
        if lookback < 0:
            if hasattr(ql, "NullNatural"):
                lookback = ql.NullNatural()
            elif hasattr(ql, "NullInteger"):
                lookback = ql.NullInteger()
            else:
                lookback = 0
        lockout = on_leg.get("lockout_days", 0)
        apply_shift = on_leg.get("apply_observation_shift", False)
        telescopic = on_leg.get("telescopic_value_dates", False)

        try:
            swap = ql.OvernightIndexedSwap(
                swap_type,
                fixed_leg["notional"],
                fixed_schedule,
                fixed_leg["rate"],
                get_day_counter(fixed_leg.get("day_counter", "Actual360")),
                on_schedule,
                on_index,
                on_leg.get("spread", 0.0),
                payment_lag,
                get_convention(on_leg.get("payment_convention", "Following")),
                payment_calendar,
                telescopic,
                averaging,
                lookback,
                lockout,
                apply_shift
            )
        except Exception:
            # Python QuantLib bindings might not expose the dual-schedule ctor
            swap = ql.OvernightIndexedSwap(
                swap_type,
                fixed_leg["notional"],
                fixed_schedule,
                fixed_leg["rate"],
                get_day_counter(fixed_leg.get("day_counter", "Actual360")),
                on_index,
                on_leg.get("spread", 0.0),
                payment_lag,
                get_convention(on_leg.get("payment_convention", "Following")),
                payment_calendar,
                telescopic,
                averaging,
                lookback,
                lockout,
                apply_shift
            )
    else:
        fixed_leg = underlying["fixed_leg"]
        fixed_sch = fixed_leg["schedule"]
        fixed_schedule = ql.Schedule(
            parse_date(fixed_sch["effective_date"]),
            parse_date(fixed_sch["termination_date"]),
            ql.Period(get_frequency(fixed_sch["frequency"])),
            get_calendar(fixed_sch.get("calendar", "TARGET")),
            get_convention(fixed_sch.get("convention", "ModifiedFollowing")),
            get_convention(fixed_sch.get("termination_date_convention", "ModifiedFollowing")),
            get_date_generation(fixed_sch.get("date_generation_rule", "Forward")),
            False
        )

        float_leg = underlying["floating_leg"]
        float_sch = float_leg["schedule"]
        float_schedule = ql.Schedule(
            parse_date(float_sch["effective_date"]),
            parse_date(float_sch["termination_date"]),
            ql.Period(get_frequency(float_sch["frequency"])),
            get_calendar(float_sch.get("calendar", "TARGET")),
            get_convention(float_sch.get("convention", "ModifiedFollowing")),
            get_convention(float_sch.get("termination_date_convention", "ModifiedFollowing")),
            get_date_generation(float_sch.get("date_generation_rule", "Forward")),
            False
        )

        index = resolve_index_from_id(float_leg["index"]["id"], request, curve)
        swap_type = ql.VanillaSwap.Payer if underlying["swap_type"] == "Payer" else ql.VanillaSwap.Receiver

        swap = ql.VanillaSwap(
            swap_type,
            fixed_leg["notional"],
            fixed_schedule,
            fixed_leg["rate"],
            get_day_counter(fixed_leg.get("day_counter", "Thirty360")),
            float_schedule,
            index,
            float_leg.get("spread", 0.0),
            get_day_counter(float_leg.get("day_counter", "Actual360"))
        )
    
    exercise = ql.EuropeanExercise(parse_date(sw["exercise_date"]))
    settlement_type = sw.get("settlement_type", "Physical")
    settlement_method = sw.get("settlement_method", "PhysicalOTC")
    ql_settlement_type = ql.Settlement.Cash if settlement_type == "Cash" else ql.Settlement.Physical
    ql_settlement_method = get_settlement_method(settlement_method)
    swaption = ql.Swaption(swap, exercise, ql_settlement_type, ql_settlement_method)
    
    # Get volatility
    vol = 0.20
    vol_type = ql.ShiftedLognormal
    displacement = 0.0
    vol_handle = None

    quote_values = {}
    quote_types = {}
    for q in pricing.get("quotes", []):
        quote_values[q.get("id")] = q.get("value")
        quote_types[q.get("id")] = q.get("quote_type")

    for v in pricing.get("vol_surfaces", []):
        if v.get("id") == sw_data.get("volatility"):
            payload = v.get("payload", {})
            if v.get("payload_type") != "SwaptionVolSpec":
                break

            inner_type = payload.get("payload_type", "SwaptionVolConstantSpec")
            inner = payload.get("payload", {})

            if inner_type == "SwaptionVolConstantSpec":
                base = inner.get("base", {})
                vol = base.get("constant_vol", vol)
                vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
                displacement = base.get("displacement", 0.0)
                break

            if inner_type == "SwaptionVolAtmMatrixSpec":
                base = inner.get("base", {})
                vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
                displacement = base.get("displacement", 0.0)
                expiries = inner.get("expiries", [])
                tenors = inner.get("tenors", [])
                vols = inner.get("vols", {})
                n_rows = vols.get("n_rows", 0)
                n_cols = vols.get("n_cols", 0)
                values = vols.get("values", [])
                quote_ids = vols.get("quote_ids", [])

                def to_period(p):
                    num, unit = _period_n_unit(p, "tenor", 0, "Months")
                    return ql.Period(num, get_time_unit(unit))

                ql_expiries = [to_period(p) for p in expiries]
                ql_tenors = [to_period(p) for p in tenors]

                matrix = ql.Matrix(n_rows, n_cols)
                for i in range(n_rows):
                    for j in range(n_cols):
                        idx = i * n_cols + j
                        if idx < len(quote_ids) and quote_ids[idx]:
                            qid = quote_ids[idx]
                            if quote_types.get(qid) != "Volatility":
                                raise ValueError("Quote type mismatch for vol quote")
                            vval = quote_values.get(qid)
                        else:
                            vval = values[idx]
                        matrix[i][j] = vval

                # Python bindings are stricter for overloaded constructors; use
                # the stable calendar-based constructor and default interpolation settings.
                vol_matrix = ql.SwaptionVolatilityMatrix(
                    ql.TARGET(), ql.ModifiedFollowing,
                    ql_expiries, ql_tenors, matrix,
                    ql.Actual365Fixed()
                )
                vol_handle = ql.SwaptionVolatilityStructureHandle(vol_matrix)
                break

            # Smile cube and SABR are not yet supported in the Python reference pricer
            base = inner.get("base", {})
            vol = base.get("constant_vol", vol)
            vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
            displacement = base.get("displacement", 0.0)
            break

    if "vol_surfaces" not in pricing:
        for v in pricing.get("volatilities", []):
            if v["id"] == sw_data.get("volatility"):
                vol = v.get("constant_vol", 0.20)
                break

    if vol_handle is None:
        if displacement and displacement != 0.0:
            vol_handle = ql.SwaptionVolatilityStructureHandle(
                ql.ConstantSwaptionVolatility(
                    eval_date, ql.TARGET(), ql.ModifiedFollowing, vol,
                    ql.Actual365Fixed(), vol_type, displacement
                )
            )
        else:
            vol_handle = ql.SwaptionVolatilityStructureHandle(
                ql.ConstantSwaptionVolatility(
                    eval_date, ql.TARGET(), ql.ModifiedFollowing, vol,
                    ql.Actual365Fixed(), vol_type
                )
            )

    model_type = "Black"
    for m in pricing.get("models", []):
        if m.get("id") == sw_data.get("model"):
            model_type = m.get("payload", {}).get("model_type", "Black")
            break

    if model_type == "Bachelier":
        swaption.setPricingEngine(ql.BachelierSwaptionEngine(curve, vol_handle))
    else:
        swaption.setPricingEngine(ql.BlackSwaptionEngine(curve, vol_handle))
    
    return swaption.NPV()


def price_cds_ql(request: dict) -> float:
    """Price CDS using QuantLib."""
    pricing = _reference_pricing_view(request)
    cds_data = request["cds_list"][0]
    cds = cds_data["cds"]
    quote_values = {q["id"]: q.get("value", 0.0) for q in pricing.get("quotes", []) if "id" in q}
    quote_types = {q["id"]: q.get("quote_type", "Curve") for q in pricing.get("quotes", []) if "id" in q}

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Build discount curve for pricing
    curve_id = cds_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)

    # Build schedule
    sch = cds["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "Following")),
        get_convention(sch.get("termination_date_convention", "Unadjusted")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        False
    )

    protection = ql.Protection.Buyer if cds["side"] == "Buyer" else ql.Protection.Seller

    settles_accrual = cds.get("settles_accrual", True)
    pays_at_default = cds.get("pays_at_default_time", True)
    rebates_accrual = cds.get("rebates_accrual", True)
    last_period_dc = get_day_counter(cds.get("last_period_day_counter", "Actual360"))
    cash_settlement_days = cds.get("cash_settlement_days", 3)

    protection_start = parse_date(cds["protection_start"]) if cds.get("protection_start") else ql.Date()
    upfront_date = parse_date(cds["upfront_date"]) if cds.get("upfront_date") else ql.Date()
    trade_date = parse_date(cds["trade_date"]) if cds.get("trade_date") else ql.Date()

    upfront = cds.get("upfront", 0.0)
    running_coupon = cds.get("running_coupon", 0.0)
    if upfront != 0.0 or cds.get("upfront_date"):
        try:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                upfront,
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360")),
                settles_accrual,
                pays_at_default,
                protection_start,
                upfront_date,
                None,
                last_period_dc,
                rebates_accrual,
                trade_date,
                cash_settlement_days
            )
        except Exception:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                upfront,
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360"))
            )
    else:
        try:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360")),
                settles_accrual,
                pays_at_default,
                protection_start,
                None,
                last_period_dc,
                rebates_accrual,
                trade_date,
                cash_settlement_days
            )
        except Exception:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360"))
            )

    # Credit curve
    credit_curve_id = cds_data["credit_curve_id"]
    credit = next((c for c in pricing.get("credit_curves", []) if c["id"] == credit_curve_id), None)
    if credit is None:
        raise ValueError(f"credit_curve_id not found: {credit_curve_id}")

    recovery = credit.get("recovery_rate", 0.4)
    hazard = credit.get("flat_hazard_rate", 0.0)
    quotes = credit.get("quotes", [])

    # Use trade discount curve for credit helpers
    credit_discount_curve = curve

    if quotes:
        helper_conv = credit.get("helper_conventions", {})
        settlement_days = helper_conv.get("settlement_days", 0)
        calendar = get_calendar(credit.get("calendar", "TARGET"))
        freq = get_frequency(helper_conv.get("frequency", "Quarterly"))
        bdc = get_convention(helper_conv.get("business_day_convention", "Following"))
        rule = get_date_generation(helper_conv.get("date_generation_rule", "TwentiethIMM"))
        curve_dc = get_day_counter(credit.get("day_counter", "Actual365Fixed"))
        last_period_dc = get_day_counter(helper_conv.get("last_period_day_counter", "Actual365Fixed"))
        settles_accrual = helper_conv.get("settles_accrual", True)
        pays_at_default = helper_conv.get("pays_at_default_time", True)
        rebates_accrual = helper_conv.get("rebates_accrual", True)
        helper_model = get_cds_helper_model(helper_conv.get("helper_model", "MidPoint"))

        helpers = []
        for q in quotes:
            q_n, q_u = _period_n_unit(q, "tenor", 0, "Days")
            tenor = ql.Period(q_n, get_time_unit(q_u))
            quote_type = q.get("quote_type", "ParSpread")
            quote_id = q.get("quote_id")
            if quote_id:
                if quote_types.get(quote_id, "Curve") != "Credit":
                    raise ValueError(f"Quote id '{quote_id}' has wrong type for credit")
                quote_value = quote_values.get(quote_id, 0.0)
            else:
                quote_value = None

            if quote_type == "Upfront":
                upfront_value = quote_value if quote_value is not None else q.get("quoted_upfront", 0.0)
                upfront_quote = ql.QuoteHandle(ql.SimpleQuote(upfront_value))
                running = q.get("running_coupon", 0.0)
                helper = ql.UpfrontCdsHelper(
                    upfront_quote,
                    running,
                    tenor,
                    settlement_days,
                    calendar,
                    freq,
                    bdc,
                    rule,
                    curve_dc,
                    recovery,
                    credit_discount_curve,
                    settles_accrual,
                    pays_at_default,
                    ql.Date(),
                    last_period_dc,
                    rebates_accrual,
                    helper_model
                )
            else:
                spread_value = quote_value if quote_value is not None else q.get("quoted_par_spread", 0.0)
                spread_quote = ql.QuoteHandle(ql.SimpleQuote(spread_value))
                helper = ql.SpreadCdsHelper(
                    spread_quote,
                    tenor,
                    settlement_days,
                    calendar,
                    freq,
                    bdc,
                    rule,
                    curve_dc,
                    recovery,
                    credit_discount_curve,
                    settles_accrual,
                    pays_at_default,
                    ql.Date(),
                    last_period_dc,
                    rebates_accrual,
                    helper_model
                )
            helpers.append(helper)

        interp = get_interpolator(credit.get("curve_interpolator", "LogLinear"))
        if hasattr(ql, "PiecewiseDefaultCurve"):
            try:
                default_curve = ql.PiecewiseDefaultCurve(
                    ql.SurvivalProbability, helpers, curve_dc, interp
                )
            except Exception:
                default_curve = ql.PiecewiseDefaultCurve(
                    ql.SurvivalProbability, helpers, curve_dc
                )
        else:
            # Fallback for QuantLib Python builds without PiecewiseDefaultCurve
            default_curve = ql.PiecewiseFlatHazardRate(
                eval_date, helpers, curve_dc
            )
    else:
        hazard = hazard if hazard > 0.0 else 0.01
        default_curve = ql.FlatHazardRate(
            eval_date,
            ql.QuoteHandle(ql.SimpleQuote(hazard)),
            get_day_counter(credit.get("day_counter", "Actual365Fixed"))
        )

    # Model/engine
    model_id = cds_data["model"]
    model = next((m for m in pricing.get("models", []) if m["id"] == model_id), None)
    if model is None:
        raise ValueError(f"model not found: {model_id}")

    cds_model = model.get("payload", {})
    engine_type = get_cds_engine_type(cds_model.get("engine_type", "MidPoint"))
    if engine_type == "ISDA":
        engine = ql.IsdaCdsEngine(
            ql.DefaultProbabilityTermStructureHandle(default_curve),
            recovery,
            curve,
            cds_model.get("include_settlement_date_flows"),
            get_isda_numerical_fix(cds_model.get("isda_numerical_fix", "Taylor")),
            get_isda_accrual_bias(cds_model.get("isda_accrual_bias", "HalfDayBias")),
            get_isda_forwards_in_coupon_period(cds_model.get("isda_forwards_in_coupon_period", "Piecewise"))
        )
    else:
        engine = ql.MidPointCdsEngine(
            ql.DefaultProbabilityTermStructureHandle(default_curve),
            recovery,
            curve
        )

    ql_cds.setPricingEngine(engine)

    return ql_cds.NPV()


def _make_multicurve_exogenous_request() -> dict:
    """
    Build a 2-curve request:
      1) EUR_OIS: OIS discount curve built from OIS helpers
      2) EUR_6M: 6M forwarding curve using swap helper discounting off EUR_OIS (exogenous discount)
    """
    return json.loads(r"""
{
    "pricing": {
        "as_of_date": "2026-01-15",
        "rates": {
            "indices": [
                {
                    "id": "EUR_6M",
                    "name": "Euribor",
                    "index_type": "Ibor",
                    "fixing_days": 2,
                    "calendar": "TARGET",
                    "business_day_convention": "ModifiedFollowing",
                    "day_counter": "Actual360",
                    "end_of_month": false,
                    "currency": "EUR",
                    "tenor": {
                        "n": 6,
                        "unit": "Months"
                    }
                },
                {
                    "id": "EUR_ESTR",
                    "name": "ESTR",
                    "index_type": "Overnight",
                    "fixing_days": 0,
                    "calendar": "TARGET",
                    "business_day_convention": "Following",
                    "day_counter": "Actual360",
                    "currency": "EUR",
                    "tenor": {
                        "n": 0,
                        "unit": "Days"
                    }
                }
            ],
            "curves": [
                {
                    "id": "EUR_OIS",
                    "day_counter": "Actual360",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.03,
                                "overnight_index": {
                                    "id": "EUR_ESTR"
                                },
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "ModifiedFollowing",
                                "fixed_leg_day_counter": "Actual360",
                                "tenor": {
                                    "n": 1,
                                    "unit": "Years"
                                }
                            }
                        },
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.029,
                                "overnight_index": {
                                    "id": "EUR_ESTR"
                                },
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "ModifiedFollowing",
                                "fixed_leg_day_counter": "Actual360",
                                "tenor": {
                                    "n": 5,
                                    "unit": "Years"
                                }
                            }
                        },
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.028,
                                "overnight_index": {
                                    "id": "EUR_ESTR"
                                },
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "ModifiedFollowing",
                                "fixed_leg_day_counter": "Actual360",
                                "tenor": {
                                    "n": 10,
                                    "unit": "Years"
                                }
                            }
                        }
                    ]
                },
                {
                    "id": "EUR_6M",
                    "day_counter": "Actual360",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "DepositHelper",
                            "point": {
                                "rate": 0.032,
                                "fixing_days": 2,
                                "calendar": "TARGET",
                                "business_day_convention": "ModifiedFollowing",
                                "day_counter": "Actual360",
                                "tenor": {
                                    "n": 6,
                                    "unit": "Months"
                                }
                            }
                        },
                        {
                            "point_type": "SwapHelper",
                            "point": {
                                "rate": 0.031,
                                "calendar": "TARGET",
                                "sw_fixed_leg_frequency": "Annual",
                                "sw_fixed_leg_convention": "ModifiedFollowing",
                                "sw_fixed_leg_day_counter": "Thirty360",
                                "float_index": {
                                    "id": "EUR_6M"
                                },
                                "spread": 0.0,
                                "fwd_start_days": 0,
                                "deps": {
                                    "discount_curve": {
                                        "id": "EUR_OIS"
                                    }
                                },
                                "tenor": {
                                    "n": 5,
                                    "unit": "Years"
                                }
                            }
                        }
                    ]
                }
            ]
        }
    },
    "queries": [
        {
            "curve_id": "EUR_OIS",
            "measures": [
                "DF",
                "ZERO",
                "FWD"
            ],
            "grid": {
                "grid_type": "TenorGrid",
                "grid": {
                    "tenors": [
                        {
                            "n": 1,
                            "unit": "Days"
                        },
                        {
                            "n": 1,
                            "unit": "Weeks"
                        },
                        {
                            "n": 1,
                            "unit": "Months"
                        },
                        {
                            "n": 3,
                            "unit": "Months"
                        },
                        {
                            "n": 6,
                            "unit": "Months"
                        },
                        {
                            "n": 1,
                            "unit": "Years"
                        },
                        {
                            "n": 2,
                            "unit": "Years"
                        },
                        {
                            "n": 5,
                            "unit": "Years"
                        },
                        {
                            "n": 10,
                            "unit": "Years"
                        }
                    ],
                    "calendar": "TARGET",
                    "business_day_convention": "Following"
                }
            },
            "zero": {
                "use_curve_day_counter": true,
                "compounding": "Continuous",
                "frequency": "Annual"
            },
            "fwd": {
                "use_curve_day_counter": true,
                "compounding": "Simple",
                "frequency": "Annual",
                "forward_type": "Period",
                "use_grid_calendar_for_advance": true,
                "tenor": {
                    "n": 6,
                    "unit": "Months"
                }
            }
        },
        {
            "curve_id": "EUR_6M",
            "measures": [
                "DF",
                "ZERO",
                "FWD"
            ],
            "grid": {
                "grid_type": "TenorGrid",
                "grid": {
                    "tenors": [
                        {
                            "n": 1,
                            "unit": "Days"
                        },
                        {
                            "n": 1,
                            "unit": "Weeks"
                        },
                        {
                            "n": 1,
                            "unit": "Months"
                        },
                        {
                            "n": 3,
                            "unit": "Months"
                        },
                        {
                            "n": 6,
                            "unit": "Months"
                        },
                        {
                            "n": 1,
                            "unit": "Years"
                        },
                        {
                            "n": 2,
                            "unit": "Years"
                        },
                        {
                            "n": 5,
                            "unit": "Years"
                        },
                        {
                            "n": 10,
                            "unit": "Years"
                        }
                    ],
                    "calendar": "TARGET",
                    "business_day_convention": "Following"
                }
            },
            "zero": {
                "use_curve_day_counter": true,
                "compounding": "Continuous",
                "frequency": "Annual"
            },
            "fwd": {
                "use_curve_day_counter": true,
                "compounding": "Simple",
                "frequency": "Annual",
                "forward_type": "Period",
                "use_grid_calendar_for_advance": true,
                "tenor": {
                    "n": 6,
                    "unit": "Months"
                }
            }
        }
    ]
}
""")


# =============================================================================
# Response NPV extraction (API side of the parity comparison)
# =============================================================================

def api_npv(response: dict, list_key: str) -> float:
    """Pull the single-instrument NPV out of a pricing response.

    Mirrors the per-product extraction the legacy monolith did inline; every
    single-product endpoint returns its instrument under one list key
    (bonds/swaps/fras/cap_floors/swaptions/cds_list) with an "npv" field.
    """
    return response[list_key][0]["npv"]
