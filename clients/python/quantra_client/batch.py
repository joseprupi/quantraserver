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

A group may also be given as blocks packed earlier
(`("basis_swap", BlockGroup(blocks, pricing_block=market))`, see
quantra_client.blocks): its requests are assembled from the blocks on the
calling thread, without packing processes.

Results come back per group in the input trade order whatever the split, as
FlatBuffers readers (the response entry of each trade, e.g. `Swaps(j)`), with
the trades that failed and why.

The packing processes start with `forkserver` on Linux and `spawn` elsewhere:
the chunk to pack is pickled to them, so a script must guard its entry point
with `if __name__ == "__main__":`. A long-running process can start the
processes once with `BatchPool` and pass it as `pool=` to every batch.

See examples/price_batch.py for a runnable batch.
"""

import bisect
import copy
import heapq
import json
import logging
import multiprocessing
import os
import pickle
import queue
import random
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, Dict, List, Optional, Sequence, Tuple, Union

import flatbuffers
import grpc

from .client import Client, QuantraError, _error_details

from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse
from quantra.PriceFloatingRateBondResponse import PriceFloatingRateBondResponse
from quantra.PriceZeroCouponBondResponse import PriceZeroCouponBondResponse
from quantra.PriceCallableFixedRateBondResponse import PriceCallableFixedRateBondResponse
from quantra.PriceVanillaSwapResponse import PriceVanillaSwapResponse
from quantra.PriceOisSwapResponse import PriceOisSwapResponse
from quantra.PriceBasisSwapResponse import PriceBasisSwapResponse
from quantra.PriceZeroCouponSwapResponse import PriceZeroCouponSwapResponse
from quantra.PriceFRAResponse import PriceFRAResponse
from quantra.PriceFxForwardResponse import PriceFxForwardResponse
from quantra.PriceCrossCurrencySwapResponse import PriceCrossCurrencySwapResponse
from quantra.PriceCommoditySwapResponse import PriceCommoditySwapResponse
from quantra.PriceStructuredNoteResponse import PriceStructuredNoteResponse
from quantra.PriceEquityOptionResponse import PriceEquityOptionResponse
from quantra.PriceCapFloorResponse import PriceCapFloorResponse
from quantra.PriceSwaptionResponse import PriceSwaptionResponse
from quantra.PriceCDSResponse import PriceCDSResponse
from quantra.PriceZeroCouponInflationSwapResponse import PriceZeroCouponInflationSwapResponse
from quantra.PriceYearOnYearInflationSwapResponse import PriceYearOnYearInflationSwapResponse
from quantra.PriceYearOnYearInflationCapFloorResponse import PriceYearOnYearInflationCapFloorResponse

logger = logging.getLogger(__name__)

# Per method of Client.METHODS: (request trade-list attribute, response class,
# response accessor of the per-trade entry), the response holding one entry
# per trade in request order. None for the methods whose request has no trade
# list (curve builds, quote-sheet validation): price_batch refuses them.
TRADE_LISTS = {
    'fixed_rate_bond': ('bonds', PriceFixedRateBondResponse, 'Bonds'),
    'floating_rate_bond': ('bonds', PriceFloatingRateBondResponse, 'Bonds'),
    'zero_coupon_bond': ('bonds', PriceZeroCouponBondResponse, 'Bonds'),
    'callable_fixed_rate_bond': ('bonds', PriceCallableFixedRateBondResponse, 'Bonds'),
    'vanilla_swap': ('swaps', PriceVanillaSwapResponse, 'Swaps'),
    'ois_swap': ('swaps', PriceOisSwapResponse, 'Swaps'),
    'basis_swap': ('swaps', PriceBasisSwapResponse, 'Swaps'),
    'zero_coupon_swap': ('swaps', PriceZeroCouponSwapResponse, 'Swaps'),
    'fra': ('fras', PriceFRAResponse, 'Fras'),
    'fx_forward': ('fxForwards', PriceFxForwardResponse, 'FxForwards'),
    'cross_currency_swap': ('swaps', PriceCrossCurrencySwapResponse, 'Swaps'),
    'commodity_swap': ('swaps', PriceCommoditySwapResponse, 'Swaps'),
    'build_commodity_curves': None,
    'structured_note': ('notes', PriceStructuredNoteResponse, 'Notes'),
    'equity_option': ('options', PriceEquityOptionResponse, 'Options'),
    'cap_floor': ('capFloors', PriceCapFloorResponse, 'CapFloors'),
    'swaption': ('swaptions', PriceSwaptionResponse, 'Swaptions'),
    'cds': ('cdsList', PriceCDSResponse, 'CdsList'),
    'zero_coupon_inflation_swap': ('swaps', PriceZeroCouponInflationSwapResponse, 'Swaps'),
    'year_on_year_inflation_swap': ('swaps', PriceYearOnYearInflationSwapResponse, 'Swaps'),
    'year_on_year_inflation_cap_floor': (
        'capFloors', PriceYearOnYearInflationCapFloorResponse, 'CapFloors'),
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
# Below this many trades in total a process pool started for the batch costs
# more to start than packing in this process (a BatchPool passed as `pool=`
# is always used). Measured on basis swaps over three curves: a fork pool won
# from 500 trades; a forkserver pool, whose processes import the generated
# classes when they unpickle their first chunk (about 0.2 s), tied with one
# thread at 1,000 and won at 2,000 (docs/client.md).
_POOL_MIN_TRADES = {'fork': 500, 'forkserver': 1000, 'spawn': 1000}
_MAX_BACKOFF_SECONDS = 2.0
# How long a pool may take to shut down: first gracefully (close + join),
# then by terminate, then by killing its processes.
_SHUTDOWN_SECONDS = 10.0
_KILL_SECONDS = 5.0

START_METHODS = ('forkserver', 'spawn', 'fork')


def default_start_method() -> str:
    """`forkserver` on Linux, `spawn` elsewhere. Both start the packing
    processes without copying this process's threads or gRPC state."""
    if sys.platform.startswith('linux') and 'forkserver' in multiprocessing.get_all_start_methods():
        return 'forkserver'
    return 'spawn'


