#!/usr/bin/env python3
"""
Surgical Python gRPC test diagnostic.
Run from workspace: python3 tests/debug_python_client.py

Tests one product at a time, catching exact errors.
"""

import sys
import os
import traceback

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(PROJECT_ROOT, 'flatbuffers/python'))
sys.path.insert(0, os.path.join(PROJECT_ROOT, 'quantra-python'))

print("=" * 60)
print("STEP 1: Check Python FlatBuffers generation status")
print("=" * 60)

fb_dir = os.path.join(PROJECT_ROOT, 'flatbuffers/python/quantra')

# Check critical files
checks = {
    "IndexDef.py": "IndexDef table (new schema)",
    "IndexRef.py": "IndexRef table (new schema)",
    "IndexType.py": "IndexType enum (new schema)",
    "Index.py": "OLD Index table (should be replaced or coexist)",
    "Pricing.py": "Pricing table",
    "SwapHelper.py": "SwapHelper table",
    "BootstrapCurvesRequest.py": "Bootstrap request",
}

for fname, desc in checks.items():
    path = os.path.join(fb_dir, fname)
    if os.path.exists(path):
        print(f"  ✓ {fname} exists - {desc}")
    else:
        print(f"  ✗ {fname} MISSING - {desc}")

print()

# Check Pricing.py for indices field
pricing_path = os.path.join(fb_dir, "Pricing.py")
if os.path.exists(pricing_path):
    with open(pricing_path) as f:
        content = f.read()
    if "indices" in content.lower() or "Indices" in content:
        print("  ✓ Pricing.py has 'indices' field (new schema)")
    else:
        print("  ✗ Pricing.py does NOT have 'indices' - STALE! Need to re-run flatc --python")

# Check BootstrapCurvesRequest.py for pricing field
bcr_path = os.path.join(fb_dir, "BootstrapCurvesRequest.py")
if os.path.exists(bcr_path):
    with open(bcr_path) as f:
        content = f.read()
    if "pricing" in content.lower() or "Pricing" in content:
        print("  ✓ BootstrapCurvesRequest.py has 'pricing' field")
    else:
        print("  ✗ BootstrapCurvesRequest.py does NOT have 'pricing' - STALE!")

print()
print("=" * 60)
print("STEP 2: Test imports")
print("=" * 60)

import_tests = [
    ("IndexDefT", "from quantra.IndexDef import IndexDefT"),
    ("IndexRefT", "from quantra.IndexRef import IndexRefT"),
    ("IndexType", "from quantra.IndexType import IndexType"),
    ("PricingT", "from quantra.Pricing import PricingT"),
    ("SwapHelperT", "from quantra.SwapHelper import SwapHelperT"),
]

all_imports_ok = True
for name, stmt in import_tests:
    try:
        exec(stmt)
        print(f"  ✓ {name}: {stmt}")
    except Exception as e:
        print(f"  ✗ {name}: {e}")
        all_imports_ok = False

if not all_imports_ok:
    print()
    print("=" * 60)
    print("FATAL: Missing Python FlatBuffers files.")
    print()
    print("You need to regenerate Python FlatBuffers from the new .fbs schema.")
    print("Run this from /workspace/flatbuffers/fbs:")
    print()
    print("  flatc --python --gen-object-api -o ../python *.fbs")
    print()
    print("Or if flatc is elsewhere:")
    print("  find / -name flatc -type f 2>/dev/null")
    print("=" * 60)
    sys.exit(1)

print()
print("=" * 60)
print("STEP 3: Check PricingT has 'indices' attribute")
print("=" * 60)

from quantra.Pricing import PricingT
p = PricingT()
if hasattr(p, 'indices'):
    print(f"  ✓ PricingT has 'indices' attribute (value: {p.indices})")
else:
    print(f"  ✗ PricingT does NOT have 'indices' attribute!")
    print(f"    Available attrs: {[a for a in dir(p) if not a.startswith('_')]}")
    print()
    print("  This means flatc was not run with the updated common.fbs.")
    print("  The old Pricing table doesn't have indices.")
    sys.exit(1)

print()
print("=" * 60)
print("STEP 4: Check SwapHelperT has 'floatIndex' (not swFloatingLegIndex)")
print("=" * 60)

from quantra.SwapHelper import SwapHelperT
sh = SwapHelperT()
if hasattr(sh, 'floatIndex'):
    print(f"  ✓ SwapHelperT has 'floatIndex'")
elif hasattr(sh, 'swFloatingLegIndex'):
    print(f"  ✗ SwapHelperT still has OLD 'swFloatingLegIndex' - flatc not re-run")
else:
    print(f"  ? SwapHelperT attrs: {[a for a in dir(sh) if not a.startswith('_')]}")

print()
print("=" * 60)
print("STEP 5: Try building a minimal Fixed Rate Bond request")
print("=" * 60)

