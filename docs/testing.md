# Testing

Quantra tests cover pricing parity, transport integration, JSON API behavior, and Python client usability.

## Main Entry Point

Run the full suite with:

```bash
bash tests/run_all_tests.sh
```

The runner:

1. starts `build/server/sync_server` on port `50051`
2. starts `build/jsonserver/json_server` on port `8080`
3. runs C++ pricing parity tests
4. runs C++ gRPC integration tests
5. runs JSON API scenarios
6. runs Python client scenarios

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

## Direct Execution

After a successful build, you can run individual compiled tests directly:

```bash
./build/tests/test_quantra_vs_quantlib
./build/tests/test_server_client
```

For Python-based tests, make sure the expected `PYTHONPATH` is present or just use the top-level runner, which handles the common workflow for you.

## Build Before Testing

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
