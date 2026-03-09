# Quantra

Quantra is a QuantLib-based pricing service built for parallel execution. It exposes pricing functionality over gRPC with FlatBuffers, and it also includes an optional HTTP/JSON gateway for easier integration and generated OpenAPI documentation.

## Why This Exists

QuantLib is powerful, but it is not naturally suited to high-concurrency service workloads because important state such as `Settings::instance().evaluationDate()` is global to the process. Quantra works around that by running multiple isolated pricing workers and placing Envoy in front of them as a load balancer.

## What You Get

- A C++ pricing server built on QuantLib
- A gRPC API using FlatBuffers messages
- An optional JSON/HTTP gateway in `jsonserver/`
- A C++ client in `client/`
- A Python client package in `quantra-python/`
- Build, schema generation, and process-management tooling in `scripts/` and `tools/quantra-manager/`
- Integration and parity tests in `tests/`

## Supported Pricing Coverage

Representative supported request types include:

- Fixed-rate bonds
- Floating-rate bonds
- Vanilla swaps
- OIS swaps
- Basis swaps
- Zero-coupon inflation swaps
- Year-on-year inflation swaps
- FRAs
- Caps and floors
- Swaptions
- CDS
- Equity options

See `examples/data/` for sample payloads.

## Architecture

The main runtime model is a multi-process gRPC service fronted by Envoy:

```text
client -> Envoy (:50051) -> sync_server workers (:50055+)
```

Typical local or container workflow:

1. Build schemas and binaries
2. Start the gRPC worker cluster with `quantra`
3. Optionally run `json_server` to expose HTTP/JSON endpoints

## Quick Start

### Docker

The default production image starts the gRPC cluster and exposes port `50051`.

```bash
docker build -t quantra .
docker run --rm -p 50051:50051 quantra
```

### Local Build

See `docs/build.md` for environment setup details. Once dependencies are available:

```bash
./scripts/build.sh Release
./scripts/quantra start --workers 4 --foreground
```

### Optional JSON Gateway

The JSON server is built as a separate binary and connects to the gRPC endpoint:

```bash
./build/jsonserver/json_server localhost:50051 8080
```

You can then call the HTTP API with sample requests from `examples/data/`:

```bash
curl -X POST http://localhost:8080/price-fixed-rate-bond \
  -H "Content-Type: application/json" \
  -d @examples/data/fixed_rate_bond_request.json
```

The generated OpenAPI files live in `jsonserver/openapi/`.

## Development Workflow

### Build

`./scripts/build.sh` regenerates schemas, recreates `build/`, and compiles the project.

```bash
./scripts/build.sh
./scripts/build.sh Release
```

### Regenerate Schemas Only

If you are editing FlatBuffers schemas and want to regenerate artifacts without a full build:

```bash
./scripts/generate_schemas.sh
```

### Run Tests

```bash
bash tests/run_all_tests.sh
```

The test suite exercises:

- C++ pricing parity against QuantLib
- C++ gRPC integration
- JSON HTTP API scenarios
- Python client scenarios

## Repository Map

- `server/`: gRPC pricing server
- `jsonserver/`: HTTP/JSON gateway and generated OpenAPI docs
- `request/`: request entrypoints and endpoint orchestration
- `parser/`: parsing, domain conversion, pricing helpers, and builders
- `client/`: C++ client library
- `quantra-python/`: Python client package
- `flatbuffers/`: schema sources plus generated C++, Python, and JSON artifacts
- `grpc/`: gRPC service definitions and generated service bindings
- `examples/data/`: example JSON requests
- `tests/`: parity, integration, and client tests
- `scripts/`: build, code generation, and runtime helpers
- `tools/quantra-manager/`: packaged process-manager implementation
- `docs/`: project documentation and reference notes

## Documentation

- `docs/README.md`: documentation index
- `docs/build.md`: environment setup and build details
- `docs/scripts.md`: build and schema tooling
- `docs/testing.md`: test suite details
- `docs/process-manager.md`: process-manager behavior and runtime model
- `docs/client.md`: C++ client notes
- `docs/parser.md`: parser/service/builder conventions
- `docs/versioning.md`: versioning policy
- `CONTRIBUTING.md`: contribution workflow

## Requirements

The repository currently documents and builds around:

- CMake `3.16+`
- GCC `12+` or Clang `14+`
- gRPC `v1.60.0`
- FlatBuffers `v24.12.23`
- QuantLib `1.41` in Docker builds
- Envoy for worker load balancing

## License

MIT / Apache 2.0
