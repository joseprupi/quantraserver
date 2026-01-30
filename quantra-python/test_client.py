#!/usr/bin/env python3
"""
Simple test to verify the Python client models and serialization work.
"""

import sys
# FlatBuffers generated code takes priority
sys.path.insert(0, '/home/claude/quantraserver-refractor/flatbuffers/python')
sys.path.insert(0, '/home/claude/quantra-python')

print("Testing Quantra Python Client")
print("=" * 50)

# Test 1: Import FlatBuffers enums
print("\n[1] Testing FlatBuffers enum imports...")
try:
    from quantra.enums.DayCounter import DayCounter
    from quantra.enums.Calendar import Calendar
    from quantra.enums.BusinessDayConvention import BusinessDayConvention
    from quantra.enums.Frequency import Frequency
    from quantra.enums.TimeUnit import TimeUnit
    from quantra.enums.DateGenerationRule import DateGenerationRule
    from quantra.enums.Interpolator import Interpolator
    from quantra.enums.BootstrapTrait import BootstrapTrait
    
    print(f"    DayCounter.Actual360 = {DayCounter.Actual360}")
    print(f"    Calendar.TARGET = {Calendar.TARGET}")
    print(f"    Frequency.Semiannual = {Frequency.Semiannual}")
    print("    ✓ Enums OK")
