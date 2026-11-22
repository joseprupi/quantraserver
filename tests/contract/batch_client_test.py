"""Python client batch module (clients/python/quantra_client/batch.py), no engine.

The calls go to a fake engine (`call=`) that reads the trades of each request
and answers one entry per trade, so splitting, ordering, retries and failure
bookkeeping are checked without a server. Every trade carries its index as
leg1's notional and the fake answers it back as the NPV, so a result in the
wrong place shows.
"""

import http.server
import json
import pickle
import sys
import threading
import time
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
from quantra_client import batch as batch_module  # noqa: E402
from quantra_client.batch import (  # noqa: E402
    TRADE_LISTS, BatchPool, default_start_method, plan_chunks, price_batch, workers_from_meta)

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
    raise or None; every call is logged as (ids, bytes); `delay` seconds per
    call; `timeouts` keeps the timeout of every call."""

    def __init__(self, refuse=None, delay=0.0):
        self.refuse = refuse or (lambda ids, attempt: None)
        self.delay = delay
        self.log = []
        self.timeouts = []
        self.attempts = {}
        self.lock = threading.Lock()

    def __call__(self, method, data, timeout):
        assert method == "basis_swap"
        ids = trade_ids(data)
        with self.lock:
            self.log.append((ids, len(data)))
            self.timeouts.append(timeout)
            key = tuple(ids)
            attempt = self.attempts[key] = self.attempts.get(key, 0) + 1
        if self.delay:
            time.sleep(self.delay)
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


# -----------------------------------------------------------------------------
# Trades per request, ForceDefaults
# -----------------------------------------------------------------------------

def test_max_trades_caps_every_request():
    # Below split_above, and above it where the proportional rule gives fewer
    # (larger) requests.
    assert [(s, e) for _, s, e in sorted(plan_chunks([200], 8, 250, [50]))] == [
        (0, 50), (50, 100), (100, 150), (150, 200)]
    jobs = plan_chunks([2000, 30], 8, 250, [100, None])
    assert sum(1 for g, _, _ in jobs if g == 0) == 20
    assert max(e - s for _, s, e in jobs) == 100
    # A cap the proportional split already meets changes nothing.
    assert plan_chunks([2000], 8, 250, [1000]) == plan_chunks([2000], 8, 250)

    groups = [make_group(0, 95), make_group(1000, 7)]
    engine = FakeEngine()
    result = run(groups, engine, max_trades=10)
    assert result.ok and max(len(ids) for ids, _ in engine.log) == 10
    assert result.calls == 10 + 1
    assert_in_order(result, groups)
    engine = FakeEngine()
    result = run(groups, engine, max_trades={"basis_swap": 50})
    assert result.ok and result.calls == 2 + 1
    with pytest.raises(ValueError, match="unknown method"):
        run(groups, FakeEngine(), max_trades={"nope": 5})
    with pytest.raises(ValueError, match="at least 1"):
        run(groups, FakeEngine(), max_trades=0)


def _coupons_type_on_wire(data):
    request = PriceBasisSwapRequest.GetRootAs(data, 0)
    return request.Swaps(0).BasisSwap().Leg1()._tab.Offset(4 + 2 * 9) != 0


@pytest.mark.parametrize("force", [True, False])
def test_force_defaults_writes_scalars_equal_to_their_default(force):
    # SwapLeg.couponsType is a scalar whose schema default (NONE = 0) the fake
    # trades hold: on the wire only when forced.
    engine = FakeEngine()
    kwargs = {} if force else {"force_defaults": False}
    result = run([make_group(0, 3)], engine, **kwargs)
    assert result.ok
    seen = []
    engine = FakeEngine()
    run([make_group(0, 3)], lambda m, d, t: seen.append(d) or engine(m, d, t), **kwargs)
    assert _coupons_type_on_wire(seen[0]) is force


def test_requests_pickle():
    _, request = make_group(0, 4)
    copy = pickle.loads(pickle.dumps(request))
    assert [t.basisSwap.leg1.notional for t in copy.swaps] == [0.0, 1.0, 2.0, 3.0]


# -----------------------------------------------------------------------------
# Deadline, stop, callbacks, metadata
# -----------------------------------------------------------------------------

def test_deadline_bounds_every_call_and_fails_what_is_left():
    groups = [make_group(100 * k, 2) for k in range(10)]
    engine = FakeEngine(delay=0.1)
    started = time.perf_counter()
    result = run(groups, engine, in_flight=1, timeout=30, deadline=0.35)
    took = time.perf_counter() - started
    assert took < 1.0
    assert result.stopped == "deadline"
    assert all(t <= 0.35 for t in engine.timeouts)
    sent = len(engine.log)
    assert 2 <= sent <= 5 and result.calls == sent
    failed = [g for g in result.groups if not g.ok]
    assert len(failed) == 10 - sent
    error = failed[0].error(0)
    assert error.code == Code.DEADLINE_EXCEEDED and error.details == {"reason": "BatchDeadline"}
    for group in result.groups[:sent]:
        assert group.ok


def test_halved_chunks_keep_the_call_timeout_within_the_budget():
    def refuse(ids, attempt):
        return FakeRpcError(Code.DEADLINE_EXCEEDED, "Deadline Exceeded") if len(ids) > 2 else None

    engine = FakeEngine(refuse)
    result = run([make_group(0, 8)], engine, split_above=10**9, timeout=5, deadline=60)
    assert result.ok and result.splits == 1 + 2
    assert all(t <= 5 for t in engine.timeouts)


def test_stop_sends_nothing_new_and_cancels_what_is_left():
    groups = [make_group(100 * k, 2) for k in range(6)]
    engine = FakeEngine(delay=0.05)
    result = run(groups, engine, in_flight=2, stop=lambda: len(engine.log) >= 2)
    assert result.stopped == "stop"
    assert len(engine.log) == 2 and result.calls == 2
    priced = [g for g in result.groups if g.ok]
    assert len(priced) == 2  # the calls in flight finished
    cancelled = [g for g in result.groups if not g.ok]
    assert len(cancelled) == 4
    for group in cancelled:
        error = group.error(1)
        assert error.code == Code.CANCELLED and error.details == {"reason": "Cancelled"}


def test_stop_cancels_a_pending_retry():
    calls = []

    def refuse(ids, attempt):
        calls.append(ids)
        return FakeRpcError(Code.UNAVAILABLE, "no healthy upstream")

    result = run([make_group(0, 3)], FakeEngine(refuse), backoff=0.5,
                 stop=lambda: len(calls) >= 1)
    assert result.stopped == "stop" and len(calls) == 1
    assert result.groups[0].error(0).code == Code.CANCELLED


def test_on_call_and_on_result_report_every_call_on_the_callers_thread():
    def refuse(ids, attempt):
        return FakeRpcError(Code.UNAVAILABLE, "busy") if attempt == 1 and 0 in ids else None

    groups = [make_group(0, 30), make_group(1000, 4)]
    seen_calls, seen_results, threads = [], [], set()

    def on_call(method, g, start, end, seconds, error):
        threads.add(threading.get_ident())
        seen_calls.append((method, g, start, end, error.code if error else None))
        assert seconds >= 0

    def on_result(g, start, end, data):
        threads.add(threading.get_ident())
        seen_results.append((g, start, end, len(trade_ids_of_response(data))))

    result = run(groups, FakeEngine(refuse), in_flight=3, split_above=10,
                 on_call=on_call, on_result=on_result)
    assert result.ok
    assert threads == {threading.get_ident()}
    assert len(seen_calls) == result.calls
    assert sum(1 for c in seen_calls if c[4] == Code.UNAVAILABLE) == 1
    assert sorted((g, s, e) for g, s, e, _ in seen_results) == sorted(
        (g, s, e) for g, group in enumerate(result.groups) for s, e, _ in group.responses)
    assert all(n == e - s for _, s, e, n in seen_results)


def trade_ids_of_response(data):
    from quantra.PriceBasisSwapResponse import PriceBasisSwapResponse
    response = PriceBasisSwapResponse.GetRootAs(data, 0)
    return [response.Swaps(j).Npv() for j in range(response.SwapsLength())]


def test_a_failing_callback_does_not_stop_the_batch():
    def boom(*args):
        raise RuntimeError("callback bug")

    groups = [make_group(0, 3), make_group(100, 3)]
    result = run(groups, FakeEngine(), on_call=boom, on_result=boom)
    assert result.ok
    assert len(result.callback_errors) == 2 * 2
    # An exception in stop() stops the batch.
    result = run(groups, FakeEngine(), stop=boom)
    assert result.stopped == "stop" and result.calls == 0
    assert result.groups[0].error(0).code == Code.CANCELLED


class FakeFuture:
    def __init__(self, response, error):
        self._response, self._error = response, error

    def code(self):
        return self._error.code() if self._error else Code.OK

    def details(self):
        return self._error.details() if self._error else ""

    def trailing_metadata(self):
        return ()

    def result(self):
        return self._response

    def cancel(self):
        return False

    def add_done_callback(self, fn):
        threading.Thread(target=fn, args=(self,)).start()


class FakeChannel(grpc.Channel):
    """A grpc.Channel answering unary calls with the fake engine, keeping
    the (method path, timeout, metadata) of each call."""

    def __init__(self, engine):
        self.engine = engine
        self.calls = []

    def unary_unary(self, path, *args, **kwargs):
        channel = self

        class Stub:
            def future(self, data, timeout=None, metadata=None):
                channel.calls.append((path, timeout, metadata))
                try:
                    return FakeFuture(channel.engine("basis_swap", data, timeout), None)
                except grpc.RpcError as e:
                    return FakeFuture(None, e)

        return Stub()

    def subscribe(self, *args, **kwargs):
        pass

    def unsubscribe(self, *args, **kwargs):
        pass

    def unary_stream(self, *args, **kwargs):
        raise NotImplementedError

    def stream_unary(self, *args, **kwargs):
        raise NotImplementedError

    def stream_stream(self, *args, **kwargs):
        raise NotImplementedError

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def test_metadata_goes_with_every_call():
    groups = [make_group(0, 30), make_group(1000, 2)]
    channel = FakeChannel(FakeEngine())
    result = price_batch(channel, groups, processes=0, split_above=10, in_flight=2,
                         metadata=[("x-request-id", "run-7")], deadline=60, timeout=20)
    assert result.ok
    assert all(md == (("x-request-id", "run-7"),) for _, _, md in channel.calls)
    assert all(path.endswith("/PriceBasisSwap") for path, _, _ in channel.calls)
    assert all(0 < timeout <= 20 for _, timeout, _ in channel.calls)

    channel = FakeChannel(FakeEngine())
    result = price_batch(channel, groups, processes=0, split_above=10, in_flight=2,
                         metadata=lambda method, g, s, e: [("x-chunk", f"{method}:{g}:{s}-{e}")])
    assert result.ok
    sent = sorted(md[0][1] for _, _, md in channel.calls)
    expected = sorted(f"basis_swap:{g}:{s}-{e}" for g, group in enumerate(result.groups)
                      for s, e, _ in group.responses)
    assert sent == expected


# -----------------------------------------------------------------------------
# Packing processes
# -----------------------------------------------------------------------------

def test_default_start_method():
    expected = "forkserver" if sys.platform.startswith("linux") else "spawn"
    assert default_start_method() == expected
    with pytest.raises(ValueError, match="start_method"):
        run([make_group(0, 2)], FakeEngine(), start_method="threads")


@pytest.mark.parametrize("start_method", ["forkserver", "spawn", "fork"])
def test_every_start_method_packs_the_same_requests(start_method):
    groups = [make_group(0, 300), make_group(1000, 40)]
    engine = FakeEngine()
    result = run(groups, engine, processes=2, split_above=100, start_method=start_method)
    assert result.ok and not result.warnings
    assert_in_order(result, groups)
    reference = FakeEngine()
    run(groups, reference, processes=0, split_above=100)
    assert sorted(engine.log) == sorted(reference.log)


_PARENT = None


def _refuse_outside_the_parent():
    import os
    if os.getpid() != _PARENT:
        raise RuntimeError("cannot be rebuilt in a packing process")
    return "trade"


class _ParentOnly:
    def __reduce__(self):
        return (_refuse_outside_the_parent, ())


def test_a_request_that_does_not_cross_to_the_processes_fails_its_chunk_only():
    global _PARENT
    import os
    _PARENT = os.getpid()
    good = make_group(0, 300)
    # Does not pickle here: a lambda on the request.
    _, unpicklable = make_group(1000, 300)
    unpicklable.extra = lambda: None
    # Pickles here, cannot be unpickled in a packing process.
    _, unloadable = make_group(2000, 300)
    unloadable.extra = _ParentOnly()
    groups = [good, ("basis_swap", unpicklable), ("basis_swap", unloadable)]
    result = run(groups, FakeEngine(), processes=2, deadline=60)
    assert result.groups[0].ok and result.stopped is None
    for group in result.groups[1:]:
        assert all(group.error(i) is not None for i in range(300))
        assert "packing failed" in str(group.error(0))
        assert "processes=0" in str(group.error(0))


def test_a_batch_pool_is_reused_across_batches_and_threads():
    groups = [make_group(0, 120), make_group(1000, 9)]
    with BatchPool(processes=2) as pool:
        results = [run(groups, FakeEngine(), pool=pool, split_above=50) for _ in range(3)]
        threaded = []
        workers = [threading.Thread(target=lambda: threaded.append(
            run(groups, FakeEngine(), pool=pool, split_above=50))) for _ in range(3)]
        for t in workers:
            t.start()
        for t in workers:
            t.join(60)
        assert not pool.closed
    assert pool.closed
    for result in results + threaded:
        assert result.ok
        assert_in_order(result, groups)
    assert len(threaded) == 3
    with pytest.raises(ValueError, match="closed"):
        run(groups, FakeEngine(), pool=pool)


def test_a_pool_that_will_not_stop_is_left_behind_in_bounded_time(monkeypatch):
    class Stuck:
        def __init__(self):
            self.killed = False
            self.exitcode = None
            self.pid = 4242

        def kill(self):
            self.killed = True

    class StuckPool:
        def __init__(self):
            self._pool = [Stuck()]
            self.forever = threading.Event()

        def close(self):
            pass

        def join(self):
            self.forever.wait()

        def terminate(self):
            self.forever.wait()

    monkeypatch.setattr(batch_module, "_KILL_SECONDS", 0.1)
    pool = StuckPool()
    started = time.perf_counter()
    message = batch_module._shutdown_pool(pool, graceful=True, seconds=0.1)
    assert time.perf_counter() - started < 2.0
    assert "left behind" in message and "4242" in message
    assert pool._pool[0].killed
    pool.forever.set()
