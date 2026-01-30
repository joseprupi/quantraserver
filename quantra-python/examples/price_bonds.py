#!/usr/bin/env python3
"""
Benchmark the Quantra Python client.
"""

import asyncio
import time
import sys
sys.path.insert(0, '/workspace/quantra-python')

from quantra_client import (
    Client,
    FixedRateBond,
    Schedule,
    TermStructure,
    DepositHelper,
    BondHelper,
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
    
    deposit_3m = DepositHelper(
        rate=0.0096,
        tenor_number=3,
        tenor_time_unit=TimeUnit.Months,
        fixing_days=3,
        calendar=Calendar.TARGET,
        business_day_convention=BusinessDayConvention.ModifiedFollowing,
        day_counter=DayCounter.Actual365Fixed,
    )
    
    deposit_6m = DepositHelper(
        rate=0.0145,
        tenor_number=6,
        tenor_time_unit=TimeUnit.Months,
        fixing_days=3,
        calendar=Calendar.TARGET,
        business_day_convention=BusinessDayConvention.ModifiedFollowing,
        day_counter=DayCounter.Actual365Fixed,
    )
    
    deposit_1y = DepositHelper(
        rate=0.0194,
        tenor_number=1,
        tenor_time_unit=TimeUnit.Years,
        fixing_days=3,
        calendar=Calendar.TARGET,
        business_day_convention=BusinessDayConvention.ModifiedFollowing,
        day_counter=DayCounter.Actual365Fixed,
    )
    
    bond_helpers = []
    issue_dates = ["2005/03/15", "2005/06/15", "2006/06/30", "2002/11/15", "1987/05/15"]
    maturities = ["2010/08/31", "2011/08/31", "2013/08/31", "2018/08/15", "2038/05/15"]
    coupon_rates = [0.02375, 0.04625, 0.03125, 0.04000, 0.04500]
    market_quotes = [100.390625, 106.21875, 100.59375, 101.6875, 102.140625]
    
    for i in range(5):
        schedule = Schedule(
            calendar=Calendar.UnitedStatesGovernmentBond,
            effective_date=issue_dates[i],
            termination_date=maturities[i],
            frequency=Frequency.Semiannual,
            convention=BusinessDayConvention.Unadjusted,
            termination_date_convention=BusinessDayConvention.Unadjusted,
            date_generation_rule=DateGenerationRule.Backward,
            end_of_month=False,
        )
        
        bond = BondHelper(
            rate=market_quotes[i],
            settlement_days=3,
            face_amount=100.0,
            schedule=schedule,
            coupon_rate=coupon_rates[i],
            day_counter=DayCounter.ActualActualBond,
            business_day_convention=BusinessDayConvention.Unadjusted,
            redemption=100.0,
            issue_date=issue_dates[i],
        )
        bond_helpers.append(bond)
    
    points = [deposit_3m, deposit_6m, deposit_1y] + bond_helpers
    
    return TermStructure(
        id="depos",
        day_counter=DayCounter.ActualActual365,
        interpolator=Interpolator.LogLinear,
        bootstrap_trait=BootstrapTrait.Discount,
        reference_date="2008/09/18",
        points=points,
    )


def create_fixed_rate_bond() -> FixedRateBond:
    """Create a sample fixed rate bond."""
    
    schedule = Schedule(
        calendar=Calendar.UnitedStatesGovernmentBond,
        effective_date="2007/05/15",
        termination_date="2017/05/15",
        frequency=Frequency.Semiannual,
        convention=BusinessDayConvention.Unadjusted,
        termination_date_convention=BusinessDayConvention.Unadjusted,
        date_generation_rule=DateGenerationRule.Backward,
        end_of_month=False,
    )
    
    return FixedRateBond(
        settlement_days=3,
        face_amount=100.0,
        rate=0.045,
        accrual_day_counter=DayCounter.ActualActualBond,
        payment_convention=BusinessDayConvention.ModifiedFollowing,
        redemption=100.0,
        issue_date="2007/05/15",
        schedule=schedule,
    )


async def benchmark(num_bonds: int, num_requests: int):
    """Run benchmark."""
    
    curve = create_term_structure()
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
        
        # Calculate total NPV
        total_npv = sum(r.get('npv', 0) or 0 for r in results)
        errors = sum(1 for r in results if 'error' in r)
        
        return {
            'bonds': num_bonds,
            'requests': num_requests,
            'elapsed_ms': elapsed_ms,
            'throughput': num_bonds / elapsed_ms * 1000,
            'total_npv': total_npv,
            'errors': errors,
        }


async def main():
    print("=" * 70)
    print("Quantra Python Client Benchmark")
    print("=" * 70)
    
    # Test configurations
    configs = [
        (100, 10),
        (1000, 10),
        (1000, 1),   # Single request for comparison
        #(100000, 10),
    ]
    
    for num_bonds, num_requests in configs:
        result = await benchmark(num_bonds, num_requests)
        
        print(f"\nBonds: {result['bonds']:5d} | "
              f"Requests: {result['requests']:2d} | "
              f"Time: {result['elapsed_ms']:7.1f}ms | "
              f"Throughput: {result['throughput']:7.0f} bonds/sec | "
              f"NPV: {result['total_npv']:.2f}")
        
        if result['errors']:
            print(f"  WARNING: {result['errors']} errors")
    
    print("\n" + "=" * 70)
    print("Compare with C++ benchmark:")
    print("  cd /workspace/build && ./tests/benchmark 100 10 0")
    print("=" * 70)


if __name__ == "__main__":
    asyncio.run(main())