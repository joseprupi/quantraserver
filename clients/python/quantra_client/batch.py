"""
Quantra batch pricing

Prices many trades over many markets with as few requests as the engine
needs: one request per market, a large market split across the workers, calls
kept in flight up to a limit, and the requests packed in parallel processes so
that building the FlatBuffers is not the slow part.

A group is one request object (the Object API `...RequestT` a Client method
takes, market included) whose trade list holds every trade priced on that
market, and the method key of `Client.METHODS` that prices it:

Example:
    from quantra_client import Client
    from quantra_client.batch import price_batch, workers_from_meta

    groups = [("basis_swap", request_a), ("basis_swap", request_b)]
    in_flight = workers_from_meta("http://localhost:8080") or 8
    with Client("localhost:50051") as client:
        result = price_batch(client, groups, in_flight=in_flight)
    for group in result.groups:
        for i in range(len(group)):
            if group.error(i) is None:
                print(group.entry(i).Npv())

Results come back per group in the input trade order whatever the split, as
FlatBuffers readers (the response entry of each trade, e.g. `Swaps(j)`), with
the trades that failed and why.

See examples/price_batch.py for a runnable batch.
"""

import bisect
import copy
import heapq
import json
import multiprocessing
import os
import queue
import random
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from typing import List, Optional, Sequence, Tuple

import flatbuffers
import grpc

from .client import Client, QuantraError, _error_details

from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse
from quantra.PriceFloatingRateBondResponse import PriceFloatingRateBondResponse
from quantra.PriceVanillaSwapResponse import PriceVanillaSwapResponse
from quantra.PriceOisSwapResponse import PriceOisSwapResponse
from quantra.PriceBasisSwapResponse import PriceBasisSwapResponse
from quantra.PriceFRAResponse import PriceFRAResponse
from quantra.PriceFxForwardResponse import PriceFxForwardResponse
from quantra.PriceCrossCurrencySwapResponse import PriceCrossCurrencySwapResponse
from quantra.PriceCommoditySwapResponse import PriceCommoditySwapResponse
from quantra.PriceStructuredNoteResponse import PriceStructuredNoteResponse
from quantra.PriceCapFloorResponse import PriceCapFloorResponse
from quantra.PriceSwaptionResponse import PriceSwaptionResponse
from quantra.PriceCDSResponse import PriceCDSResponse

# Per method of Client.METHODS: (request trade-list attribute, response class,
# response accessor of the per-trade entry), the response holding one entry
# per trade in request order. None for the methods whose request has no trade
# list (curve builds, quote-sheet validation): price_batch refuses them.
TRADE_LISTS = {
    'fixed_rate_bond': ('bonds', PriceFixedRateBondResponse, 'Bonds'),
    'floating_rate_bond': ('bonds', PriceFloatingRateBondResponse, 'Bonds'),
    'vanilla_swap': ('swaps', PriceVanillaSwapResponse, 'Swaps'),
    'ois_swap': ('swaps', PriceOisSwapResponse, 'Swaps'),
    'basis_swap': ('swaps', PriceBasisSwapResponse, 'Swaps'),
    'fra': ('fras', PriceFRAResponse, 'Fras'),
    'fx_forward': ('fxForwards', PriceFxForwardResponse, 'FxForwards'),
    'cross_currency_swap': ('swaps', PriceCrossCurrencySwapResponse, 'Swaps'),
    'commodity_swap': ('swaps', PriceCommoditySwapResponse, 'Swaps'),
    'build_commodity_curves': None,
    'structured_note': ('notes', PriceStructuredNoteResponse, 'Notes'),
    'cap_floor': ('capFloors', PriceCapFloorResponse, 'CapFloors'),
    'swaption': ('swaptions', PriceSwaptionResponse, 'Swaptions'),
    'cds': ('cdsList', PriceCDSResponse, 'CdsList'),
    'bootstrap_curves': None,
    'bootstrap_inflation_curves': None,
    'validate_vol_surface': None,
}

