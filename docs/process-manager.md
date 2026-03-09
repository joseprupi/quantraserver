# Process Manager

Quantra uses multiple `sync_server` processes to work around QuantLib's process-global state. Envoy sits in front of those workers and exposes a single client-facing gRPC port.

## Runtime Model

```text
client -> Envoy (:50051) -> sync_server workers (:50055+)
```

## In-Repo CLI

For local development inside the repository, use:

```bash
./scripts/quantra start --workers 4 --foreground
./scripts/quantra status
./scripts/quantra health
./scripts/quantra logs --follow
./scripts/quantra stop
```

Defaults in the in-repo script:

- client port: `50051`
- first worker port: `50055`
- Envoy admin port: `9901`
- state directory: `QUANTRA_HOME/.quantra`

`QUANTRA_HOME` defaults to `/workspace`.

## Packaged Tooling

The installable process-manager sources live in `tools/quantra-manager/`:

- `tools/quantra-manager/quantra`
- `tools/quantra-manager/quantra-simple`
- `tools/quantra-manager/requirements.txt`

This packaged area is what the Docker build uses to install `quantra` into the image.

## `quantra` vs `quantra-simple`

### `quantra`

- starts Envoy plus multiple workers
- provides `start`, `stop`, `status`, `restart`, `health`, and `logs`
- intended for the normal multi-process runtime

### `quantra-simple`

- starts workers directly without Envoy
- useful for simple local experiments or environments where load balancing is handled elsewhere

## Installation Notes

If you want a system-wide installed CLI, copy the packaged scripts from `tools/quantra-manager/` rather than relying on the repo-local helper path.

## Container Usage

The production Docker image starts the process manager in foreground mode:

```bash
docker run --rm -p 50051:50051 quantra
```

To override worker count:

```bash
docker run --rm -p 50051:50051 quantra quantra start --workers 8 --foreground
```

## Troubleshooting

### Ports already in use

Make sure `50051`, `9901`, and the worker port range are free before starting the cluster.

### Workers fail to start

Verify the server binary exists:

```bash
ls -la build/server/sync_server
```

### Envoy health issues

Use:

```bash
./scripts/quantra health
```

and inspect the generated logs under `.quantra/logs/`.