def _check_start_method(start_method):
    method = start_method or default_start_method()
    if method not in START_METHODS:
        raise ValueError(f"start_method {start_method!r}: expected one of {START_METHODS}")
    if method not in multiprocessing.get_all_start_methods():
        raise ValueError(f"start_method {method!r} is not available on this platform")
    return method


class GroupResult:
    """
    The results of one group, in its input trade order.

    `entry(i)`: the response entry of trade i (a FlatBuffers reader, e.g. a
    `PriceBasisSwapResponse.Swaps(j)`), None when it failed. A request sent
    with `price_only` has no entries: use `npvs()`.
    `npvs()`: the NPV of every trade in input order (NaN where it failed),
    a numpy array when numpy is importable, else a list; from the `npvs`
    vector of price-only responses, from each entry's `Npv()` otherwise.
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
        start, end, response = row
        if _is_price_only(response, self._accessor, end - start):
            raise ValueError(
                f"trade {i} was priced with price_only: its response holds only the NPV; "
                "read it with npvs()")
        return getattr(response, self._accessor)(i - start)

    def npvs(self):
        """The NPV of every trade in input order, NaN where it failed: a numpy
        array when numpy is importable, else a list of floats."""
        np = _numpy()
        out = np.full(self.size, np.nan) if np is not None else [float('nan')] * self.size
        for start, end, response in self.responses:
            if _is_price_only(response, self._accessor, end - start):
                if np is not None:
                    out[start:end] = response.NpvsAsNumpy()
                else:
                    out[start:end] = [response.Npvs(j) for j in range(end - start)]
            else:
                read = getattr(response, self._accessor)
                values = [read(j).Npv() for j in range(end - start)]
                out[start:end] = values
        return out

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
    `pack_seconds`: time spent building FlatBuffers, summed over processes
    (assembling block groups included).
    `calls`: gRPC calls made, retries included.
    `retries`: calls resent after RESOURCE_EXHAUSTED or UNAVAILABLE.
    `splits`: chunks split in two after DEADLINE_EXCEEDED or a message the
    engine found too large.
    `stopped`: None, 'stop' (the `stop` callable returned true) or
    'deadline' (the batch `deadline` passed) when trades were left unsent.
    `callback_errors`: the exceptions raised by `on_call`, `on_result`,
    `stop` or a `metadata` callable (each logged; the batch went on).
    `warnings`: what went wrong outside the trades, e.g. a packing pool that
    did not shut down in time and was left behind.
    """

    def __init__(self, groups: List[GroupResult]):
        self.groups = groups
        self.wall_seconds = 0.0
        self.pack_seconds = 0.0
        self.calls = 0
        self.retries = 0
        self.splits = 0
        self.stopped = None
        self.callback_errors = []
        self.warnings = []

    @property
    def ok(self) -> bool:
        return all(group.ok for group in self.groups)

    def __repr__(self):
        failed = sum(end - start for g in self.groups for start, end, _ in g.failures)
        return (f"BatchResult(groups={len(self.groups)}, failed_trades={failed}, "
                f"wall={self.wall_seconds:.3f}s, pack={self.pack_seconds:.3f}s, "
                f"calls={self.calls}, retries={self.retries}, splits={self.splits}"
                + (f", stopped={self.stopped!r}" if self.stopped else "") + ")")


