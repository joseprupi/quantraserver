"""Python client request blocks against the running engine (gRPC, port 50051
by default; QUANTRA_GRPC_TARGET overrides it).

- every example request of every Client method with a trade list: the request
  assembled from per-trade blocks and a market block gets the same answer
  from the engine, byte for byte, as the request packed normally (the same
  error when the engine refuses it); the market given as an object too, for
  the first example of each method;
- a market value patched in the assembled request (one value, several, and
  a value of exactly 0.0) gets the same answer as the request rebuilt with
  those values;
- the engine reports the client's SCHEMA_HASH in GET /meta and in the Meta
  reply.
"""

import copy
import json
import os
import urllib.request

import pytest

from request_blocks_support import (  # noqa: E402  (sets sys.path)
    example_cases, example_request, get_path, pack, request_fields, set_path, trade_attr)

grpc = pytest.importorskip("grpc")

from quantra.MetaRequest import MetaRequestT  # noqa: E402
from quantra.MetaResponse import MetaResponse  # noqa: E402
from quantra_client import (  # noqa: E402
    SCHEMA_HASH, Client, assemble_request, pack_market_block, pack_trade_block, patch_values)

CASES = example_cases()
TARGET = os.environ.get("QUANTRA_GRPC_TARGET", "localhost:50051")


@pytest.fixture(scope="module")
def channel():
    ch = grpc.insecure_channel(TARGET, options=[("grpc.max_send_message_length", -1),
                                                ("grpc.max_receive_message_length", -1)])
    try:
        grpc.channel_ready_future(ch).result(timeout=15)
    except grpc.FutureTimeoutError:
        pytest.fail(f"no gRPC engine at {TARGET}", pytrace=False)
    yield ch
    ch.close()


def _call(channel, method, data):
    """(response bytes, None) or (None, (status code, details))."""
    stub = channel.unary_unary(Client.METHODS[method])
    try:
        return stub(data, timeout=120), None
    except grpc.RpcError as e:
        return None, (e.code(), e.details())


def _blocks(method, request):
    return [pack_trade_block(t) for t in getattr(request, trade_attr(method))]


def _first_per_method():
    seen, out = set(), []
    for method, rel in CASES:
        if method not in seen:
            seen.add(method)
            out.append((method, rel))
    return out


@pytest.mark.parametrize("method,rel", CASES, ids=[f"{m}:{r}" for m, r in CASES])
def test_assembled_request_prices_as_the_normal_one(channel, method, rel):
    request = example_request(method, rel)
    expected = _call(channel, method, pack(request))
    market, _ = pack_market_block(request.pricing)
    out = assemble_request(method, _blocks(method, request), pricing_block=market,
                           **request_fields(method, request))
    assert _call(channel, method, out.data) == expected


@pytest.mark.parametrize("method,rel", _first_per_method(),
                         ids=[f"{m}:{r}" for m, r in _first_per_method()])
def test_assembled_request_with_the_market_as_an_object(channel, method, rel):
    request = example_request(method, rel)
    expected = _call(channel, method, pack(request))
    out = assemble_request(method, _blocks(method, request), pricing=request.pricing,
                           **request_fields(method, request))
    assert _call(channel, method, out.data) == expected


def _patch_cases():
    """One example per market kind with values to move: curve, vol model,
    credit curve, inflation curve, spot quote, commodity contract."""
    wanted = {"rates.curves[": None, "volatility.": None, "credit.": None,
              "inflation.inflation_curves[": None,
              "quotes[": None, "commodity.": None}
    for method, rel in CASES:
        request = example_request(method, rel)
        _, slots = pack_market_block(request.pricing)
        for prefix in wanted:
            if wanted[prefix] is None:
                paths = [p for p, s in slots.items() if p.startswith(prefix) and s.kind == "d"
                         and get_path(request.pricing, p) != 0.0]
                if len(paths) >= 2:
                    wanted[prefix] = (method, rel, paths[:3])
    return [pytest.param(*v, id=f"{k}{v[0]}") for k, v in wanted.items() if v is not None]


@pytest.mark.parametrize("method,rel,paths", _patch_cases())
def test_patched_market_prices_as_the_rebuilt_one(channel, method, rel, paths):
    request = example_request(method, rel)
    market, slots = pack_market_block(request.pricing)
    fields = request_fields(method, request)
    blocks = _blocks(method, request)
    base = assemble_request(method, blocks, pricing_block=market, **fields)
    bump = {p: get_path(request.pricing, p) * 1.001 for p in paths}
    for change in ({paths[0]: 0.0}, {paths[0]: bump[paths[0]]}, bump):
        rebuilt = copy.deepcopy(request)
        for path, value in change.items():
            set_path(rebuilt.pricing, path, value)
        expected = _call(channel, method, pack(rebuilt))
        patched = patch_values(base.data, base.pricing_at, slots, change)
        got = _call(channel, method, patched)
        assert got == expected, change
    # the moved values reach the engine: its answer changes
    assert _call(channel, method, patch_values(base.data, base.pricing_at, slots, bump)) != \
        _call(channel, method, base.data)


def test_meta_reports_the_client_schema_hash(channel, pytestconfig):
    url = pytestconfig.getoption("--url").rstrip("/") + "/meta"
    with urllib.request.urlopen(url, timeout=10) as resp:
        meta = json.loads(resp.read().decode("utf-8"))
    assert meta["schema_hash"] == SCHEMA_HASH
    reply = channel.unary_unary("/quantra.QuantraServer/Meta")(pack(MetaRequestT()), timeout=10)
    assert MetaResponse.GetRootAs(reply, 0).SchemaHash().decode("utf-8") == SCHEMA_HASH
