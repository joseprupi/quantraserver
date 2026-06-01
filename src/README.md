# src/

Engine internals. Each request is handled by a per-product **handler** that wires
together a **mapper** (FlatBuffers ↔ plain C++) and an **evaluator** (pure
QuantLib). Market data is assembled once per request into a registry. The folders
below are role-based; includes are flat (CMake puts every `src/*/` on the include
path), so `#include "foo.h"` works regardless of which subfolder `foo.h` is in.

## Folders

| Folder | Holds | FlatBuffers? |
|---|---|---|
| `transport/` | per-product `*_handler.h`, the generic `product_endpoint.h` glue, `call_data_base.h`, `quantra_request.h`, `product_registry.h` | yes (request/response types) |
| `mappers/` | per-product `*_mapper.{h,cpp}` + flow builders | yes — the only FB↔domain layer |
| `evaluators/` | per-product `*_evaluator.{h,cpp}` — the QuantLib pricing/compute | **no — boundary-enforced** |
| `parsers/` | FlatBuffers instrument tables → QuantLib objects (`term_structure_parser`, `vol_surface_parsers`, `schedule_parser`, `yield_parser`, per-product `*_parser`, …) | yes |
| `market/` | shared market-data assembly: `pricing_registry(+builder)`, `curve_bootstrapper`, `curve_cache`, the `*_registry` files, `sabr_calibrate_cache`, `swaption_vol_runtime`/`_diagnostics`/`_model_calibration`, `engine_factory`, `grid_utils` | yes |
| `domain/` | plain C++ types, no QuantLib pricing logic: `*_domain.h`, `enums_domain.{h,cpp}`, `pricing_context.h`, `eval_date_guard.h`, `swaption_rebump.h` | no |
| `common/` | `enum_convert.{h,cpp}` (FB enum ↔ QuantLib), `date_convert.{h,cpp}` (date helpers), `error.{h,cpp}`, `product_catalog.h` | no |

## Request flow

For a pricing request, `ProductEndpoint<Req, Resp, Mapper, Evaluator>::request()`
(in `transport/product_endpoint.h`) runs:

1. `EvalDateGuard` — set QuantLib's global evaluation date for this request.
2. `mapper_.toInputs(req)` — FlatBuffers request → plain `Inputs` struct.
3. `PricingRegistryBuilder{}.build(req->pricing())` — assemble market data
   (curves, indices, vol surfaces, models, credit curves) into a `PricingRegistry`,
   plus a `PricingContext`. Uses `parsers/` and `market/`. Skipped for endpoints
   with no `pricing` block (e.g. calendars).
4. `evaluator_.evaluate(inputs, reg, ctx)` — **pure QuantLib**, returns a plain
   `Result`. No FlatBuffers here.
5. `mapper_.toResponse(builder, result)` — plain `Result` → FlatBuffers response.

List/query endpoints whose mapper defines an `onRegistryBuildError` hook turn a
registry-build failure into a per-item error (HTTP 200 with per-item errors)
instead of a transport-level error.

Handlers are thin: a handler just names the four type parameters (`Req`, `Resp`,
`Mapper`, `Evaluator`) and inherits `ProductEndpoint`.

## The one rule

**Files in `evaluators/` must not see FlatBuffers or gRPC** — no
`*_generated.h` includes, no `flatbuffers::`, no `grpc::`. Evaluators take plain
`Inputs` + the `PricingRegistry` and return a plain `Result`; the mapper does all
FlatBuffers translation. This is enforced in CI by
`scripts/check_evaluator_boundary.sh` (test Suite 0), which scans
`src/evaluators/*_evaluator.{h,cpp}`.

## Not in `src/` (engine internals only)

- `server/` — the gRPC server executable (`sync_server.cpp`); links the handlers.
- `jsonserver/` — HTTP→gRPC gateway.
- `grpc/` — generated FlatBuffers-gRPC service stubs (`quantra_grpc` static lib),
  linked by server, gateway, client, tests.
- `flatbuffers/` — `.fbs` schemas + generated code.
- `client/` (C++) and `quantra-python/` (Python) — client libraries.
- `tests/` — the test suite (see `tests/README.md`).
