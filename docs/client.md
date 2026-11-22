# C++ Client

The C++ client library lives in `clients/cpp/` and exposes two ways to talk to the Quantra gRPC service:

- a JSON convenience layer for quick integration
- a native FlatBuffers API for lower-overhead calls

## Layout

Key files:

- `clients/CMakeLists.txt`
- `clients/cpp/include/quantra_client.h`
- `clients/cpp/src/quantra_client.cpp`
- `clients/cpp/src/json_parser.cpp`
- `src/transport/product_catalog.h`

## Usage Modes

### JSON API

Useful when requests originate as JSON and you want the client to handle JSON-to-FlatBuffers conversion:

```cpp
#include "quantra_client.h"

quantra::QuantraClient client("localhost:50051");
quantra::JsonResponse result = client.PriceFixedRateBondJSON(json_string);
```

### Native FlatBuffers API

Useful when you already build FlatBuffers messages directly:

```cpp
quantra::QuantraClient client("localhost:50051");
grpc::Status status = client.PriceFixedRateBond(request, &response);
```

The public API is declared in `clients/cpp/include/quantra_client.h`.

## Supported Client Methods

The header exposes every product the server serves, in both modes: a
`…JSON` method taking a JSON string, and a native FlatBuffers method of the
same name without the suffix. The JSON methods take an optional `request_id`,
forwarded to the engine as `x-request-id` metadata for log correlation.

Pricing:

- fixed-rate bonds, floating-rate bonds, zero-coupon bonds, callable
  fixed-rate bonds
- vanilla swaps, OIS swaps, basis swaps, zero-coupon swaps
- zero-coupon inflation swaps, year-on-year inflation swaps
- FRAs, caps and floors, swaptions
- year-on-year inflation caps/floors
- CDS
- equity options
- FX forwards
- cross-currency swaps
- commodity swaps (preview)

Utility:

- bootstrap curves, bootstrap inflation curves
- sample vol surfaces
- calendar business days, calendar holidays, calendar advance
- calibrate swaption model (Hull-White), calibrate swaption vol (SABR)
- build commodity curves (preview)

This mirrors the served endpoints in `src/transport/product_catalog.h`; when that
catalog gains an entry, the client header gains the matching pair (see
"Extending the Client For A New Product").

## Service RPCs

Beyond the product calls, the gRPC engine serves two things a client can use to
introspect and probe it. Neither is a pricing call, and neither has a C++
client wrapper — call them with any gRPC client. Both are gRPC-only; the JSON
gateway has its own `GET /meta` and `GET /health`.

### `Meta`

`quantra.QuantraServer/Meta` takes an empty `MetaRequest` and returns a
`MetaResponse`:

| Field | Contents |
| --- | --- |
| `service` | `quantra-grpc-engine` |
| `api_version` | The `VERSION` file verbatim — the same value in OpenAPI `info.version` and the `X-Quantra-Api-Version` header |
| `backend_version` | Engine build version |
| `git_sha` | Commit the binary was built from |
| `build_time_utc` | Build timestamp |
| `products` | Every product the engine serves |
| `rpc_methods` | Every registered RPC method name, including `Meta` itself |
| `dependencies` | Versions of QuantLib, gRPC, and FlatBuffers the engine was built against |

Everything is assembled from compile-time build metadata and the shared product
catalog: `Meta` touches no market data and does no pricing, so it is safe to
call on a hot server. Each worker answers `Meta` on its own thread, beside its
pricing thread, so the reply does not wait for a running calculation. When
the deployment limits calls in flight (`QUANTRA_MAX_REQUESTS`), Envoy routes
`Meta` outside that limit, so it is never refused for capacity.

`MetaResponse` has no capacity fields: the load of the cluster (workers, calls
in flight, the limit) is in the `capacity` block of the JSON gateway's
`GET /meta`, read from Envoy's admin interface.

### Health checking

The engine serves the standard `grpc.health.v1.Health` service (`Check` and
`Watch`), reporting `SERVING` once it is up. Any standard health client works,
for example:

