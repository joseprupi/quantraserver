"""The structured-note workflow example (examples/structured_note_workflow/)
stays runnable: price_note_from_quotes.py is run as a user runs it, against
the test server, on the shipped synthetic Scotia sheets at a small path
count, and its printed table is parsed back.

Locked here: exit 0; the --closes step (the correlation matrix estimated
from the shipped NDX / RTY closes, pasted into every pricing request in
place of the booked 0.747); the grid and the SVI prices within 3 combined
standard errors of each other (same stream, same model: the test suite's
repricing test holds them to one); the `vanilla_repricing` block in both
written responses with every sheet quote of the vol-surface calibration;
the --flat comparison row; the request bodies on disk (replayable by
price_note_from_quotes.sh) and the shipped sheet files equal to the
generator's svi_scotia_ndx / svi_scotia_rty output (nothing in them is
market data).
"""

import json
import re
import subprocess
import sys
from pathlib import Path

from ql_reference import load_json

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "tools"))

import make_synthetic_quote_sheet as gen  # noqa: E402

SCRIPT = REPO / "examples" / "structured_note_workflow" / "price_note_from_quotes.py"
SHEETS = {"ndx": gen.SYNTHETIC_SVI_SCOTIA_NDX, "rty": gen.SYNTHETIC_SVI_SCOTIA_RTY}
PATHS = 2048

ROW = re.compile(
    r"^\s+(?P<run>grid|svi|flat)\s+(?P<npv>-?\d+\.\d+)\s+(?P<se>\d+\.\d+)\s+(?P<status>\w+)\s+"
    r"(?P<p_call>\d\.\d+)\s+(?P<p_breach>\d\.\d+)\s+(?P<bond>-?\d+\.\d)\s+(?P<coupons>-?\d+\.\d)\s+"
    r"(?P<loss>-?\d+\.\d)\s+(?P<inside>\d+)\s*/\s*(?P<within>\d+)\s*/\s*(?P<count>\d+)\s+(?P<err>\d+\.\d+) pts$")


def test_shipped_sheets_equal_the_generator(data_dir):
    for name, preset in SHEETS.items():
        f = load_json(data_dir / "vol_surface" / f"synthetic_scotia_{name}_sheet.json")
        req = gen.build_request(preset)
        assert f["sheet"] == req["sheet"]
        assert f["spot"] == req["pricing"]["quotes"][0]
        assert f["as_of_date"] == req["pricing"]["as_of_date"]


