"""
Serialization functions for Python models to/from FlatBuffers.

This module handles converting between Pythonic dataclasses and FlatBuffers
binary format for gRPC communication.
"""

import flatbuffers
from typing import List, Optional
import sys
import os

# Add the flatbuffers python path BEFORE any other imports
FB_PYTHON_PATH = os.environ.get(
    "QUANTRA_FB_PYTHON_PATH",
    "/workspace/flatbuffers/python"
)
if FB_PYTHON_PATH not in sys.path:
    sys.path.insert(0, FB_PYTHON_PATH)

# Import our models
from .models import (
    PriceFixedRateBondRequest,
    PriceFixedRateBondResponse,
    PriceFixedRateBond,
    FixedRateBond,
    FixedRateBondResponse,
    Pricing,
    TermStructure,
    Schedule,
    DepositHelper,
    SwapHelper,
    BondHelper,
    FRAHelper,
    FutureHelper,
)
from .enums import *

# Now import FlatBuffers generated MODULES (not classes)
FB_AVAILABLE = False
FB_IMPORT_ERROR = None

try:
    import importlib
    
    # Import modules - we need the module to access the free functions
    fb_PriceFixedRateBondRequest = importlib.import_module('quantra.PriceFixedRateBondRequest')
    fb_PriceFixedRateBondResponse = importlib.import_module('quantra.PriceFixedRateBondResponse')
    fb_PriceFixedRateBond = importlib.import_module('quantra.PriceFixedRateBond')
    fb_FixedRateBond = importlib.import_module('quantra.FixedRateBond')
    fb_FixedRateBondValues = importlib.import_module('quantra.FixedRateBondValues')
    fb_Pricing = importlib.import_module('quantra.Pricing')
    fb_TermStructure = importlib.import_module('quantra.TermStructure')
    fb_Schedule = importlib.import_module('quantra.Schedule')
    fb_DepositHelper = importlib.import_module('quantra.DepositHelper')
    fb_SwapHelper = importlib.import_module('quantra.SwapHelper')
    fb_BondHelper = importlib.import_module('quantra.BondHelper')
    fb_FRAHelper = importlib.import_module('quantra.FRAHelper')
    fb_FutureHelper = importlib.import_module('quantra.FutureHelper')
    fb_PointsWrapper = importlib.import_module('quantra.PointsWrapper')
    fb_Point = importlib.import_module('quantra.Point')
    
    FB_AVAILABLE = True
except ImportError as e:
    FB_IMPORT_ERROR = str(e)


def serialize_price_fixed_rate_bond_request(request: PriceFixedRateBondRequest) -> bytes:
    """
    Serialize a PriceFixedRateBondRequest to FlatBuffers bytes.
    """
    if not FB_AVAILABLE:
        raise ImportError(f"FlatBuffers Python code not available: {FB_IMPORT_ERROR}")
    
    builder = flatbuffers.Builder(4096)
    
    # Serialize pricing (curves, dates, etc.)
    pricing_offset = _serialize_pricing(builder, request.pricing)
    
    # Serialize bonds
    bond_offsets = []
    for bond in request.bonds:
        bond_offset = _serialize_price_fixed_rate_bond(builder, bond)
        bond_offsets.append(bond_offset)
    
    # Create bonds vector
    fb_PriceFixedRateBondRequest.StartBondsVector(builder, len(bond_offsets))
    for offset in reversed(bond_offsets):
        builder.PrependUOffsetTRelative(offset)
    bonds_vector = builder.EndVector()
    
    # Build the request
    fb_PriceFixedRateBondRequest.Start(builder)
    fb_PriceFixedRateBondRequest.AddPricing(builder, pricing_offset)
    fb_PriceFixedRateBondRequest.AddBonds(builder, bonds_vector)
    request_offset = fb_PriceFixedRateBondRequest.End(builder)
    
    builder.Finish(request_offset)
    return bytes(builder.Output())


def _serialize_pricing(builder: flatbuffers.Builder, pricing: Pricing) -> int:
    """Serialize Pricing object."""
    
    # Serialize curves
    curve_offsets = []
    for curve in pricing.curves:
        curve_offset = _serialize_term_structure(builder, curve)
        curve_offsets.append(curve_offset)
    
    fb_Pricing.StartCurvesVector(builder, len(curve_offsets))
    for offset in reversed(curve_offsets):
        builder.PrependUOffsetTRelative(offset)
    curves_vector = builder.EndVector()
    
    # Create strings
    as_of_date = builder.CreateString(pricing.as_of_date)
    settlement_date = builder.CreateString(pricing.settlement_date)
    
    # Build pricing
    fb_Pricing.Start(builder)
    fb_Pricing.AddAsOfDate(builder, as_of_date)
    fb_Pricing.AddSettlementDate(builder, settlement_date)
    fb_Pricing.AddCurves(builder, curves_vector)
    fb_Pricing.AddBondPricingDetails(builder, pricing.bond_pricing_details or False)
    fb_Pricing.AddBondPricingFlows(builder, pricing.bond_pricing_flows or False)
    return fb_Pricing.End(builder)


