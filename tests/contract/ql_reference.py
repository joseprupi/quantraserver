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

import bisect
import json
import argparse
import math
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
        # Mirrors the server's CalendarToQL: QuantLib::UnitedKingdom()
        # (default Settlement market).
        "UnitedKingdom": ql.UnitedKingdom(),
        "UnitedStates": ql.UnitedStates(ql.UnitedStates.NYSE),
        "UnitedStatesNYSE": ql.UnitedStates(ql.UnitedStates.NYSE),
        "UnitedStatesGovernmentBond": ql.UnitedStates(ql.UnitedStates.GovernmentBond),
    }
    return mapping.get(name, ql.TARGET())


def get_date_generation(name: str):
    mapping = {
        "Forward": ql.DateGeneration.Forward,
        "Backward": ql.DateGeneration.Backward,
        "ThirdWednesday": ql.DateGeneration.ThirdWednesday,
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

    def resolve_future_price(point: dict) -> float:
        # Mirrors the server: FuturesRateHelper's quote is the futures PRICE
        # (0-100 IMM scale). futures_price wins over rate; a quote_id resolves
        # to the price directly. rate is sugar via price = 100*(1-rate).
        quote_id = point.get("quote_id")
        if quote_id:
            if quote_types.get(quote_id, "Curve") != "Curve":
                raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
            return quote_values.get(quote_id, 0.0)
        if "futures_price" in point:
            return point["futures_price"]
        return 100.0 * (1.0 - point["rate"])
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
        
        elif point_type == "FutureHelper":
            # QuantLib FuturesRateHelper: quote is the futures PRICE, convexity
            # is its own argument (mirrors src/parsers/term_structure_point_parser.cpp).
            helper = ql.FuturesRateHelper(
                ql.QuoteHandle(ql.SimpleQuote(resolve_future_price(point))),
                parse_date(point["future_start_date"]),
                point.get("future_months", 3),
                get_calendar(point.get("calendar", "TARGET")),
                get_convention(point.get("business_day_convention", "ModifiedFollowing")),
                True,
                get_day_counter(point.get("day_counter", "Actual360")),
                ql.QuoteHandle(ql.SimpleQuote(point.get("convexity_adjustment", 0.0))),
            )

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
        # Mirrors the server's ScheduleParser: schedule->end_of_month()
        # (FlatBuffers schema default false).
        sch.get("end_of_month", False)
    )

    # Build bond. Mirrors the server's FixedRateBondParser, which passes
    # payment_convention, redemption and issue_date straight into the
    # QuantLib::FixedRateBond constructor. The fallbacks are the FlatBuffers
    # wire defaults (payment_convention=Following, redemption=0.0); issue_date
    # is effectively required on the wire (the server dereferences it).
    issue_date = bond.get("issue_date")
    ql_bond = ql.FixedRateBond(
        bond.get("settlement_days", 2),
        bond.get("face_amount", 100.0),
        schedule,
        [bond["rate"]],
        get_day_counter(bond.get("accrual_day_counter", "Thirty360")),
        get_convention(bond.get("payment_convention", "Following")),
        bond.get("redemption", 0.0),
        parse_date(issue_date) if issue_date else ql.Date()
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
        # Mirrors the server's ScheduleParser: schedule->end_of_month()
        # (FlatBuffers schema default false).
        sch.get("end_of_month", False)
    )

    # Get index info
    idx_ref = bond.get("index", {})
    idx_id = idx_ref.get("id", "EUR_6M") if isinstance(idx_ref, dict) else "EUR_6M"

    # Resolve the index exactly as the server does: its IndexRegistryBuilder
    # constructs a generic IborIndex from the request's IndexDef (name, tenor,
    # fixing days, currency, calendar, convention, end-of-month, day count),
    # projecting off the forwarding curve. Falls back to the Euribor family
    # when the request carries no IndexDef (legacy requests).
    idx_def = find_index_def(idx_id, request)
    if idx_def:
        index = build_ibor_index(idx_def, forward_curve)
    else:
        period_months = 6
        if "3M" in idx_id:
            period_months = 3
        if period_months == 3:
            index = ql.Euribor3M(forward_curve)
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
        # Mirrors the server's ScheduleParser: schedule->end_of_month()
        # (FlatBuffers schema default false).
        fixed_sch.get("end_of_month", False)
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
        float_sch.get("end_of_month", False)
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
        # Mirrors the server's ScheduleParser: schedule->end_of_month().
        fs.get("end_of_month", False),
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
        os.get("end_of_month", False),
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
            # Mirrors the server's ScheduleParser: schedule->end_of_month().
            sch.get("end_of_month", False),
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
    """Price FRA using QuantLib (mirrors src/evaluators/fra_evaluator.cpp).

    The server resolves the forwarding and discounting curve ids
    independently, clones the request's IndexDef onto the forwarding curve
    and passes the request's explicit maturity date to
    QuantLib::ForwardRateAgreement — this reference does the same.
    """
    pricing = _reference_pricing_view(request)
    fra_data = request["fras"][0]
    fra = fra_data["fra"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Forwarding curve projects the index; discounting curve discounts the
    # payoff (defaults keep single-curve requests working as before).
    fwd_id = fra_data.get("forwarding_curve", "discount")
    fwd_json = next((c for c in pricing["curves"] if c["id"] == fwd_id), pricing["curves"][0])
    fwd_curve = build_curve_from_json(fwd_json, eval_date, request)
    disc_id = fra_data.get("discounting_curve", fwd_id)
    if disc_id == fwd_id:
        disc_curve = fwd_curve
    else:
        disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id), fwd_json)
        disc_curve = build_curve_from_json(disc_json, eval_date, request)

    # Build the index from its IndexDef (tenor, fixing days, calendar,
    # conventions, day count) exactly like the server's IndexRegistry, and
    # link it to the forwarding curve.
    idx = fra.get("index", {})
    if isinstance(idx, dict) and "id" in idx:
        idx_def = find_index_def(idx["id"], request)
        if idx_def:
            index = build_ibor_index(idx_def, fwd_curve)
        else:
            index = ql.Euribor3M(fwd_curve) if "3M" in idx["id"] else ql.Euribor6M(fwd_curve)
    else:
        # Legacy inline-index shape.
        period_months = idx.get("period_number", 3)
        index = ql.Euribor3M(fwd_curve) if period_months == 3 else ql.Euribor6M(fwd_curve)

    start_date = parse_date(fra["start_date"])
    position = ql.Position.Long if fra["fra_type"] == "Long" else ql.Position.Short

    if fra.get("maturity_date"):
        # Explicit accrual end date, as the server passes it.
        ql_fra = ql.ForwardRateAgreement(
            index, start_date, parse_date(fra["maturity_date"]), position,
            fra["strike"], fra["notional"], disc_curve
        )
    else:
        ql_fra = ql.ForwardRateAgreement(
            index, start_date, position, fra["strike"], fra["notional"], disc_curve
        )

    return ql_fra.NPV()


