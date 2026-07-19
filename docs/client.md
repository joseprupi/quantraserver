# C++ Client

The C++ client library lives in `client/` and exposes two ways to talk to the Quantra gRPC service:

- a JSON convenience layer for quick integration
- a native FlatBuffers API for lower-overhead calls

## Layout

Key files:

- `client/CMakeLists.txt`
- `client/cpp/include/quantra_client.h`
- `client/cpp/src/quantra_client.cpp`
- `client/cpp/src/json_parser.cpp`
- `src/common/product_catalog.h`

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

The public API is declared in `client/cpp/include/quantra_client.h`.

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

Utility:

- bootstrap curves, bootstrap inflation curves
- sample vol surfaces
- calendar business days, calendar holidays, calendar advance
- calibrate swaption model (Hull-White), calibrate swaption vol (SABR)

This mirrors the served endpoints in `src/common/product_catalog.h`; when that
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
call on a hot server.

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

There is no standalone `client/cpp` CMake project; the canonical build entrypoint is the repository root.

## Configuration

FlatBuffers schema resolution is controlled by:

- `QUANTRA_FBS_DIR`
- `QUANTRA_FBS_INCLUDE_DIR`

If `QUANTRA_FBS_INCLUDE_DIR` is unset, it defaults to the same location as `QUANTRA_FBS_DIR`.

## Extending the Client For A New Product

Adding a new product usually requires coordinated changes in:

1. `src/common/product_catalog.h`
2. `client/cpp/include/quantra_client.h`
3. `client/cpp/src/quantra_client.cpp`
4. FlatBuffers request/response schema files
5. `grpc/` service definitions
6. server-side request handlers

After schema or RPC changes, regenerate code and rebuild:

```bash
./scripts/generate_schemas.sh
./scripts/build.sh
```

## Notes

- The JSON layer is easier to integrate but does more conversion work.
- The native API is the better fit for high-throughput or type-safe client code.
