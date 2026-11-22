"""Helpers shared by the request blocks tests (request_blocks_test.py and
request_blocks_engine_test.py): the example requests of every Client method
with a trade list, read into Object API requests, and an independent reader of
market paths on Object API objects."""

import importlib
import re
import shutil
import subprocess
import sys
import tempfile
from functools import lru_cache
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FBS_DIR = ROOT / "flatbuffers" / "fbs"
DATA = ROOT / "examples" / "data"
for _path in (ROOT / "flatbuffers" / "python", ROOT / "clients" / "python", ROOT / "tests" / "functional"):
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

flatbuffers = pytest.importorskip("flatbuffers")
pytest.importorskip("grpc")

from manifest import CASES  # noqa: E402
from quantra_client import Client  # noqa: E402
from quantra_client.batch import TRADE_LISTS  # noqa: E402


def flatc():
    exe = shutil.which("flatc") or "/opt/quantra-deps/bin/flatc"
    return exe if Path(exe).exists() else None


def snake(name):
    return re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def rpc_name(method):
    return Client.METHODS[method].rsplit("/", 1)[1]


def request_class(method):
    rpc = rpc_name(method)
    return getattr(importlib.import_module(f"quantra.{rpc}Request"), f"{rpc}RequestT")


def example_cases():
    """[(method, example path relative to examples/data)] for every Client
    method with a trade list, each example once."""
    out, seen = [], set()
    for method in Client.METHODS:
        if TRADE_LISTS.get(method) is None:
            continue
        for case in CASES:
            if case["product"] == method and (method, case["request"]) not in seen:
                seen.add((method, case["request"]))
                out.append((method, case["request"]))
    return out


@lru_cache(maxsize=None)
def example_binary(method, rel):
    """The example request JSON as FlatBuffers bytes (flatc)."""
    exe = flatc()
    if exe is None:
        pytest.skip("flatc not found (run the suite inside the test image)")
    schema = FBS_DIR / f"{snake(rpc_name(method))}_request.fbs"
    source = DATA / rel
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([exe, "--binary", "-I", str(FBS_DIR), "-o", tmp, str(schema), str(source)],
                       check=True, capture_output=True, text=True)
        return (Path(tmp) / (source.stem + ".bin")).read_bytes()


def example_request(method, rel):
    """A fresh Object API request of the example."""
    return request_class(method).InitFromPackedBuf(example_binary(method, rel), 0)


def pack(obj, force_defaults=True):
    builder = flatbuffers.Builder(1024)
    builder.ForceDefaults(force_defaults)
    builder.Finish(obj.Pack(builder))
    return bytes(builder.Output())


def attr_name(name):
    head, *rest = name.split("_")
    return head + "".join(part[:1].upper() + part[1:] for part in rest)


def trade_attr(method):
    return TRADE_LISTS[method][0]


def request_fields(method, request):
    """Every field of the request other than the market and the trades, by
    Object API attribute name, as assemble_request takes them."""
    skip = {"pricing", trade_attr(method)}
    return {k: v for k, v in vars(request).items() if k not in skip}


def _text(value):
    return value.decode("utf-8") if isinstance(value, bytes) else value


_SEGMENT = re.compile(r"([^.\[\]]+)((?:\[[^\]]+\])*)")


def _locate(obj, path):
    """(container, key) holding the value at `path` on an Object API object:
    setattr(container, key) for a field, container[key] for a vector item."""
    segments = _SEGMENT.findall(path)
    assert ".".join(name + keys for name, keys in segments) == path, path
    container, key = None, None
    current = obj
    for name, keys in segments:
        container, key = current, attr_name(name)
        current = getattr(current, key)
        for item in re.findall(r"\[([^\]]+)\]", keys):
            if item.isdigit():
                index = int(item)
            else:
                matches = [i for i, x in enumerate(current) if _text(getattr(x, "id", None)) == item]
                assert len(matches) == 1, (path, item)
                index = matches[0]
            container, key = current, index
            current = current[index]
    return container, key


def get_path(obj, path):
    container, key = _locate(obj, path)
    return container[key] if isinstance(key, int) else getattr(container, key)


def set_path(obj, path, value):
    container, key = _locate(obj, path)
    if isinstance(key, int):
        container[key] = value
    else:
        setattr(container, key, value)