def _serialize_term_structure(builder: flatbuffers.Builder, ts: TermStructure) -> int:
    """Serialize TermStructure object."""
    
    # Serialize points (curve instruments)
    point_offsets = []
    for point in ts.points:
        point_offset = _serialize_point(builder, point)
        point_offsets.append(point_offset)
    
    fb_TermStructure.StartPointsVector(builder, len(point_offsets))
    for offset in reversed(point_offsets):
        builder.PrependUOffsetTRelative(offset)
    points_vector = builder.EndVector()
    
    # Create strings
    id_str = builder.CreateString(ts.id or "")
    ref_date = builder.CreateString(ts.reference_date or "")
    
    # Build term structure
    fb_TermStructure.Start(builder)
    fb_TermStructure.AddId(builder, id_str)
    fb_TermStructure.AddReferenceDate(builder, ref_date)
    fb_TermStructure.AddDayCounter(builder, ts.day_counter.value if ts.day_counter else 0)
    fb_TermStructure.AddInterpolator(builder, ts.interpolator.value if ts.interpolator else 0)
    fb_TermStructure.AddBootstrapTrait(builder, ts.bootstrap_trait.value if ts.bootstrap_trait else 0)
    fb_TermStructure.AddPoints(builder, points_vector)
    return fb_TermStructure.End(builder)


def _serialize_point(builder: flatbuffers.Builder, point) -> int:
    """Serialize a curve point (deposit, swap, etc.)."""
    
    # Determine point type and serialize accordingly
    if isinstance(point, DepositHelper):
        return _serialize_deposit_helper_wrapper(builder, point)
    elif isinstance(point, SwapHelper):
        return _serialize_swap_helper_wrapper(builder, point)
    elif isinstance(point, BondHelper):
        return _serialize_bond_helper_wrapper(builder, point)
    elif isinstance(point, FRAHelper):
        return _serialize_fra_helper_wrapper(builder, point)
    elif isinstance(point, FutureHelper):
        return _serialize_future_helper_wrapper(builder, point)
    else:
        raise ValueError(f"Unknown point type: {type(point)}")


def _serialize_deposit_helper_wrapper(builder: flatbuffers.Builder, deposit: DepositHelper) -> int:
    """Serialize a deposit helper wrapped in PointsWrapper."""
    
    # Build deposit helper using module functions
    fb_DepositHelper.Start(builder)
    fb_DepositHelper.AddRate(builder, deposit.rate or 0.0)
    fb_DepositHelper.AddTenorNumber(builder, deposit.tenor_number or 0)
    fb_DepositHelper.AddTenorTimeUnit(builder, deposit.tenor_time_unit.value if deposit.tenor_time_unit else 0)
    fb_DepositHelper.AddFixingDays(builder, deposit.fixing_days or 0)
    fb_DepositHelper.AddCalendar(builder, deposit.calendar.value if deposit.calendar else 0)
    fb_DepositHelper.AddBusinessDayConvention(builder, deposit.business_day_convention.value if deposit.business_day_convention else 0)
    fb_DepositHelper.AddDayCounter(builder, deposit.day_counter.value if deposit.day_counter else 0)
    deposit_offset = fb_DepositHelper.End(builder)
    
    # Wrap in PointsWrapper
    fb_PointsWrapper.Start(builder)
    fb_PointsWrapper.AddPointType(builder, fb_Point.Point.DepositHelper)
    fb_PointsWrapper.AddPoint(builder, deposit_offset)
    return fb_PointsWrapper.End(builder)


def _serialize_swap_helper_wrapper(builder: flatbuffers.Builder, swap: SwapHelper) -> int:
    """Serialize a swap helper wrapped in PointsWrapper."""
    
    fb_SwapHelper.Start(builder)
    fb_SwapHelper.AddRate(builder, swap.rate or 0.0)
    fb_SwapHelper.AddTenorNumber(builder, swap.tenor_number or 0)
    fb_SwapHelper.AddTenorTimeUnit(builder, swap.tenor_time_unit.value if swap.tenor_time_unit else 0)
    fb_SwapHelper.AddCalendar(builder, swap.calendar.value if swap.calendar else 0)
    fb_SwapHelper.AddSwFixedLegFrequency(builder, swap.sw_fixed_leg_frequency.value if swap.sw_fixed_leg_frequency else 0)
    fb_SwapHelper.AddSwFixedLegConvention(builder, swap.sw_fixed_leg_convention.value if swap.sw_fixed_leg_convention else 0)
    fb_SwapHelper.AddSwFixedLegDayCounter(builder, swap.sw_fixed_leg_day_counter.value if swap.sw_fixed_leg_day_counter else 0)
    fb_SwapHelper.AddSwFloatingLegIndex(builder, swap.sw_floating_leg_index.value if swap.sw_floating_leg_index else 0)
    fb_SwapHelper.AddSpread(builder, swap.spread or 0.0)
    fb_SwapHelper.AddFwdStartDays(builder, swap.fwd_start_days or 0)
    swap_offset = fb_SwapHelper.End(builder)
    
    fb_PointsWrapper.Start(builder)
    fb_PointsWrapper.AddPointType(builder, fb_Point.Point.SwapHelper)
    fb_PointsWrapper.AddPoint(builder, swap_offset)
    return fb_PointsWrapper.End(builder)


