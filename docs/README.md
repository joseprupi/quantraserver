# Quantra Documentation

This directory is the canonical home for project documentation.

## Start Here

- `../README.md`: project overview, architecture, and quick start
- `build.md`: build environments, Docker targets, and local setup
- `configuration.md`: every environment variable the runtime reads
- `http-api.md`: request rules, HTTP status codes, error body, headers
- `scripts.md`: code generation, build, and runtime helper scripts
- `testing.md`: full test workflow and individual test entrypoints
- `process-manager.md`: multi-process runtime and packaged `quantra` CLI
- `client.md`: C++ client library, the gRPC `Meta` RPC, and health checking
- `versioning.md`: release and semantic-versioning policy
- `../src/README.md`: engine layering (handler → mapper → evaluator)
- `../CONTRIBUTING.md`: branching, PR, and merge expectations

## Canonical Workflow

The build dependencies (gRPC, QuantLib, `flatc`, the pinned Python) live only
in the `quantraserver:test` Docker image — a bare host cannot build or test the
project. Build and run the gate the way CI does:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
    bash -lc './scripts/build.sh Release && bash tests/run_all_tests.sh'
```

Inside that image (or any environment that already has the dependencies), the
individual steps are:

```bash
./scripts/build.sh Release
./scripts/quantra start --workers 4 --foreground
./build/jsonserver/json_server localhost:50051 8080
bash tests/run_all_tests.sh
```

## Documentation Map

| Doc | Covers |
| --- | --- |
| `build.md` | Docker, devcontainer, local dependencies, build commands, troubleshooting |
| `configuration.md` | Runtime environment variables: workers, ports, request budget, caches |
| `http-api.md` | Cross-product HTTP contract: presence rules, dates, status codes, headers |
| `scripts.md` | `generate_schemas.sh`, `generate_openapi.py`, `build.sh`, `scripts/quantra`, `envoy_config.py` |
| `testing.md` | Full suite behavior, direct test entrypoints, CI expectations |
| `process-manager.md` | Envoy-backed runtime model and installable process manager tooling |
| `client.md` | C++ client API modes, build target, configuration, extension points, `Meta` and health |
| `versioning.md` | Semantic versioning and API contract policy |
| `../src/README.md` | Engine folder roles, request flow, and the evaluator boundary rule |

## Runtime Notes

- The primary service surface is gRPC on port `50051`.
- The JSON gateway runs as a separate process, `build/jsonserver/json_server`.
  In a local build you start it yourself; the shipped container starts both and
  exposes gRPC on `50051` and HTTP on `8080`.
- Generated OpenAPI artifacts live in `jsonserver/openapi/`.
- HTTP endpoints use hyphenated paths such as `/price-fixed-rate-bond` and `/price-vanilla-swap`.

## Maintenance Rule

If project behavior, commands, build paths, or public APIs change, update the relevant page in `docs/` first, then update `README.md` only where it links into the canonical docs.
