#!/usr/bin/env python3
"""Historical-simulation VaR for a book of SOFR OIS swaps, priced by QuantraServer.

What this does
--------------
1. Builds a random (seeded, reproducible) portfolio of N vanilla USD SOFR OIS
   swaps: tenors 1..30Y, mixed payer/receiver, round-lot notionals, fixed rates
   near par so the book carries P&L.
2. Defines a base USD SOFR curve from 12 par pillars (1M..30Y, Discount trait,
   LogLinear) mirroring the conventions of the repo's SOFR curve examples
   (examples/data/curves/curves_usd_ois_sofr_*.json).
3. Generates historical-style daily bump vectors for the par quotes (all
   pillars move together: one common level factor plus small idiosyncratic
   noise). By default the history is SYNTHETIC; pass --history-csv to plug in
   real observed daily pillar moves.
4. Prices the book once on the base curve, then once per scenario. Each
   scenario is ONE request to /price-ois-swap carrying ALL N swaps and that
   scenario's full shocked quote set: the server bootstraps the scenario curve
   once and reprices the whole book against it (full QuantLib revaluation —
   no sensitivities, no approximations). Requests fan out across a thread pool.
5. Reports one-day 99% VaR (interpolated quantile), Expected Shortfall,
   best/worst scenarios, throughput, and an ASCII P&L histogram. --json writes
   the same numbers machine-readably.

Requires a running QuantraServer JSON API (v0.6.0+ wire contract) and the
`requests` package. Everything else is the Python standard library.

Usage:
    python3 historical_var.py --url http://localhost:8080
    python3 historical_var.py --swaps 100 --scenarios 250 --concurrency 12
    python3 historical_var.py --history-csv my_pillar_moves_bp.csv --json out.json
"""

import argparse
import csv
import json
import math
import random
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import requests

# --------------------------------------------------------------------------
# Market setup: dates, index and base curve (12 par pillars).
# Conventions mirror examples/data/curves/curves_usd_ois_sofr_*.json.
# --------------------------------------------------------------------------

AS_OF_DATE = "2025-01-15"     # valuation date
SPOT_DATE = "2025-01-17"      # T+2 effective date for the swaps
INDEX_ID = "USD_SOFR"
CURVE_ID = "USD_SOFR_OIS"
CALENDAR = "UnitedStatesGovernmentBond"

# (n, unit, years, base par rate, synthetic daily vol in bp)
# Vols: short end moves ~1-2bp/day, long end ~5-6bp/day.
PILLARS = [
    (1, "Months", 1.0 / 12.0, 0.0533, 1.2),
    (3, "Months", 0.25, 0.0530, 1.5),
    (6, "Months", 0.50, 0.0520, 1.8),
    (1, "Years", 1.0, 0.0495, 2.5),
    (2, "Years", 2.0, 0.0440, 3.5),
    (3, "Years", 3.0, 0.0405, 4.0),
    (5, "Years", 5.0, 0.0375, 4.5),
    (7, "Years", 7.0, 0.0365, 4.8),
    (10, "Years", 10.0, 0.0360, 5.0),
    (15, "Years", 15.0, 0.0358, 5.2),
    (20, "Years", 20.0, 0.0352, 5.4),
    (30, "Years", 30.0, 0.0332, 5.6),
]

BASE_RATES = [p[3] for p in PILLARS]

# Cross-pillar correlation of the synthetic history: each pillar's daily move
# is vol * (RHO * common_level_shock + sqrt(1-RHO^2) * own_noise).
RHO = 0.9


def sofr_index_def() -> dict:
    """USD SOFR overnight index definition (as in the SOFR curve examples)."""
    return {
        "id": INDEX_ID,
        "name": "SOFR",
        "index_type": "Overnight",
        "currency": "USD",
        "tenor": {"n": 0, "unit": "Days"},
        "fixing_days": 0,
        "calendar": CALENDAR,
        "day_counter": "Actual360",
        "business_day_convention": "ModifiedFollowing",
        "end_of_month": True,
    }