```bash
grpc_health_probe -addr=localhost:50051
```

This is what Envoy uses to health-check workers, which is how it distinguishes
a dead worker from a busy one.

## Build

The client is built as part of the main project build:

```bash
./scripts/build.sh
```

To build just the client target after configuring the main build:

```bash
cmake --build build --target quantra_client
```

There is no standalone `clients/cpp` CMake project; the canonical build entrypoint is the repository root.

## Configuration

FlatBuffers schema resolution is controlled by:

- `QUANTRA_FBS_DIR`
- `QUANTRA_FBS_INCLUDE_DIR`

If `QUANTRA_FBS_INCLUDE_DIR` is unset, it defaults to the same location as `QUANTRA_FBS_DIR`.

## Extending the Client For A New Product

Adding a new product usually requires coordinated changes in:

1. `src/transport/product_catalog.h`
2. `clients/cpp/include/quantra_client.h`
3. `clients/cpp/src/quantra_client.cpp`
4. FlatBuffers request/response schema files
5. `flatbuffers/grpc/` service definitions
6. server-side request handlers

After schema or RPC changes, regenerate code and rebuild:

```bash
./scripts/generate_schemas.sh
./scripts/build.sh
```

## Python client: batches

`clients/python/quantra_client/batch.py` prices many trades over many markets
through the gRPC engine with as few requests as it needs. Its input is a list
of groups, one per market: `(method, request)`, where `method` is a key of
`Client.METHODS` and `request` the Object API request (`...RequestT`, market
included) whose trade list holds every trade priced on that market.

```python
from quantra_client import Client
from quantra_client.batch import price_batch, workers_from_meta

in_flight = workers_from_meta("http://localhost:8080") or 8
with Client("localhost:50051") as client:
    result = price_batch(client, [("basis_swap", market_a), ("basis_swap", market_b)],
                         in_flight=in_flight)
group = result.groups[0]
for i in range(len(group)):
    if group.error(i) is None:
        print(group.entry(i).Npv())
```

What it does:

- One request per group. A group of more than `split_above` trades is split
  evenly in proportion to its share of the batch's trades, into at most
  `in_flight` requests: one large market is spread over every call in
  flight, a batch of many markets stays one request per market. A packed
  request above `max_message_bytes` is cut further.
- At most `in_flight` calls in flight, largest first. Set it to the engine's
  worker count: `workers_from_meta(gateway_url)` reads it from the JSON
  gateway's `GET /meta` (`capacity.workers`; `None` when the gateway reports
  no capacity). The gRPC `Meta` call has no capacity.
