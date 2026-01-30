#!/usr/bin/env python3
"""
Test serialization only - no gRPC needed.
"""

import sys
sys.path.insert(0, '/home/claude/quantraserver-refractor/flatbuffers/python')

print("Testing Quantra Serialization")
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

# Test 2: Import FlatBuffers tables and builder functions
print("\n[2] Testing FlatBuffers table imports...")
try:
    # Import module-level builder functions
    from quantra import FixedRateBond as FBFixedRateBondModule
    from quantra import Schedule as FBScheduleModule
    from quantra import TermStructure as FBTermStructureModule
    from quantra import Pricing as FBPricingModule
    from quantra import DepositHelper as FBDepositHelperModule
    from quantra import PointsWrapper as FBPointsWrapperModule
    from quantra import PriceFixedRateBond as FBPriceFixedRateBondModule
    from quantra import PriceFixedRateBondRequest as FBRequestModule
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
    
    # Build schedule using module functions
    FBScheduleModule.Start(builder)
    FBScheduleModule.AddCalendar(builder, Calendar.UnitedStatesGovernmentBond)
    FBScheduleModule.AddEffectiveDate(builder, effective_date)
    FBScheduleModule.AddTerminationDate(builder, termination_date)
    FBScheduleModule.AddFrequency(builder, Frequency.Semiannual)
    FBScheduleModule.AddConvention(builder, BusinessDayConvention.Unadjusted)
    FBScheduleModule.AddTerminationDateConvention(builder, BusinessDayConvention.Unadjusted)
    FBScheduleModule.AddDateGenerationRule(builder, DateGenerationRule.Backward)
    FBScheduleModule.AddEndOfMonth(builder, False)
    schedule_off = FBScheduleModule.End(builder)
    print("    Built Schedule")
    
    # Build fixed rate bond
    FBFixedRateBondModule.Start(builder)
    FBFixedRateBondModule.AddSettlementDays(builder, 3)
    FBFixedRateBondModule.AddFaceAmount(builder, 100.0)
    FBFixedRateBondModule.AddRate(builder, 0.045)
    FBFixedRateBondModule.AddAccrualDayCounter(builder, DayCounter.ActualActualBond)
    FBFixedRateBondModule.AddPaymentConvention(builder, BusinessDayConvention.ModifiedFollowing)
    FBFixedRateBondModule.AddRedemption(builder, 100.0)
    FBFixedRateBondModule.AddIssueDate(builder, issue_date)
    FBFixedRateBondModule.AddSchedule(builder, schedule_off)
    bond_off = FBFixedRateBondModule.End(builder)
    print("    Built FixedRateBond")
    
    # Build deposit helper
    FBDepositHelperModule.Start(builder)
    FBDepositHelperModule.AddRate(builder, 0.05)
    FBDepositHelperModule.AddTenorNumber(builder, 3)
    FBDepositHelperModule.AddTenorTimeUnit(builder, TimeUnit.Months)
    FBDepositHelperModule.AddFixingDays(builder, 3)
    FBDepositHelperModule.AddCalendar(builder, Calendar.TARGET)
    FBDepositHelperModule.AddBusinessDayConvention(builder, BusinessDayConvention.ModifiedFollowing)
    FBDepositHelperModule.AddDayCounter(builder, DayCounter.Actual365Fixed)
    deposit_off = FBDepositHelperModule.End(builder)
    print("    Built DepositHelper")
    
    # Wrap in PointsWrapper
    FBPointsWrapperModule.Start(builder)
    FBPointsWrapperModule.AddPointType(builder, FBPoint.DepositHelper)
    FBPointsWrapperModule.AddPoint(builder, deposit_off)
    point_wrapper_off = FBPointsWrapperModule.End(builder)
    print("    Built PointsWrapper")
    
    # Build points vector
    FBTermStructureModule.StartPointsVector(builder, 1)
    builder.PrependUOffsetTRelative(point_wrapper_off)
    points_vec = builder.EndVector()
    
    # Build term structure
    FBTermStructureModule.Start(builder)
    FBTermStructureModule.AddId(builder, curve_id)
    FBTermStructureModule.AddDayCounter(builder, DayCounter.ActualActual365)
    FBTermStructureModule.AddInterpolator(builder, Interpolator.LogLinear)
    FBTermStructureModule.AddBootstrapTrait(builder, BootstrapTrait.Discount)
    FBTermStructureModule.AddReferenceDate(builder, ref_date)
    FBTermStructureModule.AddPoints(builder, points_vec)
    term_structure_off = FBTermStructureModule.End(builder)
    print("    Built TermStructure")
    
    # Build curves vector
    FBPricingModule.StartCurvesVector(builder, 1)
    builder.PrependUOffsetTRelative(term_structure_off)
    curves_vec = builder.EndVector()
    
    # Build pricing
    FBPricingModule.Start(builder)
    FBPricingModule.AddAsOfDate(builder, as_of_date)
    FBPricingModule.AddSettlementDate(builder, settlement_date)
    FBPricingModule.AddCurves(builder, curves_vec)
    FBPricingModule.AddBondPricingDetails(builder, False)
    FBPricingModule.AddBondPricingFlows(builder, False)
    pricing_off = FBPricingModule.End(builder)
    print("    Built Pricing")
    
    # Build PriceFixedRateBond
    FBPriceFixedRateBondModule.Start(builder)
    FBPriceFixedRateBondModule.AddFixedRateBond(builder, bond_off)
    FBPriceFixedRateBondModule.AddDiscountingCurve(builder, discount_curve)
    price_bond_off = FBPriceFixedRateBondModule.End(builder)
    print("    Built PriceFixedRateBond")
    
    # Build bonds vector
    FBRequestModule.StartBondsVector(builder, 1)
    builder.PrependUOffsetTRelative(price_bond_off)
    bonds_vec = builder.EndVector()
    
    # Build request
    FBRequestModule.Start(builder)
    FBRequestModule.AddPricing(builder, pricing_off)
    FBRequestModule.AddBonds(builder, bonds_vec)
    request_off = FBRequestModule.End(builder)
    
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

# Save buffer to file for testing with server
with open('/tmp/test_request.bin', 'wb') as f:
    f.write(buf)
print(f"Saved to /tmp/test_request.bin")
