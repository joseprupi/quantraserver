"""
Quantra request blocks

Builds pricing requests from trades and markets packed once and kept as bytes,
and changes market values inside a packed request without packing it again.

A block is one table (a trade, or a market) packed alone into a FlatBuffers
byte string that does not depend on where it is placed:

    trades = [pack_trade_block(swap) for swap in request.swaps]  # once, storable
    market, slots = pack_market_block(request.pricing)             # once per market
    req = assemble_request("basis_swap", trades, pricing_block=market,
                           include_flows=False)                     # per request
    bumped = patch_values(req.data, req.pricing_at, slots,
                          {"rates.curves[EUR_6M].points[3].point.rate": 0.0251})

The assembled bytes are a finished request for the Client method `method`
(the same method names as Client.METHODS and price_batch). price_batch also
takes block groups: `("basis_swap", BlockGroup(trades, pricing_block=market))`.

Block layout (what to store): `data` is a byte string whose length is a
multiple of 8; `table_offset` is the position of the block's table counted
back from the END of `data` (the table starts at len(data) - table_offset).
Everything the table refers to (its vtables, strings, vectors, sub-tables) is
inside `data`, all references are relative, and every value is aligned
relative to the end of `data`: placed anywhere whose end is 8-aligned, the
block reads the same. Leading bytes before the packed content are zero
padding. `type_name` is the table type ("quantra.PriceBasisSwap"); it is not
part of the bytes and may be None for a block rebuilt from storage.

A stored block is valid only for the schema it was packed with: keep
`SCHEMA_HASH` next to it and repack when it changes (the engine reports its
own value as `schema_hash` in GET /meta and in the Meta reply).

Market values: `pack_market_block` returns, next to the market block, the
position of every numeric value of the market (doubles, floats and integers
that are not enums), addressed by a path built from the schema:

    field names joined by '.'; a vector element as name[k] (index k), or as
    name[ID] when every element of that vector is a table with a non-empty,
    unique string field `id` made of characters other than '.', '[' and ']'
    and not all digits; a union by its field name (the member type is the one
    in the buffer); a vector of numbers as name[k].

    e.g. "rates.curves[EUR_OIS].points[3].point.rate",
         "quotes[USD_SOFR_5Y].value", "volatility.vol_surfaces[EQ].payload.vols[12]"

Only values present in the buffer have a slot: an optional scalar left unset
(`= null` in the schema) has none. Strings, booleans, enums and anything
whose change alters the length of the buffer (a pillar added, a fixing
appended, another union member) cannot be patched; pack the market again.
"""

import difflib
import importlib
import numbers
import re
import struct
import time
from dataclasses import dataclass
from typing import Dict, Iterable, List, Mapping, NamedTuple, Optional, Sequence, Tuple, Union

import flatbuffers

from . import _schema
from .batch import TRADE_LISTS
from .client import Client

SCHEMA_HASH = _schema.SCHEMA_HASH

__all__ = [
    'SCHEMA_HASH', 'Block', 'AssembledRequest', 'Slot', 'BlockGroup',
    'pack_trade_block', 'pack_market_block', 'assemble_request', 'market_slots',
    'patch_values', 'request_type', 'verify_request',
]

_OBJECTS = _schema.OBJECTS
_UNIONS = _schema.UNIONS
_PRICING = 'quantra.Pricing'
_ALIGN = 8

# struct format and size per FlatBuffers scalar base type
_SCALARS = {
    'Bool': ('?', 1), 'Byte': ('b', 1), 'UByte': ('B', 1), 'UType': ('B', 1),
    'Short': ('h', 2), 'UShort': ('H', 2), 'Int': ('i', 4), 'UInt': ('I', 4),
    'Long': ('q', 8), 'ULong': ('Q', 8), 'Float': ('f', 4), 'Double': ('d', 8),
}
_U32 = struct.Struct('<I')
_I32 = struct.Struct('<i')
_U16 = struct.Struct('<H')

if any(obj[2] > _ALIGN for obj in _OBJECTS.values()):  # pragma: no cover - schema guard
    raise ImportError("quantra_client.blocks: a schema type needs an alignment above 8 bytes")


