# Historical-simulation VaR on QuantraServer

`historical_var.py` computes one-day 99% historical-simulation VaR for a book
of vanilla USD SOFR OIS swaps by full revaluation against a running
QuantraServer JSON API. It is a single self-contained script — Python standard
library plus `requests`, nothing else.

## The methodology in five lines

1. Build a book of N SOFR OIS swaps (seeded random: tenors 1–30Y, mixed
   payer/receiver, 1–50M notionals, fixed rates within ±25bp of par).
2. Define the base curve as 12 par pillars (1M…30Y) of an OIS-bootstrapped
   USD SOFR curve (Discount trait, LogLinear).
3. Take 250 historical daily moves of those par quotes (all pillars at once).
4. For each day: shock the quotes, re-bootstrap the curve, reprice the entire
   book, record P&L versus the base valuation.
5. Sort the 250 P&Ls: the 99% VaR is the loss quantile near the second-worst
   outcome; Expected Shortfall is the average loss beyond it.

## Run it

Start the server (v0.6.0+ wire contract required). On a 12-core machine, run
12 workers — the image defaults to one worker per core but caps at 8, so set
`QUANTRA_WORKERS` explicitly:

```bash
docker run --rm \
  -e QUANTRA_WORKERS=12 \
  -p 8080:8080 \
  -p 50051:50051 \
  ghcr.io/joseprupi/quantra-server:0.6.0
```

Then:

```bash
pip install requests
python3 historical_var.py --url http://localhost:8080
```

Defaults: 100 swaps, 250 scenarios, 12 concurrent requests, fixed seed
(reproducible). Knobs: `--swaps`, `--scenarios`, `--seed`, `--concurrency`,
`--direction payer|receiver|mixed` (make the book directional), `--vol-scale`
(stress the synthetic vols), `--json out.json` (machine-readable results),
`--sweep 1,2,4,8,12,24` (scaling sweep, see below; mutually exclusive with
`--concurrency`).

Keep `--concurrency` at or below the server's worker count. The JSON gateway
enforces a fixed 10-second deadline per forwarded request that *includes time
spent queued behind other requests*, so queueing more concurrent requests than
there are workers makes the excess ones time out with HTTP 504 instead of
waiting. `--concurrency 12` is matched to the `QUANTRA_WORKERS=12` run line
above; against a single-worker server, use `--concurrency 3` or so.

## What the output means

```
Base book value: ...    # book NPV on the unshocked curve
99% 1-day VaR  : ...    # interpolated 1% loss quantile of the 250 P&Ls
                        # (for 250 scenarios: essentially the 2nd-worst loss)
99% ES         : ...    # average loss of the scenarios beyond VaR
Throughput     : ...    # req/s across the scenario fan-out
```

Each request's latency is one curve bootstrap plus a full QuantLib repricing
of every swap in the book. The ASCII histogram at the end is the empirical
one-day P&L distribution of the book.

## Why one request per scenario

Each scenario is ONE `POST /price-ois-swap` carrying **all N swaps and that
scenario's full shocked quote set**. The server bootstraps the scenario curve
once per request and prices the whole book against it, so the bootstrap cost
is amortized across all 100 swaps instead of being paid per trade. The 250
scenario requests are independent, so the script fans them out over a thread
pool and the server spreads them across its worker processes. Note the curve
cache does not help here — every scenario is a *different* curve, so this
workload measures genuine parallel pricing throughput, not cache hits.

## Measuring parallel scaling

`--sweep` turns one run against one server into the sequential-vs-full-fleet
comparison: it executes the FULL identical workload once per client
concurrency level (in the order given) and prints a scaling table.

```bash
# Bench server on a Ryzen 3900X (12 cores / 24 threads). Workers are
# single-threaded, compute-bound processes, so ~12 is where near-linear
# scaling should end and 24 tests what SMT adds on top. The image's default
# worker count caps at 8, so QUANTRA_WORKERS must be set explicitly:
docker run --rm \
  -e QUANTRA_WORKERS=24 \
  -p 8080:8080 \
  -p 50051:50051 \
  ghcr.io/joseprupi/quantra-server:0.6.0

python3 historical_var.py --sweep 1,2,4,8,12,24
```

The deadline rule from above applies per level: every sweep level's
concurrency must stay at or below the server's `QUANTRA_WORKERS`, because the
gateway's fixed 10-second deadline includes queue time — sweeping up to 24
therefore needs the `QUANTRA_WORKERS=24` server.

