#!/usr/bin/env python3
"""
Example usage of the Quantra Python client with FlatBuffers T classes.
"""

import asyncio
import time
import sys
sys.path.insert(0, '/workspace/quantra-python')

from quantra_client.client_v2 import (
    Client,
    # T classes (aliased for convenience)
    FixedRateBond,
    Schedule,
    TermStructure,
    DepositHelper,
    BondHelper,
    # Helper functions for unions
    create_deposit_point,
    create_bond_point,
    # Enums
    DayCounter,
    Calendar,
    BusinessDayConvention,
    Frequency,
    TimeUnit,
    DateGenerationRule,
    Interpolator,
    BootstrapTrait,
)


def create_term_structure() -> TermStructure:
    """Create a sample term structure for discounting."""
    
    # Deposit helpers
    deposit_3m = DepositHelper()
    deposit_3m.rate = 0.0096
    deposit_3m.tenorNumber = 3
    deposit_3m.tenorTimeUnit = TimeUnit.Months
    deposit_3m.fixingDays = 3
    deposit_3m.calendar = Calendar.TARGET
    deposit_3m.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    deposit_3m.dayCounter = DayCounter.Actual365Fixed
    
    deposit_6m = DepositHelper()
    deposit_6m.rate = 0.0145
    deposit_6m.tenorNumber = 6
    deposit_6m.tenorTimeUnit = TimeUnit.Months
    deposit_6m.fixingDays = 3
    deposit_6m.calendar = Calendar.TARGET
    deposit_6m.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    deposit_6m.dayCounter = DayCounter.Actual365Fixed
    
    deposit_1y = DepositHelper()
    deposit_1y.rate = 0.0194
    deposit_1y.tenorNumber = 1
    deposit_1y.tenorTimeUnit = TimeUnit.Years
    deposit_1y.fixingDays = 3
    deposit_1y.calendar = Calendar.TARGET
    deposit_1y.businessDayConvention = BusinessDayConvention.ModifiedFollowing
    deposit_1y.dayCounter = DayCounter.Actual365Fixed
    
    # Bond helpers
    bond_helpers = []
    issue_dates = ["2005/03/15", "2005/06/15", "2006/06/30", "2002/11/15", "1987/05/15"]
    maturities = ["2010/08/31", "2011/08/31", "2013/08/31", "2018/08/15", "2038/05/15"]
    coupon_rates = [0.02375, 0.04625, 0.03125, 0.04000, 0.04500]
    market_quotes = [100.390625, 106.21875, 100.59375, 101.6875, 102.140625]
    
    for i in range(5):
        schedule = Schedule()
        schedule.calendar = Calendar.UnitedStatesGovernmentBond
        schedule.effectiveDate = issue_dates[i]
        schedule.terminationDate = maturities[i]
        schedule.frequency = Frequency.Semiannual
        schedule.convention = BusinessDayConvention.Unadjusted
        schedule.terminationDateConvention = BusinessDayConvention.Unadjusted
        schedule.dateGenerationRule = DateGenerationRule.Backward
        schedule.endOfMonth = False
        
        bond = BondHelper()
        bond.rate = market_quotes[i]
        bond.settlementDays = 3
        bond.faceAmount = 100.0
        bond.schedule = schedule
        bond.couponRate = coupon_rates[i]
        bond.dayCounter = DayCounter.ActualActualBond
        bond.businessDayConvention = BusinessDayConvention.Unadjusted
        bond.redemption = 100.0
        bond.issueDate = issue_dates[i]
        bond_helpers.append(bond)
    
    # Build term structure with points
    ts = TermStructure()
    ts.id = "depos"
    ts.dayCounter = DayCounter.ActualActual365
    ts.interpolator = Interpolator.LogLinear
    ts.bootstrapTrait = BootstrapTrait.Discount
    ts.referenceDate = "2008/09/18"
    ts.points = [
        create_deposit_point(deposit_3m),
        create_deposit_point(deposit_6m),
        create_deposit_point(deposit_1y),
    ] + [create_bond_point(b) for b in bond_helpers]
    
    return ts


def create_fixed_rate_bond() -> FixedRateBond:
    """Create a sample fixed rate bond."""
    
    schedule = Schedule()
    schedule.calendar = Calendar.UnitedStatesGovernmentBond
    schedule.effectiveDate = "2007/05/15"
    schedule.terminationDate = "2017/05/15"
    schedule.frequency = Frequency.Semiannual
    schedule.convention = BusinessDayConvention.Unadjusted
    schedule.terminationDateConvention = BusinessDayConvention.Unadjusted
    schedule.dateGenerationRule = DateGenerationRule.Backward
    schedule.endOfMonth = False
    
    bond = FixedRateBond()
    bond.settlementDays = 3
    bond.faceAmount = 100.0
    bond.rate = 0.045
    bond.accrualDayCounter = DayCounter.ActualActualBond
    bond.paymentConvention = BusinessDayConvention.ModifiedFollowing
    bond.redemption = 100.0
    bond.issueDate = "2007/05/15"
    bond.schedule = schedule
    
    return bond


async def main():
    print("=" * 70)
    print("Quantra Python Client (using FlatBuffers T classes)")
    print("=" * 70)
    
    # Create market data
    curve = create_term_structure()
    print(f"Created curve: {curve.id}")
    
    # Benchmark different sizes
    configs = [
        (100, 10),
        (1000, 10),
    ]
    
    for num_bonds, num_requests in configs:
        bonds = [create_fixed_rate_bond() for _ in range(num_bonds)]
        
        async with Client('localhost:50051') as client:
            start = time.perf_counter()
            
            results = await client.price_fixed_rate_bonds(
                bonds=bonds,
                curves=[curve],
                as_of_date='2008/09/18',
                settlement_date='2008/09/18',
                num_requests=num_requests,
                discount_curve='depos',
            )
            
            elapsed_ms = (time.perf_counter() - start) * 1000
            
            total_npv = sum(r.get('npv', 0) or 0 for r in results)
            errors = sum(1 for r in results if 'error' in r)
            
            print(f"\nBonds: {num_bonds:5d} | "
                  f"Requests: {num_requests:2d} | "
                  f"Time: {elapsed_ms:7.1f}ms | "
                  f"Throughput: {num_bonds / elapsed_ms * 1000:7.0f} bonds/sec | "
                  f"NPV: {total_npv:.2f}")
            
            if errors:
                print(f"  WARNING: {errors} errors")
                # Show first error with traceback
                for r in results:
                    if 'error' in r:
                        print(f"  Error: {r['error']}")
                        if 'traceback' in r:
                            print(r['traceback'])
                        break
    
    print("\n" + "=" * 70)


if __name__ == "__main__":
    asyncio.run(main())