# =============================================================================
# Types
# =============================================================================

@dataclass(frozen=True)
class Block:
    """One table packed alone. `data`: the bytes (length a multiple of 8);
    `table_offset`: the table's position counted back from the end of
    `data`; `type_name`: the table type, None when unknown."""

    data: bytes
    table_offset: int
    type_name: Optional[str] = None

    def __post_init__(self):
        if not isinstance(self.data, bytes):
            object.__setattr__(self, 'data', bytes(self.data))
        if len(self.data) % _ALIGN:
            raise ValueError(f"block length {len(self.data)} is not a multiple of {_ALIGN}")
        if not 4 <= self.table_offset <= len(self.data) or self.table_offset % 4:
            raise ValueError(f"table_offset {self.table_offset} outside the block "
                             f"({len(self.data)} bytes) or not 4-aligned")

    @property
    def table_position(self) -> int:
        """The table's position from the start of `data`."""
        return len(self.data) - self.table_offset

    def __len__(self):
        return len(self.data)


class AssembledRequest(NamedTuple):
    """A finished request: `data` (bytes) and `pricing_at`, the position of
    its Pricing table (None when the request has no market)."""

    data: bytes
    pricing_at: Optional[int]


class Slot(NamedTuple):
    """Where a market value lives: `offset` from the Pricing table's position,
    `kind` its struct format ('d' double, 'f' float, 'b'/'B'/'h'/'H'/'i'/'I'/
    'q'/'Q' integers, little-endian)."""

    offset: int
    kind: str


# =============================================================================
# Packing
# =============================================================================

def _type_name(obj) -> Optional[str]:
    name = type(obj).__name__
    if name.endswith('T') and 'quantra.' + name[:-1] in _OBJECTS:
        return 'quantra.' + name[:-1]
    return None


def _pack_alone(obj, force_defaults) -> Block:
    builder = flatbuffers.Builder(1024)
    builder.ForceDefaults(force_defaults)
    builder.Finish(obj.Pack(builder))
    out = bytes(builder.Output())
    root = _U32.unpack_from(out, 0)[0]
    body = out[4:]  # the packed content, after Finish's alignment padding
    pad = -len(body) % _ALIGN
    return Block(bytes(pad) + body, len(out) - root, _type_name(obj))


def pack_trade_block(trade_obj, force_defaults: bool = True) -> Block:
    """One trade packed alone: the element object of a request's trade list
    (e.g. a `PriceBasisSwapT`, the object a Client method's request holds in
    its `swaps`). With `force_defaults` (default) a scalar equal to its schema
    default is written too, as price_batch packs."""
    return _pack_alone(trade_obj, force_defaults)


def pack_market_block(pricing_obj) -> Tuple[Block, Dict[str, Slot]]:
    """The market (a `PricingT`) packed alone with ForceDefaults, and the slot
    of every numeric market value in it ({path: Slot}, positions relative to
    the Pricing table; see the module help for the paths)."""
    if _type_name(pricing_obj) != _PRICING:
        raise TypeError(f"pack_market_block takes a PricingT, not {type(pricing_obj).__name__}")
    block = _pack_alone(pricing_obj, True)
    return block, market_slots(block.data, block.table_position)


# =============================================================================
# Assembly
# =============================================================================

def _snake_to_attr(name):
    head, *rest = name.split('_')
    return head + ''.join(part[:1].upper() + part[1:] for part in rest)


def request_type(method: str) -> Tuple[str, str, str]:
    """(root table, trade-list field, trade table) of a Client method, e.g.
    ('quantra.PriceBasisSwapRequest', 'swaps', 'quantra.PriceBasisSwap')."""
    if method not in Client.METHODS:
        raise ValueError(f"unknown method {method!r}; expected one of Client.METHODS")
    if TRADE_LISTS.get(method) is None:
        raise ValueError(f"{method!r} has no trade list")
    root = 'quantra.' + Client.METHODS[method].rsplit('/', 1)[1] + 'Request'
    attr = TRADE_LISTS[method][0]
    for field in _OBJECTS[root][3]:
        if _snake_to_attr(field[0]) == attr and field[2] == 'Vector' and field[3] == 'Obj':
            return root, field[0], field[4]
    raise ValueError(f"{root} has no trade list {attr!r}")  # pragma: no cover


