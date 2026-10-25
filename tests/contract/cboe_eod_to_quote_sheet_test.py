"""Unit tests for tools/option_quotes/cboe_eod_to_quote_sheet.py.

Pure Python: no server, no QuantLib. A hand-written CSV in the Cboe DataShop
Option EOD Summary column format (every column of the product's field list)
is converted under both snapshots, with the spot from the underlying
columns or from --spot, and the pairing, root / date narrowing and
one-sided series are checked. No vendor data: the numbers are made up.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "option_quotes"))

import cboe_eod_to_quote_sheet as tool  # noqa: E402

HEADER = ("underlying_symbol,quote_date,root,expiration,strike,option_type,open,high,low,close,"
          "trade_volume,vwap,open_interest,bid_size_1545,bid_1545,ask_size_1545,ask_1545,"
          "underlying_bid_1545,underlying_ask_1545,active_underlying_price_1545,"
          "bid_size_eod,bid_eod,ask_size_eod,ask_eod,underlying_bid_eod,underlying_ask_eod,"
          "implied_volatility_1545,delta_1545,gamma_1545,theta_1545,vega_1545,rho_1545")


def row(root, expiration, strike, cp, bid1545, ask1545, bid_eod, ask_eod, oi, vol=10,
        date="2026-09-30", u1545=("24000.5", "24001.5", "24001.0"), ueod=("24010.0", "24012.0")):
    return (f"^NDX,{date},{root},{expiration},{strike},{cp},1,2,0.5,1.5,{vol},1.2,{oi},"
            f"5,{bid1545},5,{ask1545},{u1545[0]},{u1545[1]},{u1545[2]},"
            f"5,{bid_eod},5,{ask_eod},{ueod[0]},{ueod[1]},0.2,0.5,0.0001,-1,10,1")


ROWS = [
    row("NDX", "2026-10-16", 23000, "C", 1100.0, 1110.0, 1101.0, 1111.0, 500),
    row("NDX", "2026-10-16", 23000, "P", 90.0, 92.0, 91.0, 93.0, 800),
    row("NDX", "2026-10-16", 24000, "C", 400.0, 405.0, 401.0, 406.0, 1500),
    row("NDX", "2026-10-16", 24000, "P", 390.0, 395.0, 391.0, 396.0, 1600),
    row("NDX", "2026-10-16", 25000, "C", 80.0, 82.0, 81.0, 83.0, 300),
    row("NDX", "2026-10-16", 25000, "P", 1050.0, 1060.0, 1051.0, 1061.0, 200),
    row("NDX", "2026-11-20", 24000, "C", 700.0, 710.0, 701.0, 711.0, 900),
    row("NDX", "2026-11-20", 24000, "P", 650.0, 660.0, 651.0, 661.0, 950),
    row("NDX", "2026-11-20", 26000, "C", 30.0, 32.0, 0.0, 33.0, 50),      # zero EOD bid
    row("NDX", "2026-11-20", 26000, "P", 2000.0, 2020.0, 2001.0, 2021.0, 40),
    row("NDX", "2026-11-20", 22000, "P", 150.0, 155.0, 151.0, 156.0, 120),  # put only
]
WEEKLY = [row("NDXP", "2026-10-02", 24000, "C", 100.0, 101.0, 100.5, 101.5, 10),
          row("NDXP", "2026-10-02", 24000, "P", 95.0, 96.0, 95.5, 96.5, 10)]


def write_csv(path: Path, rows) -> Path:
    path.write_text(HEADER + "\n" + "\n".join(rows) + "\n")
    return path


def test_eod_snapshot_pairs_calls_and_puts(tmp_path):
    out = tool.convert_file(write_csv(tmp_path / "ndx.csv", ROWS))
    assert out["as_of_date"] == "2026-09-30"
    assert out["spot"] == {"id": "^NDX_SPOT", "kind": "Price", "value": 24011.0,
                           "quote_type": "Curve"}
    assert out["source"]["spot_from"] == "underlying_bid_eod/underlying_ask_eod mid"
    assert out["source"]["bid_column"] == "bid_eod" and out["source"]["ask_column"] == "ask_eod"
    sheet = out["sheet"]
    assert sheet["underlying_id"] == "^NDX" and sheet["option_style"] == "European"
    assert sheet["id"] == "^NDX_2026-09-30_eod"
    quotes = sheet["quotes"]
    assert [(q["expiry"], q["strike"]) for q in quotes] == [
        ("2026-10-16", 23000.0), ("2026-10-16", 24000.0), ("2026-10-16", 25000.0),
        ("2026-11-20", 22000.0), ("2026-11-20", 24000.0), ("2026-11-20", 26000.0)]
    q = quotes[1]
    assert q == {"expiry": "2026-10-16", "strike": 24000.0,
                 "call_bid": 401.0, "call_ask": 406.0, "put_bid": 391.0, "put_ask": 396.0,
                 "call_volume": 10.0, "put_volume": 10.0,
                 "call_open_interest": 1500.0, "put_open_interest": 1600.0}
    # Zero EOD bid is kept as is; the engine's spread filter drops it.
    assert quotes[5]["call_bid"] == 0.0 and quotes[5]["call_ask"] == 33.0
    # A put-only series has no call side.
    assert "call_bid" not in quotes[3] and quotes[3]["put_bid"] == 151.0
    assert out["source"]["rows_read"] == 11 and out["source"]["rows_skipped"] == 0
    assert out["source"]["expiries"] == ["2026-10-16", "2026-11-20"]


def test_1545_snapshot_and_spot_override(tmp_path):
    path = write_csv(tmp_path / "ndx.csv", ROWS)
    out = tool.convert_file(path, snapshot="1545")
    assert out["spot"]["value"] == 24001.0
    assert out["source"]["spot_from"] == "active_underlying_price_1545"
    assert out["sheet"]["quotes"][1]["call_bid"] == 400.0
    assert out["sheet"]["quotes"][1]["put_ask"] == 395.0
    assert out["sheet"]["id"] == "^NDX_2026-09-30_1545"
    out = tool.convert_file(path, snapshot="1545", spot=23999.0, underlying_id="NDX",
                            sheet_id="my_sheet")
    assert out["spot"] == {"id": "NDX_SPOT", "kind": "Price", "value": 23999.0,
                           "quote_type": "Curve"}
    assert out["source"]["spot_from"] == "--spot"
    assert out["sheet"]["underlying_id"] == "NDX" and out["sheet"]["id"] == "my_sheet"


def test_several_roots_need_a_choice(tmp_path):
    path = write_csv(tmp_path / "mixed.csv", ROWS + WEEKLY)
    with pytest.raises(tool.ConversionError, match="several roots"):
        tool.convert_file(path)
    out = tool.convert_file(path, root="NDXP")
    assert out["sheet"]["underlying_id"] == "^NDX"
    assert [q["expiry"] for q in out["sheet"]["quotes"]] == ["2026-10-02"]
    assert out["source"]["root_used"] == "NDXP" and out["source"]["roots_present"] == ["NDX", "NDXP"]
    with pytest.raises(tool.ConversionError, match="no rows with root"):
        tool.convert_file(path, root="SPX")


def test_several_dates_need_a_choice(tmp_path):
    other = [row("NDX", "2026-10-16", 24000, "C", 1, 2, 1, 2, 1, date="2026-10-01"),
             row("NDX", "2026-10-16", 24000, "P", 1, 2, 1, 2, 1, date="2026-10-01")]
    path = write_csv(tmp_path / "two_days.csv", ROWS + other)
    with pytest.raises(tool.ConversionError, match="several quote dates"):
        tool.convert_file(path)
    out = tool.convert_file(path, quote_date="2026-10-01")
    assert out["as_of_date"] == "2026-10-01" and len(out["sheet"]["quotes"]) == 1


def test_column_name_variants_and_missing_columns(tmp_path):
    header = "Underlying Symbol,Quote Date,Expiry Date,Strike Price,Call/Put,Bid,Ask,OI"
    lines = ["RUT,09/30/2026,10/16/2026,2300,call,50.5,51.5,100",
             "RUT,09/30/2026,10/16/2026,2300,put,40.5,41.5,120"]
    path = tmp_path / "variant.csv"
    path.write_text(header + "\n" + "\n".join(lines) + "\n")
    with pytest.raises(tool.ConversionError, match="no usable underlying price"):
        tool.convert_file(path)
    out = tool.convert_file(path, spot=2310.0)
    assert out["as_of_date"] == "2026-09-30"
    assert out["sheet"]["underlying_id"] == "RUT"
    assert out["sheet"]["quotes"] == [{"expiry": "2026-10-16", "strike": 2300.0,
                                       "call_bid": 50.5, "call_ask": 51.5,
                                       "put_bid": 40.5, "put_ask": 41.5,
                                       "call_open_interest": 100.0, "put_open_interest": 120.0}]
    assert out["source"]["columns"]["option_type"] == "Call/Put"
    assert out["source"]["bid_column"] == "Bid"
    with pytest.raises(tool.ConversionError, match="snapshot '1545' not found"):
        tool.convert_file(path, snapshot="1545", spot=1.0)
    path.write_text("a,b,c\n1,2,3\n")
    with pytest.raises(tool.ConversionError, match="column for 'expiration' not found"):
        tool.convert_file(path)


def test_cli_writes_json(tmp_path, capsys):
    path = write_csv(tmp_path / "ndx.csv", ROWS)
    out = tmp_path / "sheet.json"
    assert tool.main([str(path), "--out", str(out)]) == 0
    data = json.loads(out.read_text())
    assert data["sheet"]["underlying_id"] == "^NDX" and len(data["sheet"]["quotes"]) == 6
    assert "6 quote lines over 2 expiries" in capsys.readouterr().err
    assert tool.main([str(path), "--root", "SPX"]) == 2
