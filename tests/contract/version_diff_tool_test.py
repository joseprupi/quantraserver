"""Unit tests for tools/version_diff/quantra_version_diff.py.

Pure comparison/report logic on canned responses, plus one end-to-end run of
the CLI against two throw-away in-process HTTP servers. None of these tests
use the Quantra server (no ``client`` fixture).
"""

import importlib.util
import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = REPO_ROOT / "tools" / "version_diff" / "quantra_version_diff.py"

_spec = importlib.util.spec_from_file_location("quantra_version_diff", TOOL_PATH)
vd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vd)


def _case(case_id="case_a", endpoint="price-vanilla-swap"):
    return {"id": case_id, "endpoint": endpoint, "request_file": f"{case_id}.json"}


def _reply(body, status=200):
    text = body if isinstance(body, str) else json.dumps(body)
    return {"status": status, "body": text, "error": None}


def _server(quantlib):
    return {"url": f"http://ql-{quantlib}", "meta_error": None,
            "meta": {"api_version": "0.6.0", "dependencies": {"quantlib": quantlib}}}


# --- flatten ---------------------------------------------------------------

def test_flatten_nested_objects_and_list_indexes():
    flat = vd.flatten({"swaps": [{"npv": 1.5, "legs": [{"npv": 2}, {"npv": 3}]}],
                       "as_of": "2025-01-02"})
    assert flat == {"swaps[0].npv": 1.5, "swaps[0].legs[0].npv": 2,
                    "swaps[0].legs[1].npv": 3, "as_of": "2025-01-02"}


def test_flatten_keeps_empty_containers_scalars_and_odd_keys():
    assert vd.flatten({"a": [], "b": {}, "c": None}) == {"a": [], "b": {}, "c": None}
    assert vd.flatten(3.0) == {"$": 3.0}
    assert vd.flatten([]) == {"$": []}
    assert vd.flatten({"a.b": {"x y": 1}}) == {'"a.b"."x y"': 1}


# --- compare_values --------------------------------------------------------

@pytest.mark.parametrize("old,new,expected", [
    (1.0, 1.0, vd.IDENTICAL),
    (1, 1.0, vd.IDENTICAL),                      # int vs float, same number
    (100.0, 100.0 * (1 + 5e-13), vd.NOISE),      # below relative tolerance
    (100.0, 100.0 * (1 + 5e-12), vd.CHANGED),    # above relative tolerance
    (0.0, 5e-13, vd.NOISE),                      # absolute floor (rel would be 1)
    (0.0, 5e-12, vd.CHANGED),
    (1e-20, -1e-20, vd.NOISE),
    (float("nan"), float("nan"), vd.IDENTICAL),
    (1.0, float("nan"), vd.CHANGED),
    ("2025-01-02", "2025-01-02", vd.IDENTICAL),
    ("2025-01-02", "2025-01-03", vd.VALUE_CHANGED),
    (True, False, vd.VALUE_CHANGED),
    (True, 1, vd.VALUE_CHANGED),                 # bool is not a number here
    (None, 0.0, vd.VALUE_CHANGED),
    ("1.0", 1.0, vd.VALUE_CHANGED),
    (None, None, vd.IDENTICAL),
])
def test_compare_values_categories(old, new, expected):
    assert vd.compare_values(old, new)[0] == expected


def test_compare_values_reports_abs_and_rel_diff():
    category, abs_diff, rel_diff = vd.compare_values(100.0, 110.0)
    assert category == vd.CHANGED
    assert abs_diff == pytest.approx(10.0)
    assert rel_diff == pytest.approx(10.0 / 110.0)
    assert vd.compare_values(1.0, float("inf")) == (vd.CHANGED, None, None)


def test_compare_values_honours_custom_tolerances():
    assert vd.compare_values(100.0, 100.001, rel_tol=1e-4)[0] == vd.NOISE
    assert vd.compare_values(100.0, 100.001, rel_tol=1e-6)[0] == vd.CHANGED
    assert vd.compare_values(0.0, 1e-9, abs_tol=1e-8)[0] == vd.NOISE


# --- compare_case ----------------------------------------------------------

