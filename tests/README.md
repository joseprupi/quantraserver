# tests/

How the test suite is organized and what every file does.

## Running everything

The build dependencies (gRPC, QuantLib, flatc, the pinned Python) live in the
`quantraserver:test` Docker image, so the canonical way to build and run the
whole gate — the same command CI runs — is:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
    bash -lc './scripts/build.sh Release && bash tests/run_all_tests.sh'
```

Inside an environment that already has the dependencies (a shell in that
image), the two steps are:

```bash
./scripts/build.sh Release    # produces the binaries under build/
bash tests/run_all_tests.sh   # the gate
```

`run_all_tests.sh` is the gate. It starts the gRPC server and the JSON gateway
(plus a second, cache-ON server pair for Suite 6), runs the seven suites below
in order, and exits non-zero if any of them fail. It does **not** build the
project — build first, as above.

At the end it prints a human-readable summary box, plus machine-readable lines
you can grep:

```
RESULT suite=1 name="1. C++ QuantLib Parity" status=PASS count=118 unit=cases
SUMMARY suites_passed=7 suites_failed=0 total_cases=628
```

## The seven suites

Counts are from the current green gate run (`SUMMARY suites_passed=7
suites_failed=0 total_cases=628`).

| # | What it checks | How | Files | Count |
|---|---|---|---|---|
| 0 | No FlatBuffers/gRPC types leak into the pricing core | static grep, no server | `../scripts/check_evaluator_boundary.sh` | 48 files |
| 1 | Engine prices match QuantLib | C++, in-process (no socket) | `parity/` | 118 cases |
| 2 | gRPC server round-trips correctly | C++, real gRPC call over a socket | `integration/test_server_client.cpp` | 28 cases |
| 3 | JSON API prices match QuantLib + returns right HTTP status codes | Python, real HTTP POST | `contract/` + `functional/` | 319 tests |
| 4 | The Python client library works against the server | Python, gRPC | `client/test_python_client.py` | 7 scenarios |
| 5 | Concurrent JSON requests don't race | Python, many parallel HTTP POSTs | `concurrency/test_json_concurrency.py` | 96 requests |
| 6 | The curve, SABR, and Hull-White caches are transparent (cache-OFF == cache-ON, warm == hit) | Python, real HTTP POST to two servers | `caching/` | 12 comparisons |

Suites 1 and 3 are the two correctness anchors: both build the equivalent
instrument in raw QuantLib and compare numbers. Suite 1 tests the C++ code path
directly; Suite 3 tests the full JSON-over-HTTP stack a real client uses.
Within Suite 3, the **[functional parity catalog](functional/README.md)**
(`functional/`) is the QuantLib-parity showcase: 172 curated cases across 13
product families, each a complete request JSON asserted against an independent
QuantLib reference — browse them all in
[`functional/CATALOG.md`](functional/CATALOG.md).

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
- Cross-cutting guards that are not per-product parity:
  `helper_presence_test.cpp` (an omitted required helper quote is an error,
  not a silent zero), `union_absent_value_test.cpp` (absent union members),
  `enum_convert_daycounter_test.cpp` (day-counter enum conversion), and
  `curve_cache_key_unknown_point_test.cpp` /
  `curve_cache_key_unresolvable_ref_test.cpp` (the curve-cache key refuses to
  under-key a request it cannot fully describe).

All of these compile into **one** binary, `build/tests/test_quantra_vs_quantlib`
(see `CMakeLists.txt`).

## `contract/` — JSON API vs QuantLib (Suite 3)

pytest. POSTs the example payloads from `examples/data/*.json` to a running JSON
server (default `http://localhost:8080`), verbatim — exactly what a real client
sends — and compares the returned NPV against an independent QuantLib reference.
Suite 3 runs `contract/` and `functional/` (next section) together.

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
- `gateway_dx_test.py` — gateway developer experience: `X-Request-Id` is
  sanitized, forwarded to the engine, tagged onto its log lines and echoed
  back; `X-Quantra-Api-Version` is present; error bodies carry the real cause.
- `body_limits_test.py` — the request-body cap and the `Content-Type` guard
  (413 / 415 / empty-body 400).
- `status_envoy_test.py` — how `GET /status` parses `QUANTRA_ENVOY_ADMIN`,
  including the malformed-target case.
- `envoy_config_test.py` — the generated Envoy configuration (timeouts,
  load-balancing policy, gRPC health checks).
- `swaption_vol_diagnostics_test.py` — the swaption vol diagnostics block,
  including that period fields serialize with the right time unit.

## `functional/` — the functional parity catalog (Suite 3)

The QuantLib-parity showcase: a manifest-driven catalog of **172 cases across
13 product families** (IR swaps, zero-coupon swaps, bonds, callable bonds,
FRAs, caps/floors, swaptions, CDS, curves, calendars, inflation, equity,
vol/calibration), each a complete, curated request JSON POSTed to the server
and asserted against an independent QuantLib reference within a tight
tolerance. It runs as part of Suite 3, in the same pytest invocation as
`contract/`.

- [`functional/CATALOG.md`](functional/CATALOG.md) — the browsable catalog:
  every case with its description, exercise tags, request JSON link and
  QuantLib value (also available as a self-contained
  `functional/catalog.html`). This is the best "worked example per product"
  reference in the repo.
- [`functional/README.md`](functional/README.md) — how the catalog works:
  comparison modes and tolerances, per-family coverage and planned lists, and
  the step-by-step recipe for adding a case.

`CATALOG.md` and `catalog.html` are **generated** from `functional/manifest.py`
— never edit them by hand; a Suite 3 test fails if they drift from the
manifest.

## `caching/` — cache transparency (Suite 6)

pytest. The engine has three result caches — the bootstrapped-curve cache
(`../src/market/curve_cache.h`), the SABR calibration cache
(`../src/market/sabr_calibrate_cache.cpp`) and the Hull-White calibration cache
(`../src/market/hw_calibrate_cache.cpp`). All are off by default and all must
be invisible: turning them on may not change any result. The suite POSTs each
representative example payload to two servers — the default cache-OFF server on
`:8080` and a cache-ON server on `:8081`, started by `run_all_tests.sh`'s
`start_cache_servers` with `QUANTRA_CURVE_CACHE_ENABLED=1`,
`QUANTRA_SABR_CACHE_ENABLED=1` and `QUANTRA_HW_CACHE_ENABLED=1` (plus the
matching `_LOG` flags) — and asserts the responses are bit-for-bit identical,
then POSTs to the cache-ON server twice (warm → hit) and asserts the hit
response is identical too. This is **not** a QuantLib parity check (that is
Suite 3): the cache-OFF server is the reference, and transparency is the only
property under test.

- `conftest.py` — pytest setup: `--url-nocache` / `--url-cache` / `--data-dir` /
  `--cache-log`, the two `ApiClient` fixtures (reused from `contract/`), and the
  terminal-summary hook that emits the greppable comparison count.
- `cache_correctness_test.py` — the `CASES` table (one row per product) drives
  `test_cache_transparency`; `test_cache_engaged` reads the cache-ON gRPC log
  (`/tmp/grpc_cache.log`) and asserts at least one `CurveCache` L1 hit fired, so
  the suite can't silently pass with caching that never engages. The table is
  the extension point for further cases, such as bumped curves (curveBump ≠ 0).

The caches live in the pricing engine (the gRPC server), so the cache env vars
and the `[CurveCache] … event=L1_HIT` log lines belong to the gRPC process, not
the JSON gateway.

## `integration/` — gRPC round-trip (Suite 2)

- `test_server_client.cpp` — Suite 2. C++ gtest that talks to the gRPC server
  over a real socket. Builds into `build/tests/test_server_client`.

## `client/` — Python client library (Suite 4)

- `test_python_client.py` — Suite 4. Exercises the Python client library against
  the running server and compares to QuantLib.

## `concurrency/` — gateway thread-safety (Suite 5)

- `test_json_concurrency.py` — Suite 5. Fires many concurrent JSON requests at
  one endpoint and asserts every response is identical (guards the gateway's
  thread-safety).

## `bench/` — curve-cache benchmark (not part of the gate)

- `run_bench.sh` + `bench_cache.py` — measure the curve-cache speedup. These are
  informational only; run them by hand, they are not in the pass/fail gate.

```bash
bash tests/bench/run_bench.sh        # all modes
bash tests/bench/run_bench.sh bond   # one mode
```

## Top-level files

- `run_all_tests.sh` — the gate (above). Starts/stops servers (including the
  cache-ON pair for Suite 6), runs the seven suites, prints the summary +
  `RESULT`/`SUMMARY` lines.
- `CMakeLists.txt` — builds the two C++ test binaries
  (`test_quantra_vs_quantlib` from `parity/`, and `test_server_client` from
  `integration/`).
- `requirements.txt` — pinned Python dependencies for the test environment
  (requests, QuantLib, grpcio, flatbuffers, pytest).
