import sys
import os

# --- Import our new Client Wrapper ---
from client_wrapper import Client

# --- Import Generated Data Structures ---
from quantra.Pricing import PricingT
from quantra.TermStructure import TermStructureT
from quantra.Point import Point
from quantra.PointsWrapper import PointsWrapperT
from quantra.DepositHelper import DepositHelperT
from quantra.FixedRateBond import FixedRateBondT
from quantra.Schedule import ScheduleT
from quantra.PriceFixedRateBond import PriceFixedRateBondT
from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT

# Enums
from quantra.enums.TimeUnit import TimeUnit
from quantra.enums.Calendar import Calendar
from quantra.enums.BusinessDayConvention import BusinessDayConvention
from quantra.enums.DayCounter import DayCounter
from quantra.enums.Frequency import Frequency
from quantra.enums.DateGenerationRule import DateGenerationRule
from quantra.enums.Interpolator import Interpolator
from quantra.enums.BootstrapTrait import BootstrapTrait

def run():
    print("Initializing Client...")
    client = Client(target="localhost:50051")

    # --- 1. Create Curve Data ---
    # Short term rate (6 Months)
    deposit_short = DepositHelperT()
    deposit_short.rate = 0.03
    deposit_short.tenorNumber = 6
    deposit_short.tenorTimeUnit = TimeUnit.Months
    deposit_short.fixingDays = 2
    deposit_short.calendar = Calendar.TARGET
    deposit_short.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    deposit_short.dayCounter = DayCounter.Actual360

    wrapper_short = PointsWrapperT()
    wrapper_short.point = deposit_short
    wrapper_short.pointType = Point.DepositHelper

    # Long term rate (30 Years) - Required for 2017 bond
    deposit_long = DepositHelperT()
    deposit_long.rate = 0.05
    deposit_long.tenorNumber = 30
    deposit_long.tenorTimeUnit = TimeUnit.Years
    deposit_long.fixingDays = 2
    deposit_long.calendar = Calendar.TARGET
    deposit_long.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    deposit_long.dayCounter = DayCounter.Actual360

    wrapper_long = PointsWrapperT()
    wrapper_long.point = deposit_long
    wrapper_long.pointType = Point.DepositHelper

    curve = TermStructureT()
    curve.id = "depos"
    curve.referenceDate = "2008/09/18"
    curve.dayCounter = DayCounter.Actual360
    curve.interpolator = Interpolator.Linear
    curve.bootstrapTrait = BootstrapTrait.Discount
    curve.points = [wrapper_short, wrapper_long]

    # --- 2. Create Bond Data (Matures 2017) ---
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

    # --- 3. Build Request ---
    pricing_config = PricingT()
    pricing_config.asOfDate = "2008/09/18"
    pricing_config.settlementDate = "2008/09/18"
    pricing_config.curves = [curve]

    price_request_item = PriceFixedRateBondT()
    price_request_item.fixedRateBond = bond
    price_request_item.discountingCurve = "depos"

    full_request = PriceFixedRateBondRequestT()
    full_request.pricing = pricing_config
    full_request.bonds = [price_request_item]

    # --- 4. Call Server and Read Response ---
    print("Sending Request...")
    try:
        # response here is the Raw Reader (because of our change in client_wrapper)
        response = client.price_fixed_rate_bonds(full_request)
        
        # Use Reader API (Capitalized Methods)
        num_bonds = response.BondsLength()
        print(f"Received {num_bonds} results.")

        for i in range(num_bonds):
            bond_res = response.Bonds(i)
            
            # Access fields via methods
            npv = bond_res.Npv()
            clean_price = bond_res.CleanPrice()
            dirty_price = bond_res.DirtyPrice()
            yld = bond_res.Yield() # Safe method name
            accrued = bond_res.AccruedAmount()

            print(f"\n✅ Bond {i+1} Pricing Successful:")
            print(f"   NPV:          {npv:.4f}")
            print(f"   Clean Price:  {clean_price:.4f}")
            print(f"   Dirty Price:  {dirty_price:.4f}")
            print(f"   Yield:        {yld:.4%}")
            print(f"   Accrued Amt:  {accrued:.4f}")
            
    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    run()