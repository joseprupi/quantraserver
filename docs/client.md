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
| `schema_hash` | SHA-256 over the schema files (`scripts/schema_hash.py`); the Python client's `SCHEMA_HASH` for the same source tree (see "Python client: request blocks") |
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

## Prices only

Every pricing request with a trade list takes `price_only` (default false;
`docs/http-api.md`, "Prices only"). With it the response holds one NPV per
trade in `npvs`, in request order and equal bit for bit to the NPVs of the
full response, and an empty per-trade list; the engine skips the work that
feeds only the dropped fields. Without it nothing changes.

- **Python, single calls**: set `request.priceOnly = True` on the request
  object; `response_npvs(response)` (from `quantra_client`) returns the NPVs,
  a numpy array when numpy is importable, else a list (`response.Npvs(j)`,
  `response.NpvsLength()` and `response.NpvsAsNumpy()` are the generated
  accessors).
- **Python, `price_batch`**: object groups carry the flag on their request
  object; block groups take it as a request field
  (`BlockGroup(blocks, pricing_block=market, price_only=True)`,
  `assemble_request(..., price_only=True)`). `GroupResult.npvs()` returns the
  NPVs of the group in input trade order across its requests (NaN for a
  failed trade), a numpy array when numpy is importable, else a list; it
  reads `Npv()` of each entry for a group priced without the flag.
  `GroupResult.entry(i)` of a price-only result raises `ValueError` (there is
  no entry to read).
- **C++**: the generated code is enough. Set the flag with the request
  builder (`PriceVanillaSwapRequestBuilder::add_price_only(true)`) or, through
  the JSON client methods, `"price_only": true` in the body; read
  `response->npvs()` (a `flatbuffers::Vector<double>`; `swaps()` is empty).

Measured on one engine worker called directly over gRPC, about 2,000 copies
of one example trade per request, warm market (the machine was shared;
engine times are ranges of three runs):