def sofr_curve(pillar_rates) -> dict:
    """USD SOFR OIS curve: Discount trait, LogLinear, one OISHelper per pillar.

    `pillar_rates` is the full par-quote vector for this curve build — the base
    rates for the reference valuation, or a shocked vector for a scenario.
    """
    points = []
    for (n, unit, _years, _rate, _vol), rate in zip(PILLARS, pillar_rates):
        points.append(
            {
                "point_type": "OISHelper",
                "point": {
                    "rate": rate,
                    "settlement_days": 2,
                    "overnight_index": {"id": INDEX_ID},
                    "tenor": {"n": n, "unit": unit},
                    "calendar": CALENDAR,
                    "fixed_leg_convention": "ModifiedFollowing",
                    "fixed_leg_frequency": "Annual",
                    "payment_lag": 2,
                    "averaging_method": "Compound",
                    "lookback_days": 0,
                    "lockout_days": 0,
                    "apply_observation_shift": False,
                },
            }
        )
    return {
        "id": CURVE_ID,
        "reference_date": AS_OF_DATE,
        "day_counter": "Actual365Fixed",
        "interpolator": "LogLinear",
        "bootstrap_trait": "Discount",
        "points": points,
    }


# --------------------------------------------------------------------------
# Portfolio: N vanilla SOFR OIS swaps.
# --------------------------------------------------------------------------


def par_rate(tenor_years: float) -> float:
    """Base par rate for a tenor, linearly interpolated across the pillars."""
    xs = [p[2] for p in PILLARS]
    if tenor_years <= xs[0]:
        return BASE_RATES[0]
    if tenor_years >= xs[-1]:
        return BASE_RATES[-1]
    for i in range(1, len(xs)):
        if tenor_years <= xs[i]:
            w = (tenor_years - xs[i - 1]) / (xs[i] - xs[i - 1])
            return BASE_RATES[i - 1] + w * (BASE_RATES[i] - BASE_RATES[i - 1])
    return BASE_RATES[-1]


def _schedule(termination_date: str) -> dict:
    return {
        "calendar": CALENDAR,
        "effective_date": SPOT_DATE,
        "termination_date": termination_date,
        "frequency": "Annual",
        "convention": "ModifiedFollowing",
        "termination_date_convention": "ModifiedFollowing",
        "date_generation_rule": "Forward",
        "end_of_month": False,
    }


def make_ois_trade(swap_type: str, notional: float, tenor_years: int,
                   fixed_rate: float) -> dict:
    """One vanilla SOFR OIS swap trade in the /price-ois-swap wire format."""
    termination = f"{2025 + tenor_years}-01-17"
    return {
        "ois_swap": {
            "swap_type": swap_type,
            "fixed_leg": {
                "notional": notional,
                "schedule": _schedule(termination),
                "rate": fixed_rate,
                "day_counter": "Actual360",
                "payment_convention": "ModifiedFollowing",
            },
            "overnight_leg": {
                "notional": notional,
                "schedule": _schedule(termination),
                "index": {"id": INDEX_ID},
                "spread": 0.0,
                "payment_convention": "ModifiedFollowing",
                "payment_calendar": CALENDAR,
                "averaging_method": "Compound",
                "payment_lag": 2,
                "lookback_days": 0,
                "lockout_days": 0,
                "apply_observation_shift": False,
                "telescopic_value_dates": False,
            },
        },
        "discounting_curve": CURVE_ID,
        "forwarding_curve": CURVE_ID,
    }


def build_portfolio(n_swaps: int, seed: int, direction: str = "mixed") -> list:
    """Seeded random book of N SOFR OIS swaps.

    Tenors 1..30Y, round-lot notionals 1..50M, fixed rates within +/-25bp of
    the base par rate for the tenor (so every trade carries P&L), payer or
    receiver per `direction` ("mixed" | "payer" | "receiver").
    """
    rng = random.Random(seed)
    trades = []
    for _ in range(n_swaps):
        tenor = rng.randint(1, 30)
        if direction == "mixed":
            swap_type = rng.choice(["Payer", "Receiver"])
        else:
            swap_type = "Payer" if direction == "payer" else "Receiver"
        notional = rng.randint(1, 50) * 1_000_000
        fixed = round(par_rate(tenor) + rng.uniform(-0.0025, 0.0025), 6)
        trades.append(make_ois_trade(swap_type, float(notional), tenor, fixed))
    return trades


# --------------------------------------------------------------------------
# Scenarios: daily bump vectors on the par quotes (decimal, not bp).
# --------------------------------------------------------------------------


def synthetic_history(n_scenarios: int, seed: int, vol_scale: float = 1.0) -> list:
    """Seeded synthetic daily pillar moves: correlated, realistic magnitudes.

    Each scenario is one 'day': a common level shock hits every pillar (scaled
    by that pillar's vol) plus a small idiosyncratic term. Returns a list of
    bump vectors in DECIMAL (1bp = 1e-4), one per scenario, pillar-ordered.
    """
    rng = random.Random(seed)
    scenarios = []
    for _ in range(n_scenarios):
        level = rng.gauss(0.0, 1.0)
        bumps = []
        for (_n, _u, _y, _r, vol_bp) in PILLARS:
            shock = RHO * level + math.sqrt(1.0 - RHO * RHO) * rng.gauss(0.0, 1.0)
            bumps.append(vol_scale * vol_bp * shock * 1e-4)
        scenarios.append(bumps)
    return scenarios


