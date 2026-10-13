#!/usr/bin/env python3
"""Cross-version diff for the Quantra JSON API.

Sends the same request files to two running servers (an "old" and a "new"
build), flattens both JSON responses to ``path -> value`` and reports, field
by field, which values are identical, which differ only by floating-point
noise and which actually changed. See README.md next to this file.

Standard library + ``requests`` only. ``requests`` is imported lazily so the
comparison logic can be used (and unit-tested) without it.
"""

import argparse
import importlib.util
import json
import math
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

FORMAT_VERSION = 1
DEFAULT_REL_TOL = 1e-12
DEFAULT_ABS_TOL = 1e-12
DEFAULT_TIMEOUT = 60.0
DEFAULT_TOP = 10

# Field categories (per response path).
IDENTICAL = "identical"
NOISE = "noise"
CHANGED = "changed"
ONLY_IN_OLD = "only_in_old"
ONLY_IN_NEW = "only_in_new"
VALUE_CHANGED = "value_changed"
DIFF_CATEGORIES = (CHANGED, VALUE_CHANGED, ONLY_IN_OLD, ONLY_IN_NEW)

# Case outcomes, in report order.
OUT_UNCHANGED = "unchanged"
OUT_NOISE = "noise"
OUT_CHANGED = "changed"
OUT_STATUS_CHANGED = "status_changed"
OUT_REQUEST_FAILED = "request_failed"
OUTCOMES = (OUT_UNCHANGED, OUT_NOISE, OUT_CHANGED, OUT_STATUS_CHANGED,
            OUT_REQUEST_FAILED)

_PLAIN_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_\-]*$")


# ---------------------------------------------------------------------------
# Flatten + compare (pure functions, no I/O)
# ---------------------------------------------------------------------------

def flatten(value, prefix=""):
    """Flatten a decoded JSON value to ``{path: leaf}``.

    Object keys are joined with ``.``, list elements appear as ``[i]``
    (``swaps[0].legs[1].npv``). Empty objects/lists are kept as leaves so
    their presence is still compared. A top-level scalar gets the path ``$``.
    """
    out = {}
    if isinstance(value, dict) and value:
        for key in value:
            name = key if _PLAIN_KEY.match(key) else json.dumps(key)
            out.update(flatten(value[key], f"{prefix}.{name}" if prefix else name))
    elif isinstance(value, list) and value:
        for i, item in enumerate(value):
            out.update(flatten(item, f"{prefix}[{i}]"))
    else:
        out[prefix or "$"] = value
    return out


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def compare_values(old, new, rel_tol=DEFAULT_REL_TOL, abs_tol=DEFAULT_ABS_TOL):
    """Classify one pair of leaves.

    Returns ``(category, abs_diff, rel_diff)``; the diffs are ``None`` unless
    both values are finite numbers that differ. Two numbers are noise when
    ``abs_diff <= abs_tol`` or ``abs_diff / max(|old|, |new|) <= rel_tol``.
    """
    if _is_number(old) and _is_number(new):
        if old == new:
            return IDENTICAL, None, None
        old_f, new_f = float(old), float(new)
        if math.isnan(old_f) and math.isnan(new_f):
            return IDENTICAL, None, None
        if not (math.isfinite(old_f) and math.isfinite(new_f)):
            return CHANGED, None, None
        abs_diff = abs(old_f - new_f)
        rel_diff = abs_diff / max(abs(old_f), abs(new_f))
        if abs_diff <= abs_tol or rel_diff <= rel_tol:
            return NOISE, abs_diff, rel_diff
        return CHANGED, abs_diff, rel_diff
    if type(old) is type(new) and old == new:
        return IDENTICAL, None, None
    return VALUE_CHANGED, None, None


