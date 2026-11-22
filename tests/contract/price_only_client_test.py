"""Python client and `price_only`, no engine.

A fake engine (`call=` of price_batch) answers a request that sets
`price_only` as the engine does: an empty per-trade list and one NPV per
trade in `npvs`. Every trade carries its index as leg1's notional and the
fake answers it back as the NPV.
"""

import math

import pytest

from batch_client_test import (  # noqa: E402  (sets sys.path)
    Code, FakeRpcError, answer, make_group, trade_ids)

flatbuffers = pytest.importorskip("flatbuffers")

from quantra.PriceBasisSwapRequest import PriceBasisSwapRequest  # noqa: E402
from quantra.PriceBasisSwapResponse import PriceBasisSwapResponse  # noqa: E402
from quantra.PriceBasisSwapResponse import PriceBasisSwapResponseT  # noqa: E402
from quantra_client import BlockGroup, pack_trade_block, response_npvs  # noqa: E402
from quantra_client import batch as batch_module  # noqa: E402
from quantra_client.batch import price_batch  # noqa: E402


def price_only_answer(ids):
    response = PriceBasisSwapResponseT()
    response.swaps = []
    response.npvs = [float(i) for i in ids]
    builder = flatbuffers.Builder(1024)
    builder.Finish(response.Pack(builder))
    return bytes(builder.Output())


def engine(method, data, timeout, refuse=lambda ids: None):
    ids = trade_ids(data)
    error = refuse(ids)
    if error is not None:
        raise error
    if PriceBasisSwapRequest.GetRootAs(data, 0).PriceOnly():
        return price_only_answer(ids)
    return answer(ids)


def price_only_group(first, n):
    method, request = make_group(first, n)
    request.priceOnly = True
    return method, request


def run(groups, **kwargs):
    kwargs.setdefault("processes", 0)
    kwargs.setdefault("backoff", 0.001)
    return price_batch(None, groups, **kwargs)


def test_npvs_come_back_in_input_order_across_chunks():
    groups = [price_only_group(0, 700), make_group(1000, 30), price_only_group(2000, 3)]
    result = run(groups, call=engine, in_flight=4, max_trades=90)
    assert result.ok and result.calls > 3
    for group, first in zip(result.groups, (0, 1000, 2000)):
        assert list(group.npvs()) == [float(first + i) for i in range(len(group))]
    with pytest.raises(ValueError, match="price_only"):
        result.groups[0].entry(5)
    assert result.groups[1].entry(5).Npv() == 1005.0


def test_block_groups_take_price_only_as_a_request_field():
    method, request = make_group(0, 40)
    group = BlockGroup([pack_trade_block(s) for s in request.swaps], price_only=True)
    result = run([(method, group)], call=engine, max_trades=7)
    assert result.ok
    assert list(result.groups[0].npvs()) == [float(i) for i in range(40)]


def test_failed_trades_read_as_nan():
    def refuse(ids):
        return FakeRpcError(Code.INVALID_ARGUMENT, "bad trade") if 10 in ids else None
    result = run([price_only_group(0, 40)], call=lambda m, d, t: engine(m, d, t, refuse),
                 max_trades=10)
    npvs = list(result.groups[0].npvs())
    assert all(math.isnan(x) for x in npvs[10:20])
    assert npvs[:10] == [float(i) for i in range(10)] and npvs[20:] == [float(i) for i in range(20, 40)]


def test_npvs_without_numpy(monkeypatch):
    monkeypatch.setattr(batch_module, "_numpy", lambda: None)
    result = run([price_only_group(0, 12), make_group(100, 3)], call=engine, max_trades=5)
    assert result.groups[0].npvs() == [float(i) for i in range(12)]
    assert result.groups[1].npvs() == [100.0, 101.0, 102.0]


def test_a_price_only_answer_of_the_wrong_length_is_a_failure():
    def short(method, data, timeout):
        return price_only_answer(trade_ids(data)[:-1])
    result = run([price_only_group(0, 6)], call=short)
    assert not result.ok and "entries" in str(result.groups[0].error(0))


def test_response_npvs():
    response = PriceBasisSwapResponse.GetRootAs(price_only_answer([3, 1, 2]), 0)
    assert list(response_npvs(response)) == [3.0, 1.0, 2.0]
