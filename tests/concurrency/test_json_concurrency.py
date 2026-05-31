#!/usr/bin/env python3
"""
Quantra JSON API concurrency / race test.

Fires many identical requests at a single endpoint from many threads at once.
The JSON gateway shares one flatbuffers::Parser per product across all Crow
worker threads; if that parser is raced (the bug fixed in
client/cpp/src/json_parser.cpp), concurrent requests to the same endpoint can
return corrupted/failed responses or crash the gateway. With per-thread parsers,
every identical request must return the same successful result.

Standard-library only (urllib) so it runs without extra dependencies.

Usage:
    python3 tests/test_json_concurrency.py \
        --url http://localhost:8080 \
        --data-dir examples/data \
        --requests 96 --workers 24
"""

import argparse
import copy
import json
import os
import sys
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor


# The example payloads are stored in the canonical nested schema, so they are
# POSTed verbatim — exactly what a real client sends.
def post_json(url: str, payload: dict, timeout: float = 30.0):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="JSON API concurrency/race test")
    parser.add_argument("--url", default="http://localhost:8080")
    parser.add_argument("--data-dir", default="examples/data")
    parser.add_argument("--route", default="price-fixed-rate-bond")
    parser.add_argument("--request-file", default="fixed_rate_bond_request.json")
    parser.add_argument("--requests", type=int, default=96)
    parser.add_argument("--workers", type=int, default=24)
    args = parser.parse_args()

    endpoint = f"{args.url.rstrip('/')}/{args.route}"
    with open(os.path.join(args.data_dir, args.request_file)) as f:
        payload = json.load(f)

    # Warm-up establishes the expected (reference) response.
    ref_status, ref_body = post_json(endpoint, payload)
    if ref_status != 200:
        print(f"Warm-up request failed: HTTP {ref_status}: {ref_body[:200]}")
        print("CONCURRENCY TEST FAILED")
        return 1
    reference = json.dumps(json.loads(ref_body), sort_keys=True)

    def one_call(_i):
        status, body = post_json(endpoint, copy.deepcopy(payload))
        if status != 200:
            return f"HTTP {status}: {body[:120]}"
        normalized = json.dumps(json.loads(body), sort_keys=True)
        return None if normalized == reference else "response differs from reference"

    failures = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for i, result in enumerate(pool.map(one_call, range(args.requests))):
            if result is not None:
                failures.append(f"request {i}: {result}")

    total = args.requests
    print("=" * 44)
    print(f"  JSON concurrency test: {args.route} x{total} ({args.workers} threads)")
    print("=" * 44)
    print(f"TOTAL SCENARIOS: {total - len(failures)}/{total}")
    if failures:
        for msg in failures[:10]:
            print(f"  FAIL {msg}")
        print("CONCURRENCY TEST FAILED")
        return 1
    print("CONCURRENCY TEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