def _serialize_bond_helper_wrapper(builder: flatbuffers.Builder, bond: BondHelper) -> int:
    """Serialize a bond helper wrapped in PointsWrapper."""
    
    # Serialize schedule if present
    schedule_offset = None
    if bond.schedule:
        schedule_offset = _serialize_schedule(builder, bond.schedule)
    
    issue_date = builder.CreateString(bond.issue_date or "")
    
    fb_BondHelper.Start(builder)
    fb_BondHelper.AddRate(builder, bond.rate or 0.0)
    fb_BondHelper.AddSettlementDays(builder, bond.settlement_days or 0)
    fb_BondHelper.AddFaceAmount(builder, bond.face_amount or 0.0)
    if schedule_offset:
        fb_BondHelper.AddSchedule(builder, schedule_offset)
    fb_BondHelper.AddCouponRate(builder, bond.coupon_rate or 0.0)
    fb_BondHelper.AddDayCounter(builder, bond.day_counter.value if bond.day_counter else 0)
    fb_BondHelper.AddBusinessDayConvention(builder, bond.business_day_convention.value if bond.business_day_convention else 0)
    fb_BondHelper.AddRedemption(builder, bond.redemption or 100.0)
    fb_BondHelper.AddIssueDate(builder, issue_date)
    bond_offset = fb_BondHelper.End(builder)
    
    fb_PointsWrapper.Start(builder)
    fb_PointsWrapper.AddPointType(builder, fb_Point.Point.BondHelper)
    fb_PointsWrapper.AddPoint(builder, bond_offset)
    return fb_PointsWrapper.End(builder)


def _serialize_fra_helper_wrapper(builder: flatbuffers.Builder, fra: FRAHelper) -> int:
    """Serialize a FRA helper wrapped in PointsWrapper."""
    
    fb_FRAHelper.Start(builder)
    fb_FRAHelper.AddRate(builder, fra.rate or 0.0)
    fb_FRAHelper.AddMonthsToStart(builder, fra.months_to_start or 0)
    fb_FRAHelper.AddMonthsToEnd(builder, fra.months_to_end or 0)
    fb_FRAHelper.AddFixingDays(builder, fra.fixing_days or 0)
    fb_FRAHelper.AddCalendar(builder, fra.calendar.value if fra.calendar else 0)
    fb_FRAHelper.AddBusinessDayConvention(builder, fra.business_day_convention.value if fra.business_day_convention else 0)
    fb_FRAHelper.AddDayCounter(builder, fra.day_counter.value if fra.day_counter else 0)
    fra_offset = fb_FRAHelper.End(builder)
    
    fb_PointsWrapper.Start(builder)
    fb_PointsWrapper.AddPointType(builder, fb_Point.Point.FRAHelper)
    fb_PointsWrapper.AddPoint(builder, fra_offset)
    return fb_PointsWrapper.End(builder)


def _serialize_future_helper_wrapper(builder: flatbuffers.Builder, future: FutureHelper) -> int:
    """Serialize a future helper wrapped in PointsWrapper."""
    
    future_start_date = builder.CreateString(future.future_start_date or "")
    
    fb_FutureHelper.Start(builder)
    fb_FutureHelper.AddRate(builder, future.rate or 0.0)
    fb_FutureHelper.AddFutureStartDate(builder, future_start_date)
    fb_FutureHelper.AddFutureMonths(builder, future.future_months or 0)
    fb_FutureHelper.AddCalendar(builder, future.calendar.value if future.calendar else 0)
    fb_FutureHelper.AddBusinessDayConvention(builder, future.business_day_convention.value if future.business_day_convention else 0)
    fb_FutureHelper.AddDayCounter(builder, future.day_counter.value if future.day_counter else 0)
    future_offset = fb_FutureHelper.End(builder)
    
    fb_PointsWrapper.Start(builder)
    fb_PointsWrapper.AddPointType(builder, fb_Point.Point.FutureHelper)
    fb_PointsWrapper.AddPoint(builder, future_offset)
    return fb_PointsWrapper.End(builder)


