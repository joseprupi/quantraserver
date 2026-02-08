#!/usr/bin/env python3
"""
Quantra Curve Cache Benchmark

Modes:
    bond  - 1 curve, 8 helpers (3 deposits + 5 bonds)
    swap  - 2 EUR curves, 24 helpers, OIS dep chain
"""

import json, time, argparse, statistics, requests

# --- BOND: 1 curve, 8 helpers (3 deposits + 5 bonds) ---
BOND_REQUEST = {
    "pricing": {
        "as_of_date": "2008-09-15", "settlement_date": "2008-09-18",
        "curves": [{
            "id": "depos_curve", "day_counter": "ActualActualISDA", "interpolator": "LogLinear", "reference_date": "2008-09-18", "bootstrap_trait": "Discount",
            "points": [
                {"point_type": "DepositHelper", "point": {"rate": 0.0096, "tenor_time_unit": "Months", "tenor_number": 3, "fixing_days": 3, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed"}},
                {"point_type": "DepositHelper", "point": {"rate": 0.0145, "tenor_time_unit": "Months", "tenor_number": 6, "fixing_days": 3, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed"}},
                {"point_type": "DepositHelper", "point": {"rate": 0.0194, "tenor_time_unit": "Months", "tenor_number": 12, "fixing_days": 3, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed"}},
                {"point_type": "BondHelper", "point": {"rate": 100.390625, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2005/03/15", "termination_date": "2010/08/31", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.02375, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2005/03/15"}},
                {"point_type": "BondHelper", "point": {"rate": 106.21875, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2005/06/15", "termination_date": "2011/08/31", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.04625, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2005/06/15"}},
                {"point_type": "BondHelper", "point": {"rate": 100.59375, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2006/06/30", "termination_date": "2013/08/31", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.03125, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2006/06/30"}},
                {"point_type": "BondHelper", "point": {"rate": 101.6875, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2002/11/15", "termination_date": "2018/08/15", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.04, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2002/11/15"}},
                {"point_type": "BondHelper", "point": {"rate": 102.140625, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "1987/05/15", "termination_date": "2038/05/15", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.045, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "1987/05/15"}},
            ]
        }]
    },
    "bonds": [{
        "fixed_rate_bond": {
            "settlement_days": 3, "face_amount": 100, "rate": 0.045,
            "accrual_day_counter": "ActualActualBond", "payment_convention": "ModifiedFollowing",
            "redemption": 100, "issue_date": "2007/05/15",
            "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2007/05/15", "termination_date": "2017/05/15", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}
        },
        "discounting_curve": "depos_curve",
        "yield": {"day_counter": "Actual360", "compounding": "Compounded", "frequency": "Annual"}
    }]
}

# --- SWAP: EUR multicurve, 24 helpers (OIS + Euribor 6M with dep chain) ---
SWAP_REQUEST = {
    "pricing": {
        "as_of_date": "2024-01-15",
        "indices": [
            {"id": "EUR_6M", "name": "Euribor", "index_type": "Ibor", "tenor_number": 6, "tenor_time_unit": "Months", "fixing_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual360", "end_of_month": True, "currency": "EUR"},
            {"id": "EUR_ESTR", "name": "ESTR", "index_type": "Overnight", "tenor_number": 0, "tenor_time_unit": "Days", "fixing_days": 0, "calendar": "TARGET", "business_day_convention": "Following", "day_counter": "Actual360", "currency": "EUR"}
        ],
        "curves": [
            {
                "id": "EUR_OIS", "day_counter": "Actual365Fixed", "interpolator": "LogLinear", "bootstrap_trait": "Discount",
                "points": [
                    {"point_type": "DepositHelper", "point": {"rate": 0.0390, "tenor_number": 1, "tenor_time_unit": "Days", "fixing_days": 0, "calendar": "TARGET", "business_day_convention": "Following", "day_counter": "Actual360"}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0385, "tenor_number": 1,  "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0365, "tenor_number": 2,  "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0352, "tenor_number": 3,  "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0343, "tenor_number": 4,  "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0335, "tenor_number": 5,  "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0325, "tenor_number": 7,  "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0315, "tenor_number": 10, "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0305, "tenor_number": 15, "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0298, "tenor_number": 20, "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0288, "tenor_number": 30, "tenor_time_unit": "Years", "overnight_index": {"id": "EUR_ESTR"}}},
                ]
            },
            {
                "id": "EUR_6M_CURVE", "day_counter": "Actual365Fixed", "interpolator": "LogLinear", "bootstrap_trait": "Discount",
                "points": [
                    {"point_type": "DepositHelper", "point": {"rate": 0.0395, "tenor_number": 6, "tenor_time_unit": "Months", "fixing_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual360"}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0370, "tenor_number": 2,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0358, "tenor_number": 3,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0348, "tenor_number": 4,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0340, "tenor_number": 5,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0335, "tenor_number": 6,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0330, "tenor_number": 7,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0326, "tenor_number": 8,  "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0320, "tenor_number": 10, "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0316, "tenor_number": 12, "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0312, "tenor_number": 15, "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0308, "tenor_number": 20, "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0302, "tenor_number": 30, "tenor_time_unit": "Years", "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                ]
            }
        ]
    },
    "swaps": [{
        "vanilla_swap": {
            "swap_type": "Payer",
            "fixed_leg": {
                "schedule": {"calendar": "TARGET", "effective_date": "2024-01-17", "termination_date": "2034-01-17", "frequency": "Annual", "convention": "ModifiedFollowing", "termination_date_convention": "ModifiedFollowing", "date_generation_rule": "Forward", "end_of_month": False},
                "notional": 10000000, "rate": 0.032, "day_counter": "Thirty360", "payment_convention": "ModifiedFollowing"
            },
            "floating_leg": {
                "schedule": {"calendar": "TARGET", "effective_date": "2024-01-17", "termination_date": "2034-01-17", "frequency": "Semiannual", "convention": "ModifiedFollowing", "termination_date_convention": "ModifiedFollowing", "date_generation_rule": "Forward", "end_of_month": False},
                "notional": 10000000, "index": {"id": "EUR_6M"}, "spread": 0.0, "day_counter": "Actual360", "payment_convention": "ModifiedFollowing"
            }
        },
        "discounting_curve": "EUR_OIS",
        "forwarding_curve": "EUR_6M_CURVE"
    }]
}

# Mode config
MODES = {
    "bond": {"payload": BOND_REQUEST, "endpoint": "price-fixed-rate-bond", "npv_path": lambda r: r.get("bonds", [{}])[0].get("npv"), "desc": "1 curve, 8 helpers"},
    "swap": {"payload": SWAP_REQUEST, "endpoint": "price-vanilla-swap",    "npv_path": lambda r: r.get("swaps", [{}])[0].get("npv"), "desc": "EUR 2 curves, 24 helpers, OIS dep chain"},
}


# =============================================================================
# Benchmark runner
# =============================================================================

def run_benchmark(url: str, mode: str, n_requests: int = 100, warmup: int = 3) -> dict:
    cfg = MODES[mode]
    session = requests.Session()
    session.headers.update({"Content-Type": "application/json"})
    endpoint = f"{url.rstrip('/')}/{cfg['endpoint']}"
    payload = json.dumps(cfg["payload"])

    try:
        r = session.get(f"{url.rstrip('/')}/health", timeout=5)
        assert r.status_code == 200
    except Exception as e:
        print(f"ERROR: Cannot reach server at {url}: {e}")
        return None

    print(f"  Mode: {mode} ({cfg['desc']})")
    print(f"  Warming up ({warmup} requests)...")
    for i in range(warmup):
        r = session.post(endpoint, data=payload, timeout=30)
        if r.status_code != 200:
            print(f"  ERROR on warmup {i}: {r.status_code} - {r.text[:300]}")
            return None

    r = session.post(endpoint, data=payload, timeout=30)
    baseline_result = r.json()

    print(f"  Running {n_requests} requests...")
    latencies = []
    errors = 0

    for i in range(n_requests):
        t0 = time.perf_counter()
        r = session.post(endpoint, data=payload, timeout=30)
        t1 = time.perf_counter()
        if r.status_code == 200:
            latencies.append((t1 - t0) * 1000)
        else:
            errors += 1

    if not latencies:
        print("  ERROR: All requests failed")
        return None

    return {
        "n_requests": n_requests,
        "errors": errors,
        "latencies_ms": latencies,
        "mean_ms": statistics.mean(latencies),
        "median_ms": statistics.median(latencies),
        "p95_ms": sorted(latencies)[int(len(latencies) * 0.95)],
        "p99_ms": sorted(latencies)[int(len(latencies) * 0.99)],
        "min_ms": min(latencies),
        "max_ms": max(latencies),
        "stdev_ms": statistics.stdev(latencies) if len(latencies) > 1 else 0,
        "total_s": sum(latencies) / 1000,
        "throughput_rps": len(latencies) / (sum(latencies) / 1000),
        "result_sample": baseline_result,
    }


def print_results(tag: str, results: dict):
    print(f"\n{'='*60}")
    print(f"  {tag}")
    print(f"{'='*60}")
    print(f"  Requests:    {results['n_requests']} ({results['errors']} errors)")
    print(f"  Mean:        {results['mean_ms']:.2f} ms")
    print(f"  Median:      {results['median_ms']:.2f} ms")
    print(f"  P95:         {results['p95_ms']:.2f} ms")
    print(f"  P99:         {results['p99_ms']:.2f} ms")
    print(f"  Min:         {results['min_ms']:.2f} ms")
    print(f"  Max:         {results['max_ms']:.2f} ms")
    print(f"  Stdev:       {results['stdev_ms']:.2f} ms")
    print(f"  Throughput:  {results['throughput_rps']:.1f} req/s")
    print(f"  Total time:  {results['total_s']:.2f} s")
    print(f"{'='*60}")


def main():
    parser = argparse.ArgumentParser(description="Quantra curve cache benchmark")
    parser.add_argument("--url", default="http://localhost:8080")
    parser.add_argument("--tag", default="benchmark")
    parser.add_argument("-n", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--mode", choices=list(MODES.keys()), default="bond",
                        help="bond=1 curve/8 helpers, swap=EUR multicurve 24 helpers")
    args = parser.parse_args()

    print(f"\n--- {args.tag} ({args.url}) ---")
    results = run_benchmark(args.url, args.mode, args.n, args.warmup)
    if results:
        print_results(args.tag, results)
        outfile = f"bench_{args.tag.replace(' ', '_')}.json"
        with open(outfile, 'w') as f:
            json.dump({"tag": args.tag, "mode": args.mode,
                        "latencies_ms": results["latencies_ms"],
                        "mean_ms": results["mean_ms"],
                        "median_ms": results["median_ms"],
                        "p95_ms": results["p95_ms"]}, f, indent=2)
        print(f"\n  Raw data saved to: {outfile}")


if __name__ == "__main__":
    main()