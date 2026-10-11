"""Rebump greeks are independent of how a curve pillar carries its quote.

A curve pillar can carry its rate inline (`rate`) or reference an entry of
`pricing.quotes[]` by `quote_id`. The two spellings are the same market, so a
plain price and every `swaption_pricing_rebump` greek must be identical for
both. The rebump path shifts every curve quote by +/-1bp; a pillar written with
`quote_id` used to receive that shift twice (once in the quote registry and
once again when the pillar resolved it), which doubled the rebump dv01 and
quadrupled gamma for curves written with `quote_id`, and gave mixed curves a
lopsided shock. Plain pricing (no bump) was never affected.

Each test derives the quote_id spelling programmatically from the inline
fixture, so the only difference between the two requests is the spelling.
"""

import copy

import pytest

from ql_reference import load_json

FIXTURE = "swaption/swpt_eur_1y5y_payer_near_atm_physical.json"
REBUMP_GREEKS = ("dv01", "gamma", "vega", "theta")
REL_TOL = 1e-9

# Pillar types whose inline value slot and quote_id slot carry the same number
# (a FutureHelper `rate` is converted to a price, so it is not interchangeable).
_INLINE_KEY = {
    "DepositHelper": "rate",
    "FRAHelper": "rate",
    "SwapHelper": "rate",
    "OISHelper": "rate",
    "DatedOISHelper": "rate",
    "BondHelper": "price",
}


def _with_options(request, **flags):
    out = copy.deepcopy(request)
    out["pricing"].setdefault("options", {}).update(flags)
    return out


def _row(client, request):
    response = client.price("swaption", request)
    rows = response.get("swaptions")
    assert isinstance(rows, list) and len(rows) == 1, response
    return rows[0]


def _quote_id_twin(request, select=lambda index: True):
    """Move the inline value of every selected pillar into pricing.quotes[]
    and reference it via quote_id. `select(index)` picks which pillars move."""
    out = copy.deepcopy(request)
    pricing = out["pricing"]
    quotes = pricing.setdefault("quotes", [])
    moved = 0
    for curve in pricing["rates"]["curves"]:
        for index, wrapper in enumerate(curve["points"]):
            if not select(index):
                continue
            point_type = wrapper["point_type"]
            assert point_type in _INLINE_KEY, f"unsupported pillar type {point_type}"
            point = wrapper["point"]
            key = _INLINE_KEY[point_type]
            assert key in point and "quote_id" not in point, point
            quote_id = f"{curve['id']}_{point_type}_{index}"
            quotes.append({"id": quote_id, "value": point.pop(key), "quote_type": "Curve"})
            point["quote_id"] = quote_id
            moved += 1
    assert moved > 0
    return out


def _rebump_greeks(client, request):
    row = _row(client, _with_options(request, swaption_pricing_rebump=True))
    for key in REBUMP_GREEKS:
        assert key in row, f"rebump response lacks {key!r}: {row}"
    return {key: row[key] for key in REBUMP_GREEKS}


def _assert_same_greeks(inline, twin, label):
    for key in REBUMP_GREEKS:
        assert twin[key] == pytest.approx(inline[key], rel=REL_TOL, abs=1e-12), (
            f"{label}: {key} inline {inline[key]!r} vs quote_id {twin[key]!r} "
            f"(ratio {twin[key] / inline[key] if inline[key] else float('nan')})"
        )


def test_quote_id_pillars_price_identically(client, data_dir):
    request = load_json(data_dir / FIXTURE)
    twin = _quote_id_twin(request)
    assert twin["pricing"]["quotes"], twin
    inline_npv = _row(client, request)["npv"]
    twin_npv = _row(client, twin)["npv"]
    assert twin_npv == pytest.approx(inline_npv, abs=1e-9), (inline_npv, twin_npv)


def test_quote_id_pillars_rebump_greeks_identical(client, data_dir):
    """All pillars by quote_id: the +/-1bp rebump shock must be applied once,
    exactly as for inline pillars."""
    request = load_json(data_dir / FIXTURE)
    inline = _rebump_greeks(client, request)
    twin = _rebump_greeks(client, _quote_id_twin(request))
    _assert_same_greeks(inline, twin, "all pillars by quote_id")


def test_mixed_inline_and_quote_id_pillars_rebump_greeks_identical(client, data_dir):
    """Half the pillars inline, half by quote_id: the curve must still receive
    one parallel 1bp shock, not a lopsided one."""
    request = load_json(data_dir / FIXTURE)
    inline = _rebump_greeks(client, request)
    mixed = _rebump_greeks(client, _quote_id_twin(request, select=lambda i: i % 2 == 0))
    _assert_same_greeks(inline, mixed, "mixed inline/quote_id pillars")