# Workers refuse a message above 8 MiB (QUANTRA_MAX_MESSAGE_BYTES); keep a
# margin below it.
DEFAULT_MAX_MESSAGE_BYTES = 8 * 1024 * 1024 - 64 * 1024
# A market is split across calls only above this many trades: below it the
# extra market builds (one per chunk) cost more than the parallel pricing
# saves. Measured on swaps over three curves: splitting lost at 100 trades
# and won from 250 (docs/client.md).
DEFAULT_SPLIT_ABOVE = 250
# Below this many trades in total a process pool costs more to start than
# packing in this process.
_POOL_MIN_TRADES = 500
_MAX_BACKOFF_SECONDS = 2.0


class GroupResult:
    """
    The results of one group, in its input trade order.

    `entry(i)`: the response entry of trade i (a FlatBuffers reader, e.g. a
    `PriceBasisSwapResponse.Swaps(j)`), None when it failed.
    `error(i)`: the QuantraError of the call that failed trade i, else None.
    `failures`: [(start, end, QuantraError)], trades start..end-1 failed.
    `responses`: [(start, end, response reader)] of the calls that succeeded.
    """

    def __init__(self, method: str, size: int):
        self.method = method
        self.size = size
        self.responses = []
        self.failures = []
        self._accessor = TRADE_LISTS[method][2]

    def __len__(self):
        return self.size

    @property
    def ok(self) -> bool:
        """True when every trade of the group priced."""
        return not self.failures

    def _find(self, rows, i):
        if not 0 <= i < self.size:
            raise IndexError(i)
        starts = [row[0] for row in rows]
        k = bisect.bisect_right(starts, i) - 1
        if k >= 0 and i < rows[k][1]:
            return rows[k]
        return None

    def entry(self, i: int):
        """The response entry of trade i, None when it failed."""
        row = self._find(self.responses, i)
        if row is None:
            return None
        start, _, response = row
        return getattr(response, self._accessor)(i - start)

    def error(self, i: int) -> Optional[QuantraError]:
        """Why trade i failed, None when it priced."""
        row = self._find(self.failures, i)
        return None if row is None else row[2]

    def _sort(self):
        self.responses.sort(key=lambda row: row[0])
        self.failures.sort(key=lambda row: row[0])


class BatchResult:
    """
    The outcome of price_batch.

    `groups`: one GroupResult per input group, in input order.
    `wall_seconds`: time of the whole batch.
    `pack_seconds`: time spent building FlatBuffers, summed over processes.
    `calls`: gRPC calls made, retries included.
    `retries`: calls resent after RESOURCE_EXHAUSTED or UNAVAILABLE.
    `splits`: chunks split in two after DEADLINE_EXCEEDED or a message the
    engine found too large.
    """

    def __init__(self, groups: List[GroupResult]):
        self.groups = groups
        self.wall_seconds = 0.0
        self.pack_seconds = 0.0
        self.calls = 0
        self.retries = 0
        self.splits = 0

    @property
    def ok(self) -> bool:
        return all(group.ok for group in self.groups)

    def __repr__(self):
        failed = sum(end - start for g in self.groups for start, end, _ in g.failures)
        return (f"BatchResult(groups={len(self.groups)}, failed_trades={failed}, "
                f"wall={self.wall_seconds:.3f}s, pack={self.pack_seconds:.3f}s, "
                f"calls={self.calls}, retries={self.retries}, splits={self.splits})")


def workers_from_meta(gateway_url: str, timeout: float = 5.0) -> Optional[int]:
    """The engine's worker count, from the JSON gateway's GET /meta
    (`capacity.workers`); None when the gateway reports no capacity (its
    Envoy admin is not configured). The gRPC Meta call carries no capacity."""
    url = gateway_url.rstrip('/') + '/meta'
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        capacity = json.loads(resp.read().decode('utf-8')).get('capacity')
    return capacity.get('workers') if capacity else None


def plan_chunks(sizes: Sequence[int], in_flight: int, split_above: int) -> List[Tuple[int, int, int]]:
    """The first cut of a batch: (group, start, end) per request, largest first.
    A group of more than `split_above` trades is split evenly in proportion
    to its share of the batch's trades: round(in_flight * size / total)
    chunks, at least 1, at most `in_flight`. A single large group is spread
    over every call in flight; a batch of many markets stays one request per
    market, so no worker builds a market only to price a slice of it."""
    total = sum(sizes)
    jobs = []
    for g, n in enumerate(sizes):
        parts = 1
        if n and n > split_above:
            parts = max(1, min(in_flight, n, round(in_flight * n / total)))
        for k in range(parts):
            start, end = k * n // parts, (k + 1) * n // parts
            if end > start:
                jobs.append((g, start, end))
    jobs.sort(key=lambda job: job[1] - job[2])  # stable: largest first
    return jobs