def test_workflow_script_runs_end_to_end(client, data_dir, tmp_path):
    out_dir = tmp_path / "bodies"
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--url", client.base_url, "--paths", str(PATHS),
         "--out-dir", str(out_dir), "--flat", "--closes",
         "--sheet-ndx", str(data_dir / "vol_surface" / "synthetic_scotia_ndx_sheet.json"),
         "--sheet-rty", str(data_dir / "vol_surface" / "synthetic_scotia_rty_sheet.json"),
         "--note", str(data_dir / "structured_note" /
                       "scotia_autocallable_trigger_ndx_rty_us06419jme72.json")],
        capture_output=True, text=True, timeout=900)
    assert proc.returncode == 0, proc.stdout[-2000:] + proc.stderr[-2000:]
    out = proc.stdout
    for step in ("(a)", "(b)", "(c)", "(d)", "(e)", "(f)", "(g)"):
        assert step in out, step
    assert "(b) correlation NDX_RTY from" in out
    assert "shrinkage None" in out

    rows = {m["run"]: m.groupdict() for m in map(ROW.match, out.splitlines()) if m}
    assert set(rows) == {"grid", "svi", "flat"}, out
    grid, svi, flat = rows["grid"], rows["svi"], rows["flat"]
    se = (float(grid["se"]) ** 2 + float(svi["se"]) ** 2) ** 0.5
    assert abs(float(grid["npv"]) - float(svi["npv"])) <= 3.0 * se, (grid["npv"], svi["npv"], se)
    for r in (grid, svi, flat):
        assert r["status"] == "Alive"
        assert 0.0 < float(r["p_call"]) < 1.0 and 0.0 < float(r["p_breach"]) < 1.0
        assert 0 <= int(r["inside"]) <= int(r["within"]) <= int(r["count"])
        assert int(r["count"]) == 189 + 194  # the quotes /calibrate-vol-surface used
    # Local vol reprices the smile; a flat vol does not (the point of level 2).
    assert int(grid["within"]) > 0.9 * int(grid["count"])
    assert int(flat["within"]) < int(grid["within"])
    assert float(flat["err"]) > float(grid["err"])

    # The bodies on disk: every call's request and response, replayable.
    names = {"correlation", "implied_forwards_NDX", "implied_forwards_RTY", "vol_surface_NDX",
             "vol_surface_RTY", "price_note_grid", "price_note_svi", "price_note_flat"}
    for name in names:
        assert (out_dir / f"{name}_request.json").is_file(), name
        assert (out_dir / f"{name}_response.json").is_file(), name
    for name in ("grid", "svi", "flat"):
        req = load_json(out_dir / f"price_note_{name}_request.json")
        assert req["diagnostics"]["vanilla_repricing"] is True
        assert [s["underlying_id"] for s in req["diagnostics"]["vanilla_repricing_sheets"]] == ["NDX", "RTY"]
        assert req["notes"][0]["monte_carlo"]["paths"] == PATHS
        assert ("steps_per_year" in req["notes"][0]["monte_carlo"]) == (name != "flat")
        ids = {u["id"]: u["dividend_yield_curve_id"] for u in req["pricing"]["equity"]["equity_underlyings"]}
        assert ids == {"NDX": "NDX_DIV_IMPLIED", "RTY": "RTY_DIV_IMPLIED"}
        curve_ids = [c["id"] for c in req["pricing"]["rates"]["curves"]]
        assert set(curve_ids) == {"USD_TSY", "NDX_DIV_IMPLIED", "RTY_DIV_IMPLIED"}
        # The calibrated matrix replaced the booked one in every pricing request.
        corr = load_json(out_dir / "correlation_response.json")["correlation_spec"]
        assert corr["labels"] == ["NDX", "RTY"]
        assert abs(corr["values"]["values"][1] - 0.7472070756883675) < 1e-9
        assert req["pricing"]["correlations"]["matrices"] == [corr]
        assert req["notes"][0]["correlation_id"] == "NDX_RTY"
        note = load_json(out_dir / f"price_note_{name}_response.json")["notes"][0]
        vr = note["vanilla_repricing"]
        assert vr["count"] == int(rows[name]["count"])
        assert abs(note["npv"] - float(rows[name]["npv"])) < 0.01
        assert [u["underlying_id"] for u in vr["underlyings"]] == ["NDX", "RTY"]
    grid_req = load_json(out_dir / "price_note_grid_request.json")
    svi_req = load_json(out_dir / "price_note_svi_request.json")
    assert {v["payload_type"] for v in grid_req["pricing"]["volatility"]["vol_surfaces"]} == {"BlackVolSpec"}
    assert {v["payload_type"] for v in svi_req["pricing"]["volatility"]["vol_surfaces"]} == {"SviVolSpec"}
    assert grid_req["notes"] == svi_req["notes"]
    # The forwards the surfaces were fitted on are the ones pasted from step (b).
    fwd = load_json(out_dir / "implied_forwards_NDX_response.json")
    vs_req = load_json(out_dir / "vol_surface_NDX_request.json")
    assert vs_req["forwards"] == fwd["expiries"]
    assert vs_req["vol_surface_id"] == "NDX_VOL"


def test_workflow_script_reports_engine_errors(client, data_dir, tmp_path):
    """A rejected request ends the script with the engine's message and a
    non-zero exit: a sheet with no quotes is a 400 of
    /calibrate-implied-forwards. A sheet whose underlying is not in the note
    is refused by the script itself before any call."""
    sheet = load_json(data_dir / "vol_surface" / "synthetic_scotia_ndx_sheet.json")
    empty = tmp_path / "empty_sheet.json"
    empty.write_text(json.dumps({**sheet, "sheet": {**sheet["sheet"], "quotes": []}}))
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--url", client.base_url, "--paths", "256",
         "--out-dir", str(tmp_path / "bodies"), "--sheet-ndx", str(empty)],
        capture_output=True, text=True, timeout=300)
    assert proc.returncode == 2, proc.stdout[-500:] + proc.stderr[-500:]
    assert "HTTP 400" in proc.stderr and "quotes" in proc.stderr, proc.stderr
    assert (tmp_path / "bodies" / "implied_forwards_NDX_response.json").is_file()

    # A closes file whose series are not the note's underlyings is refused
    # by the script before any call.
    closes = load_json(data_dir / "correlation" / "ndx_rty_spx_closes_2025-10-17_to_2026-10-19.json")
    three = tmp_path / "three.json"
    three.write_text(json.dumps(closes))
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--url", client.base_url, "--paths", "256",
         "--out-dir", str(tmp_path / "bodies3"), "--closes", str(three)],
        capture_output=True, text=True, timeout=300)
    assert proc.returncode != 0
    assert "SPX" in proc.stderr and "not the note's underlyings" in proc.stderr

    spx = tmp_path / "spx_sheet.json"
    spx.write_text(json.dumps({**sheet, "sheet": {**sheet["sheet"], "underlying_id": "SPX"}}))
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--url", client.base_url, "--paths", "256",
         "--out-dir", str(tmp_path / "bodies2"), "--sheet-ndx", str(spx)],
        capture_output=True, text=True, timeout=300)
    assert proc.returncode != 0
    assert "SPX" in proc.stderr