try:
    import QuantLib as ql
    from quantra.IndexDef import IndexDefT
    from quantra.IndexRef import IndexRefT
    from quantra.IndexType import IndexType
    from quantra.Pricing import PricingT
    from quantra.TermStructure import TermStructureT
    from quantra.PointsWrapper import PointsWrapperT
    from quantra.DepositHelper import DepositHelperT
    from quantra.SwapHelper import SwapHelperT
    from quantra.Schedule import ScheduleT
    from quantra.FixedRateBond import FixedRateBondT
    from quantra.PriceFixedRateBond import PriceFixedRateBondT
    from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT
    from quantra.Yield import YieldT
    from quantra.enums.DayCounter import DayCounter
    from quantra.enums.Calendar import Calendar
    from quantra.enums.BusinessDayConvention import BusinessDayConvention
    from quantra.enums.Frequency import Frequency
    from quantra.enums.TimeUnit import TimeUnit
    from quantra.enums.DateGenerationRule import DateGenerationRule
    from quantra.enums.Interpolator import Interpolator
    from quantra.enums.BootstrapTrait import BootstrapTrait
    from quantra.enums.Compounding import Compounding
    from quantra.Point import Point
    
    FLAT_RATE = 0.03
    EVAL_DATE_STR = "2025-01-15"
    
    # Build IndexDef
    idx_def = IndexDefT()
    idx_def.id = "EUR_6M"
    idx_def.name = "Euribor"
    idx_def.indexType = IndexType.Ibor
    idx_def.tenorNumber = 6
    idx_def.tenorTimeUnit = TimeUnit.Months
    idx_def.fixingDays = 2
    idx_def.calendar = Calendar.TARGET
    idx_def.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    idx_def.dayCounter = DayCounter.Actual360
    idx_def.endOfMonth = False
    idx_def.currency = "EUR"
    print("  ✓ IndexDefT created")
    
    # Build IndexRef for SwapHelper
    idx_ref = IndexRefT()
    idx_ref.id = "EUR_6M"
    print("  ✓ IndexRefT created")
    
    # Build curve
    ts = TermStructureT()
    ts.id = "discount"
    ts.dayCounter = DayCounter.Actual365Fixed
    ts.interpolator = Interpolator.LogLinear
    ts.bootstrapTrait = BootstrapTrait.Discount
    ts.referenceDate = EVAL_DATE_STR
    ts.points = []
    
    # Deposit 3M
    dep = DepositHelperT()
    dep.rate = FLAT_RATE
    dep.tenorNumber = 3
    dep.tenorTimeUnit = TimeUnit.Months
    dep.fixingDays = 2
    dep.calendar = Calendar.TARGET
    dep.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    dep.dayCounter = DayCounter.Actual365Fixed
    pw = PointsWrapperT()
    pw.pointType = Point.DepositHelper
    pw.point = dep
    ts.points.append(pw)
    
    # Swap 5Y
    sw = SwapHelperT()
    sw.rate = FLAT_RATE
    sw.tenorNumber = 5
    sw.tenorTimeUnit = TimeUnit.Years
    sw.calendar = Calendar.TARGET
    sw.swFixedLegFrequency = Frequency.Annual
    sw.swFixedLegConvention = BusinessDayConvention.ModifiedFollowing
    sw.swFixedLegDayCounter = DayCounter.Thirty360
    sw.floatIndex = idx_ref
    sw.spread = 0.0
    sw.fwdStartDays = 0
    pw2 = PointsWrapperT()
    pw2.pointType = Point.SwapHelper
    pw2.point = sw
    ts.points.append(pw2)
    print("  ✓ Curve with SwapHelper.floatIndex built")
    
    # Build Pricing
    pricing = PricingT()
    pricing.asOfDate = EVAL_DATE_STR
    pricing.settlementDate = EVAL_DATE_STR
    pricing.indices = [idx_def]
    pricing.curves = [ts]
    pricing.bondPricingDetails = True
    print(f"  ✓ PricingT built with {len(pricing.indices)} indices, {len(pricing.curves)} curves")
    
    # Build bond
    sch = ScheduleT()
    sch.effectiveDate = "2024-01-15"
    sch.terminationDate = "2029-01-15"
    sch.calendar = Calendar.TARGET
    sch.frequency = Frequency.Annual
    sch.convention = BusinessDayConvention.Unadjusted
    sch.terminationDateConvention = BusinessDayConvention.Unadjusted
    sch.dateGenerationRule = DateGenerationRule.Backward
    sch.endOfMonth = False
    
    bond = FixedRateBondT()
    bond.settlementDays = 2
    bond.faceAmount = 100.0
    bond.schedule = sch
    bond.rate = 0.05
    bond.accrualDayCounter = DayCounter.ActualActual
    bond.issueDate = "2024-01-15"
    bond.redemption = 100.0
    bond.paymentConvention = BusinessDayConvention.Unadjusted
    
    yld = YieldT()
    yld.dayCounter = DayCounter.Actual360
    yld.compounding = Compounding.Compounded
    yld.frequency = Frequency.Annual
    
    price_bond = PriceFixedRateBondT()
    price_bond.fixedRateBond = bond
    price_bond.discountingCurve = "discount"
    price_bond.yield_ = yld
    
    req = PriceFixedRateBondRequestT()
    req.pricing = pricing
    req.bonds = [price_bond]
    print("  ✓ Full request built")
    
    # Try serializing
    import flatbuffers
    builder = flatbuffers.Builder(4096)
    packed = req.Pack(builder)
    builder.Finish(packed)
    request_bytes = bytes(builder.Output())
    print(f"  ✓ Serialized to {len(request_bytes)} bytes")
    
    # Try sending to server
    import grpc
    channel = grpc.insecure_channel("localhost:50051")
    unary_call = channel.unary_unary(
        '/quantra.QuantraServer/PriceFixedRateBond',
        request_serializer=lambda x: x,
        response_deserializer=lambda x: x,
    )
    
    try:
        response_bytes = unary_call(request_bytes, timeout=5)
        from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse
        resp = PriceFixedRateBondResponse.GetRootAs(response_bytes, 0)
        npv = resp.Bonds(0).Npv()
        print(f"  ✓ Server returned NPV = {npv}")
    except grpc.RpcError as e:
        print(f"  ✗ gRPC error: {e.code()} - {e.details()}")
    finally:
        channel.close()

except Exception as e:
    print(f"  ✗ Error: {e}")
    traceback.print_exc()

print()
print("=" * 60)
print("DONE")
print("=" * 60)