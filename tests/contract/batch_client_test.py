"""Python client batch module (clients/python/quantra_client/batch.py), no engine.

The calls go to a fake engine (`call=`) that reads the trades of each request
and answers one entry per trade, so splitting, ordering, retries and failure
bookkeeping are checked without a server. Every trade carries its index as
leg1's notional and the fake answers it back as the NPV, so a result in the
wrong place shows.
"""

import http.server
import json
import sys
import threading
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "flatbuffers" / "python"))
sys.path.insert(0, str(ROOT / "clients" / "python"))

grpc = pytest.importorskip("grpc")
flatbuffers = pytest.importorskip("flatbuffers")

from quantra.BasisSwap import BasisSwapT  # noqa: E402
from quantra.BasisSwapResponse import BasisSwapResponseT  # noqa: E402
from quantra.PriceBasisSwap import PriceBasisSwapT  # noqa: E402
from quantra.PriceBasisSwapRequest import PriceBasisSwapRequest, PriceBasisSwapRequestT  # noqa: E402
from quantra.PriceBasisSwapResponse import PriceBasisSwapResponseT  # noqa: E402
from quantra.SwapLeg import SwapLegT  # noqa: E402
from quantra_client import Client  # noqa: E402
from quantra_client.batch import TRADE_LISTS, plan_chunks, price_batch, workers_from_meta  # noqa: E402

Code = grpc.StatusCode


class FakeRpcError(grpc.RpcError):
    def __init__(self, code, details):
        super().__init__(details)
        self._code, self._details = code, details

    def code(self):
        return self._code

    def details(self):
        return self._details

    def trailing_metadata(self):
        return ()


def make_group(first_id, n):
    request = PriceBasisSwapRequestT()
    request.swaps = []
    for i in range(n):
        leg = SwapLegT()
        leg.notional = float(first_id + i)
        swap = BasisSwapT()
        swap.leg1 = leg
        trade = PriceBasisSwapT()
        trade.basisSwap = swap
        request.swaps.append(trade)
    return ("basis_swap", request)


def trade_ids(data):
    request = PriceBasisSwapRequest.GetRootAs(data, 0)
    return [int(request.Swaps(j).BasisSwap().Leg1().Notional()) for j in range(request.SwapsLength())]


def answer(ids):
    response = PriceBasisSwapResponseT()
    response.swaps = []
    for i in ids:
        entry = BasisSwapResponseT()
        entry.npv = float(i)
        response.swaps.append(entry)
    builder = flatbuffers.Builder(1024)
    builder.Finish(response.Pack(builder))
    return bytes(builder.Output())


class FakeEngine:
    """call(method, data, timeout): `refuse(ids, attempt)` returns an error to
    raise or None; every call is logged as (ids, bytes)."""

    def __init__(self, refuse=None):
        self.refuse = refuse or (lambda ids, attempt: None)
        self.log = []
        self.attempts = {}
        self.lock = threading.Lock()

    def __call__(self, method, data, timeout):
        assert method == "basis_swap"
        ids = trade_ids(data)
        with self.lock:
            self.log.append((ids, len(data)))
            key = tuple(ids)
            attempt = self.attempts[key] = self.attempts.get(key, 0) + 1
        error = self.refuse(ids, attempt)
        if error is not None:
            raise error
        return answer(ids)


def run(groups, engine, **kwargs):
    kwargs.setdefault("processes", 0)
    kwargs.setdefault("backoff", 0.001)
    return price_batch(None, groups, call=engine, **kwargs)


def assert_in_order(result, groups):
    for (_, request), group in zip(groups, result.groups):
        assert len(group) == len(request.swaps)
        for i, trade in enumerate(request.swaps):
            assert group.error(i) is None, group.error(i)
            assert group.entry(i).Npv() == trade.basisSwap.leg1.notional


def test_table_covers_every_client_method():
    assert set(TRADE_LISTS) == set(Client.METHODS)
    for method, entry in TRADE_LISTS.items():
        if entry is None:
            continue
        attr, response_class, accessor = entry
        rpc = Client.METHODS[method].rsplit("/", 1)[1]
        module = __import__(f"quantra.{rpc}Request", fromlist=[f"{rpc}RequestT"])
        assert attr in vars(getattr(module, f"{rpc}RequestT")()), method
        assert hasattr(response_class, accessor) and hasattr(response_class, accessor + "Length")


def test_methods_without_a_trade_list_are_refused():
    with pytest.raises(ValueError, match="no trade list"):
        price_batch(None, [("bootstrap_curves", object())], call=FakeEngine())
    with pytest.raises(ValueError, match="unknown method"):
        price_batch(None, [("nope", object())], call=FakeEngine())


def test_plan_chunks():
    # One large market: spread over every call in flight, evenly, contiguous.
    jobs = plan_chunks([16000], 8, 250)
    assert [(s, e) for _, s, e in sorted(jobs)] == [(k * 2000, (k + 1) * 2000) for k in range(8)]
    # At or below split_above: one request.
    assert plan_chunks([250], 8, 250) == [(0, 0, 250)]
    # Many markets: one request each, even above split_above.
    assert len(plan_chunks([1000] * 16, 8, 250)) == 16
    # Split in proportion to the share of the batch, largest chunks first.
    jobs = plan_chunks([12] * 10 + [4000] * 4, 8, 250)
    assert sum(1 for g, _, _ in jobs if g >= 10) == 8
    sizes = [e - s for _, s, e in jobs]
    assert sizes == sorted(sizes, reverse=True)
    # Never more chunks than trades.
    assert len(plan_chunks([3], 8, 0)) == 3


