"""Functional parity catalog driver.

Parametrizes over tests/functional/manifest.py: each case's request JSON is
POSTed to the running server (via the shared `client` fixture) and the
response is compared against the independent QuantLib reference named in the
manifest (from tests/contract/ql_reference.py). Three comparison modes:

  * compare="npv" (default) — the response holds one instrument NPV under
    `list_key`; assert abs(api_npv - ql_npv) < tolerance.
  * compare="series" — the response holds sampled curve series under
    `list_key` (e.g. /bootstrap-curves results[0].series[]); the reference
    returns the expected values ({series_name: [floats]} keyed like the
    response's `measure`, or a flat list when there is exactly one series)
    and every point must satisfy abs(api[i] - ql[i]) < tolerance.
  * compare="exact" — for date-utility endpoints (calendar business days /
    holidays / advance): the reference returns the expected value directly —
    a list of ISO date strings or a single date string — and the response
    value under `list_key` must equal it exactly (same length, same order,
    same strings; no tolerance, so exact cases carry no tolerance field).

Also keeps the committed catalog honest: test_catalog_in_sync regenerates
CATALOG.md / catalog.html content in memory and asserts it matches the files
checked into the repo, so a manifest edit without a catalog regeneration
fails the gate.
"""

from pathlib import Path

import pytest

import ql_reference
from ql_reference import api_npv, load_json
from manifest import CASES

FUNCTIONAL_DIR = Path(__file__).resolve().parent


# ---------------------------------------------------------------------------
# Manifest sanity — cheap static checks, no server required
# ---------------------------------------------------------------------------

def test_manifest_well_formed(data_dir):
    required = {"id", "product", "family", "title", "description", "request",
                "list_key", "ql_pricer", "tolerance", "exercises"}
    ids = set()
    for case in CASES:
        compare = case.get("compare", "npv")
        assert compare in ("npv", "series", "exact"), (
            f"{case.get('id')}: unknown compare mode: {case.get('compare')}"
        )
        # Exact cases have no tolerance by construction — the response must
        # equal the reference value verbatim.
        needed = required - ({"tolerance"} if compare == "exact" else set())
        missing = needed - case.keys()
        assert not missing, f"case {case.get('id')} missing keys: {missing}"
        assert case["id"] not in ids, f"duplicate case id: {case['id']}"
        ids.add(case["id"])
        assert (data_dir / case["request"]).is_file(), (
            f"{case['id']}: request file not found: {case['request']}"
        )
        assert callable(getattr(ql_reference, case["ql_pricer"], None)), (
            f"{case['id']}: unknown ql_reference pricer: {case['ql_pricer']}"
        )
        if compare == "exact":
            assert "tolerance" not in case, (
                f"{case['id']}: exact-match cases must not carry a tolerance"
            )
        else:
            assert case["tolerance"] > 0
        assert case["exercises"], f"{case['id']}: empty exercises list"


# ---------------------------------------------------------------------------
# The parity cases themselves
# ---------------------------------------------------------------------------

def _api_series(response, list_key, case_id):
    """Pull {series_name: [values]} out of a sampled-series response.

    The response's first entry under `list_key` (e.g. /bootstrap-curves
    results[0]) carries series[] items with a `measure` name and aligned
    `values`. FlatBuffers JSON omits default enum values, so a missing
    measure means the first enum value, DF.
    """
    entries = response[list_key]
    assert entries, f"{case_id}: empty '{list_key}' in response"
    entry = entries[0]
    error = entry.get("error")
    assert not error, f"{case_id}: server returned a per-curve error: {error}"
    series = entry.get("series", [])
    assert series, f"{case_id}: no series in response entry"
    return {s.get("measure", "DF"): s["values"] for s in series}