- Requests are packed in a pool of processes while the calls already packed
  are in flight. The processes start with `forkserver` on Linux and `spawn`
  elsewhere, and each chunk is pickled to them (see "Inside a long-running
  process" below). `max_trades` caps the trades of one request.
- `RESOURCE_EXHAUSTED` from a full engine (`QUANTRA_MAX_REQUESTS`) and
  `UNAVAILABLE` are retried with exponential backoff. `DEADLINE_EXCEEDED` on
  a chunk of more than one trade splits it in two and retries, a bounded
  number of times; so does a message the engine finds too large (its
  `RESOURCE_EXHAUSTED` carries "larger than max"). Any other error is
  recorded for that chunk's trades only; the other results are kept.
- Results: `result.groups[g]` per input group, in input trade order whatever
  the split. `entry(i)` is the response entry of trade `i` as a FlatBuffers
  reader (for example `Swaps(j)` of the response that priced it), `error(i)`
  the `QuantraError` of the call that failed it, `failures` the list of
  `(start, end, error)`. `result` also carries `wall_seconds`,
  `pack_seconds` (summed over processes), `calls`, `retries`, `splits`,
  `stopped` (`None`, `'stop'` or `'deadline'`), `callback_errors` and
  `warnings` (for example a packing pool that did not shut down in time).
- `deadline` bounds the whole batch: no call gets a timeout beyond the time
  left, nothing is sent once it has passed, and the trades not priced by
  then fail with `DEADLINE_EXCEEDED` and details
  `{"reason": "BatchDeadline"}`. A chunk halved after `DEADLINE_EXCEEDED`
  keeps `timeout` per call, within that budget.
- `stop` is checked before each send: once it returns true nothing new is
  sent, the calls in flight finish, and the trades left fail with
  `CANCELLED` and details `{"reason": "Cancelled"}`.
- `metadata` is added to every call (static, or computed per call, for
  example a request id). `on_call(method, group_index, start, end, seconds,
  error)` runs after every call attempt and `on_result(group_index, start,
  end, response_bytes)` as each chunk prices, so results can be read while
  the batch runs. The callbacks run on the thread that called `price_batch`;
  one that raises is logged and kept in `result.callback_errors`, and the
  batch goes on (a `stop` that raises counts as true).

Arguments of `price_batch(target, groups, ...)`:

| Argument | Default | Meaning |
| --- | --- | --- |
| `target` | | A `Client`, a `grpc.Channel` or a `host:port` address. |
| `in_flight` | `8` | Most calls in flight. |
| `split_above` | `250` | A group is split only above this many trades. |
| `max_message_bytes` | 8 MiB − 64 KiB | A packed request above it is cut further. Keep it under the workers' `QUANTRA_MAX_MESSAGE_BYTES`. |
| `timeout` | `30` | Deadline of each call, in seconds (the engine's default limit). |
| `processes` | `None` | Packing processes started for this batch: `None` = one per CPU when the batch has 1,000 trades or more (500 with `start_method='fork'`), `0` = pack in a thread of this process (no process pool). Ignored with `pool`. |
| `max_retries` | `5` | Resends of a call refused with `RESOURCE_EXHAUSTED` or `UNAVAILABLE`. |
| `backoff` | `0.1` | First backoff, in seconds; doubled at each retry, capped at 2 s. |
| `max_splits` | `3` | Times a chunk may be split after `DEADLINE_EXCEEDED`. |
| `call` | `None` | `call(method, data, timeout)` returning the response bytes or raising `grpc.RpcError`, used instead of gRPC (tests; `metadata` is not passed to it). |
| `max_trades` | `None` | Most trades in one request: an int for every method or `{method: int}`. A group above it is cut evenly into requests of at most that many trades, also below `split_above` and beyond `in_flight` requests. |
| `deadline` | `None` | Seconds for the whole batch; `None` = no limit. |
| `force_defaults` | `True` | Pack with `ForceDefaults(True)`: a scalar equal to its schema default is written to the wire too. |
| `start_method` | `None` | `'forkserver'` (the default on Linux), `'spawn'` (the default elsewhere) or `'fork'`, for the processes started for this batch. |
| `pool` | `None` | A `BatchPool` started by the caller and reused across batches, instead of processes started for this batch. |
| `metadata` | `None` | gRPC metadata for every call: a sequence of `(key, value)` or a callable `(method, group_index, start, end)` returning one. |
| `on_call` | `None` | `on_call(method, group_index, start, end, seconds, error)` after every call attempt (`error` `None` on success). |
| `on_result` | `None` | `on_result(group_index, start, end, response_bytes)` as each chunk prices. |
| `stop` | `None` | Callable checked before each send; true = send nothing more. |

`force_defaults` changes nothing with the generated classes of this
repository: every scalar the engine requires is an optional (`= null`)
field, written whenever it is set, `Frequency.Annual` (0) included. Over the
235 example requests of the functional catalog, packed with and without
it, the engine's answers were the same bytes (the three quote-sheet
validations differ between two identical calls anyway), for 4 % more
request bytes. It matters for classes generated with a non-null default on
such a field: without it a field set to that default is left off the wire
and the engine refuses the request ("Schedule.frequency is required").
`Client(force_defaults=True)` does the same for single calls; the `Client`
default stays `False`.

Methods whose request has no trade list (`bootstrap_curves`,
`bootstrap_inflation_curves`, `build_commodity_curves`,
`validate_vol_surface`) are refused; call the `Client` method instead.

### Inside a long-running process

The packing processes start with `forkserver` on Linux and `spawn`
elsewhere. Both start them from a clean interpreter, so they never hold a
copy of the caller's threads, locks or gRPC state, and each chunk is pickled
to them (the generated `...T` classes pickle; a request that does not fails
its chunks with "packing failed", and `processes=0` packs it in a thread
instead). As with any `spawn` or `forkserver` pool, a script must guard its
entry point with `if __name__ == "__main__":`, and the forkserver imports
the main module once, so that module should not open channels or start
threads when imported.

`start_method='fork'` copies the calling process. It is the fastest to
start, but it is unsafe in a process that holds gRPC channels or runs
threads: in a process with other threads calling gRPC, forked children
aborted inside gRPC or blocked in its fork handlers, the pool restarted
them, and a pool shut down with `terminate()` could wait forever for a child
that inherited the caller's `SIGTERM` handler (the 0.17.2 hang). Use it only
in a plain script.

A pool started for one batch is shut down gracefully (`close()` then
`join()`) when the batch ends, with `terminate()` only when the batch raised;
no step waits more than about ten seconds, and a pool that will not stop is
left behind with a warning in `result.warnings` rather than blocking the
caller. A process that prices batch after batch can start the processes once
and pass them to every batch:

```python
from quantra_client.batch import BatchPool, price_batch

with BatchPool(processes=8) as pool:          # forkserver on Linux
    for groups in batches:
        result = price_batch(client, groups, pool=pool, deadline=300)
```

A `BatchPool` may serve several batches at once from different threads;
`close()` (or leaving the `with` block) shuts it down the same bounded way.
Starting a forkserver pool costs little, but each of its processes imports
the generated classes when it unpickles its first chunk (about 0.2 s), so a
pool started per batch adds about 0.15–0.3 s to every batch; a reused pool
pays it once.

### Measurements

Measured on 8 workers with basis swaps over three curves: building a market
costs a worker about 28 ms and pricing a swap about 0.4 ms, while packing a
swap with the Python Object API costs about 0.17 ms in one process, so a
16,000-swap market packed in one thread took 2.7 s against 0.9 s to price.
End to end, median of three runs, by how the packing processes start (one
thread = `processes=0`):

| Batch | forkserver, per batch | `BatchPool` reused | fork, per batch | one thread |
| --- | --- | --- | --- | --- |
| 1 market of 16,000 swaps | 1.75 s | 1.63 s | 1.41 s | 3.75 s |
| 64 markets of 24 swaps | 0.61 s | 0.42 s | 0.45 s | 0.83 s |
| 200 markets of 12 swaps | 1.23 s | 1.06 s | 1.05 s | 2.09 s |
| 4 markets of 250 swaps | 0.33 s | 0.30 s | 0.22 s | 0.32 s |
| 8 markets of 250 swaps | 0.38 s | 0.25 s | 0.28 s | 0.52 s |

(`spawn`: 1.74 s, 0.86 s and 1.40 s on the first three.) Splitting a market
lost at 100 swaps and won from 250 (a single market of 250: 0.18 s whole,
0.13 s split), hence the default `split_above`; for trades that take much
longer to price than to build their market (structured notes), lower it.

`max_trades` has no default: the best cap depends on the product. On the
16,000-swap market, requests of at most 250 swaps took 1.22 s against 1.63 s
for the 8 requests of the default split (smaller requests start pricing
sooner). On 10 markets of 200 Bermudan swaptions under an explicit
Hull-White model a cap of 100 took 0.79 s against 1.13 s for one request per
market, while on 10 markets of 200 swaptions whose Hull-White model is
calibrated in each request a cap of 25 took 0.56 s against 0.47 s, the
calibration being repeated in every request.

`clients/python/examples/price_batch.py` prices a synthetic bond book over
several markets.

## Notes

- The JSON layer is easier to integrate but does more conversion work.
- The native API is the better fit for high-throughput or type-safe client code.
