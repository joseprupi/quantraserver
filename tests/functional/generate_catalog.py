#!/usr/bin/env python3
"""Generate the browsable functional parity catalog from the manifest.

Reads tests/functional/manifest.py, prices every case with its independent
QuantLib reference pricer (tests/contract/ql_reference.py) and writes:

  * tests/functional/CATALOG.md   — grouped-by-family markdown tables
  * tests/functional/catalog.html — the same catalog as a self-contained
                                    single HTML file (inline CSS, no external
                                    assets); open it directly in a browser

Both outputs are committed so the catalog is browsable on GitHub;
tests/functional/test_functional_parity.py::test_catalog_in_sync fails the
gate whenever the committed files drift from the manifest.

Requires the QuantLib Python package (pinned in tests/requirements.txt to the
same version the server links against), so run it inside the test image:

    docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
        bash -lc 'python3 tests/functional/generate_catalog.py'

The generator is idempotent: same manifest + same request JSONs + same
QuantLib version always produce byte-identical output.
"""

import html
import sys
from pathlib import Path

FUNCTIONAL_DIR = Path(__file__).resolve().parent
TESTS_DIR = FUNCTIONAL_DIR.parent
REPO_ROOT = TESTS_DIR.parent
CONTRACT_DIR = TESTS_DIR / "contract"
DATA_DIR = REPO_ROOT / "examples" / "data"

for path in (str(FUNCTIONAL_DIR), str(CONTRACT_DIR)):
    if path not in sys.path:
        sys.path.insert(0, path)

import ql_reference  # noqa: E402
from ql_reference import load_json  # noqa: E402
from manifest import CASES  # noqa: E402

# Path from tests/functional/ to the request files, used for relative links
# that work both on GitHub and in a locally opened catalog.html.
DATA_RELATIVE_PREFIX = "../../examples/data/"


def compute_rows():
    """Price every manifest case with its QuantLib reference pricer.

    Returns a list of (family, [row, ...]) pairs preserving manifest order,
    where each row is the case dict plus a formatted reference NPV.
    """
    families = []
    by_family = {}
    for case in CASES:
        pricer = getattr(ql_reference, case["ql_pricer"])
        request = load_json(DATA_DIR / case["request"])
        npv = pricer(request)
        row = dict(case)
        row["npv"] = npv
        row["npv_text"] = f"{npv:,.2f}"
        family = case["family"]
        if family not in by_family:
            by_family[family] = []
            families.append(family)
        by_family[family].append(row)
    return [(family, by_family[family]) for family in families]


INTRO = (
    "Every case below is a complete JSON pricing request that is POSTed to "
    "the running Quantra server and whose returned NPV is asserted to match "
    "an independent QuantLib reference pricer "
    "(`tests/contract/ql_reference.py`) within a tolerance of 0.01. The "
    "cases, groupings and descriptions all come from one place — "
    "`tests/functional/manifest.py` — and the assertions run inside the "
    "standard gate (`bash tests/run_all_tests.sh`, suite 3). See "
    "`tests/functional/README.md` for how to add a case."
)


def build_markdown(rows=None) -> str:
    if rows is None:
        rows = compute_rows()
    out = []
    out.append("# Functional Parity Catalog")
    out.append("")
    out.append("<!-- GENERATED FILE — do not edit by hand. -->")
    out.append("<!-- Regenerate with: python3 tests/functional/generate_catalog.py "
               "(inside the test image). -->")
    out.append("")
    out.append(INTRO)
    out.append("")
    for family, cases in rows:
        out.append(f"## {family}")
        out.append("")
        out.append("| Case | What it exercises | Request JSON | QuantLib NPV |")
        out.append("|---|---|---|---:|")
        for row in cases:
            case_cell = f"**{row['title']}**<br><sub>{row['description']}</sub>"
            exercises = ", ".join(f"`{tag}`" for tag in row["exercises"])
            link_target = DATA_RELATIVE_PREFIX + row["request"]
            link_name = Path(row["request"]).name
            link = f"[{link_name}]({link_target})"
            out.append(f"| {case_cell} | {exercises} | {link} | {row['npv_text']} |")
        out.append("")
    out.append(f"_{sum(len(c) for _, c in rows)} cases across "
               f"{len(rows)} family(ies)._")
    out.append("")
    return "\n".join(out)