def load_history_csv(path: str) -> list:
    """Load real daily pillar moves: rows = days, columns = pillar bumps in bp.

    Columns must follow the pillar order (1M,3M,6M,1Y,2Y,3Y,5Y,7Y,10Y,15Y,
    20Y,30Y). A single non-numeric header row is skipped. Values are converted
    from bp to decimal.
    """
    scenarios = []
    with open(path, newline="") as f:
        for row_num, row in enumerate(csv.reader(f), start=1):
            cells = [c.strip() for c in row if c.strip() != ""]
            if not cells:
                continue
            try:
                values = [float(c) for c in cells]
            except ValueError:
                if row_num == 1:
                    continue  # header row
                raise ValueError(f"{path}:{row_num}: non-numeric cell in {row}")
            if len(values) != len(PILLARS):
                raise ValueError(
                    f"{path}:{row_num}: expected {len(PILLARS)} pillar columns "
                    f"(1M,3M,6M,1Y,2Y,3Y,5Y,7Y,10Y,15Y,20Y,30Y), got {len(values)}"
                )
            scenarios.append([v * 1e-4 for v in values])
    if not scenarios:
        raise ValueError(f"{path}: no scenario rows found")
    return scenarios


# --------------------------------------------------------------------------
# Pricing: one request per curve state, carrying the whole book.
# --------------------------------------------------------------------------


def build_request(portfolio: list, pillar_rates) -> dict:
    """Full /price-ois-swap request: one curve state + ALL swaps in the book.

    This is the design point: the server bootstraps this quote vector's curve
    once per request and prices every swap against it, so the bootstrap cost
    is amortized across the whole book.
    """
    return {
        "pricing": {
            "as_of_date": AS_OF_DATE,
            "rates": {
                "indices": [sofr_index_def()],
                "curves": [sofr_curve(pillar_rates)],
            },
        },
        "swaps": portfolio,
    }


_tls = threading.local()


def _session() -> requests.Session:
    """One keep-alive session per worker thread (requests.Session is not
    guaranteed thread-safe)."""
    s = getattr(_tls, "session", None)
    if s is None:
        s = requests.Session()
        s.headers.update({"Content-Type": "application/json"})
        _tls.session = s
    return s


def price_book(url: str, request: dict, expected_swaps: int) -> tuple:
    """POST one request; return (book value = sum of swap NPVs, latency seconds)."""
    t0 = time.perf_counter()
    resp = _session().post(f"{url.rstrip('/')}/price-ois-swap", json=request)
    latency = time.perf_counter() - t0
    if resp.status_code != 200:
        raise RuntimeError(
            f"/price-ois-swap returned {resp.status_code}: {resp.text[:300]}"
        )
    swaps = resp.json()["swaps"]
    if len(swaps) != expected_swaps:
        raise RuntimeError(f"expected {expected_swaps} swap NPVs, got {len(swaps)}")
    return sum(s.get("npv", 0.0) for s in swaps), latency


# --------------------------------------------------------------------------
# Risk statistics.
# --------------------------------------------------------------------------


def interpolated_quantile(sorted_vals: list, q: float) -> float:
    """Type-6 (Weibull) interpolated quantile of pre-sorted ascending values.

    pos = q*(n+1) - 1, clamped to the sample. For 250 scenarios at q=0.01 this
    lands between the 2nd and 3rd worst — the classic hist-sim convention.
    """
    n = len(sorted_vals)
    pos = q * (n + 1) - 1.0
    pos = min(max(pos, 0.0), n - 1.0)
    lo = int(math.floor(pos))
    hi = min(lo + 1, n - 1)
    frac = pos - lo
    return sorted_vals[lo] + frac * (sorted_vals[hi] - sorted_vals[lo])


def tail_stats(pnl: list, confidence: float = 0.99) -> dict:
    """One-day VaR / Expected Shortfall from a P&L vector (losses negative).

    VaR and ES are reported as positive loss amounts. ES is the average P&L of
    the scenarios at or beyond the VaR quantile (the tail mean).
    """
    if len(pnl) < 2:
        raise ValueError("need at least 2 scenarios for tail statistics")
    ordered = sorted(pnl)
    q_val = interpolated_quantile(ordered, 1.0 - confidence)
    tail = [p for p in ordered if p <= q_val] or [ordered[0]]
    return {
        "confidence": confidence,
        "var": -q_val,
        "es": -(sum(tail) / len(tail)),
        "tail_scenarios": len(tail),
        "worst_pnl": ordered[0],
        "second_worst_pnl": ordered[1],
        "best_pnl": ordered[-1],
    }


