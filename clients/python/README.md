# quantra-client (Python)

A thin gRPC client for the Quantra pricing engine. It serializes requests with
the generated FlatBuffers Object API and calls the engine directly — no JSON,
no HTTP.

## Scope

This client is **not** a complete binding for the server. It wraps the server
endpoints below:

| Method | Endpoint |
| --- | --- |
| `price_fixed_rate_bonds` | `PriceFixedRateBond` |
| `price_floating_rate_bonds` | `PriceFloatingRateBond` |
| `price_zero_coupon_bonds` | `PriceZeroCouponBond` |
| `price_callable_fixed_rate_bonds` | `PriceCallableFixedRateBond` |
| `price_vanilla_swaps` | `PriceVanillaSwap` |
| `price_ois_swaps` | `PriceOisSwap` |
| `price_basis_swaps` | `PriceBasisSwap` |
| `price_zero_coupon_swaps` | `PriceZeroCouponSwap` |
| `price_fras` | `PriceFRA` |
| `price_fx_forwards` | `PriceFxForward` |
| `price_cross_currency_swaps` | `PriceCrossCurrencySwap` |
| `price_commodity_swaps` | `PriceCommoditySwap` |
| `build_commodity_curves` | `BuildCommodityCurves` |
| `price_structured_notes` | `PriceStructuredNote` |
| `price_equity_options` | `PriceEquityOption` |
| `price_cap_floors` | `PriceCapFloor` |
| `price_swaptions` | `PriceSwaption` |
| `price_cds` | `PriceCDS` |
| `price_zero_coupon_inflation_swaps` | `PriceZeroCouponInflationSwap` |
| `price_year_on_year_inflation_swaps` | `PriceYearOnYearInflationSwap` |
| `price_year_on_year_inflation_cap_floors` | `PriceYearOnYearInflationCapFloor` |
| `bootstrap_curves` | `BootstrapCurves` |
| `bootstrap_inflation_curves` | `BootstrapInflationCurves` |
| `validate_vol_surface` | `ValidateVolSurface` |

Everything else the server offers — vol-surface sampling, the calibration
endpoints (swaption model and vol, implied forwards, vol surface,
correlation), the calendar utilities and `Meta` — has no wrapper here. Use
one of:

- **HTTP/JSON**, against the gateway. Every endpoint is described by the
  generated OpenAPI spec in `jsonserver/openapi/`; see `docs/http-api.md` for
  the cross-product contract. This is the shortest path for Python.
- **gRPC directly**, using the generated FlatBuffers classes in
  `flatbuffers/python/` plus a raw `grpc` channel. `quantra_client/client.py`
  shows the pattern: `Client.METHODS` maps a name to a gRPC method path, and
  `Client._call` does the pack / invoke / unpack.

Many trades over many markets: `quantra_client.batch.price_batch` (see
`docs/client.md`, "Python client: batches").

Requests from trades and markets packed once and stored as bytes, and market
values changed in place in a packed request (scenarios without repacking):
`quantra_client.blocks` (`pack_trade_block`, `pack_market_block`,
`assemble_request`, `patch_values`, `BlockGroup` for `price_batch`,
`SCHEMA_HASH`; see `docs/client.md`, "Python client: request blocks").

## Install

From a source checkout:

```bash
pip install -e clients/python
```

The generated `quantra` FlatBuffers package is not installed by `setup.py`; put
`flatbuffers/python/` on `PYTHONPATH`:

```bash
export PYTHONPATH="$PWD/flatbuffers/python:$PYTHONPATH"
```

## Usage

```python
from quantra_client import Client
from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT

request = PriceFixedRateBondRequestT()
# ... populate request.pricing (asOfDate, and the curves under
# pricing.rates = RatesMarketDataT()) and request.bonds ...

with Client("localhost:50051") as client:
    response = client.price_fixed_rate_bonds(request)
    for i in range(response.BondsLength()):
        print(response.Bonds(i).Npv())
```

`Client` takes `target`, `secure`, `credentials`, `options` and
`force_defaults`; it defaults to `localhost:50051`, an insecure channel, a
100 MiB message cap in both directions, and packing without
`ForceDefaults` (`force_defaults=True` writes every scalar equal to its schema
default too; the generated classes of this repository do not need it). Use it
as a context manager or call `close()`.

## Example

`examples/price_bonds.py` builds a full request — deposit and bond curve
helpers, a bootstrapped curve, and a fixed-rate bond — and times repeated
pricing calls:

```bash
python3 clients/python/examples/price_bonds.py 10 100 1
```

The arguments are bonds per request, number of requests, and whether to share
one curve (`1`) or send one copy per bond (`0`). It resolves its own import
paths, so it runs from a checkout without installing anything.

## Batches

`quantra_client.batch.price_batch` prices many trades over many markets: one
request per market, a large market split across the calls in flight, the
requests packed in a pool of processes, retries on a full or unreachable
engine, and results per market in input trade order. See `docs/client.md`
("Python client: batches") and `examples/price_batch.py`:

```bash
python3 clients/python/examples/price_batch.py --target localhost:50051 \
    --gateway http://localhost:8080 --markets 20 --bonds 500
```

## Errors

A failed call raises `QuantraError` carrying the gRPC status code and the
engine's real error message. The status-code meanings are the same ones listed
in `docs/http-api.md` (`INVALID_ARGUMENT` for a malformed request,
`NOT_FOUND` for an unresolvable id, `ABORTED` for a well-formed but unpriceable
request, `DEADLINE_EXCEEDED` when the request budget expires).
