"""Python client request blocks (clients/python/quantra_client/blocks.py), no engine.

- the schema hash and the generated schema description are current;
- every example request of every Client method with a trade list, assembled
  from per-trade blocks (market as a block and as an object), passes the
  structural check and reads back to the same request as the one packed
  normally;
- every market value slot of every example market round-trips: patched
  through its slot, it reads back through the generated classes at its path;
- patching matches rebuilding, byte for byte, 0.0 included;
- price_batch with block groups: splits under the byte limit, splits on a
  size refusal, mixed with object groups, results in input order.
"""

import copy
import importlib.util
import struct
import subprocess
import sys

import pytest

from request_blocks_support import (  # noqa: E402  (sets sys.path)
    ROOT, example_cases, example_request, flatc, get_path, pack, request_fields, set_path,
    trade_attr)

grpc = pytest.importorskip("grpc")

import quantra_client  # noqa: E402
from quantra.Pricing import PricingT  # noqa: E402
from quantra_client import (  # noqa: E402
    SCHEMA_HASH, Block, BlockGroup, assemble_request, market_slots, pack_market_block,
    pack_trade_block, patch_values, verify_request)
from quantra_client.batch import price_batch  # noqa: E402
from quantra_client.blocks import request_type  # noqa: E402

from batch_client_test import (  # noqa: E402
    Code, FakeEngine, FakeRpcError, make_group, trade_ids)

CASES = example_cases()
IDS = [f"{m}:{rel}" for m, rel in CASES]


