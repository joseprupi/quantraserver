import sys
import time
import argparse
from client_wrapper import Client

# Generated Objects
from quantra.Pricing import PricingT
from quantra.TermStructure import TermStructureT
from quantra.Point import Point
from quantra.PointsWrapper import PointsWrapperT
from quantra.DepositHelper import DepositHelperT
from quantra.BondHelper import BondHelperT
from quantra.FixedRateBond import FixedRateBondT
from quantra.Schedule import ScheduleT
from quantra.PriceFixedRateBond import PriceFixedRateBondT
from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT
from quantra.Yield import YieldT

# Enums
from quantra.enums.TimeUnit import TimeUnit
from quantra.enums.Calendar import Calendar
from quantra.enums.BusinessDayConvention import BusinessDayConvention
from quantra.enums.DayCounter import DayCounter
from quantra.enums.Frequency import Frequency
from quantra.enums.DateGenerationRule import DateGenerationRule
from quantra.enums.Interpolator import Interpolator
from quantra.enums.BootstrapTrait import BootstrapTrait
from quantra.enums.Compounding import Compounding

def create_market_data():
    """
    Replicates the C++ 'build_bond_engine' exactly:
    - 3 Deposits (3M, 6M, 1Y)
    - 5 Bonds (2005-2038 maturities)
    - LogLinear Interpolation
    """
    points = []

    # --- 1. DEPOSITS ---
    # Rates: 0.0096, 0.0145, 0.0194
    dep_data = [
        (0.0096, 3, TimeUnit.Months),
        (0.0145, 6, TimeUnit.Months),
        (0.0194, 1, TimeUnit.Years)
    ]

    for rate, n, unit in dep_data:
        dep = DepositHelperT()
        dep.rate = rate
        dep.tenorNumber = n
        dep.tenorTimeUnit = unit
        dep.fixingDays = 3
        dep.calendar = Calendar.TARGET
        dep.businessDayConvention = BusinessDayConvention.ModifiedFollowing
        dep.dayCounter = DayCounter.Actual365Fixed
        
        wrapper = PointsWrapperT()
        wrapper.point = dep
        wrapper.pointType = Point.DepositHelper
        points.append(wrapper)

    # --- 2. BOND HELPERS ---
    # Matching the C++ arrays: issueDates, maturities, couponRates, marketQuotes
    bond_data = [
        # Issue, Maturity, Coupon, Quote
        ("2005/03/15", "2010/08/31", 0.02375, 100.390625),
        ("2005/06/15", "2011/08/31", 0.04625, 106.21875),
        ("2006/06/30", "2013/08/31", 0.03125, 100.59375),
        ("2002/11/15", "2018/08/15", 0.04000, 101.6875),
        ("1987/05/15", "2038/05/15", 0.04500, 102.140625)
    ]

    for issue, mat, cpn, quote in bond_data:
        sched = ScheduleT()
        sched.effectiveDate = issue
        sched.terminationDate = mat
        sched.frequency = Frequency.Semiannual
        sched.calendar = Calendar.UnitedStatesGovernmentBond
        sched.convention = BusinessDayConvention.Unadjusted
        sched.terminationDateConvention = BusinessDayConvention.Unadjusted
        sched.dateGenerationRule = DateGenerationRule.Backward
        sched.endOfMonth = False

        helper = BondHelperT()
        helper.rate = quote
        helper.settlementDays = 3
        helper.faceAmount = 100.0
        helper.schedule = sched
        helper.couponRate = cpn
        helper.dayCounter = DayCounter.ActualActualBond
        helper.businessDayConvention = BusinessDayConvention.Unadjusted
        helper.redemption = 100.0
        helper.issueDate = issue

        wrapper = PointsWrapperT()
        wrapper.point = helper
        wrapper.pointType = Point.BondHelper
        points.append(wrapper)

    # --- 3. CURVE DEFINITION ---
    curve = TermStructureT()
    curve.id = "depos"
    curve.referenceDate = "2008/09/18"
    curve.dayCounter = DayCounter.ActualActualISDA # Matching C++
    curve.interpolator = Interpolator.LogLinear    # Matching C++
    curve.bootstrapTrait = BootstrapTrait.Discount
    curve.points = points
    
    return curve