def test_case_unchanged_and_noise_only():
    body = {"swaps": [{"npv": 1234.5, "ccy": "EUR"}]}
    same = vd.compare_case(_case(), _reply(body), _reply(body))
    assert same["outcome"] == vd.OUT_UNCHANGED
    assert same["counts"]["identical"] == 2 and same["max_abs_diff"] == 0.0

    noisy = vd.compare_case(_case(), _reply(body),
                            _reply({"swaps": [{"npv": 1234.5 * (1 + 2e-14), "ccy": "EUR"}]}))
    assert noisy["outcome"] == vd.OUT_NOISE
    assert [f["path"] for f in noisy[vd.NOISE]] == ["swaps[0].npv"]
    assert noisy[vd.CHANGED] == []


def test_case_changed_numeric_structural_and_non_numeric():
    old = {"swaps": [{"npv": 100.0, "fair_rate": 0.03, "gone": 1, "date": "2025-01-02"}]}
    new = {"swaps": [{"npv": 101.0, "fair_rate": 0.0301, "added": 2, "date": "2025-01-03"}]}
    rec = vd.compare_case(_case(), _reply(old), _reply(new))
    assert rec["outcome"] == vd.OUT_CHANGED
    # Largest relative difference first.
    assert [f["path"] for f in rec[vd.CHANGED]] == ["swaps[0].npv", "swaps[0].fair_rate"]
    assert rec[vd.CHANGED][0]["old"] == 100.0 and rec[vd.CHANGED][0]["new"] == 101.0
    assert rec[vd.ONLY_IN_OLD] == [{"path": "swaps[0].gone", "value": 1}]
    assert rec[vd.ONLY_IN_NEW] == [{"path": "swaps[0].added", "value": 2}]
    assert rec[vd.VALUE_CHANGED] == [
        {"path": "swaps[0].date", "old": "2025-01-02", "new": "2025-01-03"}]
    assert rec["max_abs_diff"] == pytest.approx(1.0)
    assert rec["counts"] == {"identical": 0, "ignored": 0, "noise": 0, "changed": 2,
                             "value_changed": 1, "only_in_old": 1, "only_in_new": 1}


def test_case_only_structural_difference_is_changed():
    rec = vd.compare_case(_case(), _reply({"a": 1}), _reply({"a": 1, "b": None}))
    assert rec["outcome"] == vd.OUT_CHANGED and rec["counts"]["only_in_new"] == 1


def test_case_ignore_path_patterns():
    import re
    rec = vd.compare_case(_case(), _reply({"npv": 1.0, "elapsed_ms": 3}),
                          _reply({"npv": 1.0, "elapsed_ms": 9}),
                          ignore=[re.compile(r"elapsed_ms$")])
    assert rec["outcome"] == vd.OUT_UNCHANGED and rec["counts"]["ignored"] == 1


def test_case_status_change_keeps_both_messages():
    rec = vd.compare_case(_case(), _reply({"swaps": [{"npv": 1.0}]}),
                          _reply("negative   rate\nnot allowed", status=422))
    assert rec["outcome"] == vd.OUT_STATUS_CHANGED
    assert (rec["old_status"], rec["new_status"]) == (200, 422)
    assert "npv" in rec["old_message"]
    assert "negative rate not allowed" in rec["new_message"]
    assert "counts" not in rec


def test_case_same_error_on_both_is_flagged_and_bodies_compared():
    err = _reply({"error": "curve 'x' not found"}, status=400)
    rec = vd.compare_case(_case(), err, err)
    assert rec["outcome"] == vd.OUT_UNCHANGED and rec["error_on_both"] is True
    other = vd.compare_case(_case(), err, _reply({"error": "different text"}, status=400))
    assert other["outcome"] == vd.OUT_CHANGED
    assert other[vd.VALUE_CHANGED][0]["path"] == "error"


def test_case_request_failure_and_non_json_body():
    down = {"status": None, "body": None, "error": "ConnectionError: refused"}
    rec = vd.compare_case(_case(), _reply({"a": 1}), down)
    assert rec["outcome"] == vd.OUT_REQUEST_FAILED
    assert rec["new_message"] == "ConnectionError: refused"
    text = vd.compare_case(_case(), _reply("plain old"), _reply("plain new"))
    assert text["outcome"] == vd.OUT_CHANGED
    assert text[vd.VALUE_CHANGED] == [{"path": "$body", "old": "plain old", "new": "plain new"}]


# --- report ----------------------------------------------------------------

