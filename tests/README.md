# tests/

How the test suite is organized and what every file does.

## Running everything

```bash
bash tests/run_all_tests.sh
```

This is the gate. It starts the gRPC server and the JSON gateway (plus a
second, cache-ON server pair for Suite 6), runs the seven suites below in order,
and exits non-zero if any of them fail. It does **not** build the project —
build first with `./scripts/build.sh`, which produces the binaries under
`build/` that the runner calls.

At the end it prints a human-readable summary box, plus machine-readable lines
you can grep:

```
RESULT suite=1 name="1. C++ QuantLib Parity" status=PASS count=72 unit=cases
SUMMARY suites_passed=7 suites_failed=0 total_cases=270
```

## The seven suites

| # | What it checks | How | Files |
|---|---|---|---|
| 0 | No FlatBuffers/gRPC types leak into the pricing core | static grep, no server | `../scripts/check_pricer_boundary.sh` |
| 1 | Engine prices match QuantLib | C++, in-process (no socket) | `parity/` |
| 2 | gRPC server round-trips correctly | C++, real gRPC call over a socket | `test_server_client.cpp` |
| 3 | JSON API prices match QuantLib + returns right HTTP status codes | Python, real HTTP POST | `contract/` |
| 4 | The Python client library works against the server | Python, gRPC | `test_python_client.py` |
| 5 | Concurrent JSON requests don't race | Python, many parallel HTTP POSTs | `test_json_concurrency.py` |
| 6 | The curve cache is transparent (cache-OFF == cache-ON, warm == hit) | Python, real HTTP POST to two servers | `caching/` |

Suites 1 and 3 are the two correctness anchors: both build the equivalent
instrument in raw QuantLib and compare numbers. Suite 1 tests the C++ code path
directly; Suite 3 tests the full JSON-over-HTTP stack a real client uses.

## `parity/` — C++ vs QuantLib (Suite 1)

Each test builds a FlatBuffers request, calls the product handler directly
in-process, reads the NPV/greeks out of the FlatBuffers response, then builds
the same instrument with raw QuantLib in the same test and asserts they match.
No server, no network.

- `parity_fixture.h` — shared gtest fixture: includes every product handler and
  the helpers for building QuantLib reference objects.
- `<product>_test.cpp` — one file per product (fixed_rate_bond,
  floating_rate_bond, vanilla_swap, ois_swap, basis_swap, fra, cap_floor,
  swaption, cds, equity_option, the two inflation swaps, the two swaption
  calibrations, bootstrap_curves, bootstrap_inflation_curves,
  sample_vol_surfaces, and the calendar utilities calendar_business_days,
  calendar_holidays, calendar_advance).

All of these compile into **one** binary, `build/tests/test_quantra_vs_quantlib`
(see `CMakeLists.txt`).

## `contract/` — JSON API vs QuantLib (Suite 3)

pytest. POSTs the example payloads from `examples/data/*.json` to a running JSON
server (default `http://localhost:8080`), verbatim — exactly what a real client
sends — and compares the returned NPV against an independent QuantLib reference.

- `ql_reference.py` — the correctness anchor. Holds `ApiClient` (POSTs the
  requests), the QuantLib reference pricers, the enum/index/curve builders, and
  `_reference_pricing_view()` (a read-only helper that flattens the nested
  request shape for the reference pricers only — never used for a POST).
- `conftest.py` — pytest setup: server URL, data directory, the `ApiClient`
  fixture.
- `<product>_test.py` — per-product parity tests (bond, swap, fra, cap_floor,
  cds, inflation, swaption, bootstrap).
- `contract_checks.py` — shared scenario helpers for the bootstrap/inflation
  checks. These are `check_*` functions (not `test_*`, so pytest doesn't collect
  them) that return a result dict; the per-product test files call them and
  assert on the result.
- `error_contract_test.py` — error contract. Sends malformed / missing-reference
  / unsupported requests and asserts the HTTP status: 404 (not found),
  400 (invalid argument), 501 (not implemented), 500 (other).

## `caching/` — curve-cache transparency (Suite 6)

pytest. The curve cache (`../parser/curve_cache.h`) is off by default and must
be invisible: turning it on may not change any result. The suite POSTs each
representative example payload to two servers — the default cache-OFF server on
`:8080` and a cache-ON server on `:8081` (`QUANTRA_CURVE_CACHE_ENABLED=1`,
started by `run_all_tests.sh`'s `start_cache_servers`) — and asserts the
responses are bit-for-bit identical, then POSTs to the cache-ON server twice
(warm → hit) and asserts the hit response is identical too. This is **not** a
QuantLib parity check (that is Suite 3): the cache-OFF server is the reference,
and transparency is the only property under test.

- `conftest.py` — pytest setup: `--url-nocache` / `--url-cache` / `--data-dir` /
  `--cache-log`, the two `ApiClient` fixtures (reused from `contract/`), and the
  terminal-summary hook that emits the greppable comparison count.
- `cache_correctness_test.py` — the `CASES` table (one row per product) drives
  `test_cache_transparency`; `test_cache_engaged` reads the cache-ON gRPC log
  (`/tmp/grpc_cache.log`) and asserts at least one `CurveCache` L1 hit fired, so
  the suite can't silently pass with caching that never engages. The table is
  the extension point for D23's bumped-curve (curveBump ≠ 0) cases.

The cache lives in the pricing engine (the gRPC server), so the cache env vars
and the `[CurveCache] … event=L1_HIT` log lines belong to the gRPC process, not
the JSON gateway.

## Top-level files

- `run_all_tests.sh` — the gate (above). Starts/stops servers (including the
  cache-ON pair for Suite 6), runs the seven suites, prints the summary +
  `RESULT`/`SUMMARY` lines.
- `test_server_client.cpp` — Suite 2. C++ gtest that talks to the gRPC server
  over a real socket. Builds into `build/tests/test_server_client`.
- `test_python_client.py` — Suite 4. Exercises the Python client library against
  the running server and compares to QuantLib.
- `test_json_concurrency.py` — Suite 5. Fires many concurrent JSON requests at
  one endpoint and asserts every response is identical (guards the gateway's
  thread-safety).
- `CMakeLists.txt` — builds the two C++ test binaries
  (`test_quantra_vs_quantlib` from `parity/`, and `test_server_client`).
- `requirements.txt` — pinned Python dependencies for the test environment
  (requests, QuantLib, grpcio, flatbuffers, pytest).

## Benchmark (not part of the gate)

- `run_bench.sh` + `bench_cache.py` — measure the curve-cache speedup. These are
  informational only; run them by hand, they are not in the pass/fail gate.

```bash
bash tests/run_bench.sh        # all modes
bash tests/run_bench.sh bond   # one mode
```
