#!/usr/bin/env python3
"""Convert a Cboe DataShop "Option EOD Summary" CSV into an OptionQuoteSheet.

Reads one underlying's end-of-day option summary (one row per option series:
call or put, expiration, strike) and writes the JSON pieces a
`/calibrate-implied-forwards` request needs: the `sheet` (OptionQuoteSheet:
one line per expiration x strike with the call and put sides side by side),
the `spot` (a QuoteSpec for `pricing.quotes`) and the `as_of_date`. The
discounting curve and the request wrapper are the caller's (see README.md);
no market data is invented here. Licensed vendor files are never committed.

Expected columns (the product's field list; names are matched after
lower-casing and replacing spaces/dashes by underscores, and the variants
in COLUMN_CANDIDATES are accepted — the ones actually used are printed and
written under "source.columns"):

    underlying_symbol, quote_date, root, expiration, strike, option_type,
    open, high, low, close, trade_volume, vwap, open_interest,
    bid_size_1545, bid_1545, ask_size_1545, ask_1545,
    underlying_bid_1545, underlying_ask_1545, active_underlying_price_1545,
    bid_size_eod, bid_eod, ask_size_eod, ask_eod,
    underlying_bid_eod, underlying_ask_eod,
    [implied_volatility_1545, delta_1545, gamma_1545, theta_1545, vega_1545, rho_1545]

Snapshot: `--snapshot eod` (default) takes bid_eod/ask_eod and the spot as
the mid of underlying_bid_eod/underlying_ask_eod; `--snapshot 1545` takes
bid_1545/ask_1545 and active_underlying_price_1545 (falling back to the
15:45 underlying bid/ask mid). `--spot` overrides the spot in either case.
A file with several roots (e.g. NDX monthlies and NDXP weeklies, which
settle differently) must be narrowed with `--root`; a file with several
quote dates must be narrowed with `--quote-date`.

Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import sys
from pathlib import Path

COLUMN_CANDIDATES = {
    "underlying_symbol": ["underlying_symbol", "underlying", "symbol", "ticker"],
    "quote_date": ["quote_date", "quotedate", "date", "trade_date", "as_of"],
    "root": ["root", "option_root", "symbol_root", "class"],
    "expiration": ["expiration", "expiry", "expiration_date", "expiry_date", "exdate", "maturity"],
    "strike": ["strike", "strike_price", "k"],
    "option_type": ["option_type", "type", "cp", "call_put", "putcall", "put_call", "right",
                    "cp_flag", "callput"],
    "trade_volume": ["trade_volume", "volume", "total_volume", "vol"],
    "open_interest": ["open_interest", "oi", "openinterest"],
    "bid_eod": ["bid_eod", "eod_bid", "bid", "bid_price"],
    "ask_eod": ["ask_eod", "eod_ask", "ask", "ask_price"],
    "bid_1545": ["bid_1545", "bid_15_45", "bid1545"],
    "ask_1545": ["ask_1545", "ask_15_45", "ask1545"],
    "underlying_bid_eod": ["underlying_bid_eod", "underlying_bid"],
    "underlying_ask_eod": ["underlying_ask_eod", "underlying_ask"],
    "underlying_bid_1545": ["underlying_bid_1545"],
    "underlying_ask_1545": ["underlying_ask_1545"],
    "active_underlying_price_1545": ["active_underlying_price_1545", "active_underlying_price",
                                     "underlying_price_1545", "underlying_price",
                                     "underlying_close", "close_underlying"],
}

CALL_VALUES = {"c", "call", "calls"}
PUT_VALUES = {"p", "put", "puts"}


def normalize(name: str) -> str:
    return name.strip().lower().replace(" ", "_").replace("-", "_").replace(".", "_").replace("/", "_")


def resolve_columns(header: list) -> dict:
    """{canonical: actual header} for every canonical column found."""
    norm = {normalize(h): h for h in header}
    found = {}
    for canonical, candidates in COLUMN_CANDIDATES.items():
        for cand in candidates:
            if cand in norm:
                found[canonical] = norm[cand]
                break
    return found


def parse_date(text: str) -> str:
    text = text.strip()
    for fmt in ("%Y-%m-%d", "%m/%d/%Y", "%Y%m%d", "%d-%b-%Y", "%Y/%m/%d"):
        try:
            return dt.datetime.strptime(text, fmt).date().isoformat()
        except ValueError:
            continue
    raise ValueError(f"unrecognised date '{text}'")


def parse_float(text) -> float | None:
    if text is None:
        return None
    text = str(text).strip()
    if text == "" or text.lower() in ("na", "nan", "null", "none"):
        return None
    return float(text)


class ConversionError(Exception):
    pass


def convert(rows: list, header: list, *, snapshot: str = "eod", spot: float | None = None,
            root: str | None = None, quote_date: str | None = None,
            underlying_id: str | None = None, sheet_id: str | None = None,
            source_name: str = "") -> dict:
    cols = resolve_columns(header)
    for needed in ("expiration", "strike", "option_type"):
        if needed not in cols:
            raise ConversionError(
                f"column for '{needed}' not found; accepted names: "
                f"{COLUMN_CANDIDATES[needed]} (have: {header})")
    if snapshot == "eod":
        bid_col, ask_col = cols.get("bid_eod"), cols.get("ask_eod")
    elif snapshot == "1545":
        bid_col, ask_col = cols.get("bid_1545"), cols.get("ask_1545")
    else:
        raise ConversionError("snapshot must be 'eod' or '1545'")
    if bid_col is None or ask_col is None:
        raise ConversionError(
            f"bid/ask columns for snapshot '{snapshot}' not found (accepted: "
            f"{COLUMN_CANDIDATES['bid_' + snapshot]} / {COLUMN_CANDIDATES['ask_' + snapshot]})")

    # Narrow by root and quote date.
    roots = sorted({r[cols["root"]].strip() for r in rows if r.get(cols["root"])}) \
        if "root" in cols else []
    if root is not None:
        if "root" not in cols:
            raise ConversionError("--root given but the file has no root column")
        rows = [r for r in rows if r[cols["root"]].strip() == root]
        if not rows:
            raise ConversionError(f"no rows with root '{root}' (roots present: {roots})")
    elif len(roots) > 1:
        raise ConversionError(
            f"several roots in the file: {roots}; pick one with --root (monthlies and "
            "weeklies settle differently and must not be mixed in one sheet)")
    dates = sorted({parse_date(r[cols["quote_date"]]) for r in rows if r.get(cols["quote_date"])}) \
        if "quote_date" in cols else []
    if quote_date is not None:
        if "quote_date" in cols:
            rows = [r for r in rows if parse_date(r[cols["quote_date"]]) == quote_date]
            if not rows:
                raise ConversionError(f"no rows dated {quote_date} (dates present: {dates})")
        as_of = quote_date
    elif len(dates) == 1:
        as_of = dates[0]
    elif len(dates) > 1:
        raise ConversionError(f"several quote dates in the file: {dates}; pick one with --quote-date")
    else:
        raise ConversionError("no quote_date column; pass --quote-date YYYY-MM-DD")

    symbols = sorted({r[cols["underlying_symbol"]].strip() for r in rows
                      if r.get(cols["underlying_symbol"])}) if "underlying_symbol" in cols else []
    if underlying_id is None:
        if len(symbols) == 1:
            underlying_id = symbols[0]
        elif root is not None:
            underlying_id = root
        elif len(roots) == 1:
            underlying_id = roots[0]
        else:
            raise ConversionError("cannot infer the underlying id; pass --underlying-id")

    # Spot.
    spot_from = "--spot"
    if spot is None:
        def mid(bcol, acol, r):
            b, a = parse_float(r.get(bcol)), parse_float(r.get(acol))
            return 0.5 * (b + a) if b is not None and a is not None else None
        first = rows[0]
        candidates = []
        if snapshot == "1545":
            candidates = [("active_underlying_price_1545",
                           lambda r: parse_float(r.get(cols.get("active_underlying_price_1545")))),
                          ("underlying_bid_1545/underlying_ask_1545 mid",
                           lambda r: mid(cols.get("underlying_bid_1545"),
                                         cols.get("underlying_ask_1545"), r)),
                          ("underlying_bid_eod/underlying_ask_eod mid",
                           lambda r: mid(cols.get("underlying_bid_eod"),
                                         cols.get("underlying_ask_eod"), r))]
        else:
            candidates = [("underlying_bid_eod/underlying_ask_eod mid",
                           lambda r: mid(cols.get("underlying_bid_eod"),
                                         cols.get("underlying_ask_eod"), r)),
                          ("active_underlying_price_1545",
                           lambda r: parse_float(r.get(cols.get("active_underlying_price_1545"))))]
        for label, fn in candidates:
            try:
                value = fn(first)
            except (TypeError, ValueError, AttributeError):
                value = None
            if value is not None and value > 0.0:
                spot, spot_from = value, label
                break
        if spot is None:
            raise ConversionError(
                "no usable underlying price column for the spot; pass --spot")

    # Pair calls and puts by (expiration, strike).
    lines = {}
    skipped = 0
    for r in rows:
        try:
            expiry = parse_date(r[cols["expiration"]])
            strike = parse_float(r[cols["strike"]])
            kind = r[cols["option_type"]].strip().lower()
        except (ValueError, KeyError, AttributeError):
            skipped += 1
            continue
        if strike is None:
            skipped += 1
            continue
        if kind in CALL_VALUES:
            side = "call"
        elif kind in PUT_VALUES:
            side = "put"
        else:
            skipped += 1
            continue
        key = (expiry, strike)
        line = lines.setdefault(key, {"expiry": expiry, "strike": strike})
        bid, ask = parse_float(r.get(bid_col)), parse_float(r.get(ask_col))
        if bid is not None:
            line[f"{side}_bid"] = bid
        if ask is not None:
            line[f"{side}_ask"] = ask
        if "trade_volume" in cols:
            v = parse_float(r.get(cols["trade_volume"]))
            if v is not None:
                line[f"{side}_volume"] = v
        if "open_interest" in cols:
            oi = parse_float(r.get(cols["open_interest"]))
            if oi is not None:
                line[f"{side}_open_interest"] = oi
    quotes = [lines[k] for k in sorted(lines)]
    if not quotes:
        raise ConversionError("no option rows could be read")

    sheet_id = sheet_id or f"{underlying_id}_{as_of}_{snapshot}"
    return {
        "as_of_date": as_of,
        "spot": {"id": f"{underlying_id}_SPOT", "kind": "Price", "value": spot,
                 "quote_type": "Curve"},
        "sheet": {
            "id": sheet_id,
            "underlying_id": underlying_id,
            "option_style": "European",
            "quotes": quotes,
        },
        "source": {
            "file": source_name,
            "snapshot": snapshot,
            "rows_read": len(rows),
            "rows_skipped": skipped,
            "lines": len(quotes),
            "expiries": sorted({q["expiry"] for q in quotes}),
            "roots_present": roots,
            "root_used": root if root is not None else (roots[0] if roots else None),
            "spot_from": spot_from,
            "columns": {k: cols[k] for k in sorted(cols)},
            "bid_column": bid_col,
            "ask_column": ask_col,
            "note": ("option_style European is assumed (index options); set it to American "
                     "for single-stock sheets, which /calibrate-implied-forwards rejects"),
        },
    }


def convert_file(path: Path, **kwargs) -> dict:
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        header = reader.fieldnames or []
        rows = list(reader)
    return convert(rows, header, source_name=path.name, **kwargs)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("csv", type=Path, help="Cboe DataShop Option EOD Summary CSV")
    ap.add_argument("--snapshot", choices=["eod", "1545"], default="eod")
    ap.add_argument("--spot", type=float, default=None, help="override the spot")
    ap.add_argument("--root", default=None, help="option root to keep (e.g. NDX, not NDXP)")
    ap.add_argument("--quote-date", default=None, help="YYYY-MM-DD, when the file has several")
    ap.add_argument("--underlying-id", default=None, help="EquityUnderlyingSpec id to reference")
    ap.add_argument("--sheet-id", default=None)
    ap.add_argument("--out", type=Path, default=None, help="output JSON (default: stdout)")
    args = ap.parse_args(argv)
    try:
        out = convert_file(args.csv, snapshot=args.snapshot, spot=args.spot, root=args.root,
                           quote_date=args.quote_date, underlying_id=args.underlying_id,
                           sheet_id=args.sheet_id)
    except (ConversionError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    src = out["source"]
    print(f"{src['lines']} quote lines over {len(src['expiries'])} expiries from "
          f"{src['rows_read']} rows ({src['rows_skipped']} skipped); spot {out['spot']['value']} "
          f"from {src['spot_from']}; bid/ask columns {src['bid_column']}/{src['ask_column']}",
          file=sys.stderr)
    text = json.dumps(out, indent=2) + "\n"
    if args.out:
        args.out.write_text(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