**Why each level nudges the quotes.** The server's curve cache is keyed on
curve *content*, so re-running byte-identical scenarios would serve later
levels from cache and make them artificially fast. The sweep therefore adds a
deterministic per-level epsilon (level position × 1e-9, i.e. 1e-5 bp) to every
par quote — base curve included — so every cache key changes and every level
re-bootstraps honestly. The epsilon is economically negligible and cancels
out of the P&L (base and scenarios shift together): the script asserts that
VaR agrees across levels to well under $1 and prints the check
(`results identical across levels: max VaR delta $...`), which doubles as a
demonstration that parallelism does not change the numbers. Purist variant:
start the bench server with `-e QUANTRA_CURVE_CACHE_ENABLED=0` and the cache
is out of the picture entirely (the epsilon then simply does nothing).

Example output (**placeholder numbers — entirely machine-dependent**, shape
shown for a 12-core/24-thread box with `QUANTRA_WORKERS=24`):

```
concurrency | wall time |   req/s | avg latency | speedup vs level-1
          1 |   120.00s |     2.1 |       480ms |              1.00x
          2 |    61.00s |     4.1 |       485ms |              1.97x
          4 |    31.00s |     8.1 |       492ms |              3.87x
          8 |    16.00s |    15.6 |       505ms |              7.50x
         12 |    11.00s |    22.7 |       520ms |             10.91x
         24 |     8.50s |    29.4 |       780ms |             14.12x
```

Near-linear to ~12 (the physical cores), then a flatter SMT tail to 24 —
higher per-request latency, but still more throughput. The risk numbers, the
identical-results check and the histogram are printed once after the table,
and `--json` includes the table machine-readably.

## Synthetic history — and how to plug in real data

By default the daily moves are **synthetic** (seeded): per-pillar daily vols
of realistic magnitude (~1–2bp/day at the short end, ~5–6bp/day at the long
end) with strong cross-pillar correlation (a common level factor plus small
idiosyncratic noise). The output labels this clearly. To use real observed
history:

```bash
python3 historical_var.py --history-csv moves.csv
```

where `moves.csv` has one row per day and 12 columns of par-quote changes in
basis points, ordered `1M,3M,6M,1Y,2Y,3Y,5Y,7Y,10Y,15Y,20Y,30Y` (an optional
header row is skipped). The scenario count then equals the number of rows.

## Cross-checking against raw QuantLib

`native_var_check.cpp` is a standalone C++ program that recomputes the exact
same VaR run — same book, same base and scenario curves — with **direct
QuantLib calls**: no server, no gRPC, no JSON. Comparing its numbers against
the script's proves that the server pipeline adds zero pricing error: what
you get over the wire is exactly what native QuantLib produces from the same
inputs, matching P&L for P&L (residual differences are doubles round-tripping
through JSON, i.e. cents on multi-million P&Ls).

Three steps (all runnable inside the `quantraserver:test` image, which has
QuantLib under `/opt/quantra-deps`):

```bash
# 1. Run the script and dump the exact inputs it priced (full precision):
python3 historical_var.py --swaps 50 --scenarios 100 --concurrency 3 \
    --dump-inputs work/ --json work/result.json

# 2. Compile and run the native re-pricer on the dumped inputs:
g++ -O2 -std=c++17 native_var_check.cpp \
    -I/opt/quantra-deps/include -L/opt/quantra-deps/lib -lQuantLib \
    -o native_var_check
LD_LIBRARY_PATH=/opt/quantra-deps/lib \
    ./native_var_check work/swaps.csv work/quotes.csv work/native_results.csv

# 3. Re-run the SAME configuration with the comparison flag:
python3 historical_var.py --swaps 50 --scenarios 100 --concurrency 3 \
    --compare-native work/native_results.csv
```

The comparison block reports the base-book-value difference, the max/mean
absolute per-scenario P&L difference, and the VaR/ES differences (also in
`--json` under `native_comparison`). The .cpp mirrors, call for call, the
conventions the engine applies to the script's JSON (curve trait and
interpolation, OIS helper construction, schedule generation, payment lag,
index definition, discounting engine) — see the comment at the top of the
file for the full list.

Note: `native_var_check.cpp` is a standalone validation artifact. It is
deliberately **not** wired into the repo's CMake build or test suite — it
exists so anyone can verify the server against raw QuantLib with one compile
line.

## What this is not

There is no GPU, no Taylor expansion, no DV01-times-shift shortcut and no
precomputed sensitivity ladder. Every one of the 250 scenarios is a genuine
curve re-bootstrap and full book revaluation through QuantLib — deliberately,
because the point is to show that exact repricing of a realistic book across
a full scenario set is fast enough to be done straightforwardly when requests
are batched sensibly and fanned out across parallel pricing workers.
