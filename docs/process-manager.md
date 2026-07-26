# Process Manager

Quantra uses multiple `sync_server` processes to work around QuantLib's process-global state. Envoy sits in front of those workers and exposes a single client-facing gRPC port.

## Runtime Model

```text
client -> Envoy (:50051) -> sync_server workers (:50055+)
```

## Scaling Model

Each `sync_server` worker handles one pricing request at a time. QuantLib here is
built without session support, so a worker is effectively single-threaded for
compute: while it prices a request, it does no other pricing work. Throughput
comes from running several worker processes side by side, not from threads inside
one process.

Size the worker count roughly to the number of CPU cores. The production
container defaults `QUANTRA_WORKERS` to the detected core count, capped at 8 and
floored at 1; set `QUANTRA_WORKERS` explicitly to override. More workers than
cores mostly adds memory pressure without adding parallel compute.

Envoy balances with `LEAST_REQUEST`, so a new request is sent to the worker with
the fewest in-flight requests. Because a busy worker keeps an in-flight count of
at least one, incoming work naturally routes around it to an idle worker instead
of queueing behind a slow computation.

Envoy probes each worker with a gRPC health check against the standard
`grpc.health.v1.Health` service (empty service name, i.e. overall serving
status). The health service runs on gRPC's own thread pool, so a worker that is
busy on a long calculation still answers as healthy. The check therefore
distinguishes a *dead* worker (ejected from the pool) from a *busy* one (kept in
the pool and simply avoided by `LEAST_REQUEST` until it frees up).

Each worker has its own in-process result caches (curve bootstrapping, SABR and
Hull-White calibration). These caches are independent per process: total cache
memory scales with the number of workers, and every worker starts cold and warms
up separately, so the same request may be recomputed once per worker. Sharing a
cache across workers (for example an out-of-process L2/Redis tier) is a known
future option, not implemented today.

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
- `tools/quantra-manager/requirements.txt`

This packaged area is what the Docker build uses to install `quantra` into the image.

## `quantra`

- starts Envoy plus multiple workers
- provides `start`, `stop`, `status`, `restart`, `health`, and `logs`
- intended for the normal multi-process runtime

## Installation Notes

If you want a system-wide installed CLI, copy the packaged scripts from `tools/quantra-manager/` rather than relying on the repo-local helper path.

## Container Usage

The production Docker image runs both surfaces: its entrypoint starts the
process manager in foreground mode (Envoy plus the workers, gRPC on `50051`)
and then the JSON gateway on `8080`. Both ports are exposed.

```bash
docker run --rm -p 50051:50051 -p 8080:8080 quantra
```

To override worker count, set `QUANTRA_WORKERS` — replacing the command would
start the workers without the JSON gateway:

```bash
docker run --rm -e QUANTRA_WORKERS=8 -p 50051:50051 -p 8080:8080 quantra
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