def compare_flat(old_flat, new_flat, rel_tol=DEFAULT_REL_TOL,
                 abs_tol=DEFAULT_ABS_TOL, ignore=()):
    """Compare two flattened responses. Returns the per-category detail."""
    result = {"identical": 0, NOISE: [], CHANGED: [], VALUE_CHANGED: [],
              ONLY_IN_OLD: [], ONLY_IN_NEW: [], "ignored": 0}
    for path in sorted(set(old_flat) | set(new_flat)):
        if any(rx.search(path) for rx in ignore):
            result["ignored"] += 1
        elif path not in new_flat:
            result[ONLY_IN_OLD].append({"path": path, "value": old_flat[path]})
        elif path not in old_flat:
            result[ONLY_IN_NEW].append({"path": path, "value": new_flat[path]})
        else:
            old, new = old_flat[path], new_flat[path]
            category, abs_diff, rel_diff = compare_values(old, new, rel_tol, abs_tol)
            if category == IDENTICAL:
                result["identical"] += 1
            elif category == VALUE_CHANGED:
                result[VALUE_CHANGED].append({"path": path, "old": old, "new": new})
            else:
                result[category].append({"path": path, "old": old, "new": new,
                                         "abs_diff": abs_diff, "rel_diff": rel_diff})
    return result


def _decode_body(reply):
    """Flattened view of one reply body (non-JSON bodies become ``$body``)."""
    try:
        return flatten(json.loads(reply["body"]))
    except (ValueError, TypeError):
        return {"$body": reply["body"]}


def _message(reply, limit=500):
    text = reply.get("error") or reply.get("body") or ""
    text = " ".join(str(text).split())
    return text if len(text) <= limit else text[:limit] + "..."


def compare_case(case, old_reply, new_reply, rel_tol=DEFAULT_REL_TOL,
                 abs_tol=DEFAULT_ABS_TOL, ignore=()):
    """Compare the two replies of one request.

    A reply is ``{"status": int|None, "body": str|None, "error": str|None}``;
    ``status`` is ``None`` when the request itself failed (timeout,
    connection refused, unreadable file).
    """
    record = {"id": case["id"], "endpoint": case["endpoint"],
              "request_file": case["request_file"],
              "old_status": old_reply["status"], "new_status": new_reply["status"]}
    if old_reply["status"] is None or new_reply["status"] is None:
        record["outcome"] = OUT_REQUEST_FAILED
        record["old_message"] = _message(old_reply)
        record["new_message"] = _message(new_reply)
        return record
    if old_reply["status"] != new_reply["status"]:
        record["outcome"] = OUT_STATUS_CHANGED
        record["old_message"] = _message(old_reply)
        record["new_message"] = _message(new_reply)
        return record

    detail = compare_flat(_decode_body(old_reply), _decode_body(new_reply),
                          rel_tol, abs_tol, ignore)
    # Largest relative difference first; non-finite changes (no diff) lead.
    detail[CHANGED].sort(key=lambda f: (
        -(f["rel_diff"] if f["rel_diff"] is not None else math.inf),
        -(f["abs_diff"] if f["abs_diff"] is not None else math.inf), f["path"]))
    numeric = [f for f in detail[CHANGED] + detail[NOISE] if f["abs_diff"] is not None]
    record["counts"] = {
        "identical": detail["identical"], "ignored": detail["ignored"],
        **{k: len(detail[k]) for k in (NOISE,) + DIFF_CATEGORIES}}
    if not 200 <= old_reply["status"] < 300:
        # Same error status on both servers: the bodies are still compared,
        # but the case says nothing about results. Reported separately.
        record["error_on_both"] = True
        record["old_message"] = _message(old_reply)
        record["new_message"] = _message(new_reply)
    record["max_abs_diff"] = max((f["abs_diff"] for f in numeric), default=0.0)
    record["max_rel_diff"] = max((f["rel_diff"] for f in numeric), default=0.0)
    for key in (NOISE,) + DIFF_CATEGORIES:
        record[key] = detail[key]
    if any(detail[k] for k in DIFF_CATEGORIES):
        record["outcome"] = OUT_CHANGED
    elif detail[NOISE]:
        record["outcome"] = OUT_NOISE
    else:
        record["outcome"] = OUT_UNCHANGED
    return record


# ---------------------------------------------------------------------------
# Report assembly
# ---------------------------------------------------------------------------

