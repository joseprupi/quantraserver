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
| `QUANTRA_ENVOY_ADMIN` | gateway | unset | Envoy admin endpoint (`host:port` or `http://host:port`) used to fill the `envoy` section of `GET /status` and the `capacity` block of `GET /meta`. Set to `http://127.0.0.1:9901` in the container. Unset means `/status` reports no worker membership and `capacity` is `null`. |
| `QUANTRA_HOME` | process manager | `/workspace` (`/app` in the container) | Where the manager looks for the `sync_server` binary. |
| `QUANTRA_STATE_DIR` | process manager | `/workspace/.quantra` (`/app/.quantra` in the container) | Runtime state: pid files, logs, the generated `envoy.yaml`. |
| `QUANTRA_FOREGROUND` | process manager | unset | `1`/`true`/`yes` makes `quantra start` default to foreground. |
| `QUANTRA_SERVER_BIND_HOST` | engine | `127.0.0.1` | Interface a worker binds. Change it only when a worker must be reachable from outside its own network namespace. |

## Request bounding

| Variable | Read by | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_REQUEST_BUDGET_MS` | engine | unset (no ceiling) | Server-side compute budget per request, in milliseconds. Combined with the caller's gRPC deadline (whichever is earlier); when it expires mid-request the engine abandons work at its next checkpoint (below, "What stops at the deadline") and returns `DEADLINE_EXCEEDED` (HTTP 504). The container sets `35000`. A non-positive or unparseable value means "no ceiling". |
| `QUANTRA_GATEWAY_DEADLINE_MS` | gateway (C++ client) | `30000` | Deadline, in milliseconds, the JSON gateway sets on every gRPC call to the engine (sent as the call's `grpc-timeout`); the default equals the default `QUANTRA_ROUTE_TIMEOUT_MS`. Read once at startup; the gateway logs the value it uses (`[gateway] RPC deadline 30000 ms`). A non-positive or unparseable value logs a line and keeps the default. When it expires the gateway answers HTTP 504. |
| `QUANTRA_MAX_MESSAGE_BYTES` | engine | `8388608` (8 MiB) | Largest gRPC message a worker accepts or sends, in bytes. A request above it is refused with `RESOURCE_EXHAUSTED` "Received message larger than max (N vs. limit)", a response above it with "Sent message larger than max" (the work is done, then dropped); the JSON gateway answers either with HTTP 413. A value set is logged once at startup (`[engine] max message 16777216 bytes`); a value that is not a positive whole number up to 2147483647 logs a line and keeps the default. The process manager passes it to the workers it starts, so `docker run -e` reaches them. Envoy sets no lower limit of its own (a 9 MB request at 16 MiB goes through). The JSON gateway refuses request bodies above 10 MiB of JSON with HTTP 413 whatever this value, so a request that large only arrives over gRPC. |
| `QUANTRA_ROUTE_TIMEOUT_MS` | process manager, `scripts/envoy_config.py` | `30000` | Envoy's per-request limit, in milliseconds, written into the generated `envoy.yaml` at `quantra start`: the route `timeout`, and the cap on a caller's `grpc-timeout` header (`max_stream_duration.grpc_timeout_header_max`) becomes `max(60 s, this value)`. A non-positive or unparseable value logs a line and keeps the default. `scripts/envoy_config.py --request-timeout` overrides it for a hand-generated config. |

### Which limit binds

A request through the JSON gateway meets three limits, and the smallest wins:

1. **Envoy.** For gRPC traffic (everything the gateway sends) Envoy does not
   apply the route `timeout`: it replaces it with the caller's `grpc-timeout`
   capped at `grpc_timeout_header_max`, `max(60 s, QUANTRA_ROUTE_TIMEOUT_MS)`
   (60 s by default). A gRPC client that sends no deadline gets that cap.
2. **The gateway deadline**, `QUANTRA_GATEWAY_DEADLINE_MS` (30 s by
   default): the `grpc-timeout` every gateway call carries. This is the limit
   that binds by default (an Andreasen-Huge calibration on a large sheet
   can take about 10 s).
3. **The engine budget**, `QUANTRA_REQUEST_BUDGET_MS` (35 s in the
   container): the engine stops at its next checkpoint once the earlier of
   this budget and the propagated deadline has passed, so a worker frees
   itself even when the caller has already given up.

### What stops at the deadline

The engine checks the request's time (the earlier of the caller's deadline
and `QUANTRA_REQUEST_BUDGET_MS`) at these points and, once it is over, stops
and answers `DEADLINE_EXCEEDED` (HTTP 504); the worker is then free for the
next call, and nothing half-built is kept:

- **Monte Carlo** (structured notes: the price, every Greeks bump and
  diagnostics run; the simulation check of `/validate-vol-surface`): inside
  the simulation, every 32 samples. A 5-year local-vol note at 2,048 paths
  that takes about 6 s, sent with a 1 s deadline, leaves the worker free a
  fraction of a second after the deadline (it stayed busy about 5 s more
  before this check existed). A run the check does not stop gives the same
  numbers as before, bit for bit.
- **Vol surface calibration**: before each SVI slice fit and refit, each
  local-vol refit pass, each Andreasen-Huge expiry (both stages of the
  expiry in progress finish first) and the eSSVI fit; the implied-forward
  fit, before each expiry.
- **Elsewhere**: between curves while bootstrapping, between trades or notes
  in a request, and between the runs of a note.

What still runs to its end once started: one call into a QuantLib
optimiser or pricing engine — the Hull-White and SABR calibrations, one SVI
or Andreasen-Huge expiry fit, the eSSVI fit, a finite-difference or tree
pricing (callable bonds, Bermudan swaptions), one curve's bootstrap. A
request stuck in one of them frees its worker when that call returns.

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

## Load control

By default Envoy accepts every call and sends it to a worker; when every
worker is busy, the call waits inside a busy worker until that worker is free
(or until the caller's deadline). The knobs below change that. Each is an
environment variable read by the process manager when it writes Envoy's
config at `quantra start` (the container entrypoint passes only environment
variables), and a `quantra start` / `restart` flag of the same name that wins
over the variable; `scripts/envoy_config.py` takes the same flags. With none
of them set, the generated config is exactly the one of earlier releases.

| Variable | Flag | Default | Purpose |
| --- | --- | --- | --- |
| `QUANTRA_MAX_REQUESTS` | `--max-requests N` | `10000` (no limit in practice) | Calls in flight across all workers. Set it to the worker count and, once every worker has a call, the next call is refused at once with gRPC `RESOURCE_EXHAUSTED` (8); the JSON gateway answers HTTP `429`. Set to anything other than `10000`, it also routes the `Meta` RPC to a second cluster on the same workers, so the limit never refuses `Meta`. |
| `QUANTRA_MAX_PENDING` | `--max-pending N` | `10000` | Envoy's `max_pending_requests` limit: calls waiting for a connection to a worker. It does **not** queue calls for busy workers: a call over `QUANTRA_MAX_REQUESTS` is refused at once, whatever this value. |
| `QUANTRA_RETRIES` | `--retries N` | `0` (no retry) | Resend a call whose worker died or could not be reached to another worker, up to `N` times. Nothing else is retried (below). |
| `QUANTRA_STATS_BIND` | `--stats-bind ADDR` | unset (no listener) | Address of a read-only listener (for example `0.0.0.0`) that serves only `GET /stats/prometheus` and `GET /ready` from Envoy's admin interface and answers `404` to everything else. The admin interface itself stays on `127.0.0.1`. |
| `QUANTRA_STATS_PORT` | `--stats-port N` | `9902` | Port of that listener. Publish it (`-p 9902:9902`) to scrape it from outside the container. |
| `QUANTRA_ENVOY_CONFIG` | `--envoy-config PATH` | unset (generated config) | Start Envoy with this file instead of a generated one. It is checked with `envoy --mode validate` first: if Envoy rejects it, the manager prints Envoy's error and exits without starting anything. It then warns (and starts anyway) when the file has no listener on the gRPC port or no admin interface on the manager's admin port (`GET /status` and the `capacity` of `GET /meta` are then empty). The knobs above do not apply to such a file. |

A non-integer or out-of-range value in a variable logs one line and keeps the
default; the same value on a flag is an error.

What each setting does and does not do, as measured on the shipped Envoy
(1.28):

- **A full cluster is refused only with `QUANTRA_MAX_REQUESTS` set.** Without
  it nothing is refused. Envoy's own overflow answer is gRPC `UNAVAILABLE`;
  the generated config turns it into `RESOURCE_EXHAUSTED` / `429` whenever the
  limit is set. The limit counts calls across the cluster, not per worker:
  Envoy's least-request choice can still send a call to a busy worker while
  another is free (about 1.5 % of calls with 7 of 8 workers busy), and that call
  waits inside the busy worker.
- **The limit counts callers, not computations.** When a caller gives up (its
  deadline passes), Envoy stops counting the call at once. The worker stops
  too, at its next checkpoint ("What stops at the deadline" above): within a
  fraction of a second for Monte Carlo and most calibration work, but only
  at the end of the call in progress for the computations listed there as
  running to their end. Until it does, a new call can be admitted and wait
  behind it. The same applies to `in_flight` in `GET /meta`.
- **Retries cover a worker that dies or cannot be reached** (`retry_on:
  reset,connect-failure`, preferring a worker not tried yet). The work done before
  the failure is lost and the call starts again on another worker; the route
  timeout covers all attempts. A refusal by the limit is not retried, and a
  busy worker returns no error, so nothing waits for a worker to become free.
  Pricing calls have no side effects, so resending one is safe.
- **What a client should do on `RESOURCE_EXHAUSTED` / `429`:** back off and
  send the call again. The engine does not queue calls.

Envoy's adaptive-concurrency filter is not offered. It sets its limit from
the fastest response times it sees, and pricing calls range from a
millisecond to minutes depending on the product: in a test with 4 workers,
after a period of fast calls followed by 6-second structured notes, it cut
its limit to 3 calls within 30 seconds and refused the fourth note over a
hundred times while a worker was free.

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