def _serialize_schedule(builder: flatbuffers.Builder, schedule: Schedule) -> int:
    """Serialize a Schedule object."""
    
    effective_date = builder.CreateString(schedule.effective_date or "")
    termination_date = builder.CreateString(schedule.termination_date or "")
    
    fb_Schedule.Start(builder)
    fb_Schedule.AddCalendar(builder, schedule.calendar.value if schedule.calendar else 0)
    fb_Schedule.AddEffectiveDate(builder, effective_date)
    fb_Schedule.AddTerminationDate(builder, termination_date)
    fb_Schedule.AddFrequency(builder, schedule.frequency.value if schedule.frequency else 0)
    fb_Schedule.AddConvention(builder, schedule.convention.value if schedule.convention else 0)
    fb_Schedule.AddTerminationDateConvention(builder, schedule.termination_date_convention.value if schedule.termination_date_convention else 0)
    fb_Schedule.AddDateGenerationRule(builder, schedule.date_generation_rule.value if schedule.date_generation_rule else 0)
    fb_Schedule.AddEndOfMonth(builder, schedule.end_of_month or False)
    return fb_Schedule.End(builder)


def _serialize_price_fixed_rate_bond(builder: flatbuffers.Builder, price_bond: PriceFixedRateBond) -> int:
    """Serialize a PriceFixedRateBond object."""
    
    # Serialize the bond
    bond_offset = None
    if price_bond.fixed_rate_bond:
        bond_offset = _serialize_fixed_rate_bond(builder, price_bond.fixed_rate_bond)
    
    discounting_curve = builder.CreateString(price_bond.discounting_curve or "")
    
    fb_PriceFixedRateBond.Start(builder)
    if bond_offset:
        fb_PriceFixedRateBond.AddFixedRateBond(builder, bond_offset)
    fb_PriceFixedRateBond.AddDiscountingCurve(builder, discounting_curve)
    return fb_PriceFixedRateBond.End(builder)


def _serialize_fixed_rate_bond(builder: flatbuffers.Builder, bond: FixedRateBond) -> int:
    """Serialize a FixedRateBond object."""
    
    # Serialize schedule first (must be created before Start)
    schedule_offset = None
    if bond.schedule:
        schedule_offset = _serialize_schedule(builder, bond.schedule)
    
    issue_date = builder.CreateString(bond.issue_date or "")
    
    fb_FixedRateBond.Start(builder)
    fb_FixedRateBond.AddSettlementDays(builder, bond.settlement_days or 0)
    fb_FixedRateBond.AddFaceAmount(builder, bond.face_amount or 0.0)
    fb_FixedRateBond.AddRate(builder, bond.rate or 0.0)
    fb_FixedRateBond.AddAccrualDayCounter(builder, bond.accrual_day_counter.value if bond.accrual_day_counter else 0)
    fb_FixedRateBond.AddPaymentConvention(builder, bond.payment_convention.value if bond.payment_convention else 0)
    fb_FixedRateBond.AddRedemption(builder, bond.redemption or 100.0)
    fb_FixedRateBond.AddIssueDate(builder, issue_date)
    if schedule_offset:
        fb_FixedRateBond.AddSchedule(builder, schedule_offset)
    return fb_FixedRateBond.End(builder)


# ============================================================================
# Deserialization
# ============================================================================

def deserialize_price_fixed_rate_bond_response(data: bytes) -> List[dict]:
    """
    Deserialize a PriceFixedRateBondResponse from FlatBuffers bytes.
    """
    if not FB_AVAILABLE:
        raise ImportError(f"FlatBuffers Python code not available: {FB_IMPORT_ERROR}")
    
    response = fb_PriceFixedRateBondResponse.PriceFixedRateBondResponse.GetRootAs(data, 0)
    
    results = []
    for i in range(response.BondsLength()):
        bond_result = response.Bonds(i)
        results.append({
            "npv": bond_result.Npv(),
            "clean_price": getattr(bond_result, 'CleanPrice', lambda: None)(),
            "dirty_price": getattr(bond_result, 'DirtyPrice', lambda: None)(),
            "accrued_amount": getattr(bond_result, 'AccruedAmount', lambda: None)(),
            "yield": getattr(bond_result, 'Yield', lambda: None)(),
            "duration": getattr(bond_result, 'MacaulayDuration', lambda: None)(),
            "modified_duration": getattr(bond_result, 'ModifiedDuration', lambda: None)(),
            "convexity": getattr(bond_result, 'Convexity', lambda: None)(),
            "bps": getattr(bond_result, 'Bps', lambda: None)(),
        })
    
    return results