"""Functional parity catalog driver.

Parametrizes over tests/functional/manifest.py: each case's request JSON is
POSTed to the running server (via the shared `client` fixture) and the
returned NPV is compared against the independent QuantLib reference pricer
named in the manifest (from tests/contract/ql_reference.py).

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
        missing = required - case.keys()
        assert not missing, f"case {case.get('id')} missing keys: {missing}"
        assert case["id"] not in ids, f"duplicate case id: {case['id']}"
        ids.add(case["id"])
        assert (data_dir / case["request"]).is_file(), (
            f"{case['id']}: request file not found: {case['request']}"
        )
        assert callable(getattr(ql_reference, case["ql_pricer"], None)), (
            f"{case['id']}: unknown ql_reference pricer: {case['ql_pricer']}"
        )
        assert case["tolerance"] > 0
        assert case["exercises"], f"{case['id']}: empty exercises list"


# ---------------------------------------------------------------------------
# The parity cases themselves
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("case", CASES, ids=[c["id"] for c in CASES])
def test_functional_parity(client, data_dir, case):
    request = load_json(data_dir / case["request"])
    response = client.price(case["product"], request)
    quantra = api_npv(response, case["list_key"])
    pricer = getattr(ql_reference, case["ql_pricer"])
    quantlib = pricer(request)
    tolerance = case["tolerance"]
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
