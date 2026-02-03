# Quantra Scripts

Build, code generation, and server management scripts for the Quantra project.

## Quick Reference

```bash
# Generate all code from FlatBuffers schemas
./scripts/generate_schemas.sh

# Build C++ server and tests
./scripts/build.sh [Debug|Release]

# Run all tests
./tests/run_all_tests.sh

# Start/stop servers
quantra start --workers 4 --foreground
quantra stop
quantra status
```

## Scripts Overview

- `generate_schemas.sh` - Generate C++, Python, JSON schemas, and OpenAPI docs from `.fbs` files
- `generate_openapi.py` - Generate OpenAPI 3.0 spec and HTML documentation
- `build.sh` - Compile C++ server and tests
- `quantra` - Process manager CLI (start/stop/status)
- `envoy_config.py` - Envoy load balancer configuration generator

## Code Generation

### generate_schemas.sh

Generates all code and documentation from FlatBuffers `.fbs` schema files.

Output:
- `flatbuffers/cpp/*.h` - C++ headers
- `flatbuffers/python/quantra/*.py` - Python modules
- `flatbuffers/json/*.schema.json` - JSON schemas
- `jsonserver/openapi/*` - OpenAPI docs

```bash
./scripts/generate_schemas.sh
```

Run this after modifying any `.fbs` file in `flatbuffers/fbs/`.

### generate_openapi.py

Generates OpenAPI 3.0 specification from FlatBuffers-generated JSON schemas.

Output:
- `openapi3.yaml` / `openapi3.json` - OpenAPI spec
- `docs.html` - ReDoc documentation
- `swagger.html` - Swagger UI

```bash
# Usually called by generate_schemas.sh, but can run standalone:
python3 scripts/generate_openapi.py

# View docs
python3 -m http.server 9000 -d jsonserver/openapi
# Open http://localhost:9000/docs.html
```

## Build

### build.sh

Compiles the C++ server, client, and tests using CMake.

```bash
./scripts/build.sh          # Debug build (default)
./scripts/build.sh Release  # Release build
```

Output binaries in `build/`:
- `build/server/sync_server` - gRPC server
- `build/jsonserver/json_server` - JSON HTTP server
- `build/tests/*` - Test executables

## Server Management

### quantra

Process manager CLI for running multiple Quantra workers with Envoy load balancing.

Requires `envoy_config.py` in the same directory.

Commands:
```bash
quantra start [options]    # Start cluster
quantra stop [--force]     # Stop cluster
quantra status             # Show status
quantra restart [options]  # Restart cluster
quantra health             # Check worker health
quantra logs [-f]          # View logs
```

Start options:
```bash
-w, --workers N      # Number of workers (default: 4)
-p, --port PORT      # Client port (default: 50051)
--base-port PORT     # First worker port (default: 50055)
--admin-port PORT    # Envoy admin port (default: 9901)
--foreground         # Keep running in foreground (for containers)
```

Examples:
```bash
# Start 4 workers in foreground (recommended for containers)
quantra start --workers 4 --foreground

# Start 8 workers, detached
quantra start --workers 8

# Check health
quantra health

# View logs
quantra logs --follow

# Stop
quantra stop
```

### envoy_config.py

Generates Envoy proxy configuration for load balancing across Quantra workers.

Used by the `quantra` CLI (imported as module).

Standalone usage:
```bash
# Generate config file
python3 envoy_config.py --workers 4 --output envoy.yaml

# Check health
python3 envoy_config.py --health

# Custom ports
python3 envoy_config.py --port 8080 --base-port 9000 --workers 8 -o envoy.yaml
```

## Testing

### tests/run_all_tests.sh

Runs all test suites: C++ unit tests (Quantra vs QuantLib), C++ gRPC integration tests, JSON HTTP API tests, and Python gRPC client tests.

```bash
./tests/run_all_tests.sh
```

## Installation

```bash
# Install quantra CLI system-wide
cp scripts/quantra scripts/envoy_config.py /usr/local/bin/
chmod +x /usr/local/bin/quantra
```

## Development Workflow

```bash
# 1. Edit FlatBuffers schemas
vim flatbuffers/fbs/my_type.fbs

# 2. Regenerate all code
./scripts/generate_schemas.sh

# 3. Rebuild C++ code
./scripts/build.sh

# 4. Run tests
./tests/run_all_tests.sh

# 5. Start servers
quantra start --workers 4 --foreground
```

## Deprecated Scripts

The following old scripts have been replaced:
- `config_vars.sh` → `quantra` CLI
- `start.sh` → `quantra start`
- `run-tests.sh` → `tests/run_all_tests.sh` |