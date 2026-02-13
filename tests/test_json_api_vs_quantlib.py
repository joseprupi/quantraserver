#!/usr/bin/env python3
"""
Quantra JSON API vs QuantLib Comparison

Loads JSON requests from examples/data folder, calls the JSON API,
then builds equivalent QuantLib objects and compares the NPVs.

Usage:
    python3 tests/test_json_files_vs_quantlib.py --url http://localhost:8080 --data-dir examples/data
"""

import json
import argparse
import requests
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
    ENDPOINTS = {
        "fixed_rate_bond": "price-fixed-rate-bond",
        "floating_rate_bond": "price-floating-rate-bond",
        "vanilla_swap": "price-vanilla-swap",
        "fra": "price-fra",
        "cap_floor": "price-cap-floor",
        "swaption": "price-swaption",
        "cds": "price-cds",
        "bootstrap_curves": "bootstrap-curves",
    }
    
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
        r = self.session.post(f"{self.base_url}/{endpoint}", json=request)
        if r.status_code != 200:
            raise Exception(f"API error ({r.status_code}): {r.text[:200]}")
        return r.json()


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
    period = ql.Period(
        idx_def.get("tenor_number", 6),
        get_time_unit(idx_def.get("tenor_time_unit", "Months")),
    )
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
    pricing = request_data.get("pricing", request_data)
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
                tenor_num = point.get("tenor_number", 0)
                tenor_unit = get_time_unit(point.get("tenor_time_unit", "Days"))
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
            tenor_num = point["tenor_number"]
            tenor_str = point["tenor_time_unit"]
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
            tenor_num = point["tenor_number"]
            tenor_unit = ql.Years if point["tenor_time_unit"] == "Years" else ql.Months
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
            tenor_num = point["tenor_number"]
            tenor_unit = get_time_unit(point["tenor_time_unit"])
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
                ql.QuoteHandle(ql.SimpleQuote(resolve_point_rate(point))),  # Clean price
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
    pricing = request["pricing"]
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
    pricing = request["pricing"]
    bond_data = request["bonds"][0]
    bond = bond_data["floating_rate_bond"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build discount curve
    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    discount_curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Build forecasting curve (may be different)
    forecast_id = bond_data.get("forecasting_curve", curve_id)
    forecast_json = next((c for c in pricing["curves"] if c["id"] == forecast_id), curve_json)
    forecast_curve = build_curve_from_json(forecast_json, eval_date, request)
    
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
    period_months = idx_def.get("tenor_number", 6) if idx_def else 6
    
    # Create index with forecasting curve
    if period_months == 3:
        index = ql.Euribor3M(forecast_curve)
    elif period_months == 6:
        index = ql.Euribor6M(forecast_curve)
    else:
        index = ql.Euribor6M(forecast_curve)
    
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
    pricing = request["pricing"]
    swap_data = request["swaps"][0]
    swap = swap_data["vanilla_swap"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build curve
    curve_id = swap_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)
    
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
    
    index = ql.Euribor6M(curve)
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


def price_fra_ql(request: dict) -> float:
    """Price FRA using QuantLib."""
    pricing = request["pricing"]
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
        period_months = idx_def.get("tenor_number", 3) if idx_def else 3
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
    pricing = request["pricing"]
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
        period_months = idx_def.get("tenor_number", 3) if idx_def else 3
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
    pricing = request["pricing"]
    sw_data = request["swaptions"][0]
    sw = sw_data["swaption"]
    underlying_type = sw.get("underlying_type")
    underlying = sw.get("underlying")
    if not underlying_type or underlying is None:
        underlying_type = "VanillaSwap"
        underlying = sw.get("underlying_swap")
    
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
    for v in pricing.get("vol_surfaces", []):
        if v.get("id") == sw_data.get("volatility"):
            payload = v.get("payload", {})
            base = payload.get("base", {})
            vol = base.get("constant_vol", vol)
            vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
            displacement = base.get("displacement", 0.0)
            break

    if "vol_surfaces" not in pricing:
        for v in pricing.get("volatilities", []):
            if v["id"] == sw_data.get("volatility"):
                vol = v.get("constant_vol", 0.20)
                break

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
    pricing = request["pricing"]
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
            tenor = ql.Period(q["tenor_number"], get_time_unit(q["tenor_time_unit"]))
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


# =============================================================================
# Bootstrap Curves Test
# =============================================================================

def test_bootstrap_curves(client: ApiClient, data_dir: Path) -> dict:
    """Test bootstrap-curves endpoint against QuantLib."""
    filepath = data_dir / "bootstrap_curves_tenor_grid.json"
    result = {
        "product": "bootstrap_curves",
        "file": str(filepath),
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }
    
    if not filepath.exists():
        result["error"] = "File not found"
        return result
    
    try:
        request = load_json(filepath)
        
        # Call API
        response = client.session.post(
            f"{client.base_url}/bootstrap-curves", 
            json=request
        ).json()
        
        # Build QuantLib curve from request
        as_of = parse_date(request["as_of_date"])
        ql.Settings.instance().evaluationDate = as_of
        
        curve_spec = request["curves"][0]
        ts = curve_spec["curve"]
        quote_values = {}
        quote_types = {}
        for q in request.get("quotes", []):
            if "id" in q:
                quote_values[q["id"]] = q.get("value", 0.0)
                quote_types[q["id"]] = q.get("quote_type", "Curve")

        def resolve_bootstrap_rate(point: dict) -> float:
            quote_id = point.get("quote_id")
            if quote_id:
                if quote_types.get(quote_id, "Curve") != "Curve":
                    raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
                return quote_values.get(quote_id, 0.0)
            return point["rate"]
        
        # Build helpers
        helpers = []
        for point_wrapper in ts["points"]:
            point_type = point_wrapper["point_type"]
            point = point_wrapper["point"]
            
            if point_type == "DepositHelper":
                tenor = ql.Period(point["tenor_number"], 
                                  get_time_unit(point["tenor_time_unit"]))
                helpers.append(ql.DepositRateHelper(
                    resolve_bootstrap_rate(point), tenor, point.get("fixing_days", 2),
                    get_calendar(point["calendar"]),
                    get_convention(point["business_day_convention"]),
                    True, get_day_counter(point["day_counter"])
                ))
            elif point_type == "SwapHelper":
                tenor = ql.Period(point["tenor_number"],
                                  get_time_unit(point["tenor_time_unit"]))
                # New schema: float_index is an IndexRef with just an id
                float_idx = point.get("float_index", {})
                idx_id = float_idx.get("id", "EUR_6M") if isinstance(float_idx, dict) else "EUR_6M"
                # Resolve index from indices definitions in the request
                index = resolve_index_from_id(idx_id, request)
                fwd_start = ql.Period(point.get("fwd_start_days", 0), ql.Days)
                helpers.append(ql.SwapRateHelper(
                    resolve_bootstrap_rate(point), tenor,
                    get_calendar(point["calendar"]),
                    get_frequency(point["sw_fixed_leg_frequency"]),
                    get_convention(point["sw_fixed_leg_convention"]),
                    get_day_counter(point["sw_fixed_leg_day_counter"]),
                    index,
                    ql.QuoteHandle(),  # spread
                    fwd_start
                ))
        
        # Bootstrap curve
        ref_date = parse_date(ts["reference_date"])
        ql_curve = ql.PiecewiseLogLinearDiscount(ref_date, helpers, 
                                                  get_day_counter(ts["day_counter"]))
        ql_curve.enableExtrapolation()
        
        # Compare discount factors
        api_result = response["results"][0]
        if "error" in api_result and api_result["error"]:
            result["error"] = api_result["error"].get("error_message", "Unknown error")
            return result
        
        # Find DF series (measure can be 0 for DF, "DF" string, or missing which defaults to DF)
        df_values = None
        for series in api_result.get("series", []):
            measure = series.get("measure")
            # If measure is missing, it defaults to 0 (DF) in FlatBuffers
            # Handle: missing (None->DF), integer 0 (DF), or string "DF"
            if measure is None or measure == "DF" or measure == 0:
                df_values = series.get("values")
                break
        
        if not df_values:
            result["error"] = "No DF series in response"
            return result
        
        grid_dates = api_result["grid_dates"]
        max_diff = 0.0
        
        for i, date_str in enumerate(grid_dates):
            ql_date = parse_date(date_str)
            q_df = df_values[i]
            ql_df = ql_curve.discount(ql_date)
            diff = abs(ql_df - q_df)
            max_diff = max(max_diff, diff)
        
        result["passed"] = max_diff < 1e-6
        result["quantra_npv"] = df_values[0] if df_values else 0
        result["quantlib_npv"] = ql_curve.discount(parse_date(grid_dates[0])) if grid_dates else 0
        result["diff"] = max_diff
        
    except Exception as e:
        import traceback
        result["error"] = f"{str(e)}\n{traceback.format_exc()}"
    
    return result


def _make_multicurve_exogenous_request() -> dict:
    """
    Build a 2-curve request:
      1) EUR_OIS: OIS discount curve built from OIS helpers
      2) EUR_6M: 6M forwarding curve using swap helper discounting off EUR_OIS (exogenous discount)
    """
    return {
        "as_of_date": "2026-01-15",
        "indices": [
            {
                "id": "EUR_6M",
                "name": "Euribor",
                "index_type": "Ibor",
                "tenor_number": 6,
                "tenor_time_unit": "Months",
                "fixing_days": 2,
                "calendar": "TARGET",
                "business_day_convention": "ModifiedFollowing",
                "day_counter": "Actual360",
                "end_of_month": False,
                "currency": "EUR"
            },
            {
                "id": "EUR_ESTR",
                "name": "ESTR",
                "index_type": "Overnight",
                "tenor_number": 0,
                "tenor_time_unit": "Days",
                "fixing_days": 0,
                "calendar": "TARGET",
                "business_day_convention": "Following",
                "day_counter": "Actual360",
                "currency": "EUR"
            }
        ],
        "curves": [
            {
                "curve": {
                    "id": "EUR_OIS",
                    "day_counter": "Actual360",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.0300,
                                "tenor_number": 1,
                                "tenor_time_unit": "Years",
                                "overnight_index": {"id": "EUR_ESTR"},
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "ModifiedFollowing",
                                "fixed_leg_day_counter": "Actual360"
                            }
                        },
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.0290,
                                "tenor_number": 5,
                                "tenor_time_unit": "Years",
                                "overnight_index": {"id": "EUR_ESTR"},
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "ModifiedFollowing",
                                "fixed_leg_day_counter": "Actual360"
                            }
                        },
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.0280,
                                "tenor_number": 10,
                                "tenor_time_unit": "Years",
                                "overnight_index": {"id": "EUR_ESTR"},
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "ModifiedFollowing",
                                "fixed_leg_day_counter": "Actual360"
                            }
                        }
                    ]
                },
                "query": {
                    "measures": ["DF", "ZERO", "FWD"],
                    "grid": {
                        "grid_type": "TenorGrid",
                        "grid": {
                            "tenors": [
                                {"n": 1, "unit": "Days"},
                                {"n": 1, "unit": "Weeks"},
                                {"n": 1, "unit": "Months"},
                                {"n": 3, "unit": "Months"},
                                {"n": 6, "unit": "Months"},
                                {"n": 1, "unit": "Years"},
                                {"n": 2, "unit": "Years"},
                                {"n": 5, "unit": "Years"},
                                {"n": 10, "unit": "Years"}
                            ],
                            "calendar": "TARGET",
                            "business_day_convention": "Following"
                        }
                    },
                    "zero": {
                        "use_curve_day_counter": True,
                        "compounding": "Continuous",
                        "frequency": "Annual"
                    },
                    "fwd": {
                        "use_curve_day_counter": True,
                        "compounding": "Simple",
                        "frequency": "Annual",
                        "forward_type": "Period",
                        "tenor_number": 6,
                        "tenor_time_unit": "Months",
                        "use_grid_calendar_for_advance": True
                    }
                }
            },
            {
                "curve": {
                    "id": "EUR_6M",
                    "day_counter": "Actual360",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "DepositHelper",
                            "point": {
                                "rate": 0.0320,
                                "tenor_number": 6,
                                "tenor_time_unit": "Months",
                                "fixing_days": 2,
                                "calendar": "TARGET",
                                "business_day_convention": "ModifiedFollowing",
                                "day_counter": "Actual360"
                            }
                        },
                        {
                            "point_type": "SwapHelper",
                            "point": {
                                "rate": 0.0310,
                                "tenor_number": 5,
                                "tenor_time_unit": "Years",
                                "calendar": "TARGET",
                                "sw_fixed_leg_frequency": "Annual",
                                "sw_fixed_leg_convention": "ModifiedFollowing",
                                "sw_fixed_leg_day_counter": "Thirty360",
                                "float_index": {"id": "EUR_6M"},
                                "spread": 0.0,
                                "fwd_start_days": 0,
                                "deps": {
                                    "discount_curve": {"id": "EUR_OIS"}
                                }
                            }
                        }
                    ]
                },
                "query": {
                    "measures": ["DF", "ZERO", "FWD"],
                    "grid": {
                        "grid_type": "TenorGrid",
                        "grid": {
                            "tenors": [
                                {"n": 1, "unit": "Days"},
                                {"n": 1, "unit": "Weeks"},
                                {"n": 1, "unit": "Months"},
                                {"n": 3, "unit": "Months"},
                                {"n": 6, "unit": "Months"},
                                {"n": 1, "unit": "Years"},
                                {"n": 2, "unit": "Years"},
                                {"n": 5, "unit": "Years"},
                                {"n": 10, "unit": "Years"}
                            ],
                            "calendar": "TARGET",
                            "business_day_convention": "Following"
                        }
                    },
                    "zero": {
                        "use_curve_day_counter": True,
                        "compounding": "Continuous",
                        "frequency": "Annual"
                    },
                    "fwd": {
                        "use_curve_day_counter": True,
                        "compounding": "Simple",
                        "frequency": "Annual",
                        "forward_type": "Period",
                        "tenor_number": 6,
                        "tenor_time_unit": "Months",
                        "use_grid_calendar_for_advance": True
                    }
                }
            }
        ]
    }


