#!/usr/bin/env python3
"""
Quantra parallel-throughput benchmark — SINGLE-SHOT worker (informational).

This program measures exactly ONE worker count (or the linear baseline) per
invocation and then exits. It does NO teardown: the caller (run_throughput.sh)
runs each invocation in its own fresh `docker run --rm` container, so when the
process exits Docker tears down the whole PID namespace and reaps the cluster
(Envoy + workers + json_server) automatically. Nothing can leak or hang, and
every measurement starts from a clean slate.

Topology started per invocation (inside the quantraserver:test image, which
ships /usr/local/bin/envoy):

    scripts/quantra start --workers N --foreground   (run in the background)
        -> N sync_server workers on base_port=50055.. + an Envoy LEAST_REQUEST
           front on port 50051. --foreground keeps the manager alive so Envoy
           stays up for the whole run.
    json_server localhost:50051 8080
        -> one HTTP front pointed at the Envoy gRPC front; concurrent HTTP
           requests fan out across the N workers (Envoy load-balances per
           gRPC request over the channel).

We wait for Envoy /ready + one warmup 200, fire M requests concurrently
(ThreadPoolExecutor), and print machine-readable lines that the host driver
aggregates:

    RESULT scenario=<s> workers=<N> cache=<off|on|na> rps=<f> errors=<n> elapsed=<f>
    SPOTCHECK bond api=<f> ql=<f> rel=<e> ok=<0|1>      (scenario 1 only)

Scenarios:
  1 - N different yield curves: heavy EUR multicurve swap (2 curves / 24
      helpers) with a unique curve per request -> independent bootstrap per
      request (cache cold). The headline parallel-scaling workload.
  2 - 1 curve, N bonds (identical bond repeated): CurveCache OFF vs ON.
  3 - N swaption smiles: SabrCalibrateCache OFF vs ON. The SABR cache has no
      enable/disable env var (always compiled in, keyed by cube contents - see
      parser/sabr_calibrate_cache.h); OFF == a unique cube per request (every
      lookup misses), ON == the identical cube repeated (cross-request reuse).
      QUANTRA_SABR_CACHE_LOG=1 is wired so hits/misses show in the worker logs.
  linear - no cluster: price the same M swaps single-threaded in QuantLib (the
      reference row; a networked service always loses 1-vs-1 to an in-process
      call, so this is a reference, not the speedup headline).

Informational: asserts nothing about timing; exits 0 regardless of speed.
"""

import argparse
import copy
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import local as thread_local

import requests

REPO = Path(__file__).resolve().parents[2]
CONTRACT = REPO / "tests" / "contract"
sys.path.insert(0, str(CONTRACT))
sys.path.insert(0, str(Path(__file__).resolve().parent))  # bench_cache payloads

import QuantLib as ql  # noqa: E402,F401  (import side effects / availability)
from ql_reference import (  # noqa: E402
    price_fixed_rate_bond_ql, price_vanilla_swap_ql, api_npv,
)
from bench_cache import BOND_REQUEST, SWAP_REQUEST  # canonical nested payloads  # noqa: E402

FRONT_PORT = 50051   # Envoy gRPC front (quantra default --port)
BASE_PORT = 50055    # first worker
ADMIN_PORT = 9901    # Envoy admin
HTTP_PORT = 8080     # json_server HTTP front
QUANTRA = REPO / "scripts" / "quantra"
JSON_SERVER = REPO / "build" / "jsonserver" / "json_server"
SMILE_FILE = REPO / "examples" / "data" / "swaption_smile_cube_request.json"


# =============================================================================
# Per-thread HTTP session
# =============================================================================

_tls = thread_local()


def _session():
    s = getattr(_tls, "session", None)
    if s is None:
        s = requests.Session()
        s.headers.update({"Content-Type": "application/json"})
        _tls.session = s
    return s


# =============================================================================
# Cluster start-up (no teardown — container death reaps everything)
# =============================================================================