def build_report(old_server, new_server, case_records, rel_tol=DEFAULT_REL_TOL,
                 abs_tol=DEFAULT_ABS_TOL, sources=(), skipped=()):
    """Assemble the complete machine-readable report (a plain dict)."""
    cases = sorted(case_records, key=lambda c: (c["endpoint"], c["id"]))
    endpoints = {}
    for case in cases:
        row = endpoints.setdefault(case["endpoint"], {
            "endpoint": case["endpoint"], "cases": 0,
            **{o: 0 for o in OUTCOMES}, "max_abs_diff": 0.0, "max_rel_diff": 0.0})
        row["cases"] += 1
        row[case["outcome"]] += 1
        row["max_abs_diff"] = max(row["max_abs_diff"], case.get("max_abs_diff", 0.0))
        row["max_rel_diff"] = max(row["max_rel_diff"], case.get("max_rel_diff", 0.0))
    summary = {"cases": len(cases),
               **{o: sum(1 for c in cases if c["outcome"] == o) for o in OUTCOMES},
               "error_on_both": sum(1 for c in cases if c.get("error_on_both"))}
    return {
        "tool": "quantra_version_diff", "format_version": FORMAT_VERSION,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "old": old_server, "new": new_server,
        "tolerances": {"rel_tol": rel_tol, "abs_tol": abs_tol},
        "corpus": {"size": len(cases), "sources": list(sources),
                   "skipped": list(skipped)},
        "summary": summary,
        "endpoints": [endpoints[name] for name in sorted(endpoints)],
        "cases": cases,
    }


def has_differences(report):
    """True if any field changed, any status differs or any request failed."""
    summary = report["summary"]
    return bool(summary[OUT_CHANGED] or summary[OUT_STATUS_CHANGED]
                or summary[OUT_REQUEST_FAILED])


def _json_safe(value):
    """Replace NaN/Infinity (not valid JSON) by strings, recursively."""
    if isinstance(value, float) and not math.isfinite(value):
        return str(value)
    if isinstance(value, dict):
        return {k: _json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(v) for v in value]
    return value


def render_json(report):
    return json.dumps(_json_safe(report), indent=2, sort_keys=False) + "\n"


def _fmt_value(value, limit=60):
    if isinstance(value, float):
        return repr(value)
    text = json.dumps(_json_safe(value))
    text = text if len(text) <= limit else text[:limit] + "..."
    return text.replace("|", "\\|")


def _fmt_diff(value):
    return "n/a" if value is None else ("0" if value == 0 else f"{value:.3e}")


def _case_label(case):
    label = f"`{case['endpoint']}` — {case['id']}"
    if case["request_file"] != case["id"]:
        label += f" (`{case['request_file']}`)"
    return label


def _server_line(label, server):
    meta = server.get("meta") or {}
    quantlib = (meta.get("dependencies") or {}).get("quantlib", "unknown")
    line = (f"- **{label}:** `{server['url']}` — API version "
            f"{meta.get('api_version', 'unknown')}, QuantLib {quantlib}")
    if server.get("meta_error"):
        line += f" (`/meta` not available: {server['meta_error']})"
    return line