| Method | Trades | Call, full (s) | Call, price-only (s) | Response bytes, full | Response bytes, price-only | Python: `Npv()` per entry (ms) | Python: full unpack (ms) | Python: price-only, no numpy (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fixed-rate bond | 2,000 | 0.022–0.037 | 0.022 | 56,072 | 16,032 | 16 | 48 | 4.5 |
| Floating-rate bond | 2,000 | 0.095–0.097 | 0.095–0.098 | 56,072 | 16,032 | 15 | 51 | 4.5 |
| Zero-coupon bond | 2,000 | 0.024–0.025 | 0.011–0.012 | 232,072 | 16,032 | 15 | 54 | 4.6 |
| Callable bond | 33 | 2.95–2.96 | 3.00–3.07 | 1,776 | 296 | 0.3 | 0.9 | 0.3 |
| Vanilla swap | 2,000 | 0.124–0.129 | 0.112–0.117 | 184,056 | 16,032 | 15 | 93 | 4.5 |
| OIS swap | 2,000 | 0.378–0.382 | 0.377–0.387 | 184,056 | 16,032 | 17 | 80 | 4.6 |
| Basis swap | 2,000 | 0.266–0.277 | 0.264–0.269 | 168,064 | 16,032 | 17 | 76 | 4.5 |
| Zero-coupon swap | 2,000 | 0.056–0.057 | 0.050 | 136,072 | 16,032 | 18 | 41 | 4.6 |
| FRA | 2,000 | 0.012–0.013 | 0.007 | 104,056 | 16,032 | 10 | 29 | 4.6 |
| FX forward | 2,000 | 0.022–0.024 | 0.021 | 200,056 | 16,032 | 19 | 44 | 4.6 |
| Cross-currency swap | 2,000 | 0.561–0.562 | 0.554–0.569 | 264,072 | 16,032 | 16 | 99 | 4.7 |
| Commodity swap | 391 | 0.317–0.325 | 0.033 | 6,934,800 | 3,168 | 3.5 | 1,928 | 0.9 |
| Structured note | 58 | 2.90–2.96 | 2.84–2.89 | 159,792 | 496 | 0.6 | 60 | 0.2 |
| Equity option | 2,000 | 0.031–0.040 | 0.009–0.010 | 232,048 | 16,032 | 10 | 52 | 4.3 |
| Cap/floor | 2,000 | 0.171–0.185 | 0.157–0.161 | 72,040 | 16,032 | 17 | 34 | 4.5 |
| Swaption | 2,000 | 0.144–0.149 | 0.120–0.121 | 200,104 | 16,040 | 16 | 91 | 4.3 |
| CDS | 2,000 | 1.14–1.16 | 1.13–1.16 | 104,072 | 16,032 | 17 | 31 | 4.5 |
| Zero-coupon inflation swap | 2,000 | 0.021–0.037 | 0.020 | 120,056 | 16,032 | 16 | 34 | 4.4 |
| Year-on-year inflation swap | 2,000 | 0.060–0.061 | 0.056–0.058 | 152,056 | 16,032 | 17 | 41 | 4.4 |
| YoY inflation cap/floor | 2,000 | 0.051–0.054 | 0.043 | 56,040 | 16,032 | 17 | 22 | 4.6 |

"Call" is the gRPC call as the client sees it (pricing, response building and
transfer). With numpy importable, reading the 2,000 price-only NPVs takes
about 0.07 ms. Fewer trades where 2,000 copies would take much longer than
3 s per call or exceed the 8 MiB message limit (commodity swaps answer with
their flows: 6.9 MB for 391 swaps). Price-only saves little where the NPV is
nearly all the work (callable bonds on a tree, CDS, structured notes, OIS
and cross-currency swaps) and most where the dropped outputs cost (equity
option implied vols and greeks, zero-coupon bond analytics, FRA forward
rates, commodity flows); the client side saves the decoding in every case.

## Python client: request blocks

`clients/python/quantra_client/blocks.py` builds requests from trades and
markets packed once and kept as bytes, and changes market values inside a
packed request without packing it again. A block is one table packed alone:
a trade (the element of a request's trade list, for example a
`PriceBasisSwapT`) or a market (`PricingT`).

```python
from quantra_client import (Client, SCHEMA_HASH, assemble_request, pack_market_block,
                            pack_trade_block, patch_values)

trades = [pack_trade_block(t) for t in request.swaps]     # once; can be stored
market, slots = pack_market_block(request.pricing)          # once per market
req = assemble_request("basis_swap", trades, pricing_block=market, include_flows=False)
response = client.channel.unary_unary(Client.METHODS["basis_swap"])(req.data)

# a scenario: two curve values moved in place, one of them to exactly 0.0
bumped = patch_values(req.data, req.pricing_at, slots, {
    "rates.curves[EUR_6M].points[3].point.rate": 0.0251,
    "rates.curves[EUR_6M].points[4].point.rate": 0.0,
})
```

| Function | Returns |
| --- | --- |
| `pack_trade_block(trade_obj, force_defaults=True)` | `Block(data, table_offset, type_name)`: one trade packed alone. |
| `pack_market_block(pricing_obj)` | `(Block, slots)`: the market packed alone with `ForceDefaults`, and `{path: Slot(offset, kind)}` for every numeric market value. |
| `assemble_request(method, blocks, pricing=None, pricing_block=None, **request_fields)` | `AssembledRequest(data, pricing_at)`: a finished request for the `Client.METHODS` key `method`; the market as an object (`pricing`, packed here) or as a block (`pricing_block`, copied); the request's other fields by schema or object name (`include_flows=True`). |
| `market_slots(buffer, pricing_at)` | The slots of the Pricing table at `pricing_at` of any finished request or market block. |
| `patch_values(buffer, pricing_at, slots, {path: value})` | The buffer with those values: a `bytearray` is changed in place and returned, `bytes` give new bytes. |
| `verify_request(method, data)` | `None`, or `ValueError` naming the first structural problem the engine's FlatBuffers verifier would refuse (tables, vtables, strings and vectors inside the buffer and aligned, union members known). |
| `BlockGroup(blocks, pricing=None, pricing_block=None, **request_fields)` | A `price_batch` group given as blocks: `("basis_swap", BlockGroup(trades, pricing_block=market))`. |
| `SCHEMA_HASH` | The schema the blocks are packed for (below). |

**Layout.** `Block.data` is a byte string whose length is a multiple of 8;
`Block.table_offset` is the position of the block's table counted back from
the end of `data` (the table starts at `len(data) - table_offset`,
`Block.table_position`). Everything the table refers to (its vtables,
strings, vectors, sub-tables) is inside `data`, every reference is relative,
and every value is aligned relative to the end of `data`, so the block reads
the same wherever it is placed with its end 8-aligned. Leading bytes before
the packed content are zero padding. `type_name` (for example
`"quantra.PriceBasisSwap"`) is not part of the bytes; `assemble_request`
refuses a block of another type than the method's trades, and a block rebuilt
from storage as `Block(data, table_offset)` has `type_name` `None`. To store
a block, store `data` and `table_offset`.

**Assembly.** The request's root table (its scalar fields, and the market
when given as an object) is packed once per call of `assemble_request` (once
per group in `price_batch`); the market block, the vector of trade offsets
and the trade blocks follow it, in that order. The result is a finished,
8-aligned buffer the engine verifies as any other request, and it prices
byte for byte as the request packed normally (checked over every example
request of every method). An assembled request is about 27 % larger than the
same request packed normally (12,000 basis swaps: 5.82 MB against 4.57 MB):
each block carries its own vtables, where a normal request shares them.
`max_message_bytes` and the engine's message limit (8 MiB by default,
`QUANTRA_MAX_MESSAGE_BYTES`) apply to the assembled size.

