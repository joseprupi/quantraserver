# Build Guide

This page replaces the old root `BUILD_GUIDE.md` and documents the current build flow in the repository.

## Recommended Paths

### Docker production image

The default image builds the project, starts the gRPC worker cluster behind
Envoy on port `50051`, and starts the JSON gateway on port `8080`. Both ports
are exposed; publish whichever surfaces you need.

```bash
docker build -t quantra .
docker run --rm -p 50051:50051 -p 8080:8080 quantra
```

See `configuration.md` for the environment variables the entrypoint honours
(worker count, ports, request budget, caches).

### Docker dev image

The `dev` target gives you an interactive environment with build helpers on `PATH`.

```bash
docker build --target dev -t quantra-dev .
docker run -it --rm -v "$(pwd):/workspace" -p 50051:50051 quantra-dev
```

Inside the dev container:

```bash
build.sh
build.sh Release
regen-flatbuffers.sh
quantra start --workers 4 --foreground
```

### Local development

If you are building outside Docker, make sure the dependency toolchain is available first, then use the repository build script:

```bash
./scripts/build.sh
./scripts/build.sh Release
```

## Dependency Versions

The repository currently builds around:

- gRPC `v1.60.0`
- FlatBuffers `v24.12.23`
- QuantLib `1.41` in Docker builds
- CMake `3.16+`
- GCC `12+` or Clang `14+`

## Local Dependency Setup

### Option A: extract dependencies from the Docker `deps` stage

```bash
docker build --target deps -t quantra-deps .
docker create --name quantra-deps-temp quantra-deps
docker cp quantra-deps-temp:/opt/quantra-deps ./quantra-deps
docker rm quantra-deps-temp

export CMAKE_PREFIX_PATH="$(pwd)/quantra-deps"
export LD_LIBRARY_PATH="$(pwd)/quantra-deps/lib:$LD_LIBRARY_PATH"
export DEPS_INSTALL_PREFIX="$(pwd)/quantra-deps"
```

### Option B: build dependencies manually

The Dockerfile is the source of truth for exact versions and install shape. If you install the dependencies manually, make sure:

- `flatc` is on `PATH`
- the libraries are visible through `CMAKE_PREFIX_PATH`
- runtime libraries are visible through `LD_LIBRARY_PATH` or `ldconfig`

`./scripts/build.sh` expects to find the dependency prefix at `DEPS_INSTALL_PREFIX`, defaulting to `/opt/quantra-deps`.

## What `./scripts/build.sh` Does

The build script:

1. Regenerates FlatBuffers code and OpenAPI artifacts
2. Deletes and recreates `build/`
3. Runs CMake with `-DCMAKE_PREFIX_PATH`
4. Builds all configured targets

Output artifacts typically include:

- `build/server/sync_server`
- `build/jsonserver/json_server`
- `build/tests/test_quantra_vs_quantlib`
- `build/tests/test_server_client`

## Dev Container

If you use the repository devcontainer, open the project in the container and run the same commands documented above:

```bash
./scripts/build.sh Release
bash tests/run_all_tests.sh
```

## Running After Build

### Start the gRPC service

```bash
./scripts/quantra start --workers 4 --foreground
```

### Start the optional JSON gateway

```bash
./build/jsonserver/json_server localhost:50051 8080
```

### Run the full test suite

```bash
bash tests/run_all_tests.sh
```

## Upgrading Dependencies

When changing gRPC, FlatBuffers, or QuantLib versions:

1. Update the Dockerfile first
2. Regenerate generated artifacts with `./scripts/generate_schemas.sh`
3. Rebuild with `./scripts/build.sh`
4. Run `bash tests/run_all_tests.sh`

## Troubleshooting

### `flatc` not found

Set `DEPS_INSTALL_PREFIX` so `./scripts/build.sh` can locate the FlatBuffers toolchain, and make sure `flatc` is on `PATH`.

### CMake cannot find packages

```bash
export CMAKE_PREFIX_PATH=/path/to/quantra-deps
./scripts/build.sh
```

### Runtime libraries missing

```bash
export LD_LIBRARY_PATH=/path/to/quantra-deps/lib:$LD_LIBRARY_PATH
```

### Generated code mismatch

If FlatBuffers or schema files changed, regenerate first:

```bash
./scripts/generate_schemas.sh
```