def _field(type_name, field_name):
    for field in _OBJECTS[type_name][3]:
        if field[0] == field_name:
            return field
    raise KeyError(f"{type_name} has no field {field_name!r}")


def _field_position(buf, table, voffset):
    """Absolute position of a table field, 0 when absent."""
    vtable = table - _I32.unpack_from(buf, table)[0]
    if voffset >= _U16.unpack_from(buf, vtable)[0]:
        return 0
    offset = _U16.unpack_from(buf, vtable + voffset)[0]
    return table + offset if offset else 0


class _Assembler:
    """The part of a request that does not depend on the trades (the root
    table, its scalar fields and, when given as an object, the market),
    packed once; `assemble` appends the trade vector and the blocks."""

    def __init__(self, method, pricing=None, pricing_block=None, request_fields=None):
        if pricing is not None and pricing_block is not None:
            raise ValueError("give pricing or pricing_block, not both")
        if pricing_block is not None:
            if not isinstance(pricing_block, Block):
                raise TypeError("pricing_block must be a Block (from pack_market_block)")
            if pricing_block.type_name not in (None, _PRICING):
                raise ValueError(f"pricing_block holds a {pricing_block.type_name}, not a Pricing")
        self.method = method
        self.root, self.trades_field, self.trade_type = request_type(method)
        self.pricing_block = pricing_block
        rpc = self.root.split('.', 1)[1]
        root_class = getattr(importlib.import_module(f'quantra.{rpc}'), f'{rpc}T')
        request = root_class()
        names = {f[0] for f in _OBJECTS[self.root][3] if not f[5]}
        for name, value in (request_fields or {}).items():
            attr = name if name not in names else _snake_to_attr(name)
            schema_name = name if name in names else next(
                (n for n in names if _snake_to_attr(n) == name), None)
            if schema_name is None or not hasattr(request, attr):
                raise TypeError(f"{self.root} has no field {name!r}")
            if schema_name in (self.trades_field, 'pricing'):
                raise TypeError(f"{name!r} is set by assemble_request, not as a request field")
            setattr(request, attr, value)
        setattr(request, _snake_to_attr(self.trades_field), [])
        has_pricing = any(f[0] == 'pricing' for f in _OBJECTS[self.root][3])
        if pricing_block is not None:
            # A placeholder market keeps the root's pricing field; it is
            # pointed at the block below and the placeholder is left unused.
            request.pricing = importlib.import_module('quantra.Pricing').PricingT()
        elif has_pricing:
            request.pricing = pricing
        builder = flatbuffers.Builder(1024)
        builder.ForceDefaults(True)
        builder.Finish(request.Pack(builder))
        self.head = bytes(builder.Output())
        table = _U32.unpack_from(self.head, 0)[0]
        self.trades_slot = _field_position(self.head, table, _field(self.root, self.trades_field)[1])
        if not self.trades_slot:  # pragma: no cover - an empty vector is always written
            raise RuntimeError("the packed root has no trade list field")
        self.pricing_slot = (_field_position(self.head, table, _field(self.root, 'pricing')[1])
                             if has_pricing else 0)
        self.pricing_at = None
        if self.pricing_slot and pricing_block is None:
            self.pricing_at = self.pricing_slot + _U32.unpack_from(self.head, self.pricing_slot)[0]

    def _layout(self, count):
        """(position of the market block or None, position of the trade
        vector, position of the first block) for `count` trades."""
        pos = len(self.head)
        market = None
        if self.pricing_block is not None:
            pos += -pos % _ALIGN
            market = pos
            pos += len(self.pricing_block.data)
        vector = pos
        first = vector + 4 + 4 * count
        first += -first % _ALIGN
        return market, vector, first

    def size(self, count, block_bytes):
        """Bytes of a request of `count` trades whose blocks total `block_bytes`."""
        return self._layout(count)[2] + block_bytes

    def check(self, blocks):
        for i, block in enumerate(blocks):
            if not isinstance(block, Block):
                raise TypeError(f"blocks[{i}] is a {type(block).__name__}, not a Block")
            if block.type_name is not None and block.type_name != self.trade_type:
                raise ValueError(f"blocks[{i}] holds a {block.type_name}; {self.method!r} "
                                 f"takes {self.trade_type}")

    def assemble(self, blocks: Sequence[Block]) -> AssembledRequest:
        count = len(blocks)
        market, vector, first = self._layout(count)
        head = bytearray(self.head)
        _U32.pack_into(head, self.trades_slot, vector - self.trades_slot)
        parts = [head]
        pricing_at = self.pricing_at
        if market is not None:
            parts.append(bytes(market - len(self.head)))
            parts.append(self.pricing_block.data)
            pricing_at = market + self.pricing_block.table_position
            if self.pricing_slot:
                _U32.pack_into(head, self.pricing_slot, pricing_at - self.pricing_slot)
        offsets = [0] * count
        pos = first
        entry = vector + 4
        for i, block in enumerate(blocks):
            data = block.data
            offsets[i] = pos + len(data) - block.table_offset - entry
            pos += len(data)
            entry += 4
        parts.append(struct.pack(f'<I{count}I', count, *offsets))
        parts.append(bytes(first - vector - 4 - 4 * count))
        parts.extend(block.data for block in blocks)
        return AssembledRequest(b''.join(parts), pricing_at)