@pytest.mark.parametrize("processes", [0, 2])
def test_results_come_back_in_input_order_whatever_the_split(processes):
    groups = [make_group(0, 600), make_group(1000, 7), make_group(2000, 1), make_group(3000, 300)]
    engine = FakeEngine()
    result = run(groups, engine, in_flight=4, split_above=100, processes=processes)
    assert result.ok
    assert_in_order(result, groups)
    assert result.calls == len(engine.log) > len(groups)
    assert result.retries == 0 and result.splits == 0
    if processes == 0:
        # Largest first (one call at a time, so the log order is the send
        # order; with a pool a small chunk packed early may go first).
        engine = FakeEngine()
        run(groups, engine, in_flight=1, split_above=100)
        assert [len(ids) for ids, _ in engine.log] == [600, 300, 7, 1]


def test_max_message_bytes_cuts_a_chunk_further():
    groups = [make_group(0, 400)]
    engine = FakeEngine()
    run(groups, engine, split_above=10**9)
    size = engine.log[0][1]
    engine = FakeEngine()
    result = run(groups, engine, split_above=10**9, max_message_bytes=size // 3)
    assert result.ok and result.calls >= 3
    assert all(n <= size // 3 for _, n in engine.log)
    assert_in_order(result, groups)


def test_a_trade_too_large_alone_fails_alone():
    groups = [make_group(0, 5)]
    result = run(groups, FakeEngine(), max_message_bytes=10)
    group = result.groups[0]
    assert not group.ok
    assert all("max_message_bytes" in str(group.error(i)) for i in range(5))
    assert result.calls == 0


def test_full_engine_and_unavailable_are_retried():
    def refuse(ids, attempt):
        if attempt == 1:
            return FakeRpcError(Code.RESOURCE_EXHAUSTED, "upstream overflow")
        if attempt == 2:
            return FakeRpcError(Code.UNAVAILABLE, "no healthy upstream")
        return None

    groups = [make_group(0, 40), make_group(100, 3)]
    engine = FakeEngine(refuse)
    result = run(groups, engine, in_flight=2, split_above=10)
    assert result.ok
    assert_in_order(result, groups)
    assert result.retries == 2 * (result.calls // 3)
    assert result.splits == 0


def test_retries_are_bounded():
    engine = FakeEngine(lambda ids, attempt: FakeRpcError(Code.UNAVAILABLE, "no healthy upstream"))
    result = run([make_group(0, 4)], engine, max_retries=3)
    group = result.groups[0]
    assert result.calls == 4 and result.retries == 3
    assert group.error(0).code == Code.UNAVAILABLE
    assert group.entry(0) is None


def test_message_too_large_refusal_splits_instead_of_retrying():
    def refuse(ids, attempt):
        if len(ids) > 3:
            return FakeRpcError(Code.RESOURCE_EXHAUSTED,
                                "Received message larger than max (9000000 vs. 8388608)")
        return None

    groups = [make_group(0, 16)]
    result = run(groups, FakeEngine(refuse), split_above=10**9, max_splits=0)
    assert result.ok and result.retries == 0
    assert result.splits == 1 + 2 + 4
    assert_in_order(result, groups)


def test_deadline_splits_a_chunk_a_bounded_number_of_times():
    def refuse(ids, attempt):
        if 0 <= ids[0] < 1000 and len(ids) > 2:
            return FakeRpcError(Code.DEADLINE_EXCEEDED, "Deadline Exceeded")
        return None

    groups = [make_group(0, 16), make_group(1000, 50)]
    result = run(groups, FakeEngine(refuse), split_above=10**9, max_splits=2)
    slow, other = result.groups
    assert other.ok
    assert result.splits == 1 + 2
    # Two splits leave chunks of 4 trades, still over the deadline.
    assert all(slow.error(i).code == Code.DEADLINE_EXCEEDED for i in range(16))
    assert [(s, e) for s, e, _ in slow.failures] == [(0, 4), (4, 8), (8, 12), (12, 16)]

    result = run(groups, FakeEngine(refuse), split_above=10**9, max_splits=3)
    assert result.ok
    assert_in_order(result, groups)


def test_any_other_error_is_recorded_for_its_chunk_only():
    def refuse(ids, attempt):
        if 5 in ids:
            return FakeRpcError(Code.INVALID_ARGUMENT, "swaps[5].basis_swap: bad leg")
        return None

    groups = [make_group(0, 40), make_group(100, 2)]
    engine = FakeEngine(refuse)
    result = run(groups, engine, in_flight=4, split_above=10)
    first, second = result.groups
    assert second.ok
    bad = [i for i in range(40) if first.error(i) is not None]
    assert 5 in bad and len(bad) < 40
    assert first.error(5).code == Code.INVALID_ARGUMENT
    assert "bad leg" in str(first.error(5))
    for i in range(40):
        if i not in bad:
            assert first.entry(i).Npv() == i
    assert result.retries == 0 and result.calls == len(engine.log)


def test_a_response_that_does_not_match_its_chunk_is_a_failure():
    def short(method, data, timeout):
        return answer(trade_ids(data)[:-1])

    result = run([make_group(0, 3)], short)
    assert "2 entries for 3 trades" in str(result.groups[0].error(0))


def test_workers_from_meta_reads_the_gateway_capacity():
    bodies = [{"capacity": {"workers": 6, "healthy": 6}}, {"capacity": None}]

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = json.dumps(bodies.pop(0)).encode()
            self.send_response(200 if self.path == "/meta" else 404)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        url = f"http://127.0.0.1:{server.server_port}/"
        assert workers_from_meta(url) == 6
        assert workers_from_meta(url) is None
    finally:
        server.shutdown()
