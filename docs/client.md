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
- Requests are packed in a pool of processes (fork on Linux, with only the
  trade ranges crossing to the processes; spawn elsewhere, with the chunk
  pickled), while the calls already packed are in flight.
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
  `pack_seconds` (summed over processes), `calls`, `retries` and `splits`.

Arguments of `price_batch(target, groups, ...)`:

| Argument | Default | Meaning |
| --- | --- | --- |
| `target` | | A `Client`, a `grpc.Channel` or a `host:port` address. |
| `in_flight` | `8` | Most calls in flight. |
| `split_above` | `250` | A group is split only above this many trades. |
| `max_message_bytes` | 8 MiB − 64 KiB | A packed request above it is cut further. Keep it under the workers' `QUANTRA_MAX_MESSAGE_BYTES`. |
| `timeout` | `30` | Deadline of each call, in seconds (the engine's default limit). |
| `processes` | `None` | Packing processes: `None` = one per CPU when the batch has 500 trades or more, `0` = pack in a thread of this process (no process pool). |
| `max_retries` | `5` | Resends of a call refused with `RESOURCE_EXHAUSTED` or `UNAVAILABLE`. |
| `backoff` | `0.1` | First backoff, in seconds; doubled at each retry, capped at 2 s. |
| `max_splits` | `3` | Times a chunk may be split after `DEADLINE_EXCEEDED`. |

Methods whose request has no trade list (`bootstrap_curves`,
`bootstrap_inflation_curves`, `build_commodity_curves`,
`validate_vol_surface`) are refused; call the `Client` method instead.

Measured on 8 workers with basis swaps over three curves: building a market
costs a worker about 28 ms and pricing a swap about 0.4 ms, while packing a
swap with the Python Object API costs about 0.17 ms in one process, so a
16,000-swap market packed in one thread took 2.7 s against 0.9 s to price.
Through this module that market takes 1.4–1.6 s (3.8–5.2 s packed in one
thread and sent 8 at a time), 64 markets of 24 swaps 0.5 s (0.9 s) and 200
markets of 12 swaps 1.0–1.2 s (2.3–2.6 s). Splitting a market lost at 100
swaps and won from 250 (a single market of 250: 0.18 s whole, 0.13 s split),
hence the default `split_above`; for trades that take much longer to price
than to build their market (structured notes), lower it.

`clients/python/examples/price_batch.py` prices a synthetic bond book over
several markets.

## Notes

- The JSON layer is easier to integrate but does more conversion work.
- The native API is the better fit for high-throughput or type-safe client code.