def test_bootstrap_curves_multicurve_exogenous(client: ApiClient) -> dict:
    """Bootstraps two curves in one call and checks exogenous discount dependency wiring."""
    result = {
        "product": "bootstrap_curves_multicurve_exogenous",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }

    try:
        request = _make_multicurve_exogenous_request()
        resp = client.session.post(f"{client.base_url}/bootstrap-curves", json=request)

        if resp.status_code != 200:
            result["error"] = f"HTTP {resp.status_code}: {resp.text[:300]}"
            return result

        data = resp.json()
        results = data.get("results", [])
        if len(results) != 2:
            result["error"] = f"Expected 2 curve results, got {len(results)}"
            return result

        # Basic shape checks
        ids = {r.get("id") for r in results}
        if ids != {"EUR_OIS", "EUR_6M"}:
            result["error"] = f"Unexpected curve ids: {ids}"
            return result

        by_id = {r["id"]: r for r in results}
        for cid in ["EUR_OIS", "EUR_6M"]:
            r = by_id[cid]
            if r.get("error"):
                result["error"] = f"Curve {cid} failed: {r['error']}"
                return result
            grid_dates = r.get("grid_dates") or []
            series = r.get("series") or []
            if len(grid_dates) == 0 or len(series) < 2:
                result["error"] = f"Curve {cid} missing grid/series"
                return result
            for s in series:
                vals = s.get("values") or []
                if len(vals) != len(grid_dates):
                    result["error"] = f"Curve {cid} series length mismatch"
                    return result

        # A lightweight behavioral check:
        # Discount factors should start near 1.0 and (weakly) decrease with maturity for both curves.
        def _df_series(curve_result: dict):
            for s in (curve_result.get("series") or []):
                m = s.get("measure")
                # FlatBuffers JSON might omit default DF measure=0, so treat missing as DF
                if m in (None, "DF", 0):
                    return s.get("values") or []
            return []

        df_ois = _df_series(by_id["EUR_OIS"])
        df_6m  = _df_series(by_id["EUR_6M"])

        if not df_ois or not df_6m:
            result["error"] = "Missing DF series in one or both curves"
            return result

        if not (0.95 < df_ois[0] <= 1.0001 and 0.95 < df_6m[0] <= 1.0001):
            result["error"] = f"DF at first grid point not near 1.0 (ois={df_ois[0]}, 6m={df_6m[0]})"
            return result

        if any(df_ois[i] < df_ois[i+1] - 1e-10 for i in range(len(df_ois)-1)):
            result["error"] = "OIS DF is not non-increasing"
            return result

        if any(df_6m[i] < df_6m[i+1] - 1e-10 for i in range(len(df_6m)-1)):
            result["error"] = "6M DF is not non-increasing"
            return result

        result["passed"] = True
        # Put something numeric in summary columns
        result["quantra_npv"] = float(df_6m[-1])
        result["quantlib_npv"] = float(df_ois[-1])
        result["diff"] = abs(result["quantra_npv"] - result["quantlib_npv"])
        return result

    except Exception as e:
        result["error"] = str(e)
        return result