def assemble_request(method: str, blocks: Sequence[Block], pricing=None,
                     pricing_block: Optional[Block] = None, **request_fields) -> AssembledRequest:
    """
    A finished request for Client method `method` (e.g. 'basis_swap') whose
    trade list is `blocks`, in that order.

    pricing: the market as a `PricingT`, packed here; or
    pricing_block: the market as a Block from pack_market_block, copied in.
    request_fields: the request's other fields by schema or object name, e.g.
        include_flows=True.

    Returns AssembledRequest(data, pricing_at). Send `data` with any gRPC
    client (it is what Client methods send) or price_batch. With
    `pricing_block`, the slots of pack_market_block patch the request at
    `pricing_at`; with `pricing`, use market_slots(data, pricing_at).
    """
    assembler = _Assembler(method, pricing, pricing_block, request_fields)
    blocks = list(blocks)
    assembler.check(blocks)
    return assembler.assemble(blocks)


class BlockGroup:
    """
    A group of price_batch given as blocks: `(method, BlockGroup(blocks, ...))`
    in place of `(method, request)`. The arguments are those of
    assemble_request. The batch assembles its requests from the blocks on the
    calling thread (no packing processes), cut by trade count as for any
    group and by `max_message_bytes` from the block sizes.
    """

    def __init__(self, blocks: Iterable[Block], pricing=None,
                 pricing_block: Optional[Block] = None, **request_fields):
        if pricing is not None and pricing_block is not None:
            raise ValueError("give pricing or pricing_block, not both")
        self.blocks = list(blocks)
        self.pricing = pricing
        self.pricing_block = pricing_block
        self.request_fields = request_fields
        self._assemblers = {}

    def __len__(self):
        return len(self.blocks)

    def _assembler(self, method):
        if method not in self._assemblers:
            assembler = _Assembler(method, self.pricing, self.pricing_block, self.request_fields)
            assembler.check(self.blocks)
            self._assemblers[method] = assembler
        return self._assemblers[method]