def base_env(extra=None):
    env = dict(os.environ)
    env["QUANTRA_HOME"] = str(REPO)
    if extra:
        env.update(extra)
    return env


def start_cluster(workers, env):
    """Start N workers + Envoy (foreground manager in the background) and a
    json_server HTTP front. Returns True once the whole chain serves a 200.
    Performs no cleanup; the fresh --rm container is the cleanup mechanism."""
    # Foreground manager keeps Envoy alive (non-foreground mode lets the
    # manager exit and Envoy is SIGTERM'd ~2s later). start_new_session keeps
    # it clear of our own signals.
    subprocess.Popen(
        [sys.executable, str(QUANTRA), "start",
         "--workers", str(workers), "--port", str(FRONT_PORT),
         "--base-port", str(BASE_PORT), "--admin-port", str(ADMIN_PORT),
         "--foreground"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )

    if not _poll_200(f"http://localhost:{ADMIN_PORT}/ready", 60):
        sys.stderr.write("  Envoy admin never reported ready\n")
        return False

    subprocess.Popen(
        [str(JSON_SERVER), f"localhost:{FRONT_PORT}", str(HTTP_PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
    )
    if not _poll_200(f"http://localhost:{HTTP_PORT}/health", 30):
        sys.stderr.write("  json_server never became healthy\n")
        return False

    # One real request through the whole chain so measurements never include
    # first-request cold start / gRPC channel setup.
    url = f"http://localhost:{HTTP_PORT}/price-fixed-rate-bond"
    body = json.dumps(BOND_REQUEST)
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            if _session().post(url, data=body, timeout=10).status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.25)
    sys.stderr.write("  warmup request never succeeded\n")
    return False


def _poll_200(url, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if requests.get(url, timeout=2).status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.25)
    return False


# =============================================================================
# Load driver
# =============================================================================

def fire_concurrent(endpoint, payloads, concurrency):
    """POST every payload (one HTTP request each) with up to `concurrency` in
    flight. Returns (rps, elapsed_s, errors, sample_response)."""
    url = f"http://localhost:{HTTP_PORT}/{endpoint}"
    state = {"errors": 0, "sample": None}

    def one(body):
        try:
            r = _session().post(url, data=body, timeout=120)
            if r.status_code == 200:
                if state["sample"] is None:
                    state["sample"] = r.json()
                return
            state["errors"] += 1
            sys.stderr.write(f"  HTTP {r.status_code}: {r.text[:160]}\n")
        except Exception as e:
            state["errors"] += 1
            sys.stderr.write(f"  request error: {e}\n")

    bodies = [json.dumps(p) for p in payloads]
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=concurrency) as ex:
        list(ex.map(one, bodies))
    elapsed = time.perf_counter() - t0
    n_ok = len(payloads) - state["errors"]
    rps = n_ok / elapsed if elapsed > 0 else 0.0
    return rps, elapsed, state["errors"], state["sample"]


# =============================================================================
# Payload builders
# =============================================================================

def perturb_swap_curves(base, seed):
    """Nudge every helper rate on both curves -> two independent (heavy,
    24-helper) bootstraps per request, each with a unique cache key."""
    req = copy.deepcopy(base)
    eps = 1e-6 * (seed + 1)
    for curve in req["pricing"]["rates"]["curves"]:
        for pt in curve["points"]:
            pt["point"]["rate"] += eps
    return req


def load_smile():
    """Load the smile-cube example payload (its curve already extends past the
    cube's longest tenor, so it builds cleanly)."""
    return json.loads(SMILE_FILE.read_text())


def perturb_smile(base, seed):
    """Nudge the smile vols -> a unique SABR cube per request (every cache
    lookup misses)."""
    req = copy.deepcopy(base)
    spec = req["pricing"]["volatility"]["vol_surfaces"][0]["payload"]["payload"]
    eps = 1e-5 * (seed + 1)
    spec["vols"]["values"] = [v + eps for v in spec["vols"]["values"]]
    return req


# =============================================================================
# Modes
# =============================================================================

def run_linear(m):
    """Reference row: price the same M swaps single-threaded in QuantLib."""
    payloads = [perturb_swap_curves(SWAP_REQUEST, i) for i in range(m)]
    t0 = time.perf_counter()
    for p in payloads:
        price_vanilla_swap_ql(p)
    elapsed = time.perf_counter() - t0
    rps = m / elapsed if elapsed else 0.0
    print(f"RESULT scenario=linear workers=0 cache=na "
          f"rps={rps:.2f} errors=0 elapsed={elapsed:.3f}")


def spot_check_bond():
    """Price one bond through the live cluster and compare to the independent
    QuantLib reference (bond parity is gate-covered, so it's a clean anchor)."""
    try:
        r = _session().post(
            f"http://localhost:{HTTP_PORT}/price-fixed-rate-bond",
            data=json.dumps(BOND_REQUEST), timeout=10)
        api = api_npv(r.json(), "bonds")
        ql_npv = price_fixed_rate_bond_ql(BOND_REQUEST)
        rel = abs(api - ql_npv) / max(abs(ql_npv), 1e-9)
        print(f"SPOTCHECK bond api={api:.6f} ql={ql_npv:.6f} "
              f"rel={rel:.2e} ok={1 if rel < 1e-3 else 0}")
    except Exception as e:
        print(f"SPOTCHECK bond skipped: {e}")


def run_cluster_scenario(scenario, workers, m, cache):
    # Build env (cache vars must be set before `quantra start` so workers
    # inherit them) and the payload batch.
    if scenario == 1:
        endpoint = "price-vanilla-swap"
        env = base_env({"QUANTRA_CURVE_CACHE_ENABLED": "0"})
        payloads = [perturb_swap_curves(SWAP_REQUEST, i) for i in range(m)]
        cache = "na"
    elif scenario == 2:
        endpoint = "price-fixed-rate-bond"
        env = base_env({"QUANTRA_CURVE_CACHE_ENABLED": "1" if cache == "on" else "0",
                        "QUANTRA_CURVE_CACHE_LOG": "1"})
        payloads = [BOND_REQUEST] * m   # identical curve repeated
    elif scenario == 3:
        endpoint = "price-swaption"
        env = base_env({"QUANTRA_SABR_CACHE_LOG": "1"})  # no enable/disable var
        smile = load_smile()
        if cache == "on":
            payloads = [smile] * m                                  # full reuse
        else:
            payloads = [perturb_smile(smile, i) for i in range(m)]  # no reuse
    else:
        raise ValueError(f"unknown scenario {scenario}")

    cache_tag = cache if scenario in (2, 3) else "na"

    if not start_cluster(workers, env):
        print(f"RESULT scenario={scenario} workers={workers} cache={cache_tag} "
              f"rps=0.00 errors={m} elapsed=0.000 started=0")
        return

    if scenario == 1:
        spot_check_bond()

    concurrency = max(workers * 2, 4)
    rps, elapsed, errors, _ = fire_concurrent(endpoint, payloads, concurrency)
    print(f"RESULT scenario={scenario} workers={workers} cache={cache_tag} "
          f"rps={rps:.2f} errors={errors} elapsed={elapsed:.3f}")


def main():
    ap = argparse.ArgumentParser(description="Quantra throughput benchmark (single worker count per run)")
    ap.add_argument("--scenario", default="1", choices=["1", "2", "3", "linear"])
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--requests", type=int, default=240, help="M instruments per measurement")
    ap.add_argument("--cache", default="off", choices=["off", "on"],
                    help="cache state for scenarios 2 and 3")
    args = ap.parse_args()

    if args.scenario == "linear":
        run_linear(args.requests)
    else:
        run_cluster_scenario(int(args.scenario), args.workers, args.requests, args.cache)

    # No teardown: the --rm container is reaped on exit.
    sys.exit(0)


if __name__ == "__main__":
    main()
