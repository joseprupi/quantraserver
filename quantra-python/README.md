# quantra-client (Python)

A thin gRPC client for the Quantra pricing engine. It serializes requests with
the generated FlatBuffers Object API and calls the engine directly — no JSON,
no HTTP.

## Scope

This client is **not** a complete binding for the server. It wraps nine of the
server's endpoints:

| Method | Endpoint |
| --- | --- |
| `price_fixed_rate_bonds` | `PriceFixedRateBond` |
| `price_floating_rate_bonds` | `PriceFloatingRateBond` |
| `price_vanilla_swaps` | `PriceVanillaSwap` |
| `price_fras` | `PriceFRA` |
| `price_cap_floors` | `PriceCapFloor` |
| `price_swaptions` | `PriceSwaption` |
| `price_cds` | `PriceCDS` |
| `bootstrap_curves` | `BootstrapCurves` |
| `bootstrap_inflation_curves` | `BootstrapInflationCurves` |

Everything else the server prices — zero-coupon bonds, callable bonds, OIS and
basis swaps, zero-coupon swaps, both inflation swaps, year-on-year inflation
caps/floors, equity options, vol-surface sampling, the calibration endpoints
and the calendar utilities — has no wrapper here. Use one of:

- **HTTP/JSON**, against the gateway. Every endpoint is described by the
  generated OpenAPI spec in `jsonserver/openapi/`; see `docs/http-api.md` for
  the cross-product contract. This is the shortest path for Python.
- **gRPC directly**, using the generated FlatBuffers classes in
  `flatbuffers/python/` plus a raw `grpc` channel. `quantra_client/client.py`
  shows the pattern: `Client.METHODS` maps a name to a gRPC method path, and
  `Client._call` does the pack / invoke / unpack.

## Install

From a source checkout:

```bash
pip install -e quantra-python
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
# ... populate request.pricing (as-of date, curves) and request.bonds ...

with Client("localhost:50051") as client:
    response = client.price_fixed_rate_bonds(request)
    for i in range(response.BondsLength()):
        print(response.Bonds(i).Npv())
```

`Client` takes `target`, `secure`, `credentials` and `options`; it defaults to
`localhost:50051`, an insecure channel, and a 100 MiB message cap in both
directions. Use it as a context manager or call `close()`.

## Example

`examples/price_bonds.py` builds a full request — deposit and bond curve
helpers, a bootstrapped curve, and a fixed-rate bond — and times repeated
pricing calls:

```bash
python3 quantra-python/examples/price_bonds.py 10 100 1
```

The arguments are bonds per request, number of requests, and whether to share
one curve (`1`) or send one copy per bond (`0`). It resolves its own import
paths, so it runs from a checkout without installing anything.

## Errors

A failed call raises `QuantraError` carrying the gRPC status code and the
engine's real error message. The status-code meanings are the same ones listed
in `docs/http-api.md` (`INVALID_ARGUMENT` for a malformed request,
`NOT_FOUND` for an unresolvable id, `ABORTED` for a well-formed but unpriceable
request, `DEADLINE_EXCEEDED` when the request budget expires).