# =============================================================================
# Packing (runs in the pool's processes or in a thread of this one)
# =============================================================================

def _pack_range(request, attr, start, end, max_bytes):
    """Pack trades start..end-1 of `request`. A message above `max_bytes` is
    cut into smaller ranges until each fits; a single trade that does not fit
    comes back with data None. Returns ([(start, end, data)], seconds)."""
    trades = getattr(request, attr)
    req = copy.copy(request)
    out = []
    seconds = 0.0

    def pack(s, e):
        nonlocal seconds
        setattr(req, attr, trades[s:e])
        t0 = time.perf_counter()
        builder = flatbuffers.Builder(1024)
        builder.Finish(req.Pack(builder))
        data = bytes(builder.Output())
        seconds += time.perf_counter() - t0
        if len(data) <= max_bytes:
            out.append((s, e, data))
        elif e - s == 1:
            out.append((s, e, None))
        else:
            parts = min(e - s, -(-len(data) * 11 // (max_bytes * 10)))
            parts = max(parts, 2)
            for k in range(parts):
                ks, ke = s + k * (e - s) // parts, s + (k + 1) * (e - s) // parts
                if ke > ks:
                    pack(ks, ke)

    pack(start, end)
    return out, seconds


# Fork-started pool processes read the groups from this module's copy taken at
# fork time, so only (group, start, end) crosses the process boundary.
_FORK_GROUPS = None
_FORK_LOCK = threading.Lock()


def _pack_forked(g, start, end, max_bytes):
    method, request = _FORK_GROUPS[g]
    return _pack_range(request, TRADE_LISTS[method][0], start, end, max_bytes)


def _pack_pickled(request, attr, offset, max_bytes):
    """Spawn-started pools: the request arrives pickled holding only its chunk."""
    chunks, seconds = _pack_range(request, attr, 0, len(getattr(request, attr)), max_bytes)
    return [(s + offset, e + offset, d) for s, e, d in chunks], seconds


class _Packer:
    """Packs (group, start, end) jobs and puts ('packed', job, result) or
    ('pack_error', job, exception) on the event queue."""

    def __init__(self, groups, max_bytes, processes, events):
        self.groups = groups
        self.max_bytes = max_bytes
        self.events = events
        self.pool = None
        self.mode = 'thread'
        if processes:
            methods = multiprocessing.get_all_start_methods()
            if sys.platform.startswith('linux') and 'fork' in methods:
                global _FORK_GROUPS
                with _FORK_LOCK:
                    _FORK_GROUPS = groups
                    try:
                        self.pool = multiprocessing.get_context('fork').Pool(processes)
                    finally:
                        _FORK_GROUPS = None
                self.mode = 'fork'
            else:
                self.pool = multiprocessing.get_context('spawn').Pool(processes)
                self.mode = 'spawn'
        if self.pool is None:
            self.thread = ThreadPoolExecutor(max_workers=1)

    def submit(self, job):
        g, start, end = job[:3]
        method, request = self.groups[g]
        attr = TRADE_LISTS[method][0]

        def done(result):
            self.events.put(('packed', job, result))

        def failed(exc):
            self.events.put(('pack_error', job, exc))

        if self.mode == 'fork':
            self.pool.apply_async(_pack_forked, (g, start, end, self.max_bytes),
                                  callback=done, error_callback=failed)
        elif self.mode == 'spawn':
            sub = copy.copy(request)
            setattr(sub, attr, getattr(request, attr)[start:end])
            self.pool.apply_async(_pack_pickled, (sub, attr, start, self.max_bytes),
                                  callback=done, error_callback=failed)
        else:
            def run():
                try:
                    done(_pack_range(request, attr, start, end, self.max_bytes))
                except Exception as exc:  # recorded for this chunk only
                    failed(exc)
            self.thread.submit(run)

    def close(self):
        if self.pool is not None:
            self.pool.terminate()
            self.pool.join()
        else:
            self.thread.shutdown(wait=False)


# =============================================================================
# The batch
# =============================================================================

def price_batch(
    target,
    groups: Sequence[Tuple[str, object]],
    in_flight: int = 8,
    split_above: int = DEFAULT_SPLIT_ABOVE,
    max_message_bytes: int = DEFAULT_MAX_MESSAGE_BYTES,
    timeout: float = 30.0,
    processes: Optional[int] = None,
    max_retries: int = 5,
    backoff: float = 0.1,
    max_splits: int = 3,
    call=None,
) -> BatchResult:
    """
    Price a batch of groups.

    Args:
        target: a Client, a grpc.Channel or a "host:port" address (unused
            with `call`).
        groups: [(method key of Client.METHODS, request object)], one per
            market, the request's trade list holding all that market's trades.
        in_flight: most calls in flight at once; set it to the engine's
            worker count (workers_from_meta).
        split_above: a group of more trades than this is split evenly, in
            proportion to its share of the batch's trades, into at most
            `in_flight` requests (plan_chunks); smaller groups go as one
            request.
        max_message_bytes: a packed request above this is cut further.
            Keep it below the workers' QUANTRA_MAX_MESSAGE_BYTES.
        timeout: deadline of each call, in seconds (the engine's default
            limit is 30 s).
        processes: packing processes; None = one per CPU when the batch has
            enough trades to pay for starting them, 0 = pack in a thread of
            this process (no process pool).
        max_retries: resends of a call refused with RESOURCE_EXHAUSTED (a
            full engine) or UNAVAILABLE, with exponential backoff from
            `backoff` seconds (capped at 2 s).
        max_splits: how many times a chunk that hit DEADLINE_EXCEEDED may be
            split in two and retried.
        call: for tests: call(method_key, data, timeout) returning the
            response bytes or raising grpc.RpcError, instead of gRPC.

    Returns:
        BatchResult: per group, the results in input trade order; failures
        are recorded per chunk and never lose the other results.
    """
    started = time.perf_counter()
    groups = [tuple(group) for group in groups]
    sizes = []
    for method, request in groups:
        if method not in TRADE_LISTS:
            raise ValueError(f"unknown method {method!r}; expected one of Client.METHODS")
        if TRADE_LISTS[method] is None:
            raise ValueError(f"{method!r} has no trade list to batch; call the Client method")
        sizes.append(len(getattr(request, TRADE_LISTS[method][0]) or []))
    result = BatchResult([GroupResult(method, n) for (method, _), n in zip(groups, sizes)])
    in_flight = max(1, in_flight)
    jobs = plan_chunks(sizes, in_flight, split_above)
    if not jobs:
        return result

    if processes is None:
        processes = (os.cpu_count() or 1) if sum(sizes) >= _POOL_MIN_TRADES else 0
    processes = min(processes, len(jobs)) if processes else 0

    events = queue.Queue()
    packer = _Packer(groups, max_message_bytes, processes, events)
    send, close_sender = _sync_sender(call, in_flight) if call else _grpc_sender(target)
    timers = []
    ready = []  # heap of (-trades, seq, job, data); job = (group, start, end, attempt, splits)
    seq = 0
    open_jobs = 0  # jobs not yet priced, failed or replaced by their halves
    in_flight_now = 0

    def queue_ready(job, data):
        nonlocal seq
        seq += 1
        heapq.heappush(ready, (job[1] - job[2], seq, job, data))

    def submit(job):
        nonlocal open_jobs
        open_jobs += 1
        packer.submit(job)

    def finish(job, error=None, response=None):
        nonlocal open_jobs
        open_jobs -= 1
        group = result.groups[job[0]]
        if error is not None:
            group.failures.append((job[1], job[2], error))
        else:
            _record(group, job[1], job[2], response)

    try:
        for g, start, end in jobs:
            submit((g, start, end, 0, 0))
        while open_jobs:
            while ready and in_flight_now < in_flight:
                _, _, job, data = heapq.heappop(ready)
                in_flight_now += 1
                result.calls += 1
                send(groups[job[0]][0], data, timeout,
                     lambda response, error, job=job, data=data:
                     events.put(('done', job, (data, response, error))))
            kind, job, payload = events.get()
            if kind == 'packed':
                chunks, seconds = payload
                result.pack_seconds += seconds
                open_jobs -= 1
                for s, e, data in chunks:
                    open_jobs += 1
                    sub_job = (job[0], s, e) + tuple(job[3:])
                    if data is None:
                        finish(sub_job, QuantraError(
                            f"trade {s} alone packs above max_message_bytes ({max_message_bytes})"))
                    else:
                        queue_ready(sub_job, data)
            elif kind == 'pack_error':
                finish(job, QuantraError(f"packing failed: {payload!r}"))
            elif kind == 'retry':
                queue_ready(job, payload)
            else:  # 'done'
                in_flight_now -= 1
                data, response, error = payload
                if error is None:
                    finish(job, response=response)
                    continue
                g, start, end, attempt, splits = job
                code = error.code
                too_large = (code == grpc.StatusCode.RESOURCE_EXHAUSTED
                             and 'larger than max' in str(error))
                if too_large or code == grpc.StatusCode.DEADLINE_EXCEEDED:
                    if end - start > 1 and (too_large or splits < max_splits):
                        open_jobs -= 1
                        result.splits += 1
                        mid = (start + end) // 2
                        submit((g, start, mid, 0, splits + 1))
                        submit((g, mid, end, 0, splits + 1))
                        continue
                elif code in (grpc.StatusCode.RESOURCE_EXHAUSTED, grpc.StatusCode.UNAVAILABLE):
                    if attempt < max_retries:
                        result.retries += 1
                        delay = min(_MAX_BACKOFF_SECONDS, backoff * 2 ** attempt)
                        timer = threading.Timer(delay * random.uniform(0.5, 1.0), events.put, (
                            ('retry', (g, start, end, attempt + 1, splits), data),))
                        timer.daemon = True
                        timers.append(timer)
                        timer.start()
                        continue
                finish(job, error)
    finally:
        for timer in timers:
            timer.cancel()
        packer.close()
        close_sender()
    for group in result.groups:
        group._sort()
    result.wall_seconds = time.perf_counter() - started
    return result


def _record(group, start, end, response_bytes):
    """Keep a successful response as a reader; refuse one whose entry count
    does not match the chunk (it could not be mapped back to the trades)."""
    _, response_class, accessor = TRADE_LISTS[group.method]
    response = response_class.GetRootAs(response_bytes, 0)
    count = getattr(response, accessor + 'Length')()
    if count != end - start:
        group.failures.append((start, end, QuantraError(
            f"response has {count} entries for {end - start} trades")))
    else:
        group.responses.append((start, end, response))


def _grpc_sender(target):
    """send(method, data, timeout, on_done(response, error)) over gRPC, and
    its close function (closes only a channel opened here)."""
    if isinstance(target, Client):
        channel, own = target.channel, False
    elif isinstance(target, grpc.Channel):
        channel, own = target, False
    else:
        channel, own = grpc.insecure_channel(str(target), options=[
            ('grpc.max_receive_message_length', -1),
            ('grpc.max_send_message_length', -1)]), True
    stubs = {method: channel.unary_unary(path) for method, path in Client.METHODS.items()}

    def send(method, data, timeout, on_done):
        future = stubs[method].future(data, timeout=timeout)

        def done(f):
            if f.code() == grpc.StatusCode.OK:
                on_done(f.result(), None)
            else:
                on_done(None, QuantraError(f"gRPC error: {f.code()} - {f.details()}",
                                           code=f.code(), details=_error_details(f)))
        future.add_done_callback(done)

    return send, (channel.close if own else (lambda: None))


def _sync_sender(call, in_flight):
    """The same interface around a blocking call(method, data, timeout)."""
    threads = ThreadPoolExecutor(max_workers=max(1, in_flight))

    def send(method, data, timeout, on_done):
        def run():
            try:
                response = call(method, data, timeout)
            except grpc.RpcError as e:
                on_done(None, QuantraError(f"gRPC error: {e.code()} - {e.details()}",
                                           code=e.code(), details=_error_details(e)))
                return
            except Exception as e:  # recorded for this chunk only
                on_done(None, QuantraError(f"call failed: {e!r}"))
                return
            on_done(response, None)
        threads.submit(run)

    return send, (lambda: threads.shutdown(wait=False))
