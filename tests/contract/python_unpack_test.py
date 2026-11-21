"""Generated Python classes: unpacking works for every request of the client.

flatc's Python output names classes of other modules without importing them;
clients/python/tools/fix_imports.py adds the imports. Checked here:

- no function of a generated module reads a global name the module does not
  define (a missing import would raise NameError only when that code runs);
- every example request of every Client method (the functional catalog's
  requests) round-trips: JSON -> binary (flatc) -> `InitFromPackedBuf` ->
  `Pack` -> binary, and both binaries decode (flatc) to the same JSON.
"""

import builtins
import dis
import importlib
import json
import re
import shutil
import subprocess
import sys
import types
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
FBS_DIR = ROOT / "flatbuffers" / "fbs"
GENERATED = ROOT / "flatbuffers" / "python" / "quantra"
DATA = ROOT / "examples" / "data"
sys.path.insert(0, str(ROOT / "flatbuffers" / "python"))
sys.path.insert(0, str(ROOT / "clients" / "python"))
sys.path.insert(0, str(ROOT / "tests" / "functional"))

flatbuffers = pytest.importorskip("flatbuffers")
pytest.importorskip("grpc")

from manifest import CASES  # noqa: E402
from quantra_client import Client  # noqa: E402


def _flatc():
    exe = shutil.which("flatc") or "/opt/quantra-deps/bin/flatc"
    if not Path(exe).exists():
        pytest.skip("flatc not found (run the suite inside the test image)")
    return exe


def _code_objects(code):
    yield code
    for const in code.co_consts:
        if isinstance(const, types.CodeType):
            yield from _code_objects(const)


def _module_names():
    names = [p.stem for p in GENERATED.glob("*.py") if p.stem != "__init__"]
    names += ["enums." + p.stem for p in (GENERATED / "enums").glob("*.py") if p.stem != "__init__"]
    return sorted(names)


def test_generated_modules_read_no_undefined_global():
    missing = []
    for name in _module_names():
        module = importlib.import_module(f"quantra.{name}")
        source = Path(module.__file__).read_text()
        code = compile(source, module.__file__, "exec")
        for obj in _code_objects(code):
            for ins in dis.get_instructions(obj):
                if ins.opname == "LOAD_GLOBAL":
                    glob = ins.argval
                    if glob not in vars(module) and not hasattr(builtins, glob):
                        missing.append(f"{name}.{obj.co_name}: {glob}")
    assert not missing, "\n".join(sorted(set(missing)))


def _snake(name):
    return re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def _decode(binary, schema, out_dir):
    subprocess.run([_flatc(), "--json", "--strict-json", "--raw-binary", "-I", str(FBS_DIR),
                    "-o", str(out_dir), str(schema), "--", str(binary)],
                   check=True, capture_output=True, text=True)
    return json.loads((out_dir / (binary.stem + ".json")).read_text())


def _cases():
    out = []
    for method, path in Client.METHODS.items():
        examples = [case["request"] for case in CASES if case["product"] == method]
        if not examples:
            continue
        out += [pytest.param(method, path.rsplit("/", 1)[1], rel, id=f"{method}:{rel}")
                for rel in examples]
    return out


@pytest.mark.parametrize("method,rpc,rel", _cases())
def test_example_request_round_trips_through_the_object_api(tmp_path, method, rpc, rel):
    schema = FBS_DIR / f"{_snake(rpc)}_request.fbs"
    request_class = getattr(importlib.import_module(f"quantra.{rpc}Request"), f"{rpc}RequestT")
    source = DATA / rel
    subprocess.run([_flatc(), "--binary", "-I", str(FBS_DIR), "-o", str(tmp_path),
                    str(schema), str(source)], check=True, capture_output=True, text=True)
    original = tmp_path / (source.stem + ".bin")

    request = request_class.InitFromPackedBuf(original.read_bytes(), 0)
    builder = flatbuffers.Builder(1024)
    builder.Finish(request.Pack(builder))
    repacked = tmp_path / "repacked.bin"
    repacked.write_bytes(bytes(builder.Output()))

    assert _decode(repacked, schema, tmp_path / "b") == _decode(original, schema, tmp_path / "a")


def test_every_client_method_with_an_example_is_covered():
    covered = {case.values[0] for case in _cases()}
    # The inflation-curve bootstrap has no example request under examples/data.
    assert set(Client.METHODS) - covered == {"bootstrap_inflation_curves"}