def price_cap_floor_ql(request: dict) -> float:
    """Price cap/floor using QuantLib.

    Mirrors the server's cap/floor evaluator:
      * the index projects off the trade's forwarding_curve while the engine
        discounts on the trade's discounting_curve,
      * the IborLeg carries the trade-level payment day counter and payment
        adjustment (withPaymentDayCounter / withPaymentAdjustment),
      * the volatility is the referenced OptionletVolSpec rebuilt as a
        ConstantOptionletVolatility with the spec's own reference date,
        calendar, convention, day count, volatility type and displacement,
      * the engine follows the referenced model: Bachelier for normal vols,
        BlackCapFloorEngine for Black and ShiftedBlack (QuantLib reads the
        displacement from the vol structure).
    """
    pricing = _reference_pricing_view(request)
    cf_data = request["cap_floors"][0]
    cf = cf_data["cap_floor"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Discounting and forwarding curves (may be the same id)
    disc_id = cf_data.get("discounting_curve", "discount")
    disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id), pricing["curves"][0])
    disc_curve = build_curve_from_json(disc_json, eval_date, request)
    fwd_id = cf_data.get("forwarding_curve", disc_id)
    if fwd_id == disc_id:
        fwd_curve = disc_curve
    else:
        fwd_json = next((c for c in pricing["curves"] if c["id"] == fwd_id), pricing["curves"][0])
        fwd_curve = build_curve_from_json(fwd_json, eval_date, request)

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

    # Index resolved from its IndexDef, projecting off the forwarding curve
    # (mirrors IndexRegistry::getIborWithCurve on the server).
    index = resolve_index_from_id(cf["index"]["id"], request, fwd_curve)

    # Floating leg mirrors the server's IborLeg: trade-level payment day
    # counter and payment adjustment, index-level fixing days.
    leg = ql.IborLeg(
        [cf["notional"]],
        schedule,
        index,
        get_day_counter(cf.get("day_counter", "Actual360")),
        get_convention(cf.get("business_day_convention", "ModifiedFollowing")),
    )
    cf_type = cf["cap_floor_type"]
    if cf_type == "Cap":
        ql_cf = ql.Cap(leg, [cf["strike"]])
    elif cf_type == "Floor":
        ql_cf = ql.Floor(leg, [cf["strike"]])
    else:
        # The server rejects Collar the same way.
        raise ValueError(f"Unsupported cap/floor type: {cf_type}")

    # Volatility: rebuild the referenced constant optionlet vol spec.
    vol_base = {}
    for v in pricing.get("vol_surfaces", []):
        if v.get("id") == cf_data.get("volatility"):
            vol_base = v.get("payload", {}).get("base", {})
            break
    vol_value = vol_base.get("constant_vol", 0.20)
    displacement = vol_base.get("displacement", 0.0)
    # QuantLib encodes Black lognormal vols under ShiftedLognormal; pure
    # lognormal is displacement == 0 (same mapping as the server).
    vol_type = ql.Normal if vol_base.get("volatility_type") == "Normal" else ql.ShiftedLognormal
    vol_ref = parse_date(vol_base.get("reference_date", pricing["as_of_date"]))
    vol_handle = ql.OptionletVolatilityStructureHandle(
        ql.ConstantOptionletVolatility(
            vol_ref,
            get_calendar(vol_base.get("calendar", "TARGET")),
            get_convention(vol_base.get("business_day_convention", "ModifiedFollowing")),
            vol_value,
            get_day_counter(vol_base.get("day_counter", "Actual365Fixed")),
            vol_type,
            displacement,
        )
    )

    # Engine follows the referenced model (Black / ShiftedBlack / Bachelier).
    model_type = "Black"
    for m in pricing.get("models", []):
        if m.get("id") == cf_data.get("model"):
            model_type = m.get("payload", {}).get("model_type", "Black")
            break
    if model_type == "Bachelier":
        engine = ql.BachelierCapFloorEngine(disc_curve, vol_handle)
    else:
        engine = ql.BlackCapFloorEngine(disc_curve, vol_handle)
    ql_cf.setPricingEngine(engine)

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
    
    # Exercise construction mirrors the server's swaption evaluator
    # (buildSwaptionInstrument): European takes the single exercise_date,
    # Bermudan the exercise_dates list, and American opens the exercise
    # window at the evaluation date — the server builds
    # AmericanExercise(Settings::evaluationDate(), exercise_date).
    exercise_type = sw.get("exercise_type", "European")
    if exercise_type == "Bermudan":
        exercise = ql.BermudanExercise(
            [parse_date(d) for d in sw["exercise_dates"]])
    elif exercise_type == "American":
        exercise = ql.AmericanExercise(eval_date, parse_date(sw["exercise_date"]))
    else:
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

    model_spec = {}
    for m in pricing.get("models", []):
        if m.get("id") == sw_data.get("model"):
            model_spec = m.get("payload", {})
            break
    model_type = model_spec.get("model_type", "Black")

    if model_type == "HullWhiteLattice":
        # Mirrors the server's HullWhiteLattice engine branch: a Hull-White
        # one-factor model on the discount curve with the explicit hw_a /
        # hw_sigma parameters, priced on a TreeSwaptionEngine with
        # lattice_steps time steps (schema defaults: a=0.03, sigma=0.01,
        # steps=50). param_mode=Calibrate would need the server's
        # calibration routine reproduced here, so it is not supported.
        if model_spec.get("param_mode", "Explicit") != "Explicit":
            raise ValueError(
                "Reference pricer only supports HullWhiteLattice with "
                "param_mode=Explicit")
        hw_model = ql.HullWhite(curve,
                                model_spec.get("hw_a", 0.03),
                                model_spec.get("hw_sigma", 0.01))
        swaption.setPricingEngine(
            ql.TreeSwaptionEngine(hw_model, int(model_spec.get("lattice_steps", 50))))
    elif model_type == "Bachelier":
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


# =============================================================================
# Inflation swaps (ZCIIS / YYIIS)
# =============================================================================
#
# These mirror the server's inflation stack field for field:
#   * src/parsers/inflation_curve_parsers.cpp — the InflationIndexSpec /
#     InflationCurveSpec builders (index construction, helper dates, base
#     fixing, PiecewiseZero/YoYInflationCurve<Linear> bootstrap),
#   * src/parsers/zero_coupon_inflation_swap_parser.cpp and
#     year_on_year_inflation_swap_parser.cpp — the trade construction,
#   * src/evaluators/*_inflation_swap_evaluator.cpp — engine selection
#     (DiscountingSwapEngine on the trade's discounting curve; the YoY leg
#     additionally gets a BlackYoYInflationCouponPricer holding the same
#     nominal curve and no volatility surface).


def get_cpi_interpolation(name: str):
    """Mirror the server's CPIInterpolationType mapping (fbs default AsIndex)."""
    mapping = {
        "AsIndex": ql.CPI.AsIndex,
        "Flat": ql.CPI.Flat,
        "Linear": ql.CPI.Linear,
    }
    return mapping.get(name, ql.CPI.AsIndex)


def _inflation_region(currency: str):
    """QuantLib Region for the server's currency->region mapping.

    The Python bindings expose only CustomRegion (not EURegion/USRegion/...),
    but a Region contributes nothing numerical — only the index's name, i.e.
    its fixing-store key — so a CustomRegion carrying the same name as the
    server's region is an exact behavioural match.
    """
    names = {
        "EUR": ("EU", "EU"),
        "USD": ("USA", "US"),
        "GBP": ("UK", "UK"),
        "AUD": ("Australia", "AU"),
        "ZAR": ("South Africa", "ZA"),
    }
    name, code = names.get(currency, ("N/A", "N/A"))
    return ql.CustomRegion(name, code)


def _find_inflation_index_spec(pricing: dict, index_id: str) -> dict:
    for spec in pricing.get("inflation_indices", []):
        if spec.get("id") == index_id:
            return spec
    raise ValueError(f"Inflation index spec not found: {index_id}")


def _find_inflation_curve_spec(pricing: dict, curve_id: str) -> dict:
    for spec in pricing.get("inflation_curves", []):
        if spec.get("id") == curve_id:
            return spec
    raise ValueError(f"Inflation curve spec not found: {curve_id}")


def _wire_period(container: dict, key: str) -> ql.Period:
    n, unit = _period_n_unit(container, key)
    return ql.Period(n, get_time_unit(unit))


def _apply_inflation_fixings(index, spec: dict):
    """Mirror the server: clearFixings(), then add every supplied fixing."""
    index.clearFixings()
    for fixing in spec.get("fixings", []) or []:
        index.addFixing(parse_date(fixing["date"]), fixing["value"])


def _build_inflation_index(spec: dict, zero_handle=None, yoy_handle=None):
    """Inflation index from an InflationIndexSpec (mirrors buildInflationIndex).

    Note the server never passes the spec's `interpolated` flag into the
    QuantLib index constructor (QuantLib 1.41 dropped that parameter), so the
    reference does not either. Ratio-based YoY indices
    (underlying_zero_index_id) are not supported by the reference yet — do
    not add a catalog case that needs one.
    """
    family = spec.get("family_name") or spec["id"]
    ccy_code = spec.get("currency") or "EUR"
    region = _inflation_region(ccy_code)
    ccy = get_currency(ccy_code)
    freq = get_frequency(spec.get("frequency", "Monthly"))
    avail_lag = _wire_period(spec, "availability_lag")
    revised = spec.get("revised", False)
    if spec.get("kind", "ZeroInflation") == "ZeroInflation":
        handle = zero_handle if zero_handle is not None \
            else ql.ZeroInflationTermStructureHandle()
        index = ql.ZeroInflationIndex(
            family, region, revised, freq, avail_lag, ccy, handle)
    else:
        if spec.get("underlying_zero_index_id"):
            raise ValueError(
                "Ratio-based YoY inflation indices are not supported by the "
                "reference yet")
        handle = yoy_handle if yoy_handle is not None \
            else ql.YoYInflationTermStructureHandle()
        index = ql.YoYInflationIndex(
            family, region, revised, freq, avail_lag, ccy, handle)
    _apply_inflation_fixings(index, spec)
    return index


def _inflation_helper_maturity(helper: dict, reference_date, calendar, bdc,
                               label: str):
    """Helper maturity, mirroring resolveHelperDates (tenor or end_date).

    The server also accepts explicit start_date+end_date pairs (a dated
    helper constructor the Python bindings do not expose), so those raise
    here instead of being silently mispriced.
    """
    if helper.get("start_date"):
        raise ValueError(
            f"{label}: explicit start/end dated helpers are not supported by "
            "the reference yet")
    if helper.get("end_date"):
        return parse_date(helper["end_date"])
    return calendar.advance(reference_date, _wire_period(helper, "tenor"), bdc)


def _resolve_inflation_quote(helper: dict, request: dict) -> float:
    """Inline quote_value, or quote_id resolved from pricing.quotes (Curve)."""
    quote_id = helper.get("quote_id")
    if quote_id:
        pricing = request.get("pricing", {})
        for q in pricing.get("quotes", []):
            if q.get("id") == quote_id:
                if q.get("quote_type", "Curve") != "Curve":
                    raise ValueError(
                        f"Quote id '{quote_id}' has wrong type for an "
                        "inflation curve")
                return q.get("value", 0.0)
        raise ValueError(f"Quote id not found: {quote_id}")
    return helper["quote_value"]


