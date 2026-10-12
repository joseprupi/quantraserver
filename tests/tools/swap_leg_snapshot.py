#!/usr/bin/env python3
"""Snapshot and compare swap responses across the 0.9.0 -> 0.10.0 leg migration.

The 0.10.0 release moves `price-vanilla-swap`, `price-ois-swap`,
`price-basis-swap` and the swaption's underlying onto the shared `SwapLeg`
table. The engine builds the same QuantLib legs as before, so every number a
0.9.0 server returned for a request must come back unchanged from a 0.10.0
server for the converted request. This tool proves it:

  capture   POST request files to a running server and store the responses
            (plain; with `include_flows`; swaptions also with the analytic
            and the rebump greeks turned on), one JSON per request x variant;
  diff      compare two capture directories field by field, translating the
            0.9.0 response shape (`fixed_leg_npv`, `leg1_flows`,
            `fair_spread_leg2`, ...) into the 0.10.0 one (`legs[i].npv`,
            `legs[i].flows`, `fair_spread`) and reporting every value that is
            not bit-identical at the precision the server prints. The vanilla
            and OIS `fair_rate` / `fair_spread` are compared as they are.

Usage:
  python3 tests/tools/swap_leg_snapshot.py capture --url http://localhost:8080 \
      --out /tmp/old examples/data/ir_swaps examples/data/swaption ...
  python3 tests/tools/swap_leg_snapshot.py diff /tmp/old /tmp/new

Standard library only. Request files that are not one of the four request
kinds are skipped. In `capture`, requests are sent as they are on disk: run it
once on the 0.9.0 corpus against a 0.9.0 server, convert the corpus with
tools/migrate_swap_requests, run it again against a 0.10.0 server, then `diff`.
"""

import argparse
import copy
import json
import os
import sys
import urllib.error
import urllib.request

ENDPOINTS = {
    "vanilla_swap": "/price-vanilla-swap",
    "ois_swap": "/price-ois-swap",
    "basis_swap": "/price-basis-swap",
    "swaption": "/price-swaption",
}

# Old per-leg response fields -> (leg index, field inside SwapLegResult).
LEG_FIELDS = {
    "vanilla_swap": {
        "fixed_leg_npv": (0, "npv"),
        "floating_leg_npv": (1, "npv"),
        "fixed_leg_flows": (0, "flows"),
        "floating_leg_flows": (1, "flows"),
    },
    "ois_swap": {
        "fixed_leg_npv": (0, "npv"),
        "overnight_leg_npv": (1, "npv"),
        "fixed_leg_flows": (0, "flows"),
        "overnight_leg_flows": (1, "flows"),
    },
    "basis_swap": {
        "leg1_npv": (0, "npv"),
        "leg2_npv": (1, "npv"),
        "leg1_flows": (0, "flows"),
        "leg2_flows": (1, "flows"),
    },
}

# Old scalar fields that have no 0.10.0 counterpart (dropped by design: the
# leg BPS). The vanilla / OIS `fair_rate` and `fair_spread` are compared; a
# 0.9.0 CMS swap wrote `0.0` for both, 0.10.0 computes them, so those are the
# expected differences.
DROPPED = {
    "vanilla_swap": {"fixed_leg_bps", "floating_leg_bps"},
    "ois_swap": {"fixed_leg_bps", "overnight_leg_bps"},
    "basis_swap": {"leg1_bps", "leg2_bps"},
    "swaption": set(),
}


def detect(request):
    swaps = request.get("swaps")
    if isinstance(swaps, list) and swaps:
        first = swaps[0]
        for key, kind in (
            ("vanilla_swap", "vanilla_swap"),
            ("ois_swap", "ois_swap"),
            ("basis_swap", "basis_swap"),
        ):
            if key in first:
                return kind
    if isinstance(request.get("swaptions"), list) and request["swaptions"]:
        return "swaption"
    return None


def variants(kind, request):
    """Request variants to capture: (suffix, request dict)."""
    out = [("plain", request)]
    if kind == "swaption":
        for flag in ("swaption_pricing_details", "swaption_pricing_rebump"):
            r = copy.deepcopy(request)
            r.setdefault("pricing", {}).setdefault("options", {})[flag] = True
            out.append((flag, r))
    else:
        r = copy.deepcopy(request)
        r["include_flows"] = True
        out.append(("flows", r))
    return out


def post(url, body):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=600) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def iter_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in sorted(files):
                    if f.endswith(".json"):
                        yield os.path.join(root, f)
        else:
            yield p


def capture(args):
    os.makedirs(args.out, exist_ok=True)
    n = 0
    for path in iter_files(args.inputs):
        with open(path) as fh:
            try:
                request = json.load(fh)
            except ValueError:
                continue
        kind = detect(request)
        if kind is None:
            continue
        base = os.path.splitext(os.path.basename(path))[0]
        for suffix, body in variants(kind, request):
            status, text = post(args.url + ENDPOINTS[kind], body)
            try:
                payload = json.loads(text)
            except ValueError:
                payload = {"raw": text}
            record = {"kind": kind, "request": os.path.relpath(path), "variant": suffix,
                      "status": status, "response": payload}
            with open(os.path.join(args.out, f"{base}.{suffix}.json"), "w") as out:
                json.dump(record, out, indent=1, sort_keys=True)
            n += 1
            if status != 200:
                print(f"WARN {base}.{suffix}: HTTP {status}: {text[:200]}")
    print(f"captured {n} responses into {args.out}")