except Exception as e:
    print(f"    ✗ Failed: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

# Test 2: Import FlatBuffers tables
print("\n[2] Testing FlatBuffers table imports...")
try:
    from quantra.FixedRateBond import FixedRateBond as FBFixedRateBond
    from quantra.Schedule import Schedule as FBSchedule
    from quantra.TermStructure import TermStructure as FBTermStructure
    from quantra.Pricing import Pricing as FBPricing
    from quantra.DepositHelper import DepositHelper as FBDepositHelper
    from quantra.SwapHelper import SwapHelper as FBSwapHelper
    from quantra.PointsWrapper import PointsWrapper as FBPointsWrapper
    from quantra.PriceFixedRateBond import PriceFixedRateBond as FBPriceFixedRateBond
    from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequest as FBRequest
    from quantra.Point import Point as FBPoint
    print("    ✓ FlatBuffers tables imported OK")
except Exception as e:
    print(f"    ✗ Failed: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

# Test 3: Build a simple serialized message
print("\n[3] Testing FlatBuffers serialization...")
try:
    import flatbuffers
    
    builder = flatbuffers.Builder(4096)
    
    # Create strings first (FlatBuffers requirement)
    as_of_date = builder.CreateString("2008/09/18")
    settlement_date = builder.CreateString("2008/09/18")
    curve_id = builder.CreateString("depos")
    ref_date = builder.CreateString("2008/09/18")
    issue_date = builder.CreateString("2007/05/15")
    effective_date = builder.CreateString("2007/05/15")
    termination_date = builder.CreateString("2017/05/15")
    discount_curve = builder.CreateString("depos")
    
    # Build schedule
    FBSchedule.Start(builder)
    FBSchedule.AddCalendar(builder, Calendar.UnitedStatesGovernmentBond)
    FBSchedule.AddEffectiveDate(builder, effective_date)
    FBSchedule.AddTerminationDate(builder, termination_date)
    FBSchedule.AddFrequency(builder, Frequency.Semiannual)
    FBSchedule.AddConvention(builder, BusinessDayConvention.Unadjusted)
    FBSchedule.AddTerminationDateConvention(builder, BusinessDayConvention.Unadjusted)
    FBSchedule.AddDateGenerationRule(builder, DateGenerationRule.Backward)
    FBSchedule.AddEndOfMonth(builder, False)
    schedule_off = FBSchedule.End(builder)
    print("    Built Schedule")
    
    # Build fixed rate bond
    FBFixedRateBond.Start(builder)
    FBFixedRateBond.AddSettlementDays(builder, 3)
    FBFixedRateBond.AddFaceAmount(builder, 100.0)
    FBFixedRateBond.AddRate(builder, 0.045)
    FBFixedRateBond.AddAccrualDayCounter(builder, DayCounter.ActualActualBond)
    FBFixedRateBond.AddPaymentConvention(builder, BusinessDayConvention.ModifiedFollowing)
    FBFixedRateBond.AddRedemption(builder, 100.0)
    FBFixedRateBond.AddIssueDate(builder, issue_date)
    FBFixedRateBond.AddSchedule(builder, schedule_off)
    bond_off = FBFixedRateBond.End(builder)
    print("    Built FixedRateBond")
    
    # Build deposit helper
    FBDepositHelper.Start(builder)
    FBDepositHelper.AddRate(builder, 0.05)
    FBDepositHelper.AddTenorNumber(builder, 3)
    FBDepositHelper.AddTenorTimeUnit(builder, TimeUnit.Months)
    FBDepositHelper.AddFixingDays(builder, 3)
    FBDepositHelper.AddCalendar(builder, Calendar.TARGET)
    FBDepositHelper.AddBusinessDayConvention(builder, BusinessDayConvention.ModifiedFollowing)
    FBDepositHelper.AddDayCounter(builder, DayCounter.Actual365Fixed)
    deposit_off = FBDepositHelper.End(builder)
    print("    Built DepositHelper")
    
    # Wrap in PointsWrapper
    FBPointsWrapper.Start(builder)
    FBPointsWrapper.AddPointType(builder, FBPoint.DepositHelper)
    FBPointsWrapper.AddPoint(builder, deposit_off)
    point_wrapper_off = FBPointsWrapper.End(builder)
    print("    Built PointsWrapper")
    
    # Build points vector
    FBTermStructure.StartPointsVector(builder, 1)
    builder.PrependUOffsetTRelative(point_wrapper_off)
    points_vec = builder.EndVector()
    
    # Build term structure
    FBTermStructure.Start(builder)
    FBTermStructure.AddId(builder, curve_id)
    FBTermStructure.AddDayCounter(builder, DayCounter.ActualActual365)
    FBTermStructure.AddInterpolator(builder, Interpolator.LogLinear)
    FBTermStructure.AddBootstrapTrait(builder, BootstrapTrait.Discount)
    FBTermStructure.AddReferenceDate(builder, ref_date)
    FBTermStructure.AddPoints(builder, points_vec)
    term_structure_off = FBTermStructure.End(builder)
    print("    Built TermStructure")
    
    # Build curves vector
    FBPricing.StartCurvesVector(builder, 1)
    builder.PrependUOffsetTRelative(term_structure_off)
    curves_vec = builder.EndVector()
    
    # Build pricing
    FBPricing.Start(builder)
    FBPricing.AddAsOfDate(builder, as_of_date)
    FBPricing.AddSettlementDate(builder, settlement_date)
    FBPricing.AddCurves(builder, curves_vec)
    FBPricing.AddBondPricingDetails(builder, False)
    FBPricing.AddBondPricingFlows(builder, False)
    pricing_off = FBPricing.End(builder)
    print("    Built Pricing")
    
    # Build PriceFixedRateBond
    FBPriceFixedRateBond.Start(builder)
    FBPriceFixedRateBond.AddFixedRateBond(builder, bond_off)
    FBPriceFixedRateBond.AddDiscountingCurve(builder, discount_curve)
    price_bond_off = FBPriceFixedRateBond.End(builder)
    print("    Built PriceFixedRateBond")
    
    # Build bonds vector
    FBRequest.StartBondsVector(builder, 1)
    builder.PrependUOffsetTRelative(price_bond_off)
    bonds_vec = builder.EndVector()
    
    # Build request
    FBRequest.Start(builder)
    FBRequest.AddPricing(builder, pricing_off)
    FBRequest.AddBonds(builder, bonds_vec)
    request_off = FBRequest.End(builder)
    
    builder.Finish(request_off)
    buf = bytes(builder.Output())
    
    print(f"    ✓ Serialization OK: {len(buf)} bytes")
except Exception as e:
    print(f"    ✗ Failed: {e}")
    import traceback
    traceback.print_exc()
    sys.exit(1)

print("\n" + "=" * 50)
print("All serialization tests passed! ✓")
print(f"\nSerialized request size: {len(buf)} bytes")
print("\nNext: Test against live server")
print("  Terminal 1: quantra start --workers 10 --foreground")
print("  Terminal 2: python test_client_live.py")
