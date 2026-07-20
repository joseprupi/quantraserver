# Testing

Quantra tests cover the evaluator boundary, pricing parity, transport
integration, JSON API behavior, Python client usability, concurrency, and cache
transparency.

## Docker Only

The build dependencies — gRPC, QuantLib, `flatc`, the pinned Python — live only
in the `quantraserver:test` image. **A bare host cannot build or test this
project**; attempting it fails on missing `/opt/quantra-deps` or on the pinned
`grpcio` wheel. That is an environment problem, not a test failure.

Build and run the gate the way CI does:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
    bash -lc './scripts/build.sh Release && bash tests/run_all_tests.sh'
```

Every command below assumes you are inside that image (or an equivalent
environment that already has the dependencies).

## Main Entry Point

```bash
bash tests/run_all_tests.sh
```

The runner does not build — build first. It:

1. starts `build/server/sync_server` on port `50051`
2. starts `build/jsonserver/json_server` on port `8080`
3. starts a second, cache-ON server pair on `50052` / `8081` for the cache suite
4. runs the seven suites in order
5. prints a summary box plus greppable `RESULT` / `SUMMARY` lines

The seven suites:

| # | Checks |
|---|---|
| 0 | No FlatBuffers or gRPC types leak into the pricing core (static check, no server) |
| 1 | C++ engine prices match QuantLib, in-process |
| 2 | The gRPC server round-trips correctly over a real socket |
| 3 | The JSON API matches QuantLib and returns the right HTTP status codes |
| 4 | The Python client library works against the server |
| 5 | Concurrent JSON requests do not race |
| 6 | The curve, SABR, and Hull-White caches are transparent (cache-OFF == cache-ON, warm == hit) |

`tests/README.md` documents each suite, its files, and its current case count.

## Current Suite Components

### C++ parity tests

- binary: `build/tests/test_quantra_vs_quantlib`
- purpose: compare Quantra results against direct QuantLib calculations

### C++ gRPC integration tests

- binary: `build/tests/test_server_client`
- purpose: validate end-to-end request/response behavior through the server transport

### JSON API tests

- suite: `tests/contract/` (pytest)
- run: `python3 -m pytest tests/contract/ --url http://localhost:8080 --data-dir examples/data`
- purpose: exercise representative HTTP scenarios against the running JSON gateway —
  per-product parity (API NPV vs QuantLib reference pricers) plus the HTTP
  error-contract assertions

### Python client tests

- script: `tests/client/test_python_client.py`
- purpose: validate the Python client on representative scenarios

### Evaluator boundary check

- script: `scripts/check_evaluator_boundary.sh`
- purpose: fail the gate if anything under `src/evaluators/` references
  FlatBuffers or gRPC — the pricing core stays transport-free

### Concurrency tests

- suite: `tests/concurrency/` (pytest)
- purpose: fire many concurrent JSON requests at one endpoint and assert every
  response is identical, guarding the gateway's thread-safety

### Cache transparency tests

- suite: `tests/caching/` (pytest)
- purpose: POST each representative payload to a cache-OFF and a cache-ON
  server and assert the responses are bit-for-bit identical, then repeat
  against the cache-ON server (warm → hit). Covers the curve, SABR, and
  Hull-White caches, and asserts the caches actually engaged so the suite
  cannot pass with caching that never fires.

## Direct Execution

After a successful build, you can run individual compiled tests directly:

```bash
./build/tests/test_quantra_vs_quantlib
./build/tests/test_server_client
```

For Python-based tests, make sure the expected `PYTHONPATH` is present or just use the top-level runner, which handles the common workflow for you.

## Build Before Testing

Inside the test image:

```bash
./scripts/build.sh Release
bash tests/run_all_tests.sh
```

## CI

The main CI workflow builds the project in the prebuilt CI container and then runs:

```bash
./scripts/build.sh Release
bash tests/run_all_tests.sh
```

## Adding New Tests

When adding a new C++ test executable:

1. add the source file under `tests/`
2. register it in `tests/CMakeLists.txt`
3. decide whether it belongs in the full runner

When adding Python or JSON tests, prefer wiring them into `tests/run_all_tests.sh` if they represent core coverage.

## Notes

- The old docs mentioned `run_all_tests.sh` flags such as product filtering and `--verbose`; the current runner does not implement those modes.
- The old docs also mentioned a coverage build toggle, but there is no repository-wide coverage workflow documented in the current build system.
