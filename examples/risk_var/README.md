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
(stress the synthetic vols), `--json out.json` (machine-readable results).

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

## What this is not

There is no GPU, no Taylor expansion, no DV01-times-shift shortcut and no
precomputed sensitivity ladder. Every one of the 250 scenarios is a genuine
curve re-bootstrap and full book revaluation through QuantLib — deliberately,
because the point is to show that exact repricing of a realistic book across
a full scenario set is fast enough to be done straightforwardly when requests
are batched sensibly and fanned out across parallel pricing workers.