def test_bootstrap_curves_missing_dependency_fails(client: ApiClient) -> dict:
    """Requests a curve that references an exogenous discount curve that is not provided; should fail cleanly."""
    result = {
        "product": "bootstrap_curves_missing_dependency",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }

    try:
        request = _make_multicurve_exogenous_request()
        # Drop the discount curve so EUR_6M references a missing EUR_OIS
        request["curves"] = [request["curves"][1]]

        resp = client.session.post(f"{client.base_url}/bootstrap-curves", json=request)

        # Accept either:
        # - HTTP-level failure (4xx) OR
        # - 200 with per-curve error populated
        if resp.status_code != 200:
            result["passed"] = True
            result["quantra_npv"] = 1.0
            result["quantlib_npv"] = 1.0
            result["diff"] = 0.0
            return result

        data = resp.json()
        results = data.get("results", [])
        if len(results) != 1:
            result["error"] = f"Expected 1 curve result, got {len(results)}"
            return result

        err = results[0].get("error")
        if not err:
            result["error"] = "Expected an error for missing dependency but got success"
            return result

        result["passed"] = True
        result["quantra_npv"] = 1.0
        result["quantlib_npv"] = 1.0
        result["diff"] = 0.0
        return result

    except Exception as e:
        result["error"] = str(e)
        return result