HTML_STYLE = """\
    body { font-family: -apple-system, "Segoe UI", Helvetica, Arial, sans-serif;
           margin: 2rem auto; max-width: 72rem; padding: 0 1rem;
           color: #1f2328; line-height: 1.5; }
    h1 { border-bottom: 1px solid #d1d9e0; padding-bottom: .3rem; }
    h2 { margin-top: 2rem; }
    p.intro { color: #59636e; }
    table { border-collapse: collapse; width: 100%; margin: 1rem 0; }
    th, td { border: 1px solid #d1d9e0; padding: .5rem .75rem;
             text-align: left; vertical-align: top; }
    th { background: #f6f8fa; }
    td.npv { text-align: right; font-variant-numeric: tabular-nums;
             white-space: nowrap; }
    .title { font-weight: 600; }
    .desc { color: #59636e; font-size: .85rem; margin-top: .25rem; }
    code { background: #f6f8fa; border-radius: 4px; padding: .1rem .3rem;
           font-size: .85em; white-space: nowrap; }
    footer { color: #59636e; font-size: .85rem; margin-top: 2rem; }
"""


def build_html(rows=None) -> str:
    if rows is None:
        rows = compute_rows()
    out = []
    out.append("<!DOCTYPE html>")
    out.append('<html lang="en">')
    out.append("<head>")
    out.append('<meta charset="utf-8">')
    out.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
    out.append("<title>Quantra Functional Parity Catalog</title>")
    out.append("<!-- GENERATED FILE - do not edit by hand."
                " Regenerate with tests/functional/generate_catalog.py -->")
    out.append("<style>")
    out.append(HTML_STYLE)
    out.append("</style>")
    out.append("</head>")
    out.append("<body>")
    out.append("<h1>Functional Parity Catalog</h1>")
    out.append(f'<p class="intro">{html.escape(INTRO)}</p>')
    for family, cases in rows:
        out.append(f"<h2>{html.escape(family)}</h2>")
        out.append("<table>")
        out.append("<thead><tr><th>Case</th><th>What it exercises</th>"
                   "<th>Request JSON</th><th>QuantLib NPV</th></tr></thead>")
        out.append("<tbody>")
        for row in cases:
            exercises = " ".join(
                f"<code>{html.escape(tag)}</code>" for tag in row["exercises"]
            )
            link_target = DATA_RELATIVE_PREFIX + row["request"]
            link_name = Path(row["request"]).name
            out.append("<tr>")
            out.append(
                f'<td><div class="title">{html.escape(row["title"])}</div>'
                f'<div class="desc">{html.escape(row["description"])}</div></td>'
            )
            out.append(f"<td>{exercises}</td>")
            out.append(
                f'<td><a href="{html.escape(link_target, quote=True)}">'
                f"{html.escape(link_name)}</a></td>"
            )
            out.append(f'<td class="npv">{row["npv_text"]}</td>')
            out.append("</tr>")
        out.append("</tbody>")
        out.append("</table>")
    out.append(
        f"<footer>{sum(len(c) for _, c in rows)} cases across {len(rows)} "
        "family(ies). NPVs are the QuantLib reference values the server must "
        "reproduce within 0.01.</footer>"
    )
    out.append("</body>")
    out.append("</html>")
    return "\n".join(out) + "\n"


def main() -> int:
    rows = compute_rows()
    md = build_markdown(rows)
    html_text = build_html(rows)
    (FUNCTIONAL_DIR / "CATALOG.md").write_text(md)
    (FUNCTIONAL_DIR / "catalog.html").write_text(html_text)
    total = sum(len(c) for _, c in rows)
    print(f"Wrote CATALOG.md and catalog.html ({total} cases, "
          f"{len(rows)} family(ies)).")
    for family, cases in rows:
        for row in cases:
            print(f"  {row['id']}: {row['npv_text']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