def create_target_bond():
    """The specific bond being priced in the C++ 'price_bond_quantlib' function"""
    schedule = ScheduleT()
    schedule.effectiveDate = "2007/05/15"
    schedule.terminationDate = "2017/05/15"
    schedule.frequency = Frequency.Semiannual
    schedule.calendar = Calendar.UnitedStatesGovernmentBond
    schedule.convention = BusinessDayConvention.Unadjusted
    schedule.terminationDateConvention = BusinessDayConvention.Unadjusted
    schedule.dateGenerationRule = DateGenerationRule.Backward
    schedule.endOfMonth = False

    bond = FixedRateBondT()
    bond.issueDate = "2007/05/15"
    bond.faceAmount = 100.0
    bond.rate = 0.045
    bond.accrualDayCounter = DayCounter.ActualActualBond
    bond.paymentConvention = BusinessDayConvention.ModifiedFollowing
    bond.redemption = 100.0
    bond.settlementDays = 3
    bond.schedule = schedule
    
    return bond

def run_benchmark(bonds_per_request, num_requests, share_curve):
    print("=" * 70)
    print("Quantra Python Client Benchmark (Compliant)")
    print("=" * 70)
    print(f"  Bonds per request: {bonds_per_request}")
    print(f"  Requests:          {num_requests}")
    print(f"  Share Curve:       {'Yes (1 curve)' if share_curve else 'No (N copies)'}")
    print("=" * 70)

    client = Client(target="localhost:50051")
    
    # 1. Prepare Data
    curve = create_market_data()
    bond = create_target_bond()
    
    # Yield configuration (optional in C++ benchmark, but good for completeness)
    yield_def = YieldT()
    yield_def.dayCounter = DayCounter.Actual360
    yield_def.compounding = Compounding.Compounded
    yield_def.frequency = Frequency.Annual

    # 2. Build Request Structure
    # If share_curve=1: We send ONE curve in the 'curves' list.
    # If share_curve=0: We send N copies of the curve (one per bond) to force processing overhead.
    
    curves_list = []
    if share_curve:
        curves_list = [curve]
    else:
        # C++ Logic: "if (share_curve == 0) term_structures.push_back(term_structure)"
        # We simulate the heavy payload by duplicating the curve definition
        # Note: We give them unique IDs to ensure the server doesn't just cache the first one!
        for i in range(bonds_per_request):
            # Shallow copy is fine, but we need unique IDs if we really want to force re-bootstrapping
            # However, to strictly match C++ which pushes the SAME pointer, we just push the same object.
            # If the server is smart, it might cache it. If we want to be mean, we change the ID.
            curves_list.append(curve)

    bond_requests = []
    for _ in range(bonds_per_request):
        req = PriceFixedRateBondT()
        req.fixedRateBond = bond
        req.discountingCurve = "depos" 
        req.yield_ = yield_def
        bond_requests.append(req)

    full_request = PriceFixedRateBondRequestT()
    full_request.pricing = PricingT()
    full_request.pricing.asOfDate = "2008/09/18"
    full_request.pricing.settlementDate = "2008/09/18"
    full_request.pricing.curves = curves_list
    full_request.bonds = bond_requests

    # 3. Execution
    start_time = time.time()
    total_processed = 0
    errors = 0
    total_npv = 0.0

    print("Sending requests...")
    for i in range(num_requests):
        try:
            response = client.price_fixed_rate_bonds(full_request)
            count = response.BondsLength()
            total_processed += count
            
            # Sum NPVs to verify correctness
            for j in range(count):
                total_npv += response.Bonds(j).Npv()
                
        except Exception as e:
            print(f"Request {i} failed: {e}")
            errors += 1

    elapsed = time.time() - start_time
    if elapsed == 0: elapsed = 0.0001
    
    throughput = total_processed / elapsed

    print("\n" + "=" * 70)
    print(f"RESULTS")
    print("=" * 70)
    print(f"Total Time:      {elapsed:.4f} seconds")
    print(f"Total Bonds:     {total_processed}")
    print(f"Throughput:      {throughput:.0f} bonds/sec")
    print(f"Total NPV:       {total_npv:.4f}")
    print(f"Errors:          {errors}")
    print("=" * 70)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("bonds", type=int, help="Bonds per request")
    parser.add_argument("requests", type=int, help="Number of requests")
    parser.add_argument("share", type=int, help="1=Share Curve, 0=Bootstrap per bond")
    args = parser.parse_args()
    
    run_benchmark(args.bonds, args.requests, args.share)