def _build_zero_inflation_curve(curve_spec: dict, index_spec: dict,
                                request: dict):
    """PiecewiseZeroInflation curve from an InflationCurveSpec (ZeroInflation).

    Mirrors the server's build: ZCIIS helpers with per-helper calendar /
    convention / day counter / observation lag / CPI interpolation, base date
    = inflationPeriod(reference - availability_lag).start, Linear
    interpolation (the only interpolator the server accepts), no seasonality.
    """
    if curve_spec.get("interpolator", "Linear") != "Linear":
        raise ValueError("Inflation curves only support the Linear interpolator")
    ref = parse_date(curve_spec["reference_date"])
    dc = get_day_counter(curve_spec.get("day_counter", "Actual365Fixed"))
    accuracy = curve_spec.get("bootstrap_accuracy", 1.0e-12)
    freq = get_frequency(index_spec.get("frequency", "Monthly"))
    avail_lag = _wire_period(index_spec, "availability_lag")
    bare_index = _build_inflation_index(index_spec)

    helpers = []
    for wrapper in curve_spec["points"]:
        if wrapper.get("point_type") != "ZeroCouponInflationSwapHelper":
            raise ValueError(
                "Zero inflation curves take only ZeroCouponInflationSwapHelper "
                "points")
        point = wrapper["point"]
        calendar = _calendar_from_enum(point.get("calendar", "TARGET"))
        bdc = get_convention(point.get("payment_convention",
                                       "ModifiedFollowing"))
        maturity = _inflation_helper_maturity(
            point, ref, calendar, bdc, "ZeroCouponInflationSwapHelper")
        helpers.append(ql.ZeroCouponInflationSwapHelper(
            ql.QuoteHandle(ql.SimpleQuote(_resolve_inflation_quote(point,
                                                                   request))),
            _wire_period(point, "swap_observation_lag"),
            maturity,
            calendar,
            bdc,
            get_day_counter(point.get("day_counter", "Actual365Fixed")),
            bare_index,
            get_cpi_interpolation(point.get("observation_interpolation",
                                            "AsIndex"))))

    base_date = ql.inflationPeriod(ref - avail_lag, freq)[0]
    ts = ql.PiecewiseZeroInflation(ref, base_date, freq, dc, helpers, None,
                                   accuracy)
    if curve_spec.get("allow_extrapolation", True):
        ts.enableExtrapolation()
    else:
        ts.disableExtrapolation()
    return ts


def _build_yoy_inflation_curve(curve_spec: dict, index_spec: dict,
                               request: dict, nominal_curve):
    """PiecewiseYoYInflation curve from an InflationCurveSpec (YoYInflation).

    Mirrors the server: YoY swap helpers carrying the nominal curve named by
    each helper's nominal_curve_id, base YoY rate read from the index's own
    stored fixing at inflationPeriod(reference - availability_lag).start.
    `nominal_curve` maps a curve id to its built YieldTermStructureHandle.
    """
    if curve_spec.get("interpolator", "Linear") != "Linear":
        raise ValueError("Inflation curves only support the Linear interpolator")
    ref = parse_date(curve_spec["reference_date"])
    dc = get_day_counter(curve_spec.get("day_counter", "Actual365Fixed"))
    accuracy = curve_spec.get("bootstrap_accuracy", 1.0e-12)
    freq = get_frequency(index_spec.get("frequency", "Monthly"))
    avail_lag = _wire_period(index_spec, "availability_lag")
    bare_index = _build_inflation_index(index_spec)

    helpers = []
    for wrapper in curve_spec["points"]:
        if wrapper.get("point_type") != "YearOnYearInflationSwapHelper":
            raise ValueError(
                "YoY inflation curves take only YearOnYearInflationSwapHelper "
                "points")
        point = wrapper["point"]
        calendar = _calendar_from_enum(point.get("calendar", "TARGET"))
        bdc = get_convention(point.get("payment_convention",
                                       "ModifiedFollowing"))
        maturity = _inflation_helper_maturity(
            point, ref, calendar, bdc, "YearOnYearInflationSwapHelper")
        helpers.append(ql.YearOnYearInflationSwapHelper(
            ql.QuoteHandle(ql.SimpleQuote(_resolve_inflation_quote(point,
                                                                   request))),
            _wire_period(point, "swap_observation_lag"),
            maturity,
            calendar,
            bdc,
            get_day_counter(point.get("day_counter", "Actual365Fixed")),
            bare_index,
            get_cpi_interpolation(point.get("observation_interpolation",
                                            "AsIndex")),
            nominal_curve(point["nominal_curve_id"])))

    base_date = ql.inflationPeriod(ref - avail_lag, freq)[0]
    base_yoy_rate = bare_index.fixing(base_date)
    ts = ql.PiecewiseYoYInflation(ref, base_date, base_yoy_rate, freq, dc,
                                  helpers, None, accuracy)
    if curve_spec.get("allow_extrapolation", True):
        ts.enableExtrapolation()
    else:
        ts.disableExtrapolation()
    return ts


def _nominal_curve_resolver(pricing: dict, eval_date, request: dict):
    """id -> YieldTermStructureHandle over pricing.rates.curves, cached."""
    cache = {}

    def resolve(curve_id: str):
        if curve_id not in cache:
            curve_json = next(
                (c for c in pricing.get("curves", []) if c["id"] == curve_id),
                None)
            if curve_json is None:
                raise ValueError(f"Rates curve not found: {curve_id}")
            cache[curve_id] = build_curve_from_json(curve_json, eval_date,
                                                    request)
        return cache[curve_id]

    return resolve


