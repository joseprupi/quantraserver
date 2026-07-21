# Configuration

Every runtime knob is an environment variable. This page is the complete list;
anything not here is not read by the server, the gateway, or the process
manager.

The two processes read different variables: the **engine** (`sync_server`) owns
the pricing caches and the request budget, the **gateway** (`json_server`) owns
the HTTP surface. In the shipped container both run side by side, so setting a
variable with `docker run -e` reaches whichever process reads it.

## Runtime and process layout

| Variable | Read by | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_WORKERS` | container entrypoint | `min(cores, 8)` | Number of single-threaded engine worker processes started behind Envoy. |
| `QUANTRA_GRPC_TARGET` | container entrypoint | `127.0.0.1:50051` | gRPC address the gateway connects to (the Envoy front). |
| `QUANTRA_HTTP_PORT` | container entrypoint | `8080` | Port the JSON gateway listens on. |
| `QUANTRA_STARTUP_WAIT` | container entrypoint | `3` | Seconds to wait for the engine cluster before starting the gateway. |
| `QUANTRA_ENVOY_ADMIN` | gateway | unset | Envoy admin endpoint (`host:port` or `http://host:port`) used to fill the `envoy` section of `GET /status`. Set to `http://127.0.0.1:9901` in the container. Unset means `/status` reports no worker membership. |
| `QUANTRA_HOME` | process manager | `/workspace` (`/app` in the container) | Where the manager looks for the `sync_server` binary. |
| `QUANTRA_STATE_DIR` | process manager | `/workspace/.quantra` (`/app/.quantra` in the container) | Runtime state: pid files, logs, the generated `envoy.yaml`. |
| `QUANTRA_FOREGROUND` | process manager | unset | `1`/`true`/`yes` makes `quantra start` default to foreground. |
| `QUANTRA_SERVER_BIND_HOST` | engine | `127.0.0.1` | Interface a worker binds. Change it only when a worker must be reachable from outside its own network namespace. |

## Request bounding

| Variable | Read by | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_REQUEST_BUDGET_MS` | engine | unset (no ceiling) | Server-side compute budget per request, in milliseconds. Combined with the caller's gRPC deadline (whichever is earlier); when it expires mid-request the engine abandons work at a per-curve/per-trade checkpoint and returns `DEADLINE_EXCEEDED` (HTTP 504). The container sets `35000`, slightly above Envoy's 30s route timeout. A non-positive or unparseable value means "no ceiling". |

## Pricing caches

All three caches are **off** unless enabled, and all three are transparent: a
hit is bit-for-bit identical to a miss. They live in the engine process, so
their log lines appear in the engine's output, not the gateway's.

| Variable | Read by | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_CURVE_CACHE_ENABLED` | engine | off | `1` enables the bootstrapped-curve cache. Set to `1` in the container. |
| `QUANTRA_CURVE_CACHE_LOG` | engine | off | `1` logs every curve-cache hit/miss (`[CurveCache] … event=L1_HIT`). |
| `QUANTRA_CURVE_CACHE_MAX_ENTRIES` | engine | `100` | LRU capacity of the curve cache. An unparseable or non-positive value logs a warning and keeps the default. |
| `QUANTRA_SABR_CACHE_ENABLED` | engine | off | `1` enables the SABR swaption-cube calibration cache (fixed LRU capacity). Set to `1` in the container. |
| `QUANTRA_SABR_CACHE_LOG` | engine | off | `1` logs SABR cache hits/misses. |
| `QUANTRA_HW_CACHE_ENABLED` | engine | off | `1` enables the Hull-White model-calibration cache (fixed LRU capacity). Not set in the shipped container. |
| `QUANTRA_HW_CACHE_LOG` | engine | off | `1` logs Hull-White calibration cache hits/misses. |

The cache flags are read once, at first use, and cached for the process
lifetime — changing them on a running process has no effect.

## Client and schema resolution

| Variable | Read by | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_FBS_DIR` | C++ client, gateway | `flatbuffers/fbs` | Directory holding the `.fbs` schemas used for JSON ↔ FlatBuffers conversion. Set to `/app/flatbuffers/fbs` in the container. |
| `QUANTRA_FBS_INCLUDE_DIR` | C++ client, gateway | same as `QUANTRA_FBS_DIR` | Include path passed to the schema parser. |

## Diagnostics

| Variable | Read by | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_SMILE_SANITY_CHECKS` | engine | off | `1` prints a warning when an interpolated swaption vol cube's total variance decreases with expiry. Diagnostic only; it never changes a price or fails a request. |

## Set by the shipped container

`ghcr.io/joseprupi/quantra-server` sets these in the image:

```
QUANTRA_HOME=/app
QUANTRA_STATE_DIR=/app/.quantra
QUANTRA_ENVOY_ADMIN=http://127.0.0.1:9901
QUANTRA_FBS_DIR=/app/flatbuffers/fbs
QUANTRA_FBS_INCLUDE_DIR=/app/flatbuffers/fbs
QUANTRA_CURVE_CACHE_ENABLED=1
QUANTRA_SABR_CACHE_ENABLED=1
QUANTRA_REQUEST_BUDGET_MS=35000
```

`QUANTRA_WORKERS`, `QUANTRA_GRPC_TARGET`, `QUANTRA_HTTP_PORT` and
`QUANTRA_STARTUP_WAIT` are not baked into the image; the entrypoint defaults
them at start and honours anything you pass with `-e`.

## Legacy / not used by the server

- `QUANTRA_PORT`, `QUANTRA_SERVER_PORT` — no binary reads them. Ports come
  from the `quantra` CLI flags and the gateway's command-line arguments.
- `QUANTRA_REDIS_HOST`, `QUANTRA_REDIS_PORT`,
  `QUANTRA_CURVE_CACHE_TTL_SECONDS` — named in a design comment for a
  second-level Redis curve cache that is not implemented. Nothing reads them.

`QUANTRA_GRPC_LOG`, `QUANTRA_BENCH_IMAGE` and `QUANTRA_WORKSPACE_DIR` exist
only inside the test and benchmark harnesses and are not part of the runtime
contract.
