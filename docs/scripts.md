# Scripts Reference

The repository automation lives primarily in `scripts/`.

## Core Scripts

### `scripts/generate_schemas.sh`

Regenerates artifacts derived from FlatBuffers schemas.

Outputs include:

- generated C++ headers under `flatbuffers/cpp/`
- generated Python modules under `flatbuffers/python/`
- generated JSON schemas under `flatbuffers/json/`
- generated OpenAPI files under `jsonserver/openapi/`

Run it after changing anything in `flatbuffers/fbs/`:

```bash
./scripts/generate_schemas.sh
```

### `scripts/generate_openapi.py`

Builds OpenAPI documents from the generated JSON schemas.

It is normally invoked by `generate_schemas.sh`, but can be run directly:

```bash
python3 scripts/generate_openapi.py
python3 -m http.server 9000 -d jsonserver/openapi
```

### `scripts/build.sh`

The canonical build entrypoint for the repository.

```bash
./scripts/build.sh
./scripts/build.sh Release
```

Behavior:

1. ensures the FlatBuffers toolchain is available
2. regenerates schemas and generated code
3. recreates `build/`
4. runs CMake from the repo root
5. builds all configured targets

### `scripts/quantra`

Convenience CLI for starting the local multi-process gRPC runtime behind Envoy.

Supported commands include:

```bash
./scripts/quantra start --workers 4 --foreground
./scripts/quantra stop
./scripts/quantra status
./scripts/quantra restart --workers 8
./scripts/quantra health
./scripts/quantra logs --follow
```

Important options:

- `--workers`
- `--port`
- `--base-port`
- `--admin-port`
- `--foreground`

The script stores runtime state under `QUANTRA_HOME/.quantra`, with `QUANTRA_HOME` defaulting to `/workspace`.

### `scripts/envoy_config.py`

Generates the Envoy configuration used by the `quantra` script for worker load balancing and health checks.

## Typical Development Cycle

```bash
./scripts/generate_schemas.sh
./scripts/build.sh Release
bash tests/run_all_tests.sh
./scripts/quantra start --workers 4 --foreground
```

## Notes

- For packaged process-manager details, see `process-manager.md`.