def translate_old(kind, row):
    """0.9.0 response row -> the 0.10.0 shape (legs[i], fair_spread)."""
    if kind == "swaption":
        return dict(row)
    new = {}
    legs = [{}, {}]
    for key, value in row.items():
        if key in LEG_FIELDS[kind]:
            i, field = LEG_FIELDS[kind][key]
            legs[i][field] = value
        elif key in DROPPED[kind]:
            continue
        elif kind == "basis_swap" and key in ("fair_spread_leg1", "fair_spread_leg2"):
            continue
        else:
            new[key] = value
    new["legs"] = legs
    if kind == "basis_swap":
        new["fair_spread"] = row.get("fair_spread_leg2"), row.get("fair_spread_leg1")
    return new


def basis_spread_leg(request_file):
    """Index of the leg whose spread the 0.10.0 basis response solves for."""
    try:
        with open(request_file) as fh:
            request = json.load(fh)
    except (OSError, ValueError):
        return None
    out = []
    for swap in request.get("swaps", []):
        legs = swap.get("basis_swap", {})
        spreads = []
        for name in ("leg1", "leg2"):
            leg = legs.get(name, {})
            body = leg.get("coupons", leg)
            spreads.append(float(body.get("spread", 0.0) or 0.0))
        if spreads[0] != 0.0 and spreads[1] == 0.0:
            out.append(0)
        else:
            out.append(1)
    return out


def compare(old, new, path, report):
    """Every value in `old` must be present and identical in `new`."""
    if isinstance(old, dict):
        if not isinstance(new, dict):
            report.append((path, old, new))
            return 0
        n = 0
        for key, value in old.items():
            if key not in new:
                report.append((f"{path}.{key}", value, "<absent>"))
                continue
            n += compare(value, new[key], f"{path}.{key}", report)
        return n
    if isinstance(old, list):
        if not isinstance(new, list) or len(new) != len(old):
            report.append((path, f"len {len(old)}", f"len {len(new) if isinstance(new, list) else new}"))
            return 0
        return sum(compare(o, v, f"{path}[{i}]", report) for i, (o, v) in enumerate(zip(old, new)))
    if old != new:
        report.append((path, old, new))
        return 0
    return 1


def diff(args):
    files = sorted(f for f in os.listdir(args.old) if f.endswith(".json"))
    total_values = 0
    mismatches = []
    missing = []
    compared_files = 0
    for name in files:
        new_path = os.path.join(args.new, name)
        if not os.path.exists(new_path):
            missing.append(name)
            continue
        with open(os.path.join(args.old, name)) as fh:
            old = json.load(fh)
        with open(new_path) as fh:
            new = json.load(fh)
        kind = old["kind"]
        if old["status"] != new["status"]:
            mismatches.append((name, "status", old["status"], new["status"]))
            continue
        if old["status"] != 200:
            compared_files += 1
            continue
        list_key = "swaptions" if kind == "swaption" else "swaps"
        old_rows = old["response"].get(list_key, [])
        new_rows = new["response"].get(list_key, [])
        if len(old_rows) != len(new_rows):
            mismatches.append((name, list_key, len(old_rows), len(new_rows)))
            continue
        spread_legs = basis_spread_leg(old["request"]) if kind == "basis_swap" else None
        for i, (o, n) in enumerate(zip(old_rows, new_rows)):
            expected = translate_old(kind, o)
            if kind == "basis_swap":
                leg2_value, leg1_value = expected.pop("fair_spread")
                leg = spread_legs[i] if spread_legs and i < len(spread_legs) else 1
                value = leg1_value if leg == 0 else leg2_value
                if value is not None:
                    expected["fair_spread"] = value
            report = []
            total_values += compare(expected, n, f"{name}:{list_key}[{i}]", report)
            mismatches.extend((name, p, a, b) for p, a, b in report)
        # Other top-level members (diagnostics...) must be unchanged too.
        for key, value in old["response"].items():
            if key == list_key:
                continue
            report = []
            total_values += compare(value, new["response"].get(key), f"{name}:{key}", report)
            mismatches.extend((name, p, a, b) for p, a, b in report)
        compared_files += 1

    print(f"files compared: {compared_files} (missing in new: {len(missing)})")
    print(
        f"values compared: {total_values + len(mismatches)}, identical: {total_values}, "
        f"differing: {len(mismatches)}"
    )
    for name in missing:
        print(f"MISSING {name}")
    for name, path, a, b in mismatches[: args.limit]:
        print(f"DIFF {path}: old={a!r} new={b!r}")
    if len(mismatches) > args.limit:
        print(f"... {len(mismatches) - args.limit} more")
    return 1 if mismatches or missing else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--url", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("inputs", nargs="+")
    c.set_defaults(func=capture)
    d = sub.add_parser("diff")
    d.add_argument("old")
    d.add_argument("new")
    d.add_argument("--limit", type=int, default=50)
    d.set_defaults(func=diff)
    args = parser.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
