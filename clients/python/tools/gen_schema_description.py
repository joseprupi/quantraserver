#!/usr/bin/env python3
"""Generate clients/python/quantra_client/_schema.py from the FlatBuffers schemas.

The request blocks module (quantra_client/blocks.py) reads and writes request
buffers byte by byte: it needs, for every table, each field's vtable slot and
type, and for every union, the table of each member. flatc compiles every
`flatbuffers/fbs/*.fbs` into one binary schema (`flatc -b --schema`, the
FlatBuffers reflection format); this script reads that binary schema and
writes it out as a compact Python description, together with the schema hash
of scripts/schema_hash.py (the one implementation of that hash).

Run by scripts/generate_schemas.sh after the Python code generation;
tests/contract/request_blocks_test.py regenerates it and fails when the stored
file differs.

Usage: python3 gen_schema_description.py [--check] [--flatc PATH] [OUT]
"""

import argparse
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FBS_DIR = ROOT / "flatbuffers" / "fbs"
DEFAULT_OUT = ROOT / "clients" / "python" / "quantra_client" / "_schema.py"

# reflection.fbs base types
BASE_TYPES = ("None", "UType", "Bool", "Byte", "UByte", "Short", "UShort", "Int", "UInt",
              "Long", "ULong", "Float", "Double", "String", "Vector", "Obj", "Union", "Array",
              "Vector64")
VECTORS = ("Vector", "Vector64", "Array")