def _assert_series_parity(case, api, expected):
    """Element-wise comparison of every sampled series, reporting the max gap."""
    tolerance = case["tolerance"]
    if not isinstance(expected, dict):
        # Flat list: the response must hold exactly one series.
        assert len(api) == 1, (
            f"{case['id']}: reference returned one series but the response "
            f"has {sorted(api)}"
        )
        expected = {next(iter(api)): list(expected)}
    assert set(api) == set(expected), (
        f"{case['id']}: response series {sorted(api)} != "
        f"reference series {sorted(expected)}"
    )
    max_gap, worst = -1.0, None
    for name, ql_values in expected.items():
        api_values = api[name]
        assert len(api_values) == len(ql_values), (
            f"{case['id']}: series '{name}' length {len(api_values)} != "
            f"reference {len(ql_values)}"
        )
        for i, (a, b) in enumerate(zip(api_values, ql_values)):
            gap = abs(a - b)
            if gap > max_gap:
                max_gap, worst = gap, f"{name}[{i}] api={a} ql={b}"
    assert max_gap < tolerance, (
        f"{case['id']}: max series gap {max_gap:.6g} at {worst} "
        f">= tolerance {tolerance}"
    )


def _assert_exact_parity(case, actual, expected):
    """Verbatim equality for date-utility cases — no tolerance.

    `expected` is a list of ISO date strings (business days / holidays) or a
    single ISO date string (advance); the response value must equal it
    exactly, including list order and length.
    """
    case_id = case["id"]
    if isinstance(expected, list):
        assert isinstance(actual, list), (
            f"{case_id}: expected a date list under '{case['list_key']}', "
            f"got {type(actual).__name__}"
        )
        assert len(actual) == len(expected), (
            f"{case_id}: {len(actual)} dates returned, reference expects "
            f"{len(expected)}: api={actual} ql={expected}"
        )
        mismatches = [(i, a, e)
                      for i, (a, e) in enumerate(zip(actual, expected))
                      if a != e]
        assert not mismatches, (
            f"{case_id}: {len(mismatches)} date(s) differ, first at index "
            f"{mismatches[0][0]}: api={mismatches[0][1]} "
            f"ql={mismatches[0][2]}"
        )
    else:
        assert actual == expected, (
            f"{case_id}: api={actual!r} != quantlib={expected!r}"
        )


@pytest.mark.parametrize("case", CASES, ids=[c["id"] for c in CASES])
def test_functional_parity(client, data_dir, case):
    request = load_json(data_dir / case["request"])
    response = client.price(case["product"], request)
    pricer = getattr(ql_reference, case["ql_pricer"])
    if case.get("compare", "npv") == "exact":
        expected = pricer(request)
        assert case["list_key"] in response, (
            f"{case['id']}: response has no '{case['list_key']}' field: "
            f"{sorted(response)}"
        )
        _assert_exact_parity(case, response[case["list_key"]], expected)
        return
    tolerance = case["tolerance"]
    if case.get("compare", "npv") == "series":
        expected = pricer(request)
        api = _api_series(response, case["list_key"], case["id"])
        _assert_series_parity(case, api, expected)
        return
    quantra = api_npv(response, case["list_key"])
    quantlib = pricer(request)
    assert abs(quantra - quantlib) < tolerance, (
        f"{case['id']}: API NPV {quantra} vs QuantLib {quantlib} "
        f"(diff {abs(quantra - quantlib):.6g} >= tolerance {tolerance})"
    )


# ---------------------------------------------------------------------------
# Catalog freshness — committed CATALOG.md / catalog.html match the manifest
# ---------------------------------------------------------------------------

def test_catalog_in_sync():
    """Regenerating the catalog must produce exactly the committed files."""
    import generate_catalog

    md_file = FUNCTIONAL_DIR / "CATALOG.md"
    html_file = FUNCTIONAL_DIR / "catalog.html"
    assert md_file.is_file(), "CATALOG.md missing — run generate_catalog.py"
    assert html_file.is_file(), "catalog.html missing — run generate_catalog.py"

    rows = generate_catalog.compute_rows()
    assert md_file.read_text() == generate_catalog.build_markdown(rows), (
        "CATALOG.md is stale — rerun tests/functional/generate_catalog.py"
    )
    assert html_file.read_text() == generate_catalog.build_html(rows), (
        "catalog.html is stale — rerun tests/functional/generate_catalog.py"
    )