def _sample_report():
    changed_old = {"swaps": [{"v": [float(i) for i in range(1, 16)]}]}
    changed_new = {"swaps": [{"v": [i * (1 + i * 1e-3) for i in range(1, 16)]}]}
    records = [
        vd.compare_case(_case("z_changed", "price-vanilla-swap"),
                        _reply(changed_old), _reply(changed_new)),
        vd.compare_case(_case("a_same", "price-vanilla-swap"),
                        _reply({"npv": 1.0}), _reply({"npv": 1.0})),
        vd.compare_case(_case("b_status", "price-fra"),
                        _reply({"npv": 1.0}), _reply("boom", status=500)),
        vd.compare_case(_case("c_nan", "price-fra"),
                        _reply('{"npv": 1.0}'), _reply('{"npv": NaN}')),
    ]
    return vd.build_report(_server("1.41"), _server("1.43"), records,
                           sources=["unit test"])


def test_report_summary_ordering_and_endpoint_rollup():
    report = _sample_report()
    assert report["summary"] == {"cases": 4, "unchanged": 1, "noise": 0, "changed": 2,
                                 "status_changed": 1, "request_failed": 0,
                                 "error_on_both": 0}
    assert [(c["endpoint"], c["id"]) for c in report["cases"]] == [
        ("price-fra", "b_status"), ("price-fra", "c_nan"),
        ("price-vanilla-swap", "a_same"), ("price-vanilla-swap", "z_changed")]
    rows = {r["endpoint"]: r for r in report["endpoints"]}
    assert list(rows) == ["price-fra", "price-vanilla-swap"]
    assert rows["price-vanilla-swap"]["changed"] == 1
    assert rows["price-vanilla-swap"]["unchanged"] == 1
    assert rows["price-vanilla-swap"]["max_rel_diff"] == pytest.approx(
        0.225 / 15.225, rel=1e-9)
    assert rows["price-fra"]["status_changed"] == 1
    assert vd.has_differences(report)


def test_report_without_differences():
    same = vd.compare_case(_case(), _reply({"npv": 1.0}), _reply({"npv": 1.0}))
    report = vd.build_report(_server("1.41"), _server("1.41"), [same])
    assert not vd.has_differences(report)
    assert "None." in vd.render_markdown(report)


def test_render_markdown_lists_noise_only_cases():
    noisy = vd.compare_case(_case("tiny"), _reply({"npv": 1000.0}),
                            _reply({"npv": 1000.0 * (1 + 4e-14)}))
    report = vd.build_report(_server("1.41"), _server("1.43"), [noisy])
    md = vd.render_markdown(report)
    assert report["summary"]["noise"] == 1 and not vd.has_differences(report)
    assert "## Noise-only cases" in md
    assert "- `price-vanilla-swap` — tiny (`tiny.json`): 1 fields, max abs diff" in md
    assert "## Changed cases\n\nNone." in md


def test_render_json_is_strict_json_with_full_detail():
    def no_constants(name):
        raise AssertionError(f"non-standard JSON constant {name}")

    data = json.loads(vd.render_json(_sample_report()), parse_constant=no_constants)
    assert data["format_version"] == vd.FORMAT_VERSION
    assert data["old"]["meta"]["dependencies"]["quantlib"] == "1.41"
    assert data["corpus"]["size"] == 4
    by_id = {c["id"]: c for c in data["cases"]}
    assert len(by_id["z_changed"]["changed"]) == 15          # never capped
    assert set(by_id["z_changed"]["changed"][0]) == {"path", "old", "new",
                                                     "abs_diff", "rel_diff"}
    assert by_id["c_nan"]["changed"][0]["new"] == "nan"
    assert by_id["c_nan"]["changed"][0]["rel_diff"] is None


def test_render_markdown_shape_and_cap():
    md = vd.render_markdown(_sample_report(), top=5)
    assert "API version 0.6.0, QuantLib 1.41" in md
    assert "API version 0.6.0, QuantLib 1.43" in md
    assert "**Corpus:** 4 requests" in md
    assert "| `price-vanilla-swap` | 2 | 1 | 0 | 1 | 0 | 0 |" in md
    assert "### `price-vanilla-swap` — z_changed" in md
    # Top 5 by relative difference are listed, the remaining 10 are counted.
    assert "| `swaps[0].v[14]` | 15.0 | 15.2" in md
    assert "`swaps[0].v[10]`" in md and "`swaps[0].v[9]`" not in md
    assert "... and 10 more changed numeric fields" in md
    assert "## Status changes" in md
    assert "old: HTTP 200" in md and "new: HTTP 500 — boom" in md
    assert "not a guarantee" in md


