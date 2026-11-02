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
| `QUANTRA_REQUEST_BUDGET_MS` | engine | unset (no ceiling) | Server-side compute budget per request, in milliseconds. Combined with the caller's gRPC deadline (whichever is earlier); when it expires mid-request the engine abandons work at a per-curve/per-trade checkpoint and returns `DEADLINE_EXCEEDED` (HTTP 504). The container sets `35000`. A non-positive or unparseable value means "no ceiling". |
| `QUANTRA_GATEWAY_DEADLINE_MS` | gateway (C++ client) | `10000` | Deadline, in milliseconds, the JSON gateway sets on every gRPC call to the engine (sent as the call's `grpc-timeout`). Read once at startup; the gateway logs the value it uses (`[gateway] RPC deadline 10000 ms`). A non-positive or unparseable value logs a line and keeps the default. When it expires the gateway answers HTTP 504. |
| `QUANTRA_ROUTE_TIMEOUT_MS` | process manager, `scripts/envoy_config.py` | `30000` | Envoy's per-request limit, in milliseconds, written into the generated `envoy.yaml` at `quantra start`: the route `timeout`, and the cap on a caller's `grpc-timeout` header (`max_stream_duration.grpc_timeout_header_max`) becomes `max(60 s, this value)`. A non-positive or unparseable value logs a line and keeps the default. `scripts/envoy_config.py --request-timeout` overrides it for a hand-generated config. |

### Which limit binds

A request through the JSON gateway meets three limits, and the smallest wins:

1. **Envoy.** For gRPC traffic (everything the gateway sends) Envoy does not
   apply the route `timeout`: it replaces it with the caller's `grpc-timeout`
   capped at `grpc_timeout_header_max`, `max(60 s, QUANTRA_ROUTE_TIMEOUT_MS)`
   (60 s by default). A gRPC client that sends no deadline gets that cap.
2. **The gateway deadline**, `QUANTRA_GATEWAY_DEADLINE_MS` (10 s by
   default): the `grpc-timeout` every gateway call carries. This is the limit
   that binds by default.
3. **The engine budget**, `QUANTRA_REQUEST_BUDGET_MS` (35 s in the
   container): the engine stops at its next checkpoint once the earlier of
   this budget and the propagated deadline has passed, so a worker frees
   itself even when the caller has already given up.

Whichever expires first gives HTTP 504 (`DEADLINE_EXCEEDED`). For a long
batch — a calibration on thousands of quotes, a 65,536-path local-vol note
with `steps_per_year: 252` — raise the gateway deadline and the engine budget
together, and the Envoy cap when going past 60 s, e.g. for 120 s:

```
docker run -e QUANTRA_GATEWAY_DEADLINE_MS=120000 \
           -e QUANTRA_REQUEST_BUDGET_MS=125000 \
           -e QUANTRA_ROUTE_TIMEOUT_MS=120000 ...
```

Workers are single-threaded: a long request occupies one worker for its
whole duration.

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
| `QUANTRA_HW_CACHE_ENABLED` | engine | off | `1` enables the Hull-White model-calibration cache (fixed LRU capacity). Set to `1` in the container (from 0.5.0). |
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