def _build_wire_schedule(sch: dict) -> ql.Schedule:
    """QuantLib Schedule from a wire Schedule table (mirrors ScheduleParser)."""
    return ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch.get("frequency", "Annual"))),
        _calendar_from_enum(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention",
                               "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        sch.get("end_of_month", False))


def price_zero_coupon_inflation_swap_ql(request: dict) -> float:
    """Price a zero-coupon inflation swap (ZCIIS) using QuantLib."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    trade = swap_data["zero_coupon_inflation_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    nominal_curve = _nominal_curve_resolver(pricing, eval_date, request)
    discount = nominal_curve(swap_data["discounting_curve"])

    curve_spec = _find_inflation_curve_spec(pricing,
                                            swap_data["inflation_curve"])
    if curve_spec.get("kind", "ZeroInflation") != "ZeroInflation":
        raise ValueError("ZCIIS needs a ZeroInflation curve")
    index_spec = _find_inflation_index_spec(pricing, curve_spec["index_id"])
    if index_spec["id"] != trade["inflation_index_id"]:
        raise ValueError(
            "ZCIIS inflation_index_id does not match the inflation curve's "
            "index")
    ts = _build_zero_inflation_curve(curve_spec, index_spec, request)
    index = _build_inflation_index(
        index_spec, zero_handle=ql.ZeroInflationTermStructureHandle(ts))

    swap_type = ql.Swap.Payer if trade.get("swap_type", "Payer") == "Payer" \
        else ql.Swap.Receiver
    swap = ql.ZeroCouponInflationSwap(
        swap_type,
        trade["notional"],
        parse_date(trade["start_date"]),
        parse_date(trade["maturity_date"]),
        _calendar_from_enum(trade.get("fixed_calendar", "TARGET")),
        get_convention(trade.get("fixed_convention", "ModifiedFollowing")),
        get_day_counter(trade.get("day_counter", "Actual365Fixed")),
        trade["fixed_rate"],
        index,
        _wire_period(trade, "observation_lag"),
        get_cpi_interpolation(trade.get("observation_interpolation",
                                        "AsIndex")),
        trade.get("adjust_observation_dates", False),
        _calendar_from_enum(trade.get("inflation_calendar", "NullCalendar")),
        get_convention(trade.get("inflation_convention", "Following")))
    swap.setPricingEngine(ql.DiscountingSwapEngine(discount))
    return swap.NPV()


def price_year_on_year_inflation_swap_ql(request: dict) -> float:
    """Price a year-on-year inflation swap (YYIIS) using QuantLib."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    trade = swap_data["year_on_year_inflation_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    nominal_curve = _nominal_curve_resolver(pricing, eval_date, request)
    discount = nominal_curve(swap_data["discounting_curve"])

    curve_spec = _find_inflation_curve_spec(pricing,
                                            swap_data["inflation_curve"])
    if curve_spec.get("kind") != "YoYInflation":
        raise ValueError("YYIIS needs a YoYInflation curve")
    index_spec = _find_inflation_index_spec(pricing, curve_spec["index_id"])
    if index_spec["id"] != trade["inflation_index_id"]:
        raise ValueError(
            "YYIIS inflation_index_id does not match the inflation curve's "
            "index")
    ts = _build_yoy_inflation_curve(curve_spec, index_spec, request,
                                    nominal_curve)
    index = _build_inflation_index(
        index_spec, yoy_handle=ql.YoYInflationTermStructureHandle(ts))

    swap_type = ql.Swap.Payer if trade.get("swap_type", "Payer") == "Payer" \
        else ql.Swap.Receiver
    swap = ql.YearOnYearInflationSwap(
        swap_type,
        trade["notional"],
        _build_wire_schedule(trade["fixed_schedule"]),
        trade["fixed_rate"],
        get_day_counter(trade.get("fixed_day_counter", "Actual365Fixed")),
        _build_wire_schedule(trade["yoy_schedule"]),
        index,
        _wire_period(trade, "observation_lag"),
        get_cpi_interpolation(trade.get("observation_interpolation",
                                        "AsIndex")),
        trade.get("spread", 0.0),
        get_day_counter(trade.get("yoy_day_counter", "Actual365Fixed")),
        _calendar_from_enum(trade.get("payment_calendar", "TARGET")),
        get_convention(trade.get("payment_convention", "ModifiedFollowing")))
    # The server prices the YoY leg with a BlackYoYInflationCouponPricer that
    # carries only the nominal discount curve (no optionlet vol surface); the
    # vol is never touched for plain (uncapped) YoY coupons.
    ql.setCouponPricer(
        swap.yoyLeg(),
        ql.BlackYoYInflationCouponPricer(
            ql.YoYOptionletVolatilitySurfaceHandle(), discount))
    swap.setPricingEngine(ql.DiscountingSwapEngine(discount))
    return swap.NPV()


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
# Bootstrap-curves series reference (/bootstrap-curves sampling parity)
# =============================================================================

def _curve_query_grid_dates(grid_spec: dict, reference_date: ql.Date,
                            as_of_date: ql.Date) -> list:
    """Mirror the server's date-grid construction (src/market/grid_utils.cpp).

    TenorGrid: when the grid names a calendar, each tenor is
    calendar.advance(reference_date, tenor, grid bdc); otherwise it is a plain
    reference_date + tenor. RangeGrid: literal day/week stepping (calendar
    advance for month/year steps), optionally filtered to business days
    (WeekendsOnly when no calendar is named).
    """
    grid_type = grid_spec["grid_type"]
    g = grid_spec["grid"]

    if grid_type == "TenorGrid":
        cal_name = g.get("calendar", "NullCalendar")
        use_calendar = cal_name != "NullCalendar"
        if use_calendar:
            cal = get_calendar(cal_name)
            bdc = get_convention(g.get("business_day_convention", "Following"))
        dates = []
        for t in g["tenors"]:
            period = ql.Period(int(t.get("n", 0)), get_time_unit(t.get("unit", "Days")))
            if use_calendar:
                dates.append(cal.advance(reference_date, period, bdc))
            else:
                dates.append(reference_date + period)
        return dates

    if grid_type == "RangeGrid":
        start = parse_date(g["start_date"]) if g.get("start_date") else as_of_date
        end = parse_date(g["end_date"])
        step_n = max(1, int(g.get("step_number", 1)))
        step_unit = g.get("step_time_unit", "Days")
        business_only = g.get("business_days_only", False)
        cal_name = g.get("calendar", "NullCalendar")
        if cal_name == "NullCalendar":
            cal = ql.WeekendsOnly() if business_only else ql.NullCalendar()
        else:
            cal = get_calendar(cal_name)
        bdc = get_convention(g.get("business_day_convention", "Following"))
        dates = []
        current = start
        while current <= end:
            if not business_only or cal.isBusinessDay(current):
                dates.append(current)
            if step_unit == "Days":
                current = current + step_n
            elif step_unit == "Weeks":
                current = current + 7 * step_n
            else:
                current = cal.advance(
                    current, ql.Period(step_n, get_time_unit(step_unit)), bdc)
        return dates

    raise ValueError(f"Unsupported grid_type: {grid_type}")


def _curve_fallback_calendar(curve_json: dict):
    """Mirror the server's calendarFromTermStructure: the first helper's
    calendar when it has one, TARGET otherwise."""
    points = curve_json.get("points", [])
    if points:
        cal_name = points[0].get("point", {}).get("calendar")
        if cal_name:
            return get_calendar(cal_name)
    return ql.TARGET()


def _resolve_fwd_calendar_bdc(query: dict, curve_json: dict):
    """Mirror grid_utils ResolveCalendar / ResolveBusinessDayConvention:
    options override > grid's own calendar/bdc > curve fallback calendar."""
    options = query.get("options")
    grid = query.get("grid", {}).get("grid", {})
    if options and options.get("calendar", "NullCalendar") != "NullCalendar":
        cal = get_calendar(options["calendar"])
    elif grid.get("calendar", "NullCalendar") != "NullCalendar":
        cal = get_calendar(grid["calendar"])
    else:
        cal = _curve_fallback_calendar(curve_json)
    if options is not None:
        bdc = get_convention(options.get("business_day_convention", "Following"))
    else:
        bdc = get_convention(grid.get("business_day_convention", "Following"))
    return cal, bdc


def bootstrap_curves_ql(request: dict) -> dict:
    """Reference for /bootstrap-curves: build the queried curve independently
    and sample it exactly the way the server does.

    Returns {measure_name: [values]} for the request's single query, one value
    per grid date, in the query's measure order. The curve is built with the
    same machinery the NPV families use (build_curve_from_json); the grid and
    the per-measure sampling mirror src/market/grid_utils.cpp and
    src/evaluators/bootstrap_curves_evaluator.cpp:

      * DF    -> curve.discount(d)
      * ZERO  -> curve.zeroRate(d', dc, compounding, frequency) with d'
                 clamped to referenceDate+1 when d <= referenceDate; dc is the
                 curve's own day counter unless use_curve_day_counter=false
      * FWD   -> curve.forwardRate(d, end, dc, compounding, frequency) where
                 end is d + eps (Instantaneous) or a calendar advance by the
                 query tenor (Period), floored at d+1
    """
    pricing = _reference_pricing_view(request)
    as_of = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = as_of

    queries = request.get("queries") or []
    if len(queries) != 1:
        raise ValueError("bootstrap_curves_ql expects exactly one curve query")
    query = queries[0]

    curve_id = query["curve_id"]
    curve_json = next(c for c in pricing["curves"] if c["id"] == curve_id)
    curve = build_curve_from_json(curve_json, as_of, request)

    # The server anchors tenor grids on the curve's own reference date
    # (TermStructure.reference_date, falling back to as_of_date).
    grid_ref = (parse_date(curve_json["reference_date"])
                if curve_json.get("reference_date") else as_of)
    grid_dates = _curve_query_grid_dates(query["grid"], grid_ref, as_of)

    curve_ref = curve.referenceDate()
    out = {}
    for measure in query["measures"]:
        if measure == "DF":
            values = [curve.discount(d) for d in grid_dates]
        elif measure == "ZERO":
            zq = query.get("zero") or {}
            dc = (curve.dayCounter() if zq.get("use_curve_day_counter", True)
                  else get_day_counter(zq.get("day_counter", "Actual365Fixed")))
            comp = get_compounding(zq.get("compounding", "Continuous"))
            freq = get_frequency(zq.get("frequency", "Annual"))
            values = []
            for d in grid_dates:
                dd = curve_ref + 1 if d <= curve_ref else d
                values.append(curve.zeroRate(dd, dc, comp, freq).rate())
        elif measure == "FWD":
            fq = query.get("fwd") or {}
            dc = (curve.dayCounter() if fq.get("use_curve_day_counter", True)
                  else get_day_counter(fq.get("day_counter", "Actual365Fixed")))
            comp = get_compounding(fq.get("compounding", "Simple"))
            freq = get_frequency(fq.get("frequency", "Annual"))
            instantaneous = (
                fq.get("forward_type", "Instantaneous") == "Instantaneous")
            if fq.get("use_grid_calendar_for_advance", True):
                cal, bdc = _resolve_fwd_calendar_bdc(query, curve_json)
            else:
                cal, bdc = _curve_fallback_calendar(curve_json), ql.Following
            values = []
            for d in grid_dates:
                if instantaneous:
                    eps_n = int(fq.get("instantaneous_eps_number", 1))
                    eps_unit = fq.get("instantaneous_eps_time_unit", "Days")
                    if eps_unit == "Days":
                        end = d + eps_n
                    else:
                        end = cal.advance(
                            d, ql.Period(eps_n, get_time_unit(eps_unit)), bdc)
                else:
                    t_n, t_unit = _period_n_unit(fq, "tenor", 0, "Days")
                    end = cal.advance(d, ql.Period(t_n, get_time_unit(t_unit)), bdc)
                if end <= d:
                    end = d + 1
                values.append(curve.forwardRate(d, end, dc, comp, freq).rate())
        else:
            raise ValueError(f"Unsupported curve measure: {measure}")
        out[measure] = values
    return out


# =============================================================================
# Calendar Utility Reference (exact-match parity cases)
# =============================================================================

def _calendar_from_enum(name: str):
    """QuantLib calendar for a wire ``enums.Calendar`` value.

    Mirrors the server's CalendarToQL (src/common/enum_convert.cpp) entry for
    entry, including the market variants QuantLib defaults to when the C++
    side default-constructs a calendar (e.g. UnitedKingdom -> Settlement,
    Germany -> FrankfurtStockExchange, China -> SSE). Note in particular that
    the plain "UnitedStates" enum maps to the Settlement market on the
    server, so it does here too.

    Raises on anything unmapped instead of falling back to a default —
    exact-match calendar cases must never silently compare against the wrong
    calendar. (BespokeCalendar is deliberately unmapped: an empty bespoke
    calendar has no holiday content worth pinning.)
    """
    mapping = {
        "Argentina": ql.Argentina,
        "Australia": ql.Australia,
        "Brazil": ql.Brazil,
        "Canada": ql.Canada,
        "China": ql.China,
        "CzechRepublic": ql.CzechRepublic,
        "Denmark": ql.Denmark,
        "Finland": ql.Finland,
        "Germany": ql.Germany,
        "HongKong": ql.HongKong,
        "Hungary": ql.Hungary,
        "Iceland": ql.Iceland,
        "India": ql.India,
        "Indonesia": ql.Indonesia,
        "Israel": ql.Israel,
        "Italy": ql.Italy,
        "Japan": ql.Japan,
        "Mexico": ql.Mexico,
        "NewZealand": ql.NewZealand,
        "Norway": ql.Norway,
        "NullCalendar": ql.NullCalendar,
        "Poland": ql.Poland,
        "Romania": ql.Romania,
        "Russia": ql.Russia,
        "SaudiArabia": ql.SaudiArabia,
        "Singapore": ql.Singapore,
        "Slovakia": ql.Slovakia,
        "SouthAfrica": ql.SouthAfrica,
        "SouthKorea": ql.SouthKorea,
        "Sweden": ql.Sweden,
        "Switzerland": ql.Switzerland,
        "TARGET": ql.TARGET,
        "Taiwan": ql.Taiwan,
        "Turkey": ql.Turkey,
        "Ukraine": ql.Ukraine,
        "UnitedKingdom": ql.UnitedKingdom,
        "WeekendsOnly": ql.WeekendsOnly,
    }
    if name in mapping:
        return mapping[name]()
    us_markets = {
        "UnitedStates": ql.UnitedStates.Settlement,
        "UnitedStatesSettlement": ql.UnitedStates.Settlement,
        "UnitedStatesNYSE": ql.UnitedStates.NYSE,
        "UnitedStatesGovernmentBond": ql.UnitedStates.GovernmentBond,
        "UnitedStatesNERC": ql.UnitedStates.NERC,
    }
    if name in us_markets:
        return ql.UnitedStates(us_markets[name])
    raise ValueError(f"Calendar enum value not mapped by the reference: {name}")


def _iso_date(d: ql.Date) -> str:
    """Format a QuantLib date the way the server does (io::iso_date)."""
    return f"{d.year():04d}-{int(d.month()):02d}-{d.dayOfMonth():02d}"


def calendar_business_days_ql(request: dict) -> list:
    """Business dates in [start_date, end_date], as ISO strings.

    Mirrors CalendarBusinessDaysEvaluator: walk every calendar day from
    start_date to end_date inclusive, skip the endpoints when
    include_start / include_end are false, and keep the dates the calendar
    calls business days.
    """
    cal = _calendar_from_enum(request.get("calendar", "TARGET"))
    start = parse_date(request["start_date"])
    end = parse_date(request["end_date"])
    include_start = request.get("include_start", True)
    include_end = request.get("include_end", True)
    dates = []
    d = start
    while d <= end:
        if not (d == start and not include_start) \
                and not (d == end and not include_end) \
                and cal.isBusinessDay(d):
            dates.append(_iso_date(d))
        d = d + 1
    return dates


def calendar_holidays_ql(request: dict) -> list:
    """Holiday dates in [start_date, end_date], as ISO strings.

    Mirrors CalendarHolidaysEvaluator: walk every calendar day in the
    inclusive range, keep the non-business days, and drop plain weekends
    unless include_weekends is set (weekend dates that are also listed
    holidays still count as weekends for this filter, exactly like the
    server's isWeekend check).
    """
    cal = _calendar_from_enum(request.get("calendar", "TARGET"))
    start = parse_date(request["start_date"])
    end = parse_date(request["end_date"])
    include_weekends = request.get("include_weekends", False)
    dates = []
    d = start
    while d <= end:
        if not cal.isBusinessDay(d):
            if include_weekends or not cal.isWeekend(d.weekday()):
                dates.append(_iso_date(d))
        d = d + 1
    return dates


def calendar_advance_ql(request: dict) -> str:
    """The advanced date, as an ISO string.

    Mirrors CalendarAdvanceEvaluator/Mapper: Calendar::advance(date,
    Period(tenor_number, tenor_unit), convention, end_of_month) with the
    schema defaults (tenor_number 0, Days, Following, end_of_month false).
    """
    cal = _calendar_from_enum(request.get("calendar", "TARGET"))
    date = parse_date(request["date"])
    period = ql.Period(int(request.get("tenor_number", 0)),
                       get_time_unit(request.get("tenor_unit", "Days")))
    convention = get_convention(request.get("convention", "Following"))
    end_of_month = request.get("end_of_month", False)
    return _iso_date(cal.advance(date, period, convention, end_of_month))


# =============================================================================
# Equity Option Reference Pricer
# =============================================================================

def price_equity_option_ql(request: dict) -> float:
    """Price a European equity vanilla option using QuantLib.

    Mirrors the server's equity-option evaluator field for field:
      * the underlying spec resolves its spot from pricing.quotes
        (spot_quote_id) and its dividend yield from the referenced curve in
        pricing.rates.curves, and the trade's discounting_curve is the
        risk-free leg — the three feed a BlackScholesMertonProcess together
        with the referenced Black vol,
      * the volatility is the referenced BlackVolSpec (shape=Constant only,
        like the server's equity path) rebuilt as a BlackConstantVol with the
        spec's own reference date, calendar and day count,
      * the payoff is a PlainVanillaPayoff and the exercise a
        EuropeanExercise — the only payoff/exercise combination the server's
        parser accepts today (American/Bermudan and digital payoffs are
        rejected on the wire),
      * discrete cash dividends declared on the underlying spec
        (EquityUnderlyingSpec.discrete_dividends: ex_date + amount) are
        turned into a FixedDividend schedule (via DividendVector) and priced
        with AnalyticDividendEuropeanEngine — the same escrowed-dividend
        analytic European engine the server now uses. They are layered on
        top of the continuous dividend-yield curve, which stays in the
        process. With no discrete dividends the plain no-dividend engines
        are used, exactly like the server,
      * the engine follows the referenced model: AnalyticEuropeanEngine
        (or AnalyticDividendEuropeanEngine when discrete dividends are
        present) for BlackScholesAnalytic, BinomialCRRVanillaEngine with the
        spec's own binomial_steps for BinomialCRR (the server builds
        BinomialVanillaEngine<CoxRossRubinstein> with the same step count),
      * the returned NPV is the per-option NPV scaled by the trade's
        quantity, matching the server's response field.

    Deliberately refuses (raises) what it cannot faithfully reproduce:
    barrier features, and discrete dividends combined with the binomial
    model (the server restricts discrete dividends to the analytic
    BlackScholesAnalytic path).
    """
    pricing = _reference_pricing_view(request)
    opt_data = request["options"][0]
    opt = opt_data["option"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    if opt.get("barrier"):
        raise ValueError("Equity barrier options are not reference-priced yet")

    underlying = next(
        (u for u in pricing.get("equity_underlyings", [])
         if u.get("id") == opt["underlying_id"]), None)
    if underlying is None:
        raise ValueError(f"Equity underlying not found: {opt['underlying_id']}")

    # Discrete cash dividends declared on the underlying spec (ex_date +
    # amount). Escrowed cash events layered on top of the continuous dividend
    # curve; empty when the spec carries none.
    div_dates = []
    div_amounts = []
    for dd in underlying.get("discrete_dividends", []) or []:
        div_dates.append(parse_date(dd["ex_date"]))
        div_amounts.append(dd["amount"])

    # Spot resolved from pricing.quotes by spot_quote_id (QuoteRegistry).
    spot_value = next(
        (q.get("value") for q in pricing.get("quotes", [])
         if q.get("id") == underlying["spot_quote_id"]), None)
    if spot_value is None:
        raise ValueError(f"Unknown quote id: {underlying['spot_quote_id']}")
    spot = ql.QuoteHandle(ql.SimpleQuote(spot_value))

    # Risk-free = the trade's discounting curve; dividend yield = the
    # underlying spec's curve. Both are ordinary pricing.rates.curves entries.
    disc_id = opt_data["discounting_curve"]
    disc_json = next(c for c in pricing["curves"] if c["id"] == disc_id)
    risk_free = build_curve_from_json(disc_json, eval_date, request)
    div_id = underlying["dividend_yield_curve_id"]
    div_json = next(c for c in pricing["curves"] if c["id"] == div_id)
    dividend = build_curve_from_json(div_json, eval_date, request)

    # Volatility: the referenced BlackVolSpec rebuilt as a BlackConstantVol
    # (the server's shape=Constant branch: BlackConstantVol(ref, cal, vol, dc)).
    vol_spec = next(
        (v for v in pricing.get("vol_surfaces", [])
         if v.get("id") == opt_data.get("volatility")), None)
    if vol_spec is None:
        raise ValueError(f"Black vol not found: {opt_data.get('volatility')}")
    vol_base = vol_spec.get("payload", {}).get("base", {})
    if vol_base.get("shape", "Constant") != "Constant":
        raise ValueError(
            "Only shape=Constant BlackVolSpec is reference-priced for equity")
    black_vol = ql.BlackVolTermStructureHandle(ql.BlackConstantVol(
        parse_date(vol_base.get("reference_date", pricing["as_of_date"])),
        get_calendar(vol_base.get("calendar", "TARGET")),
        vol_base["constant_vol"],
        get_day_counter(vol_base.get("day_counter", "Actual365Fixed")),
    ))

    process = ql.BlackScholesMertonProcess(spot, dividend, risk_free, black_vol)

    # Payoff / exercise: the server's parser accepts exactly
    # EquityPlainVanillaPayoff + EquityEuropeanExercise today.
    if opt.get("payoff_type") != "EquityPlainVanillaPayoff":
        raise ValueError(
            "EquityOption currently supports only EquityPlainVanillaPayoff")
    po = opt["payoff"]
    opt_type = ql.Option.Put if po.get("option_type", "Call") == "Put" else ql.Option.Call
    payoff = ql.PlainVanillaPayoff(opt_type, po["strike"])
    if opt.get("exercise_type") != "EquityEuropeanExercise":
        raise ValueError(
            "EquityOption currently supports only EquityEuropeanExercise")
    exercise = ql.EuropeanExercise(parse_date(opt["exercise"]["expiry_date"]))

    option = ql.VanillaOption(payoff, exercise)

    # Engine follows the referenced model spec (required on the server).
    model_spec = next(
        (m for m in pricing.get("models", [])
         if m.get("id") == opt_data.get("model")), None)
    if model_spec is None:
        raise ValueError(f"Model not found: {opt_data.get('model')}")
    model_payload = model_spec.get("payload", {})
    model_type = model_payload.get("model_type", "BlackScholesAnalytic")
    if model_type == "BlackScholesAnalytic":
        if div_dates:
            # Escrowed discrete-dividend analytic European engine — mirrors
            # the server's AnalyticDividendEuropeanEngine(process, schedule).
            schedule = ql.DividendVector(div_dates, div_amounts)
            engine = ql.AnalyticDividendEuropeanEngine(process, schedule)
        else:
            engine = ql.AnalyticEuropeanEngine(process)
    elif model_type == "BinomialCRR":
        if div_dates:
            raise ValueError(
                "Discrete cash dividends currently require "
                "model_type=BlackScholesAnalytic")
        steps = int(model_payload.get("binomial_steps", 500))
        if steps <= 0:
            raise ValueError("EquityVanillaModelSpec.binomial_steps must be > 0")
        engine = ql.BinomialCRRVanillaEngine(process, steps)
    else:
        raise ValueError(f"Unsupported EquityModelType: {model_type}")
    option.setPricingEngine(engine)

    return option.NPV() * opt.get("quantity", 1.0)


# =============================================================================
# Vol surface sampling + calibration references
# =============================================================================

def _find_vol_surface_spec(pricing: dict, vol_id: str) -> dict:
    spec = next((v for v in pricing.get("vol_surfaces", [])
                 if v.get("id") == vol_id), None)
    if spec is None:
        raise ValueError(f"Vol surface not found: {vol_id}")
    return spec


def _find_swap_index_def(pricing: dict, swap_index_id: str) -> dict:
    sidx = next((s for s in pricing.get("swap_indices", [])
                 if s.get("id") == swap_index_id), None)
    if sidx is None:
        raise ValueError(f"Swap index not found: {swap_index_id}")
    return sidx


def _wire_period_to_ql(p: dict) -> ql.Period:
    n, unit = _period_n_unit(p, "tenor", 0, "Months")
    return ql.Period(n, get_time_unit(unit))


def _frequency_to_period(freq) -> ql.Period:
    """Mirror of the server's frequencyToPeriod for swap-index fixed legs."""
    return {
        ql.Annual: ql.Period(1, ql.Years),
        ql.Semiannual: ql.Period(6, ql.Months),
        ql.Quarterly: ql.Period(3, ql.Months),
        ql.Monthly: ql.Period(1, ql.Months),
    }[freq]


def _ir_vol_base(base: dict):
    """Decode an IrVolBaseSpec: (ref_date, calendar, bdc, day_counter,
    vol_type, displacement). Refuses quote-referenced vols (no catalog case
    exercises the quote indirection)."""
    if base.get("quote_id"):
        raise ValueError(
            "quote-referenced vols are not reference-priced yet (inline "
            "constant_vol / matrix values only)")
    return (
        parse_date(base["reference_date"]),
        get_calendar(base.get("calendar", "TARGET")),
        get_convention(base.get("business_day_convention", "ModifiedFollowing")),
        get_day_counter(base.get("day_counter", "Actual365Fixed")),
        get_volatility_type(base.get("volatility_type", "Lognormal")),
        base.get("displacement", 0.0),
    )


def _build_swaption_atm_matrix(inner: dict, ref, cal, bdc, dc, vol_type,
                               displacement):
    """Rebuild the server's SwaptionVolatilityMatrix for an AtmMatrixSpec.

    The server constructs the matrix from Periods; the Python bindings only
    expose the date-based constructor, so the option dates are derived the
    exact way QuantLib's Period constructor does internally:
    calendar.advance(reference_date, period, bdc).
    """
    expiries = [_wire_period_to_ql(p) for p in inner["expiries"]]
    tenors = [_wire_period_to_ql(p) for p in inner["tenors"]]
    vols = inner["vols"]
    if vols.get("quote_ids"):
        raise ValueError("quote-referenced matrix vols are not supported here")
    n_rows, n_cols = vols["n_rows"], vols["n_cols"]
    matrix = ql.Matrix(n_rows, n_cols)
    for i in range(n_rows):
        for j in range(n_cols):
            matrix[i][j] = vols["values"][i * n_cols + j]
    option_dates = [cal.advance(ref, p, bdc) for p in expiries]
    shifts = (ql.Matrix(n_rows, n_cols, displacement)
              if displacement != 0.0 else ql.Matrix())
    svm = ql.SwaptionVolatilityMatrix(
        ref, cal, bdc, option_dates, tenors, matrix, dc, False, vol_type,
        shifts)
    return svm, expiries, tenors


def _tenor_grid_dates(grid_spec: dict, ref, fallback_cal, fallback_bdc):
    """Build a TenorGrid's dates the way the sampling evaluator does for
    optionlet / equity surfaces: calendar.advance(reference_date, p, bdc),
    with the grid's own calendar/bdc taking effect when the grid sets an
    explicit calendar, else the surface's own conventions.

    Only TenorGrid is supported (the catalog cases use tenor grids); the
    reference deliberately raises on RangeGrid rather than approximating the
    server's stepping loop untested.
    """
    if grid_spec.get("grid_type", "TenorGrid") != "TenorGrid":
        raise ValueError("Only TenorGrid expiry grids are reference-sampled")
    grid = grid_spec["grid"]
    if grid.get("calendar"):
        cal = get_calendar(grid["calendar"])
        bdc = get_convention(grid.get("business_day_convention", "Following"))
    else:
        cal, bdc = fallback_cal, fallback_bdc
    return [cal.advance(ref, _wire_period_to_ql(p), bdc)
            for p in grid["tenors"]]


def sample_vol_surfaces_ql(request: dict) -> list:
    """Sample a vol surface exactly like the server's /sample-vol-surfaces.

    Returns the flat `vols` list the response's results[0] must carry,
    computed from an independently rebuilt QuantLib vol structure sampled at
    independently recomputed query points. Supported subset (everything else
    raises rather than approximating):

      * Swaption surfaces of kind Constant (ConstantSwaptionVolatility) and
        AtmMatrix2D (SwaptionVolatilityMatrix, bilinear in option time and
        swap length), Cube and ExpirySlice output modes, TenorGrid expiry
        and tenor grids, AbsoluteStrike axis. Exercise dates, effective swap
        start/end dates (spot lag + fixed-leg schedule end) and the
        option-time / swap-length year fractions mirror the evaluator's
        computation node for node.
      * Optionlet surfaces (constant vol only — the only shape on the wire),
        sampled per (grid date, strike).
      * EquityBlack surfaces of shape Constant (BlackConstantVol) and
        AtmMatrix2D term vols (BlackVarianceCurve, linear in variance,
        forceMonotoneVariance=true like the server's parser), sampled per
        (grid date, strike).

    Not supported (deliberately): smile cubes / SABR sampling (needs the
    forward-aware cube the swaption family also defers), SpreadFromATM strike
    axes, RangeGrid grids, SmileSlice/TermSlice modes, quote-referenced vols
    and multi-query requests.
    """
    pricing = _reference_pricing_view(request)
    queries = request.get("queries", [])
    if len(queries) != 1:
        raise ValueError("Reference samples exactly one query per request")
    q = queries[0]
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    options = q.get("options", {})
    if options.get("calendar"):
        raise ValueError("QueryOptions.calendar overrides are not supported")
    allow_extrapolation = options.get("allow_extrapolation", True)
    strike_grid = q.get("strike_grid", {})
    if strike_grid.get("axis", "AbsoluteStrike") != "AbsoluteStrike":
        raise ValueError("Only AbsoluteStrike sampling is reference-supported")
    strikes = strike_grid["strikes"]

    surface_type = q.get("surface_type", "Swaption")
    vol_spec = _find_vol_surface_spec(pricing, q["vol_id"])
    payload = vol_spec.get("payload", {})

    if surface_type == "Swaption":
        if vol_spec.get("payload_type") != "SwaptionVolSpec":
            raise ValueError("vol_id does not reference a SwaptionVolSpec")
        inner_type = payload.get("payload_type")
        inner = payload.get("payload", {})
        base = inner.get("base", {})
        ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)

        if inner_type == "SwaptionVolConstantSpec":
            handle = ql.ConstantSwaptionVolatility(
                ref, cal, bdc, base["constant_vol"], dc, vol_type,
                displacement)
        elif inner_type == "SwaptionVolAtmMatrixSpec":
            handle, _, _ = _build_swaption_atm_matrix(
                inner, ref, cal, bdc, dc, vol_type, displacement)
        else:
            raise ValueError(
                f"Swaption sampling of {inner_type} is not reference-supported"
                " (smile cubes and SABR surfaces are deferred)")
        if allow_extrapolation:
            handle.enableExtrapolation()

        # Swaption grid dates come from the swap index's fixed-leg
        # conventions, not the grid's own calendar fields (the evaluator's
        # TenorGrid branch): exercise = fixed_cal.advance(ref, p, fixed_bdc).
        sidx = _find_swap_index_def(pricing, payload["swap_index_id"])
        if q.get("swap_index_id") and q["swap_index_id"] != sidx["id"]:
            raise ValueError("Query swap_index_id must match the surface's")
        fixed = sidx["fixed_leg"]
        fixed_cal = get_calendar(fixed["fixed_calendar"])
        fixed_bdc = get_convention(fixed["fixed_bdc"])
        fixed_term_bdc = get_convention(fixed["fixed_term_bdc"])
        fixed_rule = get_date_generation(fixed["fixed_date_rule"])
        fixed_eom = fixed.get("fixed_eom", False)
        fixed_freq = get_frequency(fixed["fixed_frequency"])
        spot_days = sidx.get("spot_days", 0)

        exp_grid = q["expiry_grid"]
        if exp_grid.get("grid_type", "TenorGrid") != "TenorGrid":
            raise ValueError("Only TenorGrid expiry grids are supported")
        exercises = [fixed_cal.advance(ref, _wire_period_to_ql(p), fixed_bdc)
                     for p in exp_grid["grid"]["tenors"]]
        ten_grid = q["tenor_grid"]
        if ten_grid.get("grid_type", "TenorGrid") != "TenorGrid":
            raise ValueError("Only TenorGrid tenor grids are supported")
        tenors = [_wire_period_to_ql(p) for p in ten_grid["grid"]["tenors"]]

        def node_times(exercise, tenor):
            # Mirrors the evaluator's computeSwaptionDates + sampleVol:
            # spot-lagged start, fixed-leg schedule end date, option time
            # from the evaluation date and swap length between start/end,
            # all on the surface's own day counter.
            start = exercise
            if spot_days > 0:
                start = fixed_cal.advance(exercise, spot_days, ql.Days,
                                          fixed_bdc)
            tentative_end = fixed_cal.advance(start, tenor, fixed_term_bdc)
            schedule = ql.Schedule(
                start, tentative_end, ql.Period(fixed_freq), fixed_cal,
                fixed_bdc, fixed_term_bdc, fixed_rule, fixed_eom)
            option_time = max(1.0e-8, dc.yearFraction(eval_date, exercise))
            swap_length = max(1.0e-8,
                              dc.yearFraction(start, schedule.endDate()))
            return option_time, swap_length

        mode = q.get("output_mode", "Cube")
        out = []
        if mode == "Cube":
            for exercise in exercises:
                for tenor in tenors:
                    option_time, swap_length = node_times(exercise, tenor)
                    for strike in strikes:
                        out.append(handle.volatility(
                            option_time, swap_length, strike, True))
        elif mode == "ExpirySlice":
            tenor = tenors[q["slice_tenor_index"]]
            if not q.get("slice_strike_is_set"):
                raise ValueError("ExpirySlice requires slice_strike_is_set")
            strike = q["slice_strike"]
            for exercise in exercises:
                option_time, swap_length = node_times(exercise, tenor)
                out.append(handle.volatility(
                    option_time, swap_length, strike, True))
        else:
            raise ValueError(
                f"Output mode {mode} is not reference-supported")
        return out

    if surface_type == "Optionlet":
        if vol_spec.get("payload_type") != "OptionletVolSpec":
            raise ValueError("vol_id does not reference an OptionletVolSpec")
        base = payload.get("base", {})
        ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)
        handle = ql.ConstantOptionletVolatility(
            ref, cal, bdc, base["constant_vol"], dc, vol_type, displacement)
        if allow_extrapolation:
            handle.enableExtrapolation()
        dates = _tenor_grid_dates(q["expiry_grid"], ref, cal, bdc)
        return [handle.volatility(d, strike, True)
                for d in dates for strike in strikes]

    if surface_type == "EquityBlack":
        if vol_spec.get("payload_type") != "BlackVolSpec":
            raise ValueError("vol_id does not reference a BlackVolSpec")
        base = payload.get("base", {})
        if base.get("quote_id"):
            raise ValueError("quote-referenced vols are not supported here")
        ref = parse_date(base["reference_date"])
        cal = get_calendar(base.get("calendar", "TARGET"))
        bdc = get_convention(
            base.get("business_day_convention", "ModifiedFollowing"))
        dc = get_day_counter(base.get("day_counter", "Actual365Fixed"))
        shape = base.get("shape", "Constant")
        if shape == "Constant":
            handle = ql.BlackConstantVol(ref, cal, base["constant_vol"], dc)
        elif shape == "AtmMatrix2D":
            # Server: pillar dates via cal.advance(ref, p, bdc), then a
            # BlackVarianceCurve (linear total variance) with
            # forceMonotoneVariance=true.
            term_vols = payload["term_vols"]
            if term_vols.get("quote_ids"):
                raise ValueError("quote-referenced term vols not supported")
            pillar_dates = [cal.advance(ref, _wire_period_to_ql(p), bdc)
                            for p in payload["expiries"]]
            handle = ql.BlackVarianceCurve(
                ref, pillar_dates, term_vols["values"], dc, True)
        else:
            raise ValueError(
                f"EquityBlack shape {shape} is not reference-supported")
        if allow_extrapolation:
            handle.enableExtrapolation()
        dates = _tenor_grid_dates(q["expiry_grid"], ref, cal, bdc)
        return [handle.blackVol(d, strike, True)
                for d in dates for strike in strikes]

    raise ValueError(f"Unknown surface_type: {surface_type}")