def workers_from_meta(gateway_url: str, timeout: float = 5.0) -> Optional[int]:
    """The engine's worker count, from the JSON gateway's GET /meta
    (`capacity.workers`); None when the gateway reports no capacity (its
    Envoy admin is not configured). The gRPC Meta call carries no capacity."""
    url = gateway_url.rstrip('/') + '/meta'
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        capacity = json.loads(resp.read().decode('utf-8')).get('capacity')
    return capacity.get('workers') if capacity else None


def plan_chunks(sizes: Sequence[int], in_flight: int, split_above: int,
                max_trades: Optional[Sequence[Optional[int]]] = None) -> List[Tuple[int, int, int]]:
    """The first cut of a batch: (group, start, end) per request, largest first.
    A group of more than `split_above` trades is split evenly in proportion
    to its share of the batch's trades: round(in_flight * size / total)
    chunks, at least 1, at most `in_flight`. A single large group is spread
    over every call in flight; a batch of many markets stays one request per
    market, so no worker builds a market only to price a slice of it.
    `max_trades[g]` (None = no cap) cuts group g further, evenly, so that no
    request holds more than that many trades."""
    total = sum(sizes)
    jobs = []
    for g, n in enumerate(sizes):
        parts = 1
        if n and n > split_above:
            parts = max(1, min(in_flight, n, round(in_flight * n / total)))
        cap = max_trades[g] if max_trades else None
        if cap and n > cap * parts:
            parts = -(-n // cap)
        for k in range(parts):
            start, end = k * n // parts, (k + 1) * n // parts
            if end > start:
                jobs.append((g, start, end))
    jobs.sort(key=lambda job: job[1] - job[2])  # stable: largest first
    return jobs


# =============================================================================
# Packing (runs in the pool's processes or in a thread of this one)
# =============================================================================

def _pack_range(request, attr, start, end, max_bytes, force_defaults=True):
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
        builder.ForceDefaults(force_defaults)
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


# Processes of a fork pool started for one batch read the groups from this
# module's copy taken at fork time, so only (group, start, end) crosses the
# process boundary.
_FORK_GROUPS = None
_FORK_LOCK = threading.Lock()


def _pack_forked(g, start, end, max_bytes, force_defaults):
    if _FORK_GROUPS is None:
        # A process the pool started after the fork-time copy: it replaces
        # one that died (in a process holding gRPC channels a forked child
        # can abort inside gRPC).
        raise RuntimeError("this fork-pool process was restarted after a process of the pool "
                           "died and has no copy of the requests; use the default start method")
    method, request = _FORK_GROUPS[g]
    return _pack_range(request, TRADE_LISTS[method][0], start, end, max_bytes, force_defaults)


def _pack_pickled(payload, attr, offset, max_bytes, force_defaults):
    """Every other pool: the request arrives pickled holding only its chunk.
    It is unpickled here, inside the task, so that a request that cannot be
    unpickled in this process fails its chunk instead of the process (a pool
    process that dies drops its task, which would never complete)."""
    request = pickle.loads(payload)
    chunks, seconds = _pack_range(request, attr, 0, len(getattr(request, attr)), max_bytes,
                                  force_defaults)
    return [(s + offset, e + offset, d) for s, e, d in chunks], seconds


def _run_bounded(fn, seconds):
    """Run fn in a daemon thread; True when it returned within `seconds`."""
    thread = threading.Thread(target=fn, daemon=True, name='quantra-batch-pool-shutdown')
    thread.start()
    thread.join(seconds)
    return not thread.is_alive()


def _shutdown_pool(pool, graceful, seconds=_SHUTDOWN_SECONDS):
    """Shut a multiprocessing pool down without ever blocking for good.
    Graceful: close() then join(), so no process is restarted behind the
    shutdown. Then (or directly when not graceful) terminate(); then kill
    the processes left. Returns None, or a message when the pool could not
    be shut down and was left behind."""
    if graceful:
        pool.close()
        if _run_bounded(pool.join, seconds):
            return None
    if _run_bounded(pool.terminate, seconds):
        return None
    for process in list(getattr(pool, '_pool', ()) or ()):
        try:
            if process.exitcode is None:
                process.kill()
        except Exception:  # a process already reaped
            pass
    if _run_bounded(pool.join, _KILL_SECONDS):
        return None
    pids = [p.pid for p in list(getattr(pool, '_pool', ()) or ())]
    return (f"the packing pool did not shut down within {2 * seconds + _KILL_SECONDS:.0f} s "
            f"and was left behind (processes {pids})")


class BatchPool:
    """
    Packing processes owned by the caller, started once and reused by every
    batch given it as `pool=`: a long-running process pays the start of the
    processes once instead of once per batch. Batches may share it from
    several threads at once.

        with BatchPool(processes=8) as pool:
            for groups in batches:
                result = price_batch(client, groups, pool=pool)

    `start_method`: 'forkserver' (default on Linux), 'spawn' (default
    elsewhere) or 'fork'. 'fork' copies this process, its threads' locks and
    its gRPC state included: unsafe in a process that holds gRPC channels or
    runs threads. With any start method the chunks are pickled to the
    processes.

    `close()` shuts the processes down (gracefully, bounded in time; True
    when they all stopped); leaving the `with` block calls it.
    """

    def __init__(self, processes: Optional[int] = None, start_method: Optional[str] = None):
        self.start_method = _check_start_method(start_method)
        self.processes = max(1, processes or os.cpu_count() or 1)
        self._pool = multiprocessing.get_context(self.start_method).Pool(self.processes)
        self._closed = False
        self._lock = threading.Lock()

    def _apply(self, fn, args, callback, error_callback):
        with self._lock:
            if self._closed:
                raise ValueError("BatchPool is closed")
            self._pool.apply_async(fn, args, callback=callback, error_callback=error_callback)

    @property
    def closed(self) -> bool:
        return self._closed

    def close(self, timeout: float = _SHUTDOWN_SECONDS) -> bool:
        """Stop the processes once the work given them is done. Never blocks
        much beyond `timeout` (twice it plus a few seconds at worst): a pool
        that will not stop is left behind with a logged warning and False."""
        with self._lock:
            if self._closed:
                return True
            self._closed = True
        message = _shutdown_pool(self._pool, graceful=True, seconds=timeout)
        if message:
            logger.warning(message)
        return message is None

    def terminate(self, timeout: float = _SHUTDOWN_SECONDS) -> bool:
        """Stop the processes now, work in progress dropped."""
        with self._lock:
            self._closed = True
        message = _shutdown_pool(self._pool, graceful=False, seconds=timeout)
        if message:
            logger.warning(message)
        return message is None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type is None:
            self.close()
        else:
            self.terminate()
        return False


class _Packer:
    """Packs (group, start, end) jobs and puts ('packed', job, result) or
    ('pack_error', job, exception) on the event queue."""

    def __init__(self, groups, max_bytes, force_defaults, processes, start_method, pool, events):
        self.groups = groups
        self.max_bytes = max_bytes
        self.force_defaults = force_defaults
        self.events = events
        self.pool = None        # a pool started for this batch
        self.shared = pool      # a BatchPool of the caller
        self.thread = None
        if pool is not None:
            self.mode = 'pickle'
        elif processes:
            method = _check_start_method(start_method)
            context = multiprocessing.get_context(method)
            if method == 'fork':
                global _FORK_GROUPS
                with _FORK_LOCK:
                    _FORK_GROUPS = groups
                    try:
                        self.pool = context.Pool(processes)
                    finally:
                        _FORK_GROUPS = None
                self.mode = 'fork'
            else:
                self.pool = context.Pool(processes)
                self.mode = 'pickle'
        else:
            self.mode = 'thread'
            self.thread = ThreadPoolExecutor(max_workers=1)

    def submit(self, job):
        g, start, end = job[:3]
        method, request = self.groups[g]
        attr = TRADE_LISTS[method][0]

        def done(result):
            self.events.put(('packed', job, result))

        def failed(exc):
            if self.mode == 'pickle':
                exc = QuantraError(
                    f"packing failed: {exc!r} (the request is pickled to the packing "
                    "processes; a request that does not pickle needs processes=0)")
            self.events.put(('pack_error', job, exc))

        if self.mode == 'fork':
            self.pool.apply_async(_pack_forked, (g, start, end, self.max_bytes, self.force_defaults),
                                  callback=done, error_callback=failed)
        elif self.mode == 'pickle':
            sub = copy.copy(request)
            setattr(sub, attr, getattr(request, attr)[start:end])
            try:
                payload = pickle.dumps(sub, protocol=pickle.HIGHEST_PROTOCOL)
            except Exception as exc:
                failed(exc)
                return
            args = (payload, attr, start, self.max_bytes, self.force_defaults)
            if self.shared is not None:
                self.shared._apply(_pack_pickled, args, done, failed)
            else:
                self.pool.apply_async(_pack_pickled, args, callback=done, error_callback=failed)
        else:
            def run():
                try:
                    done(_pack_range(request, attr, start, end, self.max_bytes, self.force_defaults))
                except Exception as exc:  # recorded for this chunk only
                    failed(exc)
            self.thread.submit(run)

    def close(self, graceful):
        """Shut down what this batch started; None or a warning message."""
        if self.thread is not None:
            self.thread.shutdown(wait=False)
        if self.pool is not None:
            return _shutdown_pool(self.pool, graceful)
        return None


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
    *,
    max_trades: Union[None, int, Dict[str, int]] = None,
    deadline: Optional[float] = None,
    force_defaults: bool = True,
    start_method: Optional[str] = None,
    pool: Optional[BatchPool] = None,
    metadata=None,
    on_call: Optional[Callable] = None,
    on_result: Optional[Callable] = None,
    stop: Optional[Callable[[], bool]] = None,
) -> BatchResult:
    """
    Price a batch of groups.

    Args:
        target: a Client, a grpc.Channel or a "host:port" address (unused
            with `call`).
        groups: [(method key of Client.METHODS, request object)], one per
            market, the request's trade list holding all that market's trades;
            or (method, BlockGroup) for trades packed earlier as blocks
            (quantra_client.blocks), assembled on the calling thread.
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
        processes: packing processes started for this batch; None = one per
            CPU when the batch has enough trades to pay for starting them,
            0 = pack in a thread of this process (no process pool). Ignored
            with `pool`.
        max_retries: resends of a call refused with RESOURCE_EXHAUSTED (a
            full engine) or UNAVAILABLE, with exponential backoff from
            `backoff` seconds (capped at 2 s).
        max_splits: how many times a chunk that hit DEADLINE_EXCEEDED may be
            split in two and retried.
        call: call(method_key, data, timeout) returning the response bytes or
            raising grpc.RpcError, instead of gRPC (`metadata` is not passed
            to it).
        max_trades: most trades in one request: an int for every method or
            {method key: int}; a group above it is cut evenly into requests
            of at most that many trades, beyond what `split_above` gives.
        deadline: seconds for the whole batch (None = no limit). No call is
            given a timeout beyond the time left and nothing is sent once it
            has passed; the trades left are recorded as failed with
            DEADLINE_EXCEEDED, details {"reason": "BatchDeadline"}.
        force_defaults: pack with ForceDefaults(True), so a scalar equal to
            its schema default is written to the wire too (block groups keep
            what their blocks were packed with).
        start_method: how the packing processes of this batch start:
            'forkserver' (default on Linux), 'spawn' (default elsewhere) or
            'fork' (unsafe in a process holding gRPC channels or threads).
        pool: a BatchPool, started once by the caller and reused across
            batches, instead of processes started for this batch.
        metadata: gRPC metadata added to every call: a sequence of
            (key, value), or a callable(method, group_index, start, end)
            returning one per call.
        on_call: on_call(method, group_index, start, end, seconds, error)
            after every call attempt, error None on success.
        on_result: on_result(group_index, start, end, response_bytes) as each
            chunk prices.
        stop: callable checked before each send; once it returns true nothing
            new is sent, the calls in flight finish and the trades left are
            recorded as failed with CANCELLED, details {"reason": "Cancelled"}.

        The callbacks (`on_call`, `on_result`, `stop`, `metadata`) run on
        the thread that called price_batch. An exception in one is logged
        and kept in `result.callback_errors`; the batch goes on (an
        exception in `stop` counts as true, one in `metadata` fails that
        chunk).

    Returns:
        BatchResult: per group, the results in input trade order; failures
        are recorded per chunk and never lose the other results.
    """
    started = time.perf_counter()
    deadline_at = None if deadline is None else started + max(0.0, deadline)
    from .blocks import BlockGroup, _BlockChunks

    groups = [tuple(group) for group in groups]
    sizes = []
    block_chunks = {}  # group index -> _BlockChunks, for the groups given as blocks
    for g, (method, request) in enumerate(groups):
        if method not in TRADE_LISTS:
            raise ValueError(f"unknown method {method!r}; expected one of Client.METHODS")
        if TRADE_LISTS[method] is None:
            raise ValueError(f"{method!r} has no trade list to batch; call the Client method")
        if isinstance(request, BlockGroup):
            block_chunks[g] = _BlockChunks(method, request)
            sizes.append(len(request))
        else:
            sizes.append(len(getattr(request, TRADE_LISTS[method][0]) or []))
    if isinstance(max_trades, dict):
        unknown = set(max_trades) - set(TRADE_LISTS)
        if unknown:
            raise ValueError(f"max_trades: unknown method(s) {sorted(unknown)}")
        caps = [max_trades.get(method) for method, _ in groups]
    else:
        caps = [max_trades] * len(groups)
    if any(cap is not None and cap < 1 for cap in caps):
        raise ValueError("max_trades must be at least 1")
    if pool is not None and pool.closed:
        raise ValueError("BatchPool is closed")
    if start_method is not None:
        _check_start_method(start_method)
    result = BatchResult([GroupResult(method, n) for (method, _), n in zip(groups, sizes)])
    in_flight = max(1, in_flight)
    jobs = plan_chunks(sizes, in_flight, split_above, caps)
    if not jobs:
        return result

    # Only the groups given as objects are packed; block groups are
    # assembled on this thread.
    object_jobs = sum(1 for job in jobs if job[0] not in block_chunks)
    object_trades = sum(n for g, n in enumerate(sizes) if g not in block_chunks)
    if pool is None:
        if processes is None:
            enough = object_trades >= _POOL_MIN_TRADES[_check_start_method(start_method)]
            processes = (os.cpu_count() or 1) if enough else 0
        processes = min(processes, object_jobs) if processes else 0

    def callback(fn, *args):
        """Run a caller's callback; (ok, value)."""
        try:
            return True, fn(*args)
        except Exception as exc:  # the caller's code: logged, the batch goes on
            logger.exception("price_batch: %s raised", getattr(fn, '__name__', 'a callback'))
            result.callback_errors.append(exc)
            return False, exc

    events = queue.Queue()
    packer = _Packer(groups, max_message_bytes, force_defaults, processes, start_method, pool,
                     events)
    if call:
        send, close_sender = _sync_sender(call, in_flight)
    else:
        send, close_sender = _grpc_sender(target)
    timers = []
    ready = []  # heap of (-trades, seq, job, data); job = (group, start, end, attempt, splits)
    seq = 0
    # Every chunk not yet priced or failed, by (group, start) (live chunks
    # never overlap): (state, job), state 'pack' | 'ready' | 'retry' | 'flight'.
    live = {}
    in_flight_now = 0
    halted = None  # the error the unsent trades get once stopped

    def queue_ready(job, data):
        nonlocal seq
        seq += 1
        live[job[:2]] = ('ready', job)
        heapq.heappush(ready, (job[1] - job[2], seq, job, data))

    def submit(job):
        live[job[:2]] = ('pack', job)
        chunks = block_chunks.get(job[0])
        if chunks is None:
            packer.submit(job)
            return
        try:
            events.put(('packed', job, chunks.pack(job[1], job[2], max_message_bytes)))
        except Exception as exc:  # recorded for this chunk only
            events.put(('pack_error', job, QuantraError(f"assembling failed: {exc!r}")))

    def fail(job, error):
        live.pop(job[:2], None)
        result.groups[job[0]].failures.append((job[1], job[2], error))

    def succeed(job, response):
        live.pop(job[:2], None)
        g, start, end = job[:3]
        if _record(result.groups[g], start, end, response) and on_result is not None:
            callback(on_result, g, start, end, response)

    def is_live(job, state):
        entry = live.get(job[:2])
        return entry is not None and entry[0] == state and entry[1] == job

    def halt(kind, error):
        nonlocal halted
        halted = error
        result.stopped = kind
        ready.clear()
        for state, job in list(live.values()):
            if state != 'flight':
                fail(job, error)

    def check_stop():
        if halted is not None:
            return
        if stop is not None:
            ok, value = callback(stop)
            if not ok or value:
                halt('stop', QuantraError(
                    "stopped before these trades were sent",
                    code=grpc.StatusCode.CANCELLED, details={'reason': 'Cancelled'}))
                return
        if deadline_at is not None and time.perf_counter() >= deadline_at:
            halt('deadline', QuantraError(
                f"the batch deadline ({deadline} s) passed before these trades were sent",
                code=grpc.StatusCode.DEADLINE_EXCEEDED, details={'reason': 'BatchDeadline'}))

    def send_one(job, data):
        nonlocal in_flight_now
        g, start, end = job[:3]
        method = groups[g][0]
        call_timeout = timeout
        if deadline_at is not None:
            call_timeout = min(timeout, max(0.0, deadline_at - time.perf_counter()))
        md = metadata
        if callable(metadata):
            ok, md = callback(metadata, method, g, start, end)
            if not ok:
                fail(job, QuantraError(f"metadata callable failed: {md!r}"))
                return
        live[job[:2]] = ('flight', job)
        in_flight_now += 1
        result.calls += 1
        sent_at = time.perf_counter()
        send(method, data, call_timeout, md,
             lambda response, error, job=job, data=data:
             events.put(('done', job, (data, response, error, time.perf_counter() - sent_at))))

    graceful = False
    try:
        for g, start, end in jobs:
            submit((g, start, end, 0, 0))
        while live:
            check_stop()
            while halted is None and ready and in_flight_now < in_flight:
                check_stop()
                if halted is not None:
                    break
                _, _, job, data = heapq.heappop(ready)
                send_one(job, data)
            if not live:
                break
            wait = None
            if halted is None:
                if deadline_at is not None:
                    wait = max(0.0, deadline_at - time.perf_counter()) + 0.001
                if stop is not None:
                    wait = 0.05 if wait is None else min(wait, 0.05)
            try:
                kind, job, payload = events.get(timeout=wait)
            except queue.Empty:
                continue
            if kind == 'packed':
                if not is_live(job, 'pack'):
                    continue  # stopped meanwhile
                chunks, seconds = payload
                result.pack_seconds += seconds
                del live[job[:2]]
                for s, e, data in chunks:
                    sub_job = (job[0], s, e) + tuple(job[3:])
                    if data is None:
                        fail(sub_job, QuantraError(
                            f"trade {s} alone packs above max_message_bytes ({max_message_bytes})"))
                    else:
                        queue_ready(sub_job, data)
            elif kind == 'pack_error':
                if is_live(job, 'pack'):
                    fail(job, payload if isinstance(payload, QuantraError)
                         else QuantraError(f"packing failed: {payload!r}"))
            elif kind == 'retry':
                if is_live(job, 'retry'):
                    queue_ready(job, payload)
            else:  # 'done'
                in_flight_now -= 1
                data, response, error, seconds = payload
                g, start, end, attempt, splits = job
                if on_call is not None:
                    callback(on_call, groups[g][0], g, start, end, seconds, error)
                if error is None:
                    succeed(job, response)
                    continue
                code = error.code
                too_large = (code == grpc.StatusCode.RESOURCE_EXHAUSTED
                             and 'larger than max' in str(error))
                if too_large or code == grpc.StatusCode.DEADLINE_EXCEEDED:
                    if end - start > 1 and (too_large or splits < max_splits):
                        if halted is not None:
                            fail(job, halted)
                            continue
                        del live[job[:2]]
                        result.splits += 1
                        mid = (start + end) // 2
                        submit((g, start, mid, 0, splits + 1))
                        submit((g, mid, end, 0, splits + 1))
                        continue
                elif code in (grpc.StatusCode.RESOURCE_EXHAUSTED, grpc.StatusCode.UNAVAILABLE):
                    if attempt < max_retries:
                        if halted is not None:
                            fail(job, halted)
                            continue
                        result.retries += 1
                        retry_job = (g, start, end, attempt + 1, splits)
                        live[job[:2]] = ('retry', retry_job)
                        delay = min(_MAX_BACKOFF_SECONDS, backoff * 2 ** attempt)
                        timer = threading.Timer(delay * random.uniform(0.5, 1.0), events.put, (
                            ('retry', retry_job, data),))
                        timer.daemon = True
                        timers.append(timer)
                        timer.start()
                        continue
                fail(job, error)
        graceful = True
    finally:
        for timer in timers:
            timer.cancel()
        warning = packer.close(graceful)
        if warning:
            logger.warning("price_batch: %s", warning)
            result.warnings.append(warning)
        close_sender(not graceful)
    for group in result.groups:
        group._sort()
    result.wall_seconds = time.perf_counter() - started
    return result


