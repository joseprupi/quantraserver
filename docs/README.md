# Quantra Documentation

This directory is the canonical home for project documentation.

## Start Here

- `../README.md`: project overview, architecture, and quick start
- `build.md`: build environments, Docker targets, and local setup
- `scripts.md`: code generation, build, and runtime helper scripts
- `testing.md`: full test workflow and individual test entrypoints
- `process-manager.md`: multi-process runtime and packaged `quantra` CLI
- `client.md`: C++ client library usage and extension notes
- `parser.md`: parser/service/builder conventions
- `versioning.md`: release and semantic-versioning policy
- `../CONTRIBUTING.md`: branching, PR, and merge expectations

## Canonical Workflow

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
| `scripts.md` | `generate_schemas.sh`, `generate_openapi.py`, `build.sh`, `scripts/quantra`, `envoy_config.py` |
| `testing.md` | Full suite behavior, direct test entrypoints, CI expectations |
| `process-manager.md` | Envoy-backed runtime model and installable process manager tooling |
| `client.md` | C++ client API modes, build target, configuration, extension points |
| `parser.md` | Naming conventions and request-layer boundaries |
| `versioning.md` | Semantic versioning and API contract policy |

## Runtime Notes

- The primary service surface is gRPC on port `50051`.
- The JSON gateway is optional and runs separately as `build/jsonserver/json_server`.
- Generated OpenAPI artifacts live in `jsonserver/openapi/`.
- HTTP endpoints use hyphenated paths such as `/price-fixed-rate-bond` and `/price-vanilla-swap`.

## Maintenance Rule

If project behavior, commands, build paths, or public APIs change, update the relevant page in `docs/` first, then update `README.md` only where it links into the canonical docs.