**Validity.** A block is valid only for the schema it was packed with. Keep
`SCHEMA_HASH` next to stored blocks and pack them again when it changes. The
engine reports its own value as `schema_hash` in the JSON gateway's
`GET /meta` and in the gRPC `Meta` reply; both come from
`scripts/schema_hash.py` (SHA-256 over `flatbuffers/fbs/*.fbs`), which also
writes the client's constant (`clients/python/quantra_client/_schema.py`,
regenerated by `scripts/generate_schemas.sh`). A block may also depend on
the market it is priced on through the ids it names (curves, indices,
surfaces): reuse it only with a market that has them.

**Market paths.** A slot's path is built from the schema, starting at the
Pricing table:

- field names joined by `.` (`rates.curves`);
- a vector element as `name[k]` (index `k`), or as `name[ID]` when every
  element of that vector is a table with a non-empty, unique string field
  `id` made of characters other than `.`, `[` and `]` and not all digits;
- a union by its field name (`points[3].point.rate`; the member type is the
  one in the buffer);
- a vector of numbers as `name[k]`.

Examples: `rates.curves[EUR_OIS].points[3].point.rate`,
`quotes[SPX_SPOT].value`, `credit.credit_curves[acme_ig_eur].recovery_rate`,
`commodity.contracts[SYN_CL].months[1].price`,
`volatility.models[hw_explicit].payload.hw_a`, `rates.indices[EUR_6M].fixing_days`.

**What can be patched.** Every number of the market present in the buffer:
doubles, floats and integers that are not enums (a curve point, a quote, a
vol node, a spread, a recovery rate, a fixing value, a model parameter, an
integer such as a fixing-day count). A float slot takes any real number,
exactly `0.0` included (the market block is packed with `ForceDefaults`, so
a value equal to the schema default is still in the buffer); an integer slot
takes an int within its width; anything else is refused before any byte is
written. Not patchable: strings (ids, dates such as the as-of date),
booleans, enums, an optional value left unset (`= null` in the schema, it is
not in the buffer), and anything whose change alters the length of the
buffer (a pillar added or removed, a fixing appended, another union member).
For those, pack the market again. The slots of `pack_market_block` belong to
the block's layout: they apply to every request assembled from that block
(`pricing_at` of the request, or `Block.table_position` for the block
itself); for a request assembled with the market as an object, read them
with `market_slots(req.data, req.pricing_at)`.

**In `price_batch`.** A `BlockGroup` is split like any group (`split_above`,
`max_trades`, `in_flight`) and cut by `max_message_bytes` from the block
sizes before anything is copied; its requests are assembled on the calling
thread, without packing processes. Retries, halving on `DEADLINE_EXCEEDED`,
splitting on a size refusal, hooks and results are those of any group, and a
batch may mix object groups and block groups.

Measured with 12,000 basis swaps over three curves (one Python thread, a
shared machine; FlatBuffers 24.12.23 / 25.12.19):

| Step | Time | Bytes |
| --- | --- | --- |
| Pack the request normally | 2.26 s / 1.75 s | 4.57 MB |
| Pack the 12,000 trade blocks (once) | 2.97 s / 2.24 s | 480 per trade |
| Pack the market block and its 474 slots (once) | 7 ms | 10.1 KB |
| Assemble the request from the blocks | 6 ms / 7 ms | 5.82 MB |
| Assemble, the market given as an object | 12 ms / 19 ms | |
| Patch 30 market values, new bytes | 2.9 ms / 1.7 ms | |
| Patch 30 market values in place (`bytearray`) | 0.04 ms / 0.03 ms | |

## Notes

- The JSON layer is easier to integrate but does more conversion work.
- The native API is the better fit for high-throughput or type-safe client code.