def render_markdown(report, top=DEFAULT_TOP):
    """Human summary of a report built by :func:`build_report`."""
    s, tol = report["summary"], report["tolerances"]
    lines = [
        "# Quantra cross-version diff", "",
        _server_line("Old", report["old"]),
        _server_line("New", report["new"]),
        f"- **Corpus:** {report['corpus']['size']} requests"
        + "".join(f"; {src}" for src in report["corpus"]["sources"]),
        f"- **Noise threshold:** relative {tol['rel_tol']:g}, absolute {tol['abs_tol']:g}",
        f"- **Generated:** {report['generated_at']}", "",
        f"**Result:** {s[OUT_UNCHANGED]} unchanged, {s[OUT_NOISE]} noise only, "
        f"{s[OUT_CHANGED]} changed, {s[OUT_STATUS_CHANGED]} status changed, "
        f"{s[OUT_REQUEST_FAILED]} failed to compare."
        + (f" {s['error_on_both']} of the compared requests returned an error "
           "on both servers (listed below)." if s["error_on_both"] else ""), "",
        "\"Unchanged\" is a statement about the requests in this corpus, not "
        "a guarantee for every request shape.", "",
        "## Summary by endpoint", "",
        "| Endpoint | Cases | Unchanged | Noise only | Changed | Status changed "
        "| Failed | Max abs diff | Max rel diff |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in report["endpoints"]:
        lines.append(
            f"| `{row['endpoint']}` | {row['cases']} | {row[OUT_UNCHANGED]} "
            f"| {row[OUT_NOISE]} | {row[OUT_CHANGED]} | {row[OUT_STATUS_CHANGED]} "
            f"| {row[OUT_REQUEST_FAILED]} | {_fmt_diff(row['max_abs_diff'])} "
            f"| {_fmt_diff(row['max_rel_diff'])} |")

    def section(outcome):
        return [c for c in report["cases"] if c["outcome"] == outcome]

    lines += ["", "## Changed cases", ""]
    if not section(OUT_CHANGED):
        lines += ["None.", ""]
    for case in section(OUT_CHANGED):
        counts = case["counts"]
        lines += [
            f"### {_case_label(case)}", "",
            f"HTTP {case['old_status']} on both servers. "
            f"{counts[CHANGED]} numeric fields changed, {counts[VALUE_CHANGED]} "
            f"non-numeric values changed, {counts[ONLY_IN_OLD]} only in old, "
            f"{counts[ONLY_IN_NEW]} only in new, {counts[NOISE]} noise, "
            f"{counts['identical']} identical. Max abs diff "
            f"{_fmt_diff(case['max_abs_diff'])}, max rel diff "
            f"{_fmt_diff(case['max_rel_diff'])}.", ""]
        if case[CHANGED]:
            lines += ["| Field | Old | New | Abs diff | Rel diff |",
                      "|---|---:|---:|---:|---:|"]
            for f in case[CHANGED][:top]:
                lines.append(f"| `{f['path']}` | {_fmt_value(f['old'])} "
                             f"| {_fmt_value(f['new'])} | {_fmt_diff(f['abs_diff'])} "
                             f"| {_fmt_diff(f['rel_diff'])} |")
            if len(case[CHANGED]) > top:
                lines.append(f"\n... and {len(case[CHANGED]) - top} more changed "
                             "numeric fields (smaller relative difference).")
            lines.append("")
        for key, title in ((VALUE_CHANGED, "Non-numeric values changed"),
                           (ONLY_IN_OLD, "Fields only in old"),
                           (ONLY_IN_NEW, "Fields only in new")):
            if not case[key]:
                continue
            lines.append(f"{title}:")
            for f in case[key][:top]:
                if key == VALUE_CHANGED:
                    lines.append(f"- `{f['path']}`: {_fmt_value(f['old'])} → "
                                 f"{_fmt_value(f['new'])}")
                else:
                    lines.append(f"- `{f['path']}`: {_fmt_value(f['value'])}")
            if len(case[key]) > top:
                lines.append(f"- ... and {len(case[key]) - top} more")
            lines.append("")

    lines += ["## Noise-only cases", ""]
    if not section(OUT_NOISE):
        lines += ["None.", ""]
    else:
        lines += ["Differences within the noise threshold; not counted as changes.", ""]
        for case in section(OUT_NOISE):
            lines.append(f"- {_case_label(case)}: {case['counts'][NOISE]} fields, "
                         f"max abs diff {_fmt_diff(case['max_abs_diff'])}, "
                         f"max rel diff {_fmt_diff(case['max_rel_diff'])}")
        lines.append("")

    for outcome, title in ((OUT_STATUS_CHANGED, "Status changes"),
                           (OUT_REQUEST_FAILED, "Requests that could not be compared")):
        lines += [f"## {title}", ""]
        if not section(outcome):
            lines += ["None.", ""]
        for case in section(outcome):
            lines += [
                f"- {_case_label(case)}",
                f"  - old: HTTP {case['old_status'] or 'no response'} — "
                f"{case['old_message'] or '(empty body)'}",
                f"  - new: HTTP {case['new_status'] or 'no response'} — "
                f"{case['new_message'] or '(empty body)'}"]
        if section(outcome):
            lines.append("")
    both = [c for c in report["cases"] if c.get("error_on_both")]
    if both:
        lines += ["## Requests answered with an error by both servers", "",
                  "These requests do not exercise any pricing; fix them or "
                  "remove them from the corpus.", ""]
        for case in both:
            lines.append(f"- {_case_label(case)}: HTTP {case['old_status']} — "
                         f"{case['old_message'] or '(empty body)'}")
        lines.append("")
    if report["corpus"]["skipped"]:
        lines += ["## Skipped files", ""]
        lines += [f"- {item}" for item in report["corpus"]["skipped"]] + [""]
    return "\n".join(lines).rstrip("\n") + "\n"


# ---------------------------------------------------------------------------
# Corpus loading
# ---------------------------------------------------------------------------

def load_requests_dir(root):
    """Corpus from a user directory: ``<root>/<endpoint>/**/*.json``.

    The first-level sub-directory name is the endpoint the files are POSTed
    to (``price-vanilla-swap/my_swap.json`` -> ``POST /price-vanilla-swap``).
    Returns ``(cases, skipped)``; JSON files directly under ``root`` have no
    endpoint and are skipped (and listed in the report).
    """
    root = Path(root)
    if not root.is_dir():
        raise SystemExit(f"--requests: {root} is not a directory")
    cases, skipped = [], []
    for path in sorted(root.rglob("*.json")):
        rel = path.relative_to(root)
        if len(rel.parts) < 2:
            skipped.append(f"{rel.as_posix()}: not inside an endpoint sub-directory")
            continue
        cases.append({"id": rel.as_posix(), "endpoint": rel.parts[0],
                      "request_file": rel.as_posix(), "path": path})
    return cases, skipped


_CATALOG_ROW = re.compile(r'^\s*X\(\s*\w+,\s*"(?P<key>[^"]+)",\s*"(?P<route>[^"]+)"')


def load_catalog(repo_root):
    """Corpus from the repository's functional catalog manifest.

    Reads ``tests/functional/manifest.py`` (plain data, imported by path) and
    the product -> route table in ``src/common/product_catalog.h``.
    """
    repo_root = Path(repo_root)
    manifest_path = repo_root / "tests" / "functional" / "manifest.py"
    header = repo_root / "src" / "common" / "product_catalog.h"
    for needed in (manifest_path, header):
        if not needed.is_file():
            raise SystemExit(f"--catalog: {needed} not found (is {repo_root} "
                             "a Quantra server checkout?)")
    spec = importlib.util.spec_from_file_location("_quantra_functional_manifest",
                                                  manifest_path)
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)
    routes = {}
    for line in header.read_text().splitlines():
        match = _CATALOG_ROW.match(line)
        if match:
            routes[match["key"]] = match["route"]
    cases = []
    for entry in manifest.CASES:
        if entry["product"] not in routes:
            raise SystemExit(f"--catalog: no route for product {entry['product']!r}")
        cases.append({"id": entry["id"], "endpoint": routes[entry["product"]],
                      "request_file": f"examples/data/{entry['request']}",
                      "path": repo_root / "examples" / "data" / entry["request"]})
    return cases


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def fetch_meta(session, url, timeout):
    server = {"url": url, "meta": None, "meta_error": None}
    try:
        reply = session.get(f"{url}/meta", timeout=timeout)
        if reply.status_code == 200:
            server["meta"] = reply.json()
        else:
            server["meta_error"] = f"HTTP {reply.status_code}"
    except Exception as exc:  # unreachable server, bad JSON, ...
        server["meta_error"] = _message({"error": f"{type(exc).__name__}: {exc}"}, 200)
    return server


