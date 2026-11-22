"""`price_only` on the pricing endpoints, against the running engine (gRPC
on port 50051 by default, QUANTRA_GRPC_TARGET overrides it; the JSON gateway
at --url).

- every example request of every method with a trade list, sent with
  `price_only`: the response holds one NPV per trade, bit for bit the NPV of
  the full response, and an empty per-trade list; a request the engine
  refuses is refused with the same code and message;
- the flag sent as false gives the same response bytes as the flag absent;
- through the JSON gateway (one example per method): `"price_only": true`
  gives `npvs` equal to the full response's NPVs and an empty list;
- a bad trade refuses the request the same way with and without the flag;
- the Python client: price_batch over object groups and block groups with
  the flag, GroupResult.npvs() and entry().
"""

import copy
import json
import math
import os
import struct

import pytest

from request_blocks_support import (  # noqa: E402  (sets sys.path)
    DATA, example_cases, example_request, pack, request_fields, trade_attr)

grpc = pytest.importorskip("grpc")
requests = pytest.importorskip("requests")

from ql_reference import ApiClient  # noqa: E402
from quantra_client import (  # noqa: E402
    BlockGroup, Client, pack_market_block, pack_trade_block, response_npvs)
from quantra_client.batch import TRADE_LISTS, price_batch  # noqa: E402

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
    try:
        return channel.unary_unary(Client.METHODS[method])(data, timeout=120), None
    except grpc.RpcError as e:
        return None, (e.code(), e.details())


def _bits(values):
    return [struct.pack("<d", v) for v in values]


def _full_npvs(method, data):
    _, response_class, accessor = TRADE_LISTS[method]
    response = response_class.GetRootAs(data, 0)
    read = getattr(response, accessor)
    return [read(j).Npv() for j in range(getattr(response, accessor + "Length")())]


def _price_only(method, data):
    """(npvs, list length) of a price-only response."""
    _, response_class, accessor = TRADE_LISTS[method]
    response = response_class.GetRootAs(data, 0)
    return ([response.Npvs(j) for j in range(response.NpvsLength())],
            getattr(response, accessor + "Length")())


def _first_per_method():
    seen, out = set(), []
    for method, rel in CASES:
        if method not in seen:
            seen.add(method)
            out.append((method, rel))
    return out


FIRST = _first_per_method()


@pytest.mark.parametrize("method,rel", CASES, ids=[f"{m}:{r}" for m, r in CASES])
def test_price_only_gives_the_full_responses_npvs(channel, method, rel):
    request = example_request(method, rel)
    full, full_error = _call(channel, method, pack(request))
    request.priceOnly = True
    prices, error = _call(channel, method, pack(request))
    assert error == full_error
    if full_error is not None:
        return
    npvs, listed = _price_only(method, prices)
    assert listed == 0
    assert len(npvs) == len(getattr(request, trade_attr(method)))
    assert _bits(npvs) == _bits(_full_npvs(method, full))
    # nothing but the empty list and the NPVs: a root table, two vectors
    assert len(prices) <= 64 + 8 * len(npvs)


@pytest.mark.parametrize("method,rel", FIRST, ids=[m for m, _ in FIRST])
def test_price_only_false_is_the_full_response(channel, method, rel):
    request = example_request(method, rel)
    absent = _call(channel, method, pack(request, force_defaults=False))
    request.priceOnly = False
    explicit = _call(channel, method, pack(request, force_defaults=True))
    assert explicit == absent


def _post(base, method, body):
    r = requests.post(f"{base}/{ApiClient.ENDPOINTS[method]}", data=json.dumps(body),
                      headers={"Content-Type": "application/json"}, timeout=120)
    try:
        return r.status_code, r.json()
    except ValueError:
        return r.status_code, r.text


@pytest.mark.parametrize("method,rel", FIRST, ids=[m for m, _ in FIRST])
def test_price_only_through_the_json_gateway(client, method, rel):
    body = json.loads((DATA / rel).read_text())
    status, full = _post(client.base_url, method, body)
    assert status == 200, full
    list_key = TRADE_LISTS[method][0]
    list_key = {"fxForwards": "fx_forwards", "cdsList": "cds_list",
                "capFloors": "cap_floors"}.get(list_key, list_key)
    body["price_only"] = True
    status, prices = _post(client.base_url, method, body)
    assert status == 200, prices
    assert prices.get(list_key) == []
    expected = [row.get("npv", 0.0) for row in full[list_key]]
    assert prices["npvs"] == expected
    assert set(prices) == {list_key, "npvs"}