# --- corpus loading --------------------------------------------------------

def test_requests_dir_convention(tmp_path):
    (tmp_path / "price-fra" / "book").mkdir(parents=True)
    (tmp_path / "price-vanilla-swap").mkdir()
    (tmp_path / "price-fra" / "book" / "b.json").write_text("{}")
    (tmp_path / "price-fra" / "a.json").write_text("{}")
    (tmp_path / "price-vanilla-swap" / "s.json").write_text("{}")
    (tmp_path / "price-fra" / "notes.txt").write_text("ignored")
    (tmp_path / "stray.json").write_text("{}")
    cases, skipped = vd.load_requests_dir(tmp_path)
    assert [(c["endpoint"], c["id"]) for c in cases] == [
        ("price-fra", "price-fra/a.json"), ("price-fra", "price-fra/book/b.json"),
        ("price-vanilla-swap", "price-vanilla-swap/s.json")]
    assert len(skipped) == 1 and skipped[0].startswith("stray.json")


def test_catalog_corpus_resolves_every_manifest_case():
    cases = vd.load_catalog(REPO_ROOT)
    assert len(cases) > 100
    assert len({c["id"] for c in cases}) == len(cases)
    for case in cases:
        assert Path(case["path"]).is_file(), case["id"]
        assert case["endpoint"] and not case["endpoint"].startswith("/")
    endpoints = {c["endpoint"] for c in cases}
    assert {"price-vanilla-swap", "bootstrap-curves", "calendar-holidays"} <= endpoints


# --- CLI end to end against canned servers ---------------------------------

def _canned_server(quantlib, responses):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, status, payload):
            data = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self._send(200, {"api_version": "9.9.9",
                             "dependencies": {"quantlib": quantlib}})

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            self._send(*responses.get(self.path, (404, {"error": "no route"})))

        def log_message(self, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def test_cli_end_to_end(tmp_path, capsys):
    old = _canned_server("1.0", {"/price-fra": (200, {"fras": [{"npv": 10.0}]}),
                                 "/price-cds": (200, {"cds": [{"npv": 5.0}]})})
    new = _canned_server("2.0", {"/price-fra": (200, {"fras": [{"npv": 10.5}]}),
                                 "/price-cds": (200, {"cds": [{"npv": 5.0}]})})
    try:
        for endpoint in ("price-fra", "price-cds"):
            (tmp_path / "req" / endpoint).mkdir(parents=True)
            (tmp_path / "req" / endpoint / "one.json").write_text("{}")
        base = ["--requests", str(tmp_path / "req"), "--quiet", "--timeout", "5",
                "--json-out", str(tmp_path / "out.json"),
                "--md-out", str(tmp_path / "out.md")]
        old_url = f"http://127.0.0.1:{old.server_port}"
        new_url = f"http://127.0.0.1:{new.server_port}"

        assert vd.main(["--old", old_url, "--new", new_url] + base) == 0
        assert vd.main(["--old", old_url, "--new", new_url, "--fail-on-change"] + base) == 1
        report = json.loads((tmp_path / "out.json").read_text())
        assert report["old"]["meta"]["dependencies"]["quantlib"] == "1.0"
        assert report["new"]["meta"]["dependencies"]["quantlib"] == "2.0"
        assert report["summary"]["changed"] == 1 and report["summary"]["unchanged"] == 1
        changed = [c for c in report["cases"] if c["outcome"] == "changed"][0]
        assert changed["endpoint"] == "price-fra"
        assert changed["changed"][0]["path"] == "fras[0].npv"
        assert "QuantLib 2.0" in (tmp_path / "out.md").read_text()

        # Old vs old: nothing changes, --fail-on-change stays 0.
        assert vd.main(["--old", old_url, "--new", old_url, "--fail-on-change"] + base) == 0

        # An unreachable server is reported per request and does not abort.
        new.shutdown()
        new.server_close()
        assert vd.main(["--old", old_url, "--new", new_url, "--fail-on-change"] + base) == 1
        report = json.loads((tmp_path / "out.json").read_text())
        assert report["summary"]["request_failed"] == 2
        assert report["new"]["meta"] is None and report["new"]["meta_error"]
    finally:
        old.shutdown()
        old.server_close()
        capsys.readouterr()