def _schema_hash_module():
    spec = importlib.util.spec_from_file_location("schema_hash", ROOT / "scripts" / "schema_hash.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# --- schema hash and description --------------------------------------------

def test_schema_hash_is_current():
    assert SCHEMA_HASH == _schema_hash_module().schema_hash(), (
        "quantra_client/_schema.py is stale: run scripts/generate_schemas.sh")


def test_schema_hash_follows_the_schema_files(tmp_path):
    module = _schema_hash_module()
    for path in module.schema_files():
        (tmp_path / path.name).write_bytes(path.read_bytes())
    assert module.schema_hash(tmp_path) == SCHEMA_HASH
    first = sorted(tmp_path.glob("*.fbs"))[0]
    first.write_bytes(first.read_bytes().replace(b"\n", b"\r\n"))
    assert module.schema_hash(tmp_path) == SCHEMA_HASH  # line endings do not count
    first.write_bytes(first.read_bytes() + b"// a change\n")
    assert module.schema_hash(tmp_path) != SCHEMA_HASH


def test_schema_description_is_current():
    if flatc() is None:
        pytest.skip("flatc not found (run the suite inside the test image)")
    tool = ROOT / "clients" / "python" / "tools" / "gen_schema_description.py"
    out = subprocess.run([sys.executable, str(tool), "--check", "--flatc", flatc()],
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr


def test_every_trade_method_has_a_request_type():
    for method, entry in quantra_client.batch.TRADE_LISTS.items():
        if entry is not None:
            root, field, trade = request_type(method)
            assert root.endswith("Request") and trade.startswith("quantra.")


# --- blocks ------------------------------------------------------------------

def test_block_layout():
    _, request = make_group(7, 1)
    block = pack_trade_block(request.swaps[0])
    assert len(block.data) % 8 == 0
    assert block.type_name == "quantra.PriceBasisSwap"
    table = block.table_position
    assert table % 4 == 0
    # the table's vtable is inside the block
    vtable = table - struct.unpack_from("<i", block.data, table)[0]
    assert 0 <= vtable < len(block.data)
    stored = Block(bytes(block.data), block.table_offset)  # as rebuilt from storage
    assert stored.type_name is None and stored.table_position == table
    with pytest.raises(ValueError):
        Block(block.data[1:], block.table_offset)
    with pytest.raises(ValueError):
        Block(block.data, len(block.data) + 4)


def test_assembled_request_is_aligned_and_reads_back():
    method, request = make_group(0, 5)
    blocks = [pack_trade_block(s) for s in request.swaps]
    for n in range(0, 6):  # odd and even trade counts (vector padding)
        out = assemble_request(method, blocks[:n], include_flows=True)
        assert len(out.data) % 8 == 0
        verify_request(method, out.data)
        assert trade_ids(out.data) == list(range(n))
        assert out.pricing_at is None


def test_wrong_block_type_and_unknown_fields_are_refused():
    method, request = make_group(0, 2)
    blocks = [pack_trade_block(s) for s in request.swaps]
    with pytest.raises(ValueError):
        assemble_request("vanilla_swap", blocks)
    with pytest.raises(TypeError):
        assemble_request(method, blocks, not_a_field=1)
    with pytest.raises(TypeError):
        assemble_request(method, blocks, swaps=[])
    with pytest.raises(ValueError):
        assemble_request("bootstrap_curves", blocks)
    market, _ = pack_market_block(PricingT())
    with pytest.raises(ValueError):
        assemble_request(method, blocks, pricing=PricingT(), pricing_block=market)


def test_the_structural_check_finds_a_broken_buffer():
    method, request = make_group(0, 3)
    out = assemble_request(method, [pack_trade_block(s) for s in request.swaps])
    verify_request(method, out.data)
    with pytest.raises(ValueError):
        verify_request(method, out.data[:-8])  # the last block cut short
    broken = bytearray(out.data)
    struct.pack_into("<I", broken, 0, len(broken) + 4)  # root outside the buffer
    with pytest.raises(ValueError):
        verify_request(method, bytes(broken))


@pytest.mark.parametrize("method,rel", CASES, ids=IDS)
def test_example_assembled_from_blocks_reads_back_as_the_request(method, rel):
    request = example_request(method, rel)
    normal = pack(request)
    verify_request(method, normal)
    trades = [pack_trade_block(t) for t in getattr(request, trade_attr(method))]
    fields = request_fields(method, request)
    market, slots = pack_market_block(request.pricing)
    cls = type(request)
    for kwargs in ({"pricing_block": market}, {"pricing": request.pricing}):
        out = assemble_request(method, trades, **kwargs, **fields)
        assert len(out.data) % 8 == 0 and out.pricing_at % 4 == 0
        verify_request(method, out.data)
        # the same request: unpacked and packed again, the bytes are the
        # normal packing's
        assert pack(cls.InitFromPackedBuf(out.data, 0)) == normal
        # slots read at pricing_at name the same values
        assert set(market_slots(out.data, out.pricing_at)) == set(slots)


# --- market slots ------------------------------------------------------------

def _distinct(kind, k):
    if kind in "fd":
        return 1000.0 + k + 0.25  # exact in float32 for the counts here
    if kind in "bB":
        return 1 + k % 100
    return 7 + k % 30000


@pytest.mark.parametrize("method,rel", CASES, ids=IDS)
def test_every_slot_round_trips_through_the_generated_classes(method, rel):
    request = example_request(method, rel)
    market, slots = pack_market_block(request.pricing)
    positions = [market.table_position + s.offset for s in slots.values()]
    assert len(set(positions)) == len(positions), "two paths share a value"
    # every path names a value the generated classes read at that path
    original = PricingT.InitFromBuf(market.data, market.table_position)
    for path in slots:
        value = get_path(original, path)
        assert isinstance(value, (int, float)) and not isinstance(value, bool), path
    values = {path: _distinct(slot.kind, k) for k, (path, slot) in enumerate(sorted(slots.items()))}
    patched = patch_values(bytearray(market.data), market.table_position, slots, values)
    read = PricingT.InitFromBuf(bytes(patched), market.table_position)
    for path, value in values.items():
        assert get_path(read, path) == value, path
    # and nothing else moved: set the same values on the object, pack it
    expected = copy.deepcopy(request.pricing)
    for path, value in values.items():
        set_path(expected, path, value)
    assert bytes(patched) == pack_market_block(expected)[0].data


def test_examples_cover_every_market_kind():
    """The round trip above runs over curves, vols, credit, inflation,
    equity, commodity, FX and quotes."""
    seen = set()
    for method, rel in CASES:
        _, slots = pack_market_block(example_request(method, rel).pricing)
        seen |= {path.split(".", 1)[0].split("[", 1)[0] for path in slots}
    assert {"rates", "volatility", "credit", "inflation", "equity", "commodity", "fx"} <= seen, seen


def _a_curve_example():
    for method, rel in CASES:
        request = example_request(method, rel)
        _, slots = pack_market_block(request.pricing)
        rates = [p for p, s in slots.items() if p.startswith("rates.curves[") and s.kind == "d"]
        if len(rates) >= 3:
            return method, request, rates
    raise AssertionError("no example with three curve values")  # pragma: no cover


def test_patch_equals_rebuild_including_zero():
    method, request, paths = _a_curve_example()
    trades = [pack_trade_block(t) for t in getattr(request, trade_attr(method))]
    fields = request_fields(method, request)
    market, slots = pack_market_block(request.pricing)
    base = assemble_request(method, trades, pricing_block=market, **fields)
    for change in ({paths[0]: 0.0},
                   {paths[0]: get_path(request.pricing, paths[0]) + 1e-4},
                   {p: get_path(request.pricing, p) * 1.01 for p in paths[:3]}):
        rebuilt = copy.deepcopy(request.pricing)
        for path, value in change.items():
            set_path(rebuilt, path, value)
        expected = assemble_request(method, trades, pricing_block=pack_market_block(rebuilt)[0],
                                    **fields)
        assert patch_values(base.data, base.pricing_at, slots, change) == expected.data
        # in place on a bytearray, then back
        buffer = bytearray(base.data)
        assert patch_values(buffer, base.pricing_at, slots, change) is buffer
        assert bytes(buffer) == expected.data
        back = {p: get_path(request.pricing, p) for p in change}
        assert bytes(patch_values(buffer, base.pricing_at, slots, back)) == base.data


def test_patch_values_checks_paths_and_types():
    method, request, paths = _a_curve_example()
    market, slots = pack_market_block(request.pricing)
    at = market.table_position
    with pytest.raises(KeyError, match="did you mean"):
        patch_values(market.data, at, slots, {paths[0] + "x": 1.0})
    with pytest.raises(TypeError):
        patch_values(market.data, at, slots, {paths[0]: "1.0"})
    with pytest.raises(TypeError):
        patch_values(market.data, at, slots, {paths[0]: True})
    ints = [p for p, s in slots.items() if s.kind == "i"]
    if ints:
        with pytest.raises(TypeError):
            patch_values(market.data, at, slots, {ints[0]: 1.5})
        with pytest.raises(ValueError):
            patch_values(market.data, at, slots, {ints[0]: 2 ** 40})
    # a refused value writes nothing, not even the valid ones before it
    buffer = bytearray(market.data)
    with pytest.raises(TypeError):
        patch_values(buffer, at, slots, {paths[0]: 0.0, paths[1]: None})
    assert bytes(buffer) == market.data


# --- price_batch with block groups -------------------------------------------

def _block_group(first_id, n, **kwargs):
    method, request = make_group(first_id, n)
    return method, BlockGroup([pack_trade_block(s) for s in request.swaps], **kwargs)


def _assert_block_results(result, groups):
    """Every trade's NPV is its id (the fake engine answers the id back)."""
    for group, (method, request) in zip(result.groups, groups):
        if isinstance(request, BlockGroup):
            expected = [trade_ids(assemble_request(method, [b]).data)[0] for b in request.blocks]
        else:
            expected = [int(s.basisSwap.leg1.notional) for s in request.swaps]
        assert [group.entry(i).Npv() for i in range(len(group))] == expected


def test_block_groups_split_under_the_byte_limit():
    groups = [_block_group(0, 40)]
    engine = FakeEngine()
    limit = 3000
    result = price_batch(None, groups, call=engine, processes=0, max_message_bytes=limit)
    assert result.ok and len(engine.log) > 1
    assert all(size <= limit for _, size in engine.log)
    _assert_block_results(result, groups)
    # one block alone above the limit fails alone
    method, group = groups[0]
    alone = len(assemble_request(method, group.blocks[:1]).data)
    result = price_batch(None, groups, call=FakeEngine(), processes=0, max_message_bytes=alone - 1)
    assert not result.ok and result.groups[0].error(0) is not None


def test_block_groups_split_on_a_size_refusal_and_retry():
    def refuse(ids, attempt):
        if len(ids) > 6:
            return FakeRpcError(Code.RESOURCE_EXHAUSTED, "Received message larger than max")
        if attempt == 1 and ids[0] == 0:
            return FakeRpcError(Code.UNAVAILABLE, "try again")
        return None
    groups = [_block_group(0, 25)]
    engine = FakeEngine(refuse)
    result = price_batch(None, groups, call=engine, processes=0, backoff=0.001)
    assert result.ok and result.splits >= 2 and result.retries >= 1
    _assert_block_results(result, groups)


def test_mixed_batches_of_object_and_block_groups():
    groups = [make_group(0, 300), _block_group(1000, 260), make_group(2000, 5),
              _block_group(3000, 3)]
    engine = FakeEngine()
    result = price_batch(None, groups, call=engine, processes=0, in_flight=4, max_trades=70)
    assert result.ok
    assert all(len(ids) <= 70 for ids, _ in engine.log)
    _assert_block_results(result, groups)


def test_block_groups_with_a_market():
    pricing = example_request(*CASES[0]).pricing
    market, _ = pack_market_block(pricing)
    groups = [_block_group(0, 9, pricing_block=market, include_flows=True),
              _block_group(100, 4, pricing=pricing)]
    seen = []

    def engine(method, data, timeout):
        verify_request(method, data)
        seen.append(data)
        return FakeEngine()(method, data, timeout)
    result = price_batch(None, groups, call=engine, processes=0)
    assert result.ok and len(seen) == 2
    _assert_block_results(result, groups)