# --- refusals ---------------------------------------------------------------

def _bad_trade_cases():
    """(method, example, mutate) whose trade names something the market does
    not have, or carries an invalid value."""
    def first_trade(body, method):
        key = {"fxForwards": "fx_forwards", "cdsList": "cds_list",
               "capFloors": "cap_floors"}.get(TRADE_LISTS[method][0], TRADE_LISTS[method][0])
        return body[key][0]

    def unknown_curve(path):
        def mutate(body, method):
            node = first_trade(body, method)
            for key in path[:-1]:
                node = node[key]
            node[path[-1]] = "no_such_curve"
        return mutate

    def negative_notional(path):
        def mutate(body, method):
            node = first_trade(body, method)
            for key in path[:-1]:
                node = node[key]
            node[path[-1]] = -node[path[-1]]
        return mutate

    out = []
    for method, rel in FIRST:
        body = json.loads((DATA / rel).read_text())
        trade = first_trade(body, method)
        for path in (("discounting_curve",), ("vanilla_swap", "fixed_leg", "discounting_curve"),
                     ("ois_swap", "fixed_leg", "discounting_curve"),
                     ("basis_swap", "leg1", "discounting_curve"),
                     ("bond", "discounting_curve"), ("credit_curve_id",),
                     ("base_discounting_curve",),
                     ("cross_currency_swap", "leg1", "discounting_curve"),
                     ("commodity_swap", "leg1", "discounting_curve"),
                     ("volatility",), ("model",)):
            node = trade
            try:
                for key in path:
                    node = node[key]
            except (KeyError, TypeError):
                continue
            if isinstance(node, str):
                out.append(pytest.param(method, rel, unknown_curve(path),
                                        id=f"{method}:unknown:{'.'.join(path)}"))
                break
    out.append(pytest.param("vanilla_swap", FIRST[[m for m, _ in FIRST].index("vanilla_swap")][1],
                            negative_notional(("vanilla_swap", "fixed_leg", "notional")),
                            id="vanilla_swap:negative_notional"))
    return out


@pytest.mark.parametrize("method,rel,mutate", _bad_trade_cases())
def test_a_bad_trade_is_refused_the_same_way(client, method, rel, mutate):
    body = json.loads((DATA / rel).read_text())
    mutate(body, method)
    full = _post(client.base_url, method, body)
    assert full[0] != 200, "the mutation must make the request fail"
    body["price_only"] = True
    assert _post(client.base_url, method, body) == full


# --- the Python client --------------------------------------------------------

def _nan_equal(a, b):
    return len(a) == len(b) and all(
        (math.isnan(x) and math.isnan(y)) or struct.pack("<d", x) == struct.pack("<d", y)
        for x, y in zip(a, b))


def _swap_example():
    return next((m, r) for m, r in CASES if m == "vanilla_swap")


def test_price_batch_with_price_only(channel):
    method, rel = _swap_example()
    request = example_request(method, rel)
    trades = getattr(request, trade_attr(method))
    big = copy.copy(request)
    setattr(big, trade_attr(method), [trades[i % len(trades)] for i in range(300)])
    full = price_batch(channel, [(method, big)], processes=0, in_flight=4, split_above=50)
    assert full.ok
    priced = copy.copy(big)
    priced.priceOnly = True
    market, _ = pack_market_block(request.pricing)
    blocks = [pack_trade_block(t) for t in getattr(big, trade_attr(method))]
    fields = request_fields(method, request)
    fields.pop("priceOnly", None)
    groups = [(method, priced),
              (method, BlockGroup(blocks, pricing_block=market, price_only=True, **fields))]
    result = price_batch(channel, groups, processes=0, in_flight=4, split_above=50)
    assert result.ok and result.calls > 2
    expected = list(full.groups[0].npvs())
    for group in result.groups:
        assert _nan_equal(list(group.npvs()), expected)
        with pytest.raises(ValueError, match="price_only"):
            group.entry(0)


def test_client_response_npvs(channel):
    method, rel = _swap_example()
    request = example_request(method, rel)
    request.priceOnly = True
    with Client(TARGET) as client:
        response = client.price_vanilla_swaps(request)
    assert response.SwapsLength() == 0
    assert len(list(response_npvs(response))) == len(request.swaps)