class _BlockChunks:
    """price_batch's packing of one BlockGroup: ranges of trades assembled
    under a byte limit, sized from the block lengths before any copy."""

    def __init__(self, method, group: BlockGroup):
        self.assembler = group._assembler(method)
        self.blocks = group.blocks
        self.cumulative = [0]
        for block in self.blocks:
            self.cumulative.append(self.cumulative[-1] + len(block.data))

    def size(self, start, end):
        return self.assembler.size(end - start, self.cumulative[end] - self.cumulative[start])

    def ranges(self, start, end, max_bytes):
        """[(start, end, fits)] covering start..end: a range above max_bytes
        is cut evenly until each fits; a single trade that does not fit
        comes back with fits False."""
        out = []

        def cut(s, e):
            size = self.size(s, e)
            if size <= max_bytes:
                out.append((s, e, True))
            elif e - s == 1:
                out.append((s, e, False))
            else:
                parts = max(2, min(e - s, -(-size * 11 // (max_bytes * 10))))
                for k in range(parts):
                    ks, ke = s + k * (e - s) // parts, s + (k + 1) * (e - s) // parts
                    if ke > ks:
                        cut(ks, ke)

        cut(start, end)
        return out

    def pack(self, start, end, max_bytes):
        """The same result as the object packer: ([(start, end, data or
        None)], seconds)."""
        t0 = time.perf_counter()
        chunks = [(s, e, self.assembler.assemble(self.blocks[s:e]).data if fits else None)
                  for s, e, fits in self.ranges(start, end, max_bytes)]
        return chunks, time.perf_counter() - t0


# =============================================================================
# Market slots and patching
# =============================================================================

_KEY_OK = re.compile(r'^(?!\d+$)[^.\[\]]+$')


def _read_string(buf, pos):
    target = pos + _U32.unpack_from(buf, pos)[0]
    n = _U32.unpack_from(buf, target)[0]
    return bytes(buf[target + 4:target + 4 + n]).decode('utf-8', 'replace')


def _table_keys(buf, tables, type_name):
    """The `id` of every table of a vector when they make unique keys, else
    the indexes."""
    id_field = next((f for f in _OBJECTS[type_name][3] if f[0] == 'id' and f[2] == 'String'), None)
    if id_field is not None:
        keys = []
        for table in tables:
            pos = _field_position(buf, table, id_field[1])
            keys.append(_read_string(buf, pos) if pos else None)
        if all(k and _KEY_OK.match(k) for k in keys) and len(set(keys)) == len(keys):
            return keys
    return [str(i) for i in range(len(tables))]


def _union_type(buf, table, fields, union_field):
    name = union_field[0] + '_type'
    for field in fields:
        if field[0] == name:
            pos = _field_position(buf, table, field[1])
            return buf[pos] if pos else 0
    return 0  # pragma: no cover - flatc always declares the type field


def _numeric(base, target):
    return base in _SCALARS and base not in ('Bool', 'UType') and target is None


def _walk_struct(buf, pos, type_name, path, base, out):
    for name, offset, kind, _, target, _ in _OBJECTS[type_name][3]:
        at = pos + offset
        if kind == 'Obj':
            _walk_struct(buf, at, target, f"{path}.{name}", base, out)
        elif _numeric(kind, target):
            out[f"{path}.{name}"] = Slot(at - base, _SCALARS[kind][0])


def _walk_table(buf, table, type_name, path, base, out):
    fields = _OBJECTS[type_name][3]
    prefix = path + '.' if path else ''
    for field in fields:
        name, voffset, kind, element, target, deprecated = field
        if deprecated:
            continue
        pos = _field_position(buf, table, voffset)
        if not pos:
            continue
        here = prefix + name
        if _numeric(kind, target):
            out[here] = Slot(pos - base, _SCALARS[kind][0])
        elif kind == 'Obj':
            if _OBJECTS[target][0]:
                _walk_struct(buf, pos, target, here, base, out)
            else:
                _walk_table(buf, pos + _U32.unpack_from(buf, pos)[0], target, here, base, out)
        elif kind == 'Union':
            member = _UNIONS[target].get(_union_type(buf, table, fields, field))
            if member is not None:
                _walk_table(buf, pos + _U32.unpack_from(buf, pos)[0], member, here, base, out)
        elif kind == 'Vector':
            vector = pos + _U32.unpack_from(buf, pos)[0]
            count = _U32.unpack_from(buf, vector)[0]
            first = vector + 4
            if _numeric(element, target):
                fmt, size = _SCALARS[element]
                for i in range(count):
                    out[f"{here}[{i}]"] = Slot(first + i * size - base, fmt)
            elif element == 'Obj' and _OBJECTS[target][0]:
                size = _OBJECTS[target][1]
                for i in range(count):
                    _walk_struct(buf, first + i * size, target, f"{here}[{i}]", base, out)
            elif element == 'Obj':
                tables = [first + 4 * i + _U32.unpack_from(buf, first + 4 * i)[0]
                          for i in range(count)]
                for key, sub in zip(_table_keys(buf, tables, target), tables):
                    _walk_table(buf, sub, target, f"{here}[{key}]", base, out)
            elif element == 'Union':
                type_field = next((f for f in fields if f[0] == name + '_type'), None)
                types_pos = _field_position(buf, table, type_field[1]) if type_field else 0
                if not types_pos:
                    continue
                types = types_pos + _U32.unpack_from(buf, types_pos)[0] + 4
                for i in range(count):
                    member = _UNIONS[target].get(buf[types + i])
                    if member is not None:
                        sub = first + 4 * i + _U32.unpack_from(buf, first + 4 * i)[0]
                        _walk_table(buf, sub, member, f"{here}[{i}]", base, out)


def market_slots(buffer, pricing_at: int) -> Dict[str, Slot]:
    """The slot of every numeric value of the Pricing table at `pricing_at`
    in `buffer` (any finished request or a market block's data), positions
    relative to `pricing_at`. Slots belong to that buffer's layout: those of
    a market block apply to every request assembled from that block."""
    out = {}
    _walk_table(buffer, pricing_at, _PRICING, '', pricing_at, out)
    return out


def patch_values(buffer: Union[bytes, bytearray], pricing_at: int, slots: Mapping[str, Slot],
                 values: Mapping[str, Union[int, float]]) -> Union[bytes, bytearray]:
    """
    Overwrite market values of a packed request (or market block) in place.

    buffer: the request bytes; a bytearray is changed in place and returned,
        bytes give new bytes.
    pricing_at: the Pricing table's position (AssembledRequest.pricing_at, or
        Block.table_position for a market block).
    slots: from pack_market_block (requests assembled from that block) or
        market_slots.
    values: {path: value}. A float slot takes any real number (0.0
        included), an integer slot an int within its width. Every value is
        checked before anything is written.
    """
    target = buffer if isinstance(buffer, bytearray) else bytearray(buffer)
    writes = []
    for path, value in values.items():
        slot = slots.get(path)
        if slot is None:
            close = difflib.get_close_matches(path, list(slots), n=3)
            hint = f"; did you mean {', '.join(close)}?" if close else ""
            raise KeyError(f"no market value at {path!r}{hint}")
        if isinstance(value, bool) or not isinstance(value, numbers.Real):
            raise TypeError(f"{path}: {value!r} is not a number")
        if slot.kind in 'fd':
            value = float(value)
        elif not isinstance(value, numbers.Integral):
            raise TypeError(f"{path} holds an integer, not {value!r}")
        fmt = '<' + slot.kind
        try:
            data = struct.pack(fmt, value)
        except (struct.error, OverflowError) as exc:
            raise ValueError(f"{path}: {value!r} does not fit a '{slot.kind}' value ({exc})")
        at = pricing_at + slot.offset
        if not 0 <= at <= len(target) - len(data):
            raise ValueError(f"{path}: slot at {at} is outside the buffer ({len(target)} bytes)")
        writes.append((at, data))
    for at, data in writes:
        target[at:at + len(data)] = data
    return target if isinstance(buffer, bytearray) else bytes(target)


# =============================================================================
# Structural check (what the engine's FlatBuffers verifier checks, in Python)
# =============================================================================

class _Verifier:
    def __init__(self, buf, max_depth=64, max_tables=1_000_000):
        self.buf = buf
        self.size = len(buf)
        self.max_depth = max_depth
        self.tables_left = max_tables

    def fail(self, path, what):
        raise ValueError(f"{path or 'root'}: {what}")

    def within(self, path, pos, length, align=1):
        if pos < 0 or pos + length > self.size:
            self.fail(path, f"{length} bytes at {pos} run outside the buffer ({self.size} bytes)")
        if pos % align:
            self.fail(path, f"position {pos} is not {align}-aligned")

    def offset(self, path, pos):
        self.within(path, pos, 4, 4)
        target = pos + _U32.unpack_from(self.buf, pos)[0]
        if _U32.unpack_from(self.buf, pos)[0] == 0:
            self.fail(path, "zero offset")
        return target

    def string(self, path, pos):
        target = self.offset(path, pos)
        self.within(path, target, 4, 4)
        n = _U32.unpack_from(self.buf, target)[0]
        self.within(path, target + 4, n + 1)
        if self.buf[target + 4 + n] != 0:
            self.fail(path, "string not zero-terminated")

    def table(self, path, table, type_name, depth):
        if depth > self.max_depth:
            self.fail(path, "nested too deep")
        self.tables_left -= 1
        if self.tables_left < 0:
            self.fail(path, "too many tables")
        self.within(path, table, 4, 4)
        vtable = table - _I32.unpack_from(self.buf, table)[0]
        self.within(path, vtable, 4, 2)
        vsize, tsize = _U16.unpack_from(self.buf, vtable)[0], _U16.unpack_from(self.buf, vtable + 2)[0]
        if vsize < 4 or vsize % 2:
            self.fail(path, f"vtable size {vsize}")
        self.within(path, vtable, vsize)
        self.within(path, table, tsize)
        fields = _OBJECTS[type_name][3]
        prefix = path + '.' if path else ''
        for field in fields:
            name, voffset, kind, element, target, _ = field
            if voffset >= vsize:
                continue
            fo = _U16.unpack_from(self.buf, vtable + voffset)[0]
            if not fo:
                continue
            pos = table + fo
            here = prefix + name
            if kind in _SCALARS:
                size = _SCALARS[kind][1]
                self.within(here, pos, size, size)
            elif kind == 'Obj' and _OBJECTS[target][0]:
                size, align = _OBJECTS[target][1], _OBJECTS[target][2]
                self.within(here, pos, size, align)
            else:
                if kind == 'String':
                    self.string(here, pos)
                elif kind == 'Obj':
                    self.table(here, self.offset(here, pos), target, depth + 1)
                elif kind == 'Union':
                    member = _UNIONS[target].get(_union_type(self.buf, table, fields, field))
                    if member is None:
                        self.fail(here, "union value without a known type")
                    self.table(here, self.offset(here, pos), member, depth + 1)
                elif kind == 'Vector':
                    self.vector(here, pos, element, target, table, fields, field, depth)

    def vector(self, path, pos, element, target, table, fields, field, depth):
        vector = self.offset(path, pos)
        self.within(path, vector, 4, 4)
        count = _U32.unpack_from(self.buf, vector)[0]
        first = vector + 4
        if element in _SCALARS:
            size = _SCALARS[element][1]
            self.within(path, first, count * size, size)
        elif element == 'Obj' and _OBJECTS[target][0]:
            self.within(path, first, count * _OBJECTS[target][1], _OBJECTS[target][2])
        else:
            self.within(path, first, count * 4, 4)
            for i in range(count):
                here = f"{path}[{i}]"
                if element == 'String':
                    self.string(here, first + 4 * i)
                elif element == 'Obj':
                    self.table(here, self.offset(here, first + 4 * i), target, depth + 1)
                elif element == 'Union':
                    type_field = next((f for f in fields if f[0] == field[0] + '_type'), None)
                    types_pos = (_field_position(self.buf, table, type_field[1])
                                 if type_field else 0)
                    if not types_pos:
                        self.fail(here, "union vector without its type vector")
                    types = types_pos + _U32.unpack_from(self.buf, types_pos)[0] + 4
                    member = _UNIONS[target].get(self.buf[types + i])
                    if member is not None:
                        self.table(here, self.offset(here, first + 4 * i), member, depth + 1)


def verify_request(method: str, data: bytes) -> None:
    """Check a request buffer as the engine's FlatBuffers verifier does
    (every reachable table, vtable, string and vector inside the buffer and
    aligned, union members known, nesting and table count bounded); raises
    ValueError naming the first problem. Required fields are not checked
    (the engine refuses those with a message of its own)."""
    root = 'quantra.' + Client.METHODS[method].rsplit('/', 1)[1] + 'Request' \
        if method in Client.METHODS else None
    if root not in _OBJECTS:
        raise ValueError(f"unknown method {method!r}")
    if len(data) < 8:
        raise ValueError(f"{len(data)} bytes is too short for a request")
    verifier = _Verifier(data)
    verifier.table('', verifier.offset('', 0), root, 0)
