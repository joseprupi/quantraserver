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

The current header exposes pricing and utility calls for:

- fixed-rate bonds
- floating-rate bonds
- vanilla swaps
- zero-coupon inflation swaps
- year-on-year inflation swaps
- OIS swaps
- basis swaps
- FRAs
- caps and floors
- swaptions
- CDS
- bootstrap curves
- bootstrap inflation curves
- sampled vol surfaces
- calendar helpers
- swaption model calibration
- equity options

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