# --------------------------------------------------------------------------
# The run.
# --------------------------------------------------------------------------


def run_var(url: str, n_swaps: int = 100, n_scenarios: int = 250, seed: int = 42,
            concurrency: int = 12, history_csv: str = None,
            direction: str = "mixed", vol_scale: float = 1.0) -> dict:
    """Full historical-simulation VaR run. Returns a machine-readable dict."""
    portfolio = build_portfolio(n_swaps, seed, direction)

    if history_csv:
        scenarios = load_history_csv(history_csv)
        synthetic = False
    else:
        # Distinct stream from the portfolio draw so --swaps does not reshuffle
        # the history and vice versa.
        scenarios = synthetic_history(n_scenarios, seed + 1, vol_scale)
        synthetic = True

    t_start = time.perf_counter()

    # Base (reference) book value first: fails fast on any wire/market problem.
    base_value, base_latency = price_book(
        url, build_request(portfolio, BASE_RATES), n_swaps
    )

    # One request per scenario: full shocked quote vector + the whole book.
    def price_scenario(bumps):
        shocked = [r + b for r, b in zip(BASE_RATES, bumps)]
        return price_book(url, build_request(portfolio, shocked), n_swaps)

    t_fan = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        results = list(pool.map(price_scenario, scenarios))
    fan_wall = time.perf_counter() - t_fan
    total_wall = time.perf_counter() - t_start

    scenario_values = [v for v, _lat in results]
    latencies = [base_latency] + [lat for _v, lat in results]
    pnl = [v - base_value for v in scenario_values]
    stats = tail_stats(pnl)

    worst_idx = pnl.index(min(pnl))
    best_idx = pnl.index(max(pnl))

    return {
        "config": {
            "url": url,
            "swaps": n_swaps,
            "scenarios": len(scenarios),
            "seed": seed,
            "concurrency": concurrency,
            "direction": direction,
            "vol_scale": vol_scale,
            "history_csv": history_csv,
            "as_of_date": AS_OF_DATE,
        },
        "synthetic_history": synthetic,
        "base_value": base_value,
        "scenario_values": scenario_values,
        "pnl": pnl,
        "var_99": stats["var"],
        "es_99": stats["es"],
        "tail_scenarios": stats["tail_scenarios"],
        "worst_pnl": stats["worst_pnl"],
        "second_worst_pnl": stats["second_worst_pnl"],
        "best_pnl": stats["best_pnl"],
        "worst_scenario_index": worst_idx,
        "best_scenario_index": best_idx,
        "worst_scenario_avg_bump_bp":
            sum(scenarios[worst_idx]) / len(PILLARS) * 1e4,
        "best_scenario_avg_bump_bp":
            sum(scenarios[best_idx]) / len(PILLARS) * 1e4,
        "timing": {
            "total_wall_s": total_wall,
            "scenario_wall_s": fan_wall,
            "requests": len(scenarios) + 1,
            "requests_per_s": len(scenarios) / fan_wall if fan_wall > 0 else 0.0,
            "avg_latency_ms": 1000.0 * sum(latencies) / len(latencies),
        },
    }


# --------------------------------------------------------------------------
# Reporting.
# --------------------------------------------------------------------------


def fmt_usd(x: float) -> str:
    sign = "-" if x < 0 else ""
    a = abs(x)
    if a >= 1e6:
        return f"{sign}${a / 1e6:,.2f}M"
    if a >= 1e3:
        return f"{sign}${a / 1e3:,.1f}k"
    return f"{sign}${a:,.0f}"


def ascii_histogram(pnl: list, bins: int = 15, width: int = 50) -> str:
    lo, hi = min(pnl), max(pnl)
    if hi <= lo:
        return f"  all scenarios at {fmt_usd(lo)}"
    step = (hi - lo) / bins
    counts = [0] * bins
    for p in pnl:
        counts[min(int((p - lo) / step), bins - 1)] += 1
    peak = max(counts)
    lines = []
    for i, c in enumerate(counts):
        left, right = lo + i * step, lo + (i + 1) * step
        bar = "#" * max(1 if c else 0, round(c / peak * width))
        lines.append(f"  {fmt_usd(left):>10} .. {fmt_usd(right):>10} | {bar} ({c})")
    return "\n".join(lines)