# =============================================================================
# Main Test Runner
# =============================================================================

def test_product(client: ApiClient, product: str, json_file: Path, ql_pricer) -> dict:
    """Test a single product: call API and compare with QuantLib."""
    result = {
        "product": product,
        "file": str(json_file),
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }
    
    try:
        request = load_json(json_file)
        
        # Call Quantra API
        response = client.price(product, request)
        
        # Extract NPV from response
        if product == "fixed_rate_bond":
            result["quantra_npv"] = response["bonds"][0]["npv"]
        elif product == "floating_rate_bond":
            result["quantra_npv"] = response["bonds"][0]["npv"]
        elif product == "vanilla_swap":
            result["quantra_npv"] = response["swaps"][0]["npv"]
        elif product == "fra":
            result["quantra_npv"] = response["fras"][0]["npv"]
        elif product == "cap_floor":
            result["quantra_npv"] = response["cap_floors"][0]["npv"]
        elif product == "swaption":
            result["quantra_npv"] = response["swaptions"][0]["npv"]
        elif product == "cds":
            result["quantra_npv"] = response["cds_list"][0]["npv"]
        
        # Price with QuantLib
        result["quantlib_npv"] = ql_pricer(request)
        
        # Compare
        result["diff"] = abs(result["quantra_npv"] - result["quantlib_npv"])
        tolerance = 100 if product in ["cap_floor", "swaption"] else 1.0
        result["passed"] = result["diff"] < tolerance
        
    except Exception as e:
        import traceback
        result["error"] = f"{str(e)}\n{traceback.format_exc()}"
    
    return result