def _numpy():
    """numpy when importable (it is optional), else None."""
    try:
        import numpy
    except ImportError:
        return None
    return numpy


def _is_price_only(response, accessor, count):
    """True for the response of a `price_only` request: no entries, one
    NPV per trade in `npvs`."""
    return (count > 0 and getattr(response, accessor + 'Length')() == 0
            and response.NpvsLength() == count)


def _record(group, start, end, response_bytes):
    """Keep a successful response as a reader; refuse one whose entry count
    (or, for a price-only response, NPV count) does not match the chunk (it
    could not be mapped back to the trades). True when kept."""
    _, response_class, accessor = TRADE_LISTS[group.method]
    response = response_class.GetRootAs(response_bytes, 0)
    count = getattr(response, accessor + 'Length')()
    if count == 0 and response.NpvsLength() > 0:
        count = response.NpvsLength()
    if count != end - start:
        group.failures.append((start, end, QuantraError(
            f"response has {count} entries for {end - start} trades")))
        return False
    group.responses.append((start, end, response))
    return True


def _grpc_sender(target):
    """send(method, data, timeout, metadata, on_done(response, error)) over
    gRPC, and its close(cancel) function (cancels the calls still in flight
    when asked; closes only a channel opened here)."""
    if isinstance(target, Client):
        channel, own = target.channel, False
    elif isinstance(target, grpc.Channel):
        channel, own = target, False
    else:
        channel, own = grpc.insecure_channel(str(target), options=[
            ('grpc.max_receive_message_length', -1),
            ('grpc.max_send_message_length', -1)]), True
    stubs = {method: channel.unary_unary(path) for method, path in Client.METHODS.items()}
    pending = set()
    lock = threading.Lock()

    def send(method, data, timeout, metadata, on_done):
        future = stubs[method].future(data, timeout=timeout,
                                      metadata=tuple(metadata) if metadata else None)
        with lock:
            pending.add(future)

        def done(f):
            with lock:
                pending.discard(f)
            if f.code() == grpc.StatusCode.OK:
                on_done(f.result(), None)
            else:
                on_done(None, QuantraError(f"gRPC error: {f.code()} - {f.details()}",
                                           code=f.code(), details=_error_details(f)))
        future.add_done_callback(done)

    def close(cancel):
        if cancel:
            with lock:
                futures = list(pending)
            for future in futures:
                future.cancel()
        if own:
            channel.close()

    return send, close


def _sync_sender(call, in_flight):
    """The same interface around a blocking call(method, data, timeout)."""
    threads = ThreadPoolExecutor(max_workers=max(1, in_flight))

    def send(method, data, timeout, metadata, on_done):
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

    return send, (lambda cancel: threads.shutdown(wait=False))