def print_report(r: dict) -> None:
    cfg, t = r["config"], r["timing"]
    n = cfg["scenarios"]
    print()
    print("Historical-simulation VaR — SOFR OIS book, full QuantLib revaluation")
    print("=" * 70)
    if r["synthetic_history"]:
        print("History        : SYNTHETIC seeded daily moves "
              "(pass --history-csv for real data)")
    else:
        print(f"History        : {cfg['history_csv']} ({n} observed days)")
    print(f"Portfolio      : {cfg['swaps']} SOFR OIS swaps "
          f"({cfg['direction']}, seed {cfg['seed']})")
    print(f"Scenarios      : {n} one-day par-quote shocks, re-bootstrapped "
          "server-side")
    print(f"Requests       : {t['requests']} (1 base + {n} scenarios), "
          f"concurrency {cfg['concurrency']}")
    print(f"Wall time      : {t['total_wall_s']:.2f}s total "
          f"({t['scenario_wall_s']:.2f}s scenario fan-out)")
    print(f"Throughput     : {t['requests_per_s']:.1f} req/s, "
          f"avg latency {t['avg_latency_ms']:.0f}ms "
          f"(each request = 1 bootstrap + {cfg['swaps']} repricings)")
    print("-" * 70)
    print(f"Base book value: {fmt_usd(r['base_value'])}")
    print(f"99% 1-day VaR  : {fmt_usd(r['var_99'])}  "
          f"(interpolated quantile; for {n} scenarios this sits by the "
          f"second-worst loss, {fmt_usd(-r['second_worst_pnl'])})")
    print(f"99% ES         : {fmt_usd(r['es_99'])}  "
          f"(mean of the {r['tail_scenarios']} scenario(s) beyond VaR)")
    print(f"Worst scenario : {fmt_usd(r['worst_pnl'])} P&L "
          f"(#{r['worst_scenario_index']}, avg curve move "
          f"{r['worst_scenario_avg_bump_bp']:+.1f}bp)")
    print(f"Best scenario  : {fmt_usd(r['best_pnl'])} P&L "
          f"(#{r['best_scenario_index']}, avg curve move "
          f"{r['best_scenario_avg_bump_bp']:+.1f}bp)")
    print()
    print("P&L distribution:")
    print(ascii_histogram(r["pnl"]))
    print()
    print("Notes: every scenario is a full server-side curve re-bootstrap and")
    print("book revaluation through QuantLib — no Taylor/DV01 approximation.")
    print("Each scenario is a distinct curve, so the server's curve cache does")
    print("not accelerate this workload; throughput comes from parallel workers.")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Historical-simulation VaR for a SOFR OIS swap book "
                    "against a running QuantraServer JSON API.")
    parser.add_argument("--url", default="http://localhost:8080",
                        help="QuantraServer JSON API base URL")
    parser.add_argument("--swaps", type=int, default=100,
                        help="number of OIS swaps in the book (default 100)")
    parser.add_argument("--scenarios", type=int, default=250,
                        help="number of synthetic scenarios (default 250; "
                             "ignored when --history-csv is given)")
    parser.add_argument("--seed", type=int, default=42,
                        help="RNG seed for portfolio + synthetic history")
    parser.add_argument("--concurrency", type=int, default=12,
                        help="parallel in-flight requests (default 12; keep at "
                             "or below the server's QUANTRA_WORKERS — the "
                             "gateway's 10s per-request deadline includes "
                             "queue time, so a deeper queue returns 504s)")
    parser.add_argument("--history-csv", default=None,
                        help="CSV of real daily pillar moves in bp "
                             "(rows=days, 12 columns: 1M,3M,6M,1Y,2Y,3Y,5Y,"
                             "7Y,10Y,15Y,20Y,30Y)")
    parser.add_argument("--direction", choices=["mixed", "payer", "receiver"],
                        default="mixed",
                        help="book directionality (default mixed)")
    parser.add_argument("--vol-scale", type=float, default=1.0,
                        help="multiplier on the synthetic daily vols")
    parser.add_argument("--json", dest="json_out", default=None,
                        help="write full machine-readable results to this file")
    args = parser.parse_args(argv)

    try:
        result = run_var(
            url=args.url,
            n_swaps=args.swaps,
            n_scenarios=args.scenarios,
            seed=args.seed,
            concurrency=args.concurrency,
            history_csv=args.history_csv,
            direction=args.direction,
            vol_scale=args.vol_scale,
        )
    except requests.ConnectionError:
        print(f"ERROR: cannot reach QuantraServer at {args.url} "
              "(is the server running?)", file=sys.stderr)
        return 1
    except (RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print_report(result)
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(result, f, indent=2)
        print(f"\nFull results written to {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