def calibrate_swaption_model_ql(request: dict) -> dict:
    """Run the server's Hull-White swaption calibration independently.

    Mirrors the server's calibration routine step for step: one SwaptionHelper
    per (expiry, tenor) grid node quoted at the surface's vol for that node
    (constant vol, or the ATM matrix interpolated at the node), priced on a
    JamshidianSwaptionEngine over HullWhite(discount_curve, a_init,
    sigma_init), calibrated with LevenbergMarquardt under
    EndCriteria(function_evaluations, max_iterations, eps, eps, eps) and the
    spec's calibrate_a/calibrate_sigma fixed-parameter flags. The helper error
    metric follows the server's vol-type pairing: PriceError for Normal vols,
    ImpliedVolError for Black/shifted Black.

    Returns the response fields to compare: hw_a, hw_sigma, rmse (root mean
    square of the helpers' calibrationError, same metric) and num_helpers.

    Both sides execute the same QuantLib C++ code, so on a well-conditioned
    problem the calibrated parameters agree to ~1e-11 (see the catalog
    tolerance note). Ill-conditioned setups — both parameters free against
    vols no Hull-White model can fit — leave a flat valley in `a` where the
    two builds stop at measurably different (equally valid) points; such
    cases are deliberately not in the catalog.
    """
    pricing = _reference_pricing_view(request)
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    model = next((m for m in pricing.get("models", [])
                  if m.get("id") == request["model_id"]), None)
    if model is None:
        raise ValueError(f"Model not found: {request['model_id']}")
    calib = request.get("calibration") or model["payload"].get(
        "hw_calibration")
    if not calib:
        raise ValueError("No calibration spec on request or model")

    disc_json = next(c for c in pricing["curves"]
                     if c["id"] == calib["discount_curve_id"])
    discount = build_curve_from_json(disc_json, eval_date, request)
    fwd_json = next(c for c in pricing["curves"]
                    if c["id"] == calib["forwarding_curve_id"])
    forwarding = build_curve_from_json(fwd_json, eval_date, request)

    sidx = _find_swap_index_def(pricing, calib["swap_index_id"])
    if sidx.get("kind", "IborSwapIndex") != "IborSwapIndex":
        raise ValueError("Hull-White calibration supports Ibor swap indices")
    fixed = sidx["fixed_leg"]
    ibor = resolve_index_from_id(sidx["float_index_id"], request, forwarding)

    vol_spec = _find_vol_surface_spec(pricing, calib["swaption_vol_id"])
    wrapper = vol_spec["payload"]
    inner_type = wrapper.get("payload_type")
    inner = wrapper.get("payload", {})
    base = inner.get("base", {})
    ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)

    matrix_expiries = matrix_tenors = None
    if inner_type == "SwaptionVolConstantSpec":
        constant_vol = base["constant_vol"]

        def market_vol(expiry, tenor):
            return constant_vol
    elif inner_type == "SwaptionVolAtmMatrixSpec":
        svm, matrix_expiries, matrix_tenors = _build_swaption_atm_matrix(
            inner, ref, cal, bdc, dc, vol_type, displacement)

        def market_vol(expiry, tenor):
            # The server calls volatility(Period, Period, 0, extrapolate);
            # QuantLib resolves the option Period through the structure's own
            # calendar/bdc, which the Python bindings only expose via the
            # (Date, Period) overload.
            return svm.volatility(cal.advance(ref, expiry, bdc), tenor, 0.0,
                                  True)
    else:
        raise ValueError(
            "Hull-White calibration supports Constant and AtmMatrix2D vols")

    expiries = ([_wire_period_to_ql(p) for p in calib.get("expiries", [])]
                or matrix_expiries)
    tenors = ([_wire_period_to_ql(p) for p in calib.get("tenors", [])]
              or matrix_tenors)
    if not expiries or not tenors:
        raise ValueError("Calibration grid is empty")

    error_type = (ql.BlackCalibrationHelper.PriceError
                  if vol_type == ql.Normal
                  else ql.BlackCalibrationHelper.ImpliedVolError)
    fixed_leg_tenor = _frequency_to_period(
        get_frequency(fixed["fixed_frequency"]))
    fixed_dc = get_day_counter(fixed["fixed_day_counter"])
    spot_days = sidx.get("spot_days", 0)

    helpers = []
    for expiry in expiries:
        for tenor in tenors:
            vol_quote = ql.QuoteHandle(
                ql.SimpleQuote(market_vol(expiry, tenor)))
            helpers.append(ql.SwaptionHelper(
                expiry, tenor, vol_quote, ibor, fixed_leg_tenor, fixed_dc,
                ibor.dayCounter(), discount, error_type, ql.nullDouble(),
                1.0, vol_type, displacement, spot_days))

    hw = ql.HullWhite(discount, calib.get("a_init", 0.03),
                      calib.get("sigma_init", 0.01))
    engine = ql.JamshidianSwaptionEngine(hw)
    for helper in helpers:
        helper.setPricingEngine(engine)

    eps = calib.get("end_criteria_eps", 1.0e-8)
    # Server arg order: EndCriteria(function_evaluations, max_iterations,
    # eps, eps, eps).
    end_criteria = ql.EndCriteria(
        calib.get("function_evaluations", 1000),
        calib.get("max_iterations", 200), eps, eps, eps)
    fix_params = [not calib.get("calibrate_a", True),
                  not calib.get("calibrate_sigma", True)]
    hw.calibrate(helpers, ql.LevenbergMarquardt(), end_criteria,
                 ql.NoConstraint(), [], fix_params)

    errors = [h.calibrationError() for h in helpers]
    rmse = math.sqrt(sum(e * e for e in errors) / len(errors))
    return {
        "hw_a": hw.params()[0],
        "hw_sigma": hw.params()[1],
        "rmse": rmse,
        "num_helpers": len(helpers),
    }


