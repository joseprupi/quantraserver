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
6. Optional --sweep 1,2,4,8,12,24 runs the FULL identical workload once per
   client-concurrency level against the same server and prints a scaling
   table (wall time, req/s, avg latency, speedup vs the first level) plus a
   check that the risk numbers agree across levels.

Requires a running QuantraServer JSON API (v0.6.0+ wire contract) and the
`requests` package. Everything else is the Python standard library.

Usage:
    python3 historical_var.py --url http://localhost:8080
    python3 historical_var.py --swaps 100 --scenarios 250 --concurrency 12
    python3 historical_var.py --history-csv my_pillar_moves_bp.csv --json out.json
    python3 historical_var.py --sweep 1,2,4,8,12,24
    python3 historical_var.py --dump-inputs work/ --json work/result.json
    python3 historical_var.py --compare-native work/native_results.csv

The last two support the raw-QuantLib cross-check (see README section
"Cross-checking against raw QuantLib" and native_var_check.cpp).
"""

import argparse
import csv
import json
import math
import os
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


def pillar_labels() -> list:
    """Human-readable pillar tenor labels, e.g. 1M, 3M, ..., 30Y."""
    return [f"{n}{'M' if unit == 'Months' else 'Y'}" for (n, unit, *_rest) in PILLARS]


def dump_inputs(dir_path: str, portfolio: list, base_rates: list,
                scenarios: list) -> None:
    """Write swaps.csv + quotes.csv for the native QuantLib cross-check.

    swaps.csv: one row per swap — tenor_years, is_payer (1/0), notional,
    fixed_rate — exactly the four degrees of freedom of a book trade (every
    other swap field is a fixed convention mirrored by native_var_check.cpp).
    quotes.csv: header = pillar tenor labels; first data row = the base par
    quotes actually priced (epsilon included, if any), then one row per
    scenario with that scenario's full shocked quote vector.

    All numbers are written with repr() (full double precision) so the native
    program prices bit-identical inputs.
    """
    os.makedirs(dir_path, exist_ok=True)
    with open(os.path.join(dir_path, "swaps.csv"), "w") as f:
        f.write("tenor_years,is_payer,notional,fixed_rate\n")
        for trade in portfolio:
            swap = trade["ois_swap"]
            fixed = swap["fixed_leg"]
            # make_ois_trade sets termination to (start_year + tenor)-01-17.
            tenor_years = (int(fixed["schedule"]["termination_date"][:4])
                           - int(SPOT_DATE[:4]))
            is_payer = 1 if swap["swap_type"] == "Payer" else 0
            f.write(f"{tenor_years},{is_payer},{fixed['notional']!r},"
                    f"{fixed['rate']!r}\n")
    with open(os.path.join(dir_path, "quotes.csv"), "w") as f:
        f.write(",".join(pillar_labels()) + "\n")
        f.write(",".join(repr(r) for r in base_rates) + "\n")
        for bumps in scenarios:
            # Same float arithmetic as price_scenario, so identical doubles.
            f.write(",".join(repr(r + b)
                             for r, b in zip(base_rates, bumps)) + "\n")


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
            direction: str = "mixed", vol_scale: float = 1.0,
            quote_epsilon: float = 0.0, dump_inputs_dir: str = None) -> dict:
    """Full historical-simulation VaR run. Returns a machine-readable dict.

    `quote_epsilon` is added to EVERY par quote (base curve and all scenario
    curves alike). The sweep mode uses a distinct epsilon per concurrency
    level as a cache-poisoning guard: the server's curve cache is keyed on
    the curve's content, so re-running byte-identical scenarios would let
    later sweep levels skip the bootstrap and look artificially fast. A
    per-level epsilon of ~1e-9 (1e-5 bp) changes every cache key, forcing an
    honest re-bootstrap, while being economically negligible — and because
    the base valuation carries the same epsilon, it cancels out of the P&L
    to first order, so VaR/ES agree across levels to well under $1.
    """
    portfolio = build_portfolio(n_swaps, seed, direction)

    if history_csv:
        scenarios = load_history_csv(history_csv)
        synthetic = False
    else:
        # Distinct stream from the portfolio draw so --swaps does not reshuffle
        # the history and vice versa.
        scenarios = synthetic_history(n_scenarios, seed + 1, vol_scale)
        synthetic = True

    base_rates = [r + quote_epsilon for r in BASE_RATES]
    if dump_inputs_dir:
        dump_inputs(dump_inputs_dir, portfolio, base_rates, scenarios)

    t_start = time.perf_counter()

    # Base (reference) book value first: fails fast on any wire/market problem.
    base_value, base_latency = price_book(
        url, build_request(portfolio, base_rates), n_swaps
    )

    # One request per scenario: full shocked quote vector + the whole book.
    def price_scenario(bumps):
        shocked = [r + b for r, b in zip(base_rates, bumps)]
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
            "quote_epsilon": quote_epsilon,
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
# Scaling sweep: the full identical workload once per concurrency level.
# --------------------------------------------------------------------------

# Sweep levels must agree on VaR to well under this (USD). The per-level
# quote epsilon (~1e-9 on each rate) cancels out of P&L to first order, so
# any real disagreement here would mean parallelism changed the numbers.
SWEEP_VAR_TOLERANCE_USD = 1.0


def run_sweep(url: str, levels: list, n_swaps: int = 100,
              n_scenarios: int = 250, seed: int = 42, history_csv: str = None,
              direction: str = "mixed", vol_scale: float = 1.0) -> dict:
    """Run the FULL identical workload once per client-concurrency level.

    One server, one workload, several client fan-out widths — the resulting
    table is the sequential-vs-full-fleet comparison. Levels run in the
    order given.

    Cache-poisoning guard: each level adds its own deterministic epsilon
    ((position+1) * 1e-9) to every par quote. The server's curve cache is
    content-keyed, so byte-identical re-runs would be served from cache and
    make later levels artificially fast; the per-level epsilon changes every
    cache key so every level re-bootstraps honestly. It is economically
    negligible and cancels out of P&L (see run_var), which we verify: VaR
    must agree across levels to well under $1 or this raises.
    """
    if not levels:
        raise ValueError("--sweep needs at least one concurrency level")
    if any(lv < 1 for lv in levels):
        raise ValueError("sweep concurrency levels must be >= 1")

    runs = []
    for pos, level in enumerate(levels):
        runs.append(run_var(
            url=url, n_swaps=n_swaps, n_scenarios=n_scenarios, seed=seed,
            concurrency=level, history_csv=history_csv, direction=direction,
            vol_scale=vol_scale, quote_epsilon=(pos + 1) * 1e-9,
        ))

    first_wall = runs[0]["timing"]["scenario_wall_s"]
    table = []
    for level, r in zip(levels, runs):
        t = r["timing"]
        table.append({
            "concurrency": level,
            "wall_s": t["scenario_wall_s"],
            "requests_per_s": t["requests_per_s"],
            "avg_latency_ms": t["avg_latency_ms"],
            "speedup": first_wall / t["scenario_wall_s"]
            if t["scenario_wall_s"] > 0 else 0.0,
        })

    vars_ = [r["var_99"] for r in runs]
    ess = [r["es_99"] for r in runs]
    max_var_delta = max(vars_) - min(vars_)
    max_es_delta = max(ess) - min(ess)
    if not (max_var_delta < SWEEP_VAR_TOLERANCE_USD):
        raise RuntimeError(
            f"sweep levels disagree on VaR by ${max_var_delta:,.2f} "
            f"(tolerance ${SWEEP_VAR_TOLERANCE_USD:,.2f}) — parallelism must "
            "not change the numbers"
        )

    return {
        "mode": "sweep",
        "config": {
            "url": url,
            "swaps": n_swaps,
            "scenarios": runs[0]["config"]["scenarios"],
            "seed": seed,
            "levels": list(levels),
            "direction": direction,
            "vol_scale": vol_scale,
            "history_csv": history_csv,
            "as_of_date": AS_OF_DATE,
        },
        "sweep_table": table,
        "identical_check": {
            "var_99_by_level": vars_,
            "es_99_by_level": ess,
            "max_var_delta": max_var_delta,
            "max_es_delta": max_es_delta,
            "tolerance_usd": SWEEP_VAR_TOLERANCE_USD,
        },
        # Risk numbers reported once, from the first level: every level prices
        # the same book over the same history (modulo the ~1e-9 epsilon).
        "risk": runs[0],
    }


# --------------------------------------------------------------------------
# Native QuantLib cross-check (see native_var_check.cpp).
# --------------------------------------------------------------------------


def load_native_results(path: str) -> tuple:
    """Read native_var_check.cpp output: first line = base book value, then
    one P&L per scenario. Returns (base_value, pnl_list)."""
    values = []
    try:
        with open(path) as f:
            for line_num, line in enumerate(f, start=1):
                cell = line.strip()
                if not cell:
                    continue
                try:
                    values.append(float(cell))
                except ValueError:
                    raise RuntimeError(
                        f"{path}:{line_num}: non-numeric line {cell!r}")
    except OSError as exc:
        raise RuntimeError(f"cannot read native results {path}: {exc}")
    if len(values) < 3:
        raise RuntimeError(
            f"{path}: expected a base value plus at least 2 scenario P&Ls, "
            f"got {len(values)} numbers")
    return values[0], values[1:]


def compare_native(result: dict, path: str) -> dict:
    """Compare this run's results against a native_var_check.cpp CSV.

    The native program prices the SAME dumped inputs with direct QuantLib
    calls (no server), so any difference beyond double round-tripping through
    JSON means a pricing-convention divergence.
    """
    base_native, pnl_native = load_native_results(path)
    pnl = result["pnl"]
    if len(pnl_native) != len(pnl):
        raise RuntimeError(
            f"native results have {len(pnl_native)} scenario P&Ls but this "
            f"run produced {len(pnl)} — compare against the SAME inputs "
            "(same --swaps/--scenarios/--seed as the --dump-inputs run)")
    diffs = [abs(a - b) for a, b in zip(pnl, pnl_native)]
    native_stats = tail_stats(pnl_native)
    return {
        "native_csv": path,
        "scenarios": len(pnl),
        "base_value_server": result["base_value"],
        "base_value_native": base_native,
        "base_value_diff": result["base_value"] - base_native,
        "max_abs_pnl_diff": max(diffs),
        "mean_abs_pnl_diff": sum(diffs) / len(diffs),
        "var_99_server": result["var_99"],
        "var_99_native": native_stats["var"],
        "var_99_diff": result["var_99"] - native_stats["var"],
        "es_99_server": result["es_99"],
        "es_99_native": native_stats["es"],
        "es_99_diff": result["es_99"] - native_stats["es"],
    }


def print_native_comparison(c: dict) -> None:
    print()
    print("Native QuantLib cross-check (direct QuantLib calls, no server)")
    print("=" * 70)
    print(f"Native results : {c['native_csv']} ({c['scenarios']} scenarios)")
    print(f"Base book value: server {c['base_value_server']:,.6f}  "
          f"native {c['base_value_native']:,.6f}  "
          f"diff {c['base_value_diff']:+.6g}")
    print(f"Scenario P&L   : max |diff| {c['max_abs_pnl_diff']:.6g}, "
          f"mean |diff| {c['mean_abs_pnl_diff']:.6g}")
    print(f"99% 1-day VaR  : server {c['var_99_server']:,.6f}  "
          f"native {c['var_99_native']:,.6f}  diff {c['var_99_diff']:+.6g}")
    print(f"99% ES         : server {c['es_99_server']:,.6f}  "
          f"native {c['es_99_native']:,.6f}  diff {c['es_99_diff']:+.6g}")


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


def print_sweep_report(r: dict) -> None:
    cfg, risk = r["config"], r["risk"]
    check = r["identical_check"]
    print()
    print("Scaling sweep — same workload, one server, rising client concurrency")
    print("=" * 70)
    print(f"Portfolio      : {cfg['swaps']} SOFR OIS swaps "
          f"({cfg['direction']}, seed {cfg['seed']})")
    print(f"Workload/level : {cfg['scenarios']} scenario requests + 1 base "
          "(full re-bootstrap + book revaluation each)")
    print("Cache guard    : per-level ~1e-9 quote epsilon defeats the "
          "content-keyed curve cache")
    print()
    print(f"{'concurrency':>11} | {'wall time':>9} | {'req/s':>7} | "
          f"{'avg latency':>11} | {'speedup vs level-1':>18}")
    print(f"{'-' * 11} | {'-' * 9} | {'-' * 7} | {'-' * 11} | {'-' * 18}")
    for row in r["sweep_table"]:
        print(f"{row['concurrency']:>11} | {row['wall_s']:>8.2f}s | "
              f"{row['requests_per_s']:>7.1f} | "
              f"{row['avg_latency_ms']:>9.0f}ms | {row['speedup']:>17.2f}x")
    print("-" * 70)
    print(f"Base book value: {fmt_usd(risk['base_value'])}")
    print(f"99% 1-day VaR  : {fmt_usd(risk['var_99'])}")
    print(f"99% ES         : {fmt_usd(risk['es_99'])}")
    print(f"Worst scenario : {fmt_usd(risk['worst_pnl'])} P&L "
          f"(#{risk['worst_scenario_index']})")
    print(f"Best scenario  : {fmt_usd(risk['best_pnl'])} P&L "
          f"(#{risk['best_scenario_index']})")
    print()
    print(f"results identical across levels: max VaR delta "
          f"${check['max_var_delta']:,.6f} "
          f"(ES delta ${check['max_es_delta']:,.6f}) — parallelism does not "
          "change the numbers")
    print()
    print("P&L distribution:")
    print(ascii_histogram(risk["pnl"]))
    print()
    print("Notes: every level re-prices the identical book over the identical")
    print("history; only the client fan-out width changes. Wall time is the")
    print("scenario fan-out. Keep each level at or below the server's")
    print("QUANTRA_WORKERS or the excess requests 504 on the gateway deadline.")


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
    parser.add_argument("--concurrency", type=int, default=None,
                        help="parallel in-flight requests (default 12; keep at "
                             "or below the server's QUANTRA_WORKERS — the "
                             "gateway's 10s per-request deadline includes "
                             "queue time, so a deeper queue returns 504s; "
                             "mutually exclusive with --sweep)")
    parser.add_argument("--sweep", default=None, metavar="1,2,4,8,12,24",
                        help="comma-separated client concurrency levels: run "
                             "the FULL identical workload once per level (in "
                             "the order given) and print a scaling table; "
                             "mutually exclusive with --concurrency")
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
    parser.add_argument("--dump-inputs", dest="dump_inputs", default=None,
                        metavar="DIR",
                        help="write swaps.csv + quotes.csv (full precision) "
                             "for the native QuantLib cross-check "
                             "(native_var_check.cpp)")
    parser.add_argument("--compare-native", dest="compare_native", default=None,
                        metavar="CSV",
                        help="compare this run against a native_results.csv "
                             "produced by native_var_check.cpp on the SAME "
                             "dumped inputs")
    args = parser.parse_args(argv)

    if args.sweep is not None and args.concurrency is not None:
        parser.error("--sweep and --concurrency are mutually exclusive "
                     "(--sweep supplies its own per-level concurrency)")
    if args.sweep is not None and (args.dump_inputs or args.compare_native):
        parser.error("--dump-inputs/--compare-native target a single run; "
                     "they are mutually exclusive with --sweep")
    sweep_levels = None
    if args.sweep is not None:
        try:
            sweep_levels = [int(tok) for tok in args.sweep.split(",") if tok.strip()]
        except ValueError:
            parser.error(f"--sweep must be comma-separated integers, got "
                         f"{args.sweep!r}")
        if not sweep_levels or any(lv < 1 for lv in sweep_levels):
            parser.error("--sweep levels must be integers >= 1")

    try:
        if sweep_levels is not None:
            result = run_sweep(
                url=args.url,
                levels=sweep_levels,
                n_swaps=args.swaps,
                n_scenarios=args.scenarios,
                seed=args.seed,
                history_csv=args.history_csv,
                direction=args.direction,
                vol_scale=args.vol_scale,
            )
        else:
            result = run_var(
                url=args.url,
                n_swaps=args.swaps,
                n_scenarios=args.scenarios,
                seed=args.seed,
                concurrency=args.concurrency if args.concurrency is not None else 12,
                history_csv=args.history_csv,
                direction=args.direction,
                vol_scale=args.vol_scale,
                dump_inputs_dir=args.dump_inputs,
            )
            if args.compare_native:
                result["native_comparison"] = compare_native(
                    result, args.compare_native)
    except requests.ConnectionError:
        print(f"ERROR: cannot reach QuantraServer at {args.url} "
              "(is the server running?)", file=sys.stderr)
        return 1
    except (RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if sweep_levels is not None:
        print_sweep_report(result)
    else:
        print_report(result)
        if "native_comparison" in result:
            print_native_comparison(result["native_comparison"])
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(result, f, indent=2)
        print(f"\nFull results written to {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
