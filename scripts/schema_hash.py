#!/usr/bin/env python3
"""The schema hash: one value identifying the FlatBuffers schemas of a source tree.

It is the SHA-256 over every `flatbuffers/fbs/*.fbs` file, in file-name order,
each contributing its file name, its length and its bytes (line endings
normalised to LF). It does not depend on file times or on where the tree lives.

This script is the only implementation. The engine build reads it (CMake runs
it at configure time and the JSON gateway's GET /meta and the gRPC Meta reply
report the value as `schema_hash`), and the Python client's generated schema
description stores the same value as `quantra_client.SCHEMA_HASH`
(clients/python/tools/gen_schema_description.py imports this file).

Usage: python3 scripts/schema_hash.py [FBS_DIR]   (prints the hash)
"""

import hashlib
import sys
from pathlib import Path

DEFAULT_FBS_DIR = Path(__file__).resolve().parents[1] / "flatbuffers" / "fbs"


def schema_files(fbs_dir=DEFAULT_FBS_DIR):
    """The schema files the hash covers, in hash order."""
    return sorted(Path(fbs_dir).glob("*.fbs"), key=lambda p: p.name)


def schema_hash(fbs_dir=DEFAULT_FBS_DIR) -> str:
    files = schema_files(fbs_dir)
    if not files:
        raise FileNotFoundError(f"no .fbs file in {fbs_dir}")
    digest = hashlib.sha256()
    for path in files:
        data = path.read_bytes().replace(b"\r\n", b"\n")
        digest.update(path.name.encode("utf-8") + b"\0")
        digest.update(str(len(data)).encode("ascii") + b"\0")
        digest.update(data)
    return digest.hexdigest()


if __name__ == "__main__":
    print(schema_hash(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_FBS_DIR))