def post_request(session, url, endpoint, payload, timeout):
    """POST one request body; never raises."""
    try:
        reply = session.post(f"{url}/{endpoint}", data=payload, timeout=timeout,
                             headers={"Content-Type": "application/json"})
        return {"status": reply.status_code, "body": reply.text, "error": None}
    except Exception as exc:
        return {"status": None, "body": None,
                "error": f"{type(exc).__name__}: {exc}"}


def run(cases, old_url, new_url, rel_tol, abs_tol, timeout, ignore=(),
        sources=(), skipped=(), progress=None):
    import requests  # lazy: only needed when talking to servers

    old_session, new_session = requests.Session(), requests.Session()
    old_server = fetch_meta(old_session, old_url, timeout)
    new_server = fetch_meta(new_session, new_url, timeout)
    records = []
    for i, case in enumerate(cases, 1):
        try:
            payload = Path(case["path"]).read_bytes()
        except OSError as exc:
            failed = {"status": None, "body": None,
                      "error": f"cannot read request file: {exc}"}
            old_reply = new_reply = failed
        else:
            old_reply = post_request(old_session, old_url, case["endpoint"], payload, timeout)
            new_reply = post_request(new_session, new_url, case["endpoint"], payload, timeout)
        record = compare_case(case, old_reply, new_reply, rel_tol, abs_tol, ignore)
        records.append(record)
        if progress:
            progress(i, len(cases), record)
    return build_report(old_server, new_server, records, rel_tol, abs_tol,
                        sources, skipped)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    default_repo = Path(__file__).resolve().parents[2]
    p = argparse.ArgumentParser(
        description="Send the same requests to two Quantra servers and report "
                    "which response fields differ.")
    p.add_argument("--old", required=True, help="base URL of the old server")
    p.add_argument("--new", required=True, help="base URL of the new server")
    p.add_argument("--catalog", nargs="?", const=str(default_repo), metavar="REPO",
                   help="use the repository's functional catalog as corpus "
                        "(REPO defaults to the checkout containing this script)")
    p.add_argument("--requests", metavar="DIR",
                   help="directory of your own requests: DIR/<endpoint>/*.json")
    p.add_argument("--rel-tol", type=float, default=DEFAULT_REL_TOL,
                   help="relative difference treated as noise (default %(default)g)")
    p.add_argument("--abs-tol", type=float, default=DEFAULT_ABS_TOL,
                   help="absolute difference treated as noise (default %(default)g)")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                   help="per-request timeout in seconds (default %(default)g)")
    p.add_argument("--top", type=int, default=DEFAULT_TOP,
                   help="changed fields listed per case in the Markdown report "
                        "(default %(default)d; the JSON report always has all)")
    p.add_argument("--ignore-path", action="append", default=[], metavar="REGEX",
                   help="skip response paths matching REGEX (repeatable)")
    p.add_argument("--json-out", metavar="FILE", help="write the full report as JSON")
    p.add_argument("--md-out", metavar="FILE", help="write the Markdown summary")
    p.add_argument("--fail-on-change", action="store_true",
                   help="exit 1 if any field changed, any status differs or any "
                        "request could not be compared")
    p.add_argument("--quiet", action="store_true", help="no per-request progress")
    args = p.parse_args(argv)
    if not args.catalog and not args.requests:
        p.error("give a corpus: --catalog and/or --requests DIR")
    return args