def _load_schema_hash():
    spec = importlib.util.spec_from_file_location("schema_hash", ROOT / "scripts" / "schema_hash.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.schema_hash


# --- a minimal reader of the reflection format (reflection.fbs vtable slots) ---

class _Table:
    """Just enough of a FlatBuffers table reader for the binary schema, with
    the standard library only: the generator also runs where the flatbuffers
    Python package is not installed (the engine image build)."""

    def __init__(self, buf, pos):
        self.Bytes = buf
        self.Pos = pos

    def Get(self, fmt, off):
        return struct.unpack_from("<" + fmt, self.Bytes, off)[0]

    def Offset(self, slot):
        vtable = self.Pos - self.Get("i", self.Pos)
        return self.Get("H", vtable + slot) if slot < self.Get("H", vtable) else 0

    def Indirect(self, off):
        return off + self.Get("I", off)

    def String(self, off):
        off += self.Get("I", off)
        return bytes(self.Bytes[off + 4:off + 4 + self.Get("I", off)])

    def Vector(self, off):
        off += self.Pos
        return off + self.Get("I", off) + 4

    def VectorLen(self, off):
        off += self.Pos
        return self.Get("I", off + self.Get("I", off))


def _table(buf, pos):
    return _Table(buf, pos)


def _string(tab, slot):
    o = tab.Offset(slot)
    return tab.String(o + tab.Pos).decode("utf-8") if o else None


def _scalar(tab, slot, flags, default=0):
    o = tab.Offset(slot)
    return tab.Get(flags, o + tab.Pos) if o else default


def _tables(tab, slot):
    o = tab.Offset(slot)
    if not o:
        return []
    start = tab.Vector(o)
    return [_table(tab.Bytes, tab.Indirect(start + 4 * i)) for i in range(tab.VectorLen(o))]


def _sub(tab, slot):
    o = tab.Offset(slot)
    return _table(tab.Bytes, tab.Indirect(o + tab.Pos)) if o else None


def _type(tab):
    return {
        "base": BASE_TYPES[_scalar(tab, 4, "b")],
        "element": BASE_TYPES[_scalar(tab, 6, "b")],
        "index": _scalar(tab, 8, "i", -1),
    }


def read_binary_schema(data):
    """(objects, enums) of a binary schema as plain lists/dicts."""
    buf = bytearray(data)
    root = _table(buf, struct.unpack_from("<I", buf, 0)[0])
    objects = []
    for obj in _tables(root, 4):
        fields = []
        for f in _tables(obj, 6):
            fields.append({
                "name": _string(f, 4),
                "type": _type(_sub(f, 6)),
                "id": _scalar(f, 8, "H"),
                "offset": _scalar(f, 10, "H"),
                "deprecated": bool(_scalar(f, 16, "?")),
            })
        objects.append({
            "name": _string(obj, 4),
            "fields": fields,
            "is_struct": bool(_scalar(obj, 8, "?")),
            "minalign": _scalar(obj, 10, "i"),
            "bytesize": _scalar(obj, 12, "i"),
        })
    enums = []
    for enum in _tables(root, 6):
        values = []
        for v in _tables(enum, 6):
            union_type = _sub(v, 10)
            values.append({
                "name": _string(v, 4),
                "value": _scalar(v, 6, "q"),
                "union_type": _type(union_type) if union_type is not None else None,
            })
        enums.append({
            "name": _string(enum, 4),
            "values": values,
            "is_union": bool(_scalar(enum, 8, "?")),
        })
    return objects, enums


def compile_binary_schema(flatc, fbs_dir=FBS_DIR):
    """Every .fbs of fbs_dir compiled into one binary schema (bytes)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        everything = tmp / "all_schemas.fbs"
        everything.write_text("".join(f'include "{p.name}";\n'
                                      for p in sorted(Path(fbs_dir).glob("*.fbs"))))
        subprocess.run([flatc, "-b", "--schema", "-I", str(fbs_dir), "-o", str(tmp),
                        str(everything)], check=True, capture_output=True, text=True)
        return (tmp / "all_schemas.bfbs").read_bytes()


def describe(objects, enums):
    """The compact description: OBJECTS and UNIONS dicts (see _schema.py)."""
    def ref(t):
        base = t["element"] if t["base"] in VECTORS else t["base"]
        if t["index"] < 0:
            return None
        if base == "Obj":
            return objects[t["index"]]["name"]
        return enums[t["index"]]["name"]  # a union, its type field, or an enum scalar

    out_objects = {}
    for obj in objects:
        fields = []
        for f in sorted(obj["fields"], key=lambda f: f["id"]):
            t = f["type"]
            base = t["base"]
            elem = t["element"] if base in VECTORS else None
            fields.append((f["name"], f["offset"], base, elem, ref(t), f["deprecated"]))
        out_objects[obj["name"]] = (obj["is_struct"], obj["bytesize"], obj["minalign"],
                                    tuple(fields))
    unions = {}
    for enum in enums:
        if not enum["is_union"]:
            continue
        members = {}
        for v in enum["values"]:
            if v["value"] == 0:
                continue
            members[v["value"]] = objects[v["union_type"]["index"]]["name"]
        unions[enum["name"]] = members
    return out_objects, unions


HEADER = '''"""Generated by clients/python/tools/gen_schema_description.py. Do not edit.

The FlatBuffers request schema as the request blocks module needs it
(quantra_client/blocks.py).

SCHEMA_HASH: scripts/schema_hash.py over flatbuffers/fbs/*.fbs; the engine
reports the same value as `schema_hash` in GET /meta and in the Meta reply.

OBJECTS: {table or struct name: (is_struct, bytesize, minalign, fields)},
fields in id order, each (name, vtable slot (table) or byte offset (struct),
base type, vector element type or None, referenced table/struct/union/enum
name or None, deprecated).

UNIONS: {union name: {type value: member table name}}.
"""

'''


def reachable(objects, unions):
    """Only what a request can hold: every table reachable from a *Request table."""
    todo = [name for name in objects if name.endswith("Request")]
    seen = set()
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        for field in objects[name][3]:
            target = field[4]
            if target in objects:
                todo.append(target)
            elif target in unions and field[2] in ("Union", "Vector"):
                todo.extend(unions[target].values())
    used_unions = {f[4] for n in seen for f in objects[n][3] if f[4] in unions}
    return ({n: objects[n] for n in seen}, {n: unions[n] for n in used_unions})


def render(schema_hash, objects, unions):
    lines = [HEADER, f"SCHEMA_HASH = {schema_hash!r}\n\n", "OBJECTS = {\n"]
    for name in sorted(objects):
        is_struct, size, align, fields = objects[name]
        lines.append(f"    {name!r}: ({is_struct!r}, {size!r}, {align!r}, (\n")
        lines.extend(f"        {field!r},\n" for field in fields)
        lines.append("    )),\n")
    lines.append("}\n\nUNIONS = {\n")
    for name in sorted(unions):
        members = ", ".join(f"{k!r}: {v!r}" for k, v in sorted(unions[name].items()))
        lines.append(f"    {name!r}: {{{members}}},\n")
    lines.append("}\n")
    return "".join(lines)


def generate(flatc, fbs_dir=FBS_DIR):
    objects, enums = read_binary_schema(compile_binary_schema(flatc, fbs_dir))
    out_objects, unions = reachable(*describe(objects, enums))
    return render(_load_schema_hash()(fbs_dir), out_objects, unions)


def find_flatc(explicit=None):
    exe = explicit or shutil.which("flatc")
    if not exe:
        prefix = os.environ.get("DEPS_INSTALL_PREFIX", "/opt/quantra-deps")
        candidate = Path(prefix) / "bin" / "flatc"
        exe = str(candidate) if candidate.exists() else None
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out", nargs="?", default=str(DEFAULT_OUT))
    parser.add_argument("--flatc", default=None)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the stored file differs, write nothing")
    args = parser.parse_args()
    flatc = find_flatc(args.flatc)
    if not flatc:
        sys.exit("flatc not found")
    text = generate(flatc)
    out = Path(args.out)
    if args.check:
        if not out.exists() or out.read_text() != text:
            sys.exit(f"{out} is stale; run {Path(__file__).name}")
        return
    out.write_text(text)
    print(f"  wrote {out}")


if __name__ == "__main__":
    main()