def main():
    parser = argparse.ArgumentParser(description='Compare JSON API vs QuantLib using example files')
    parser.add_argument('--url', default='http://localhost:8080', help='API URL')
    parser.add_argument('--data-dir', default='examples/data', help='Directory with JSON files')
    args = parser.parse_args()
    
    data_dir = Path(args.data_dir)
    
    print("=" * 70)
    print("QUANTRA JSON API vs QUANTLIB - Using Example JSON Files")
    print("=" * 70)
    print(f"API URL:   {args.url}")
    print(f"Data Dir:  {data_dir}")
    
    client = ApiClient(args.url)
    if not client.health():
        print("\n❌ Cannot connect to API server")
        return False
    print("✓ API server healthy\n")
    
    # Products and their QuantLib pricers
    products = [
        ("fixed_rate_bond", "fixed_rate_bond_request.json", price_fixed_rate_bond_ql),
        ("floating_rate_bond", "floating_rate_bond_request.json", price_floating_rate_bond_ql),
        ("vanilla_swap", "vanilla_swap_request.json", price_vanilla_swap_ql),
        ("fra", "fra_request.json", price_fra_ql),
        ("cap_floor", "cap_floor_request.json", price_cap_floor_ql),
        ("swaption", "swaption_request.json", price_swaption_ql),
        ("swaption", "swaption_ois_request.json", price_swaption_ql),
        ("swaption", "swaption_ois_bbg_zerorate_request.json", price_swaption_ql),
        ("cds", "cds_request.json", price_cds_ql),
        ("cds", "cds_complex_request.json", price_cds_ql),
    ]
    
    results = []
    for product, filename, ql_pricer in products:
        filepath = data_dir / filename
        if not filepath.exists():
            print(f"⚠ Skipping {product}: {filepath} not found")
            continue
        
        result = test_product(client, product, filepath, ql_pricer)
        results.append(result)
        
        # Print result
        print(f"\n{'='*70}")
        print(f"{product.upper().replace('_', ' ')}")
        print(f"{'='*70}")
        print(f"  File: {result['file']}")
        
        if result["error"]:
            print(f"  ❌ Error: {result['error']}")
        else:
            status = "✓ PASS" if result["passed"] else "✗ FAIL"
            print(f"  Quantra NPV:  {result['quantra_npv']:>15.6f}")
            print(f"  QuantLib NPV: {result['quantlib_npv']:>15.6f}")
            print(f"  Difference:   {result['diff']:>15.6f}")
            print(f"  Status:       {status}")
    
    # Test Bootstrap Curves separately (different response format)
    bc_result = test_bootstrap_curves(client, data_dir)
    results.append(bc_result)
    print(f"\n{'='*70}")
    print("BOOTSTRAP CURVES")
    print(f"{'='*70}")
    print(f"  File: {bc_result['file']}")
    if bc_result["error"]:
        print(f"  ❌ Error: {bc_result['error']}")
    else:
        status = "✓ PASS" if bc_result["passed"] else "✗ FAIL"
        print(f"  Quantra DF:   {bc_result['quantra_npv']:>15.8f}")
        print(f"  QuantLib DF:  {bc_result['quantlib_npv']:>15.8f}")
        print(f"  Max Diff:     {bc_result['diff']:>15.2e}")
        print(f"  Status:       {status}")
    


    # New: multi-curve + exogenous dependency tests (schema/features beyond the legacy single-curve flow)
    mc_ok = test_bootstrap_curves_multicurve_exogenous(client)
    results.append(mc_ok)
    print(f"\n{'='*70}")
    print("BOOTSTRAP CURVES (MULTI-CURVE + EXOGENOUS DISCOUNT)")
    print(f"{'='*70}")
    if mc_ok["error"]:
        print(f"  ❌ Error: {mc_ok['error']}")
    else:
        status = "✓ PASS" if mc_ok["passed"] else "✗ FAIL"
        print(f"  Status:       {status}")
        print(f"  EUR_6M DF@last grid: {mc_ok['quantra_npv']:>15.8f}")
        print(f"  EUR_OIS DF@last grid:{mc_ok['quantlib_npv']:>15.8f}")
        print(f"  Abs diff:     {mc_ok['diff']:>15.2e}")

    mc_missing = test_bootstrap_curves_missing_dependency_fails(client)
    results.append(mc_missing)
    print(f"\n{'='*70}")
    print("BOOTSTRAP CURVES (MISSING DEPENDENCY SHOULD FAIL)")
    print(f"{'='*70}")
    if mc_missing["error"]:
        print(f"  ❌ Error: {mc_missing['error']}")
    else:
        status = "✓ PASS" if mc_missing["passed"] else "✗ FAIL"
        print(f"  Status:       {status}")
    # Summary
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"{'Product':<20} {'Quantra':>15} {'QuantLib':>15} {'Diff':>12} {'Status'}")
    print("-" * 70)
    
    for r in results:
        if r["error"]:
            print(f"{r['product']:<20} {'ERROR':>15} {'':<15} {'':<12} ✗ FAIL")
        else:
            status = "✓ PASS" if r["passed"] else "✗ FAIL"
            print(f"{r['product']:<20} {r['quantra_npv']:>15.2f} {r['quantlib_npv']:>15.2f} {r['diff']:>12.4f} {status}")
    
    passed = sum(1 for r in results if r["passed"])
    print("-" * 70)
    print(f"TOTAL: {passed}/{len(results)} tests passed")
    
    return passed == len(results)


if __name__ == "__main__":
    import sys
    sys.exit(0 if main() else 1)