def calibrate_swaption_vol_ql(request: dict) -> dict:
    """Run the server's per-node SABR swaption-cube calibration independently.

    Rebuilds everything the server's SABR-calibrate finalize path builds:

      * per-node ATM forwards as the fair rates of forward-starting vanilla
        swaps generated from the swap index conventions (spot lag, fixed-leg
        schedule) on the request's discounting/forwarding curves,
      * the spread-zero ATM vol matrix (linear interpolation of each node's
        market vols at spread 0) wrapped in a SwaptionVolatilityMatrix,
      * the per-node vol spreads, the {alpha 0.04, beta beta_value, nu 0.4,
        rho 0.0} parameter guesses and the {false, beta_fixed, false, false}
        fixed flags,
      * QuantLib's SabrSwaptionVolatilityCube with the same SwapIndex (built
        on the first grid tenor) and default end criteria, whose
        sparseSabrParameters() runs the same per-node Levenberg-Marquardt
        fits the server runs.

    Returns the response diagnostics fields to compare (dotted paths into the
    response): per-node forward, ATM vol (Hagan vol at the forward on the
    calibrated parameters), alpha/beta/rho/nu grids, per-node fit RMSE and
    the overall RMSE. Lognormal, displacement-free surfaces only — the
    catalog cases keep the smile data well inside SABR's parameter bounds,
    where the two builds' optimizers agree to ~1e-10 (boundary-pinned fits,
    e.g. rho -> 1, diverge across builds and are deliberately not cataloged).
    """
    pricing = _reference_pricing_view(request)
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    disc_json = next(c for c in pricing["curves"]
                     if c["id"] == request["discounting_curve_id"])
    discount = build_curve_from_json(disc_json, eval_date, request)
    fwd_json = next(c for c in pricing["curves"]
                    if c["id"] == request["forwarding_curve_id"])
    forwarding = build_curve_from_json(fwd_json, eval_date, request)

    vol_spec = _find_vol_surface_spec(pricing, request["vol_id"])
    wrapper = vol_spec["payload"]
    if wrapper.get("payload_type") != "SwaptionSabrCalibrateSpec":
        raise ValueError("vol_id must reference a SwaptionSabrCalibrateSpec")
    inner = wrapper["payload"]
    base = inner["base"]
    ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)
    if vol_type != ql.ShiftedLognormal or displacement != 0.0:
        raise ValueError(
            "Only displacement-free lognormal SABR calibration is "
            "reference-supported")

    sidx = _find_swap_index_def(pricing, wrapper["swap_index_id"])
    fixed = sidx["fixed_leg"]
    float_leg = sidx["float_leg"]
    fixed_cal = get_calendar(fixed["fixed_calendar"])
    fixed_bdc = get_convention(fixed["fixed_bdc"])
    fixed_term_bdc = get_convention(fixed["fixed_term_bdc"])
    fixed_rule = get_date_generation(fixed["fixed_date_rule"])
    fixed_eom = fixed.get("fixed_eom", False)
    fixed_freq = get_frequency(fixed["fixed_frequency"])
    fixed_dc = get_day_counter(fixed["fixed_day_counter"])
    spot_days = sidx.get("spot_days", 0)
    ibor = resolve_index_from_id(sidx["float_index_id"], request, forwarding)

    expiries = [_wire_period_to_ql(p) for p in inner["expiries"]]
    tenors = [_wire_period_to_ql(p) for p in inner["tenors"]]
    spreads = inner["strikes"]
    tensor = inner["vols"]
    if tensor.get("quote_ids"):
        raise ValueError("quote-referenced SABR vols are not supported here")
    n_exp, n_ten, n_str = tensor["n_1"], tensor["n_2"], tensor["n_3"]
    values = tensor["values"]

    # Per-node ATM forwards: the server's computeServerAtmForwards — a
    # forward-starting vanilla swap per node whose fair rate is the forward.
    swap_engine = ql.DiscountingSwapEngine(discount)
    float_tenor = _wire_period_to_ql(float_leg["float_tenor"])
    forwards = []
    for expiry in expiries:
        exercise = fixed_cal.advance(ref, expiry, fixed_bdc)
        start = exercise
        if spot_days > 0:
            start = fixed_cal.advance(exercise, spot_days, ql.Days, fixed_bdc)
        for tenor in tenors:
            tentative_end = fixed_cal.advance(start, tenor, fixed_term_bdc)
            fixed_schedule = ql.Schedule(
                start, tentative_end, ql.Period(fixed_freq), fixed_cal,
                fixed_bdc, fixed_term_bdc, fixed_rule, fixed_eom)
            float_schedule = ql.Schedule(
                start, fixed_schedule.endDate(), float_tenor,
                get_calendar(float_leg["float_calendar"]),
                get_convention(float_leg["float_bdc"]),
                get_convention(float_leg["float_term_bdc"]),
                get_date_generation(float_leg["float_date_rule"]),
                float_leg.get("float_eom", False))
            swap = ql.VanillaSwap(
                ql.VanillaSwap.Payer, 1.0, fixed_schedule, 0.0, fixed_dc,
                float_schedule, ibor, 0.0, ibor.dayCounter())
            swap.setPricingEngine(swap_engine)
            forwards.append(swap.fairRate())

    # Spread-zero ATM vols: linear interpolation of each node's market vols
    # at spread 0 (flat-extended when the grid is one-sided), exactly the
    # server's interpolateAtmVolAtSpreadZero.
    def atm_at_spread_zero(node_vols):
        if len(node_vols) == 1 or spreads[0] >= 0.0:
            return node_vols[0]
        if spreads[-1] <= 0.0:
            return node_vols[-1]
        hi = bisect.bisect_left(spreads, 0.0)
        lo = hi - 1
        w = (0.0 - spreads[lo]) / (spreads[hi] - spreads[lo])
        return node_vols[lo] * (1.0 - w) + node_vols[hi] * w

    atm_matrix = ql.Matrix(n_exp, n_ten)
    for i in range(n_exp):
        for j in range(n_ten):
            node = values[(i * n_ten + j) * n_str:(i * n_ten + j + 1) * n_str]
            atm_matrix[i][j] = atm_at_spread_zero(node)
    option_dates = [cal.advance(ref, p, bdc) for p in expiries]
    atm_structure = ql.SwaptionVolatilityMatrix(
        ref, cal, bdc, option_dates, tenors, atm_matrix, dc, False, vol_type,
        ql.Matrix())
    atm_handle = ql.SwaptionVolatilityStructureHandle(atm_structure)

    vol_spreads = []
    for i in range(n_exp):
        for j in range(n_ten):
            atm = atm_matrix[i][j]
            vol_spreads.append([
                ql.QuoteHandle(ql.SimpleQuote(
                    values[(i * n_ten + j) * n_str + k] - atm))
                for k in range(n_str)])

    beta_fixed = inner.get("beta_fixed", True)
    beta_value = inner.get("beta_value", 0.5)
    guesses = [[ql.QuoteHandle(ql.SimpleQuote(0.04)),
                ql.QuoteHandle(ql.SimpleQuote(beta_value)),
                ql.QuoteHandle(ql.SimpleQuote(0.4)),
                ql.QuoteHandle(ql.SimpleQuote(0.0))]
               for _ in range(n_exp * n_ten)]
    is_fixed = [False, beta_fixed, False, False]

    swap_index = ql.SwapIndex(
        wrapper["swap_index_id"], tenors[0], spot_days, ibor.currency(),
        fixed_cal, _frequency_to_period(fixed_freq), fixed_bdc, fixed_dc,
        ibor, discount)
    cube = ql.SabrSwaptionVolatilityCube(
        atm_handle, expiries, tenors, spreads, vol_spreads, swap_index,
        swap_index, inner.get("vega_weighted_smile_fit", False), guesses,
        is_fixed, False)
    cube.enableExtrapolation()

    # browse() row layout is (swapLengthIdx * nOptionTimes + optionIdx) with
    # QuantLib's internal parameter order {alpha, beta, nu, rho}; re-indexed
    # into the response's row-major (expiry, tenor) layout and schema field
    # order, exactly like the server's finalize.
    browsed = cube.sparseSabrParameters()
    n_nodes = n_exp * n_ten
    alpha = [0.0] * n_nodes
    beta = [0.0] * n_nodes
    rho = [0.0] * n_nodes
    nu = [0.0] * n_nodes
    per_node_rmse = [0.0] * n_nodes
    for i in range(n_exp):
        for j in range(n_ten):
            row = j * n_exp + i
            k = i * n_ten + j
            alpha[k] = browsed[row][2]
            beta[k] = browsed[row][3]
            nu[k] = browsed[row][4]
            rho[k] = browsed[row][5]
            per_node_rmse[k] = browsed[row][7]
    overall_rmse = math.sqrt(sum(r * r for r in per_node_rmse) / n_nodes)

    # Per-node ATM vol: Hagan vol at the forward on the calibrated params,
    # with the node's time-to-expiry on the surface's own conventions —
    # the diagnostics builder's SabrSmileSection.volatility(F).
    atm_vol = []
    for i in range(n_exp):
        exercise = cal.advance(ref, expiries[i], bdc)
        tte = dc.yearFraction(ref, exercise)
        for j in range(n_ten):
            k = i * n_ten + j
            atm_vol.append(ql.sabrVolatility(
                forwards[k], forwards[k], tte, alpha[k], beta[k], nu[k],
                rho[k]))

    return {
        "diagnostics.forward_per_node": forwards,
        "diagnostics.atm_vol_per_node": atm_vol,
        "diagnostics.alpha_per_node": alpha,
        "diagnostics.beta_per_node": beta,
        "diagnostics.rho_per_node": rho,
        "diagnostics.nu_per_node": nu,
        "diagnostics.calibration.per_node_rmse": per_node_rmse,
        "diagnostics.calibration.overall_rmse": overall_rmse,
    }


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