def main(argv=None):
    args = parse_args(argv)
    cases, sources, skipped = [], [], []
    if args.catalog:
        catalog = load_catalog(args.catalog)
        cases += catalog
        sources.append(f"functional catalog ({len(catalog)} cases)")
    if args.requests:
        user_cases, skipped = load_requests_dir(args.requests)
        cases += user_cases
        sources.append(f"{args.requests} ({len(user_cases)} files)")
    if not cases:
        raise SystemExit("no request files found")

    def progress(i, total, record):
        if not args.quiet:
            print(f"[{i}/{total}] {record['outcome']:<15} {record['endpoint']} "
                  f"{record['id']}", file=sys.stderr)

    report = run(cases, args.old.rstrip("/"), args.new.rstrip("/"), args.rel_tol,
                 args.abs_tol, args.timeout,
                 ignore=[re.compile(rx) for rx in args.ignore_path],
                 sources=sources, skipped=skipped, progress=progress)
    if args.json_out:
        Path(args.json_out).write_text(render_json(report))
    if args.md_out:
        Path(args.md_out).write_text(render_markdown(report, args.top))
    s = report["summary"]
    print(f"{s['cases']} requests: {s[OUT_UNCHANGED]} unchanged, {s[OUT_NOISE]} "
          f"noise only, {s[OUT_CHANGED]} changed, {s[OUT_STATUS_CHANGED]} status "
          f"changed, {s[OUT_REQUEST_FAILED]} failed to compare"
          + (f"; {s['error_on_both']} returned an error on both servers"
             if s["error_on_both"] else ""))
    return 1 if args.fail_on_change and has_differences(report) else 0


if __name__ == "__main__":
    sys.exit(main())
