"""Response scalars equal to their schema default are still serialized.

FlatBuffers builders elide scalars that equal the schema default unless told
otherwise, and the JSON gateway prints only the fields present in the buffer.
Before the fix a response double that happened to be exactly 0.0 (or an int /
enum at its default) silently disappeared from the JSON. These tests pin the
fixed behaviour on three shapes: an int count of zero, an exact-0.0 double and
an enum at its default. `= null` optionals are a different mechanism and keep
being absent until set (response_optionals_test.py).
"""

import copy

from ql_reference import load_json


def test_calendar_holidays_empty_range_keeps_count_and_dates(client):
    # TARGET has no holidays in June 2024.
    request = {"calendar": "TARGET", "start_date": "2024-06-03",
               "end_date": "2024-06-28", "include_weekends": False}
    response = client.price("calendar_holidays", request)
    assert "count" in response, f"zero count elided from response: {response}"
    assert response["count"] == 0
    assert "dates" in response and response["dates"] == []


def test_fixed_rate_bond_zero_accrued_is_present(client, data_dir):
    # Settle exactly on a coupon date (2009-05-15, a Friday; T+3 from
    # 2009-05-12 on the US government bond calendar) so the accrued amount and
    # accrued days are exactly 0.0.
    request = copy.deepcopy(load_json(data_dir / "fixed_rate_bond_request.json"))
    request["pricing"]["as_of_date"] = "2009-05-12"
    response = client.price("fixed_rate_bond", request)
    rows = response.get("fixed_rate_bonds") or response.get("bonds")
    assert rows, response
    row = rows[0]
    for key in ("accrued_amount", "accrued_days"):
        assert key in row, f"exact-zero double {key!r} elided: {row}"
        assert row[key] == 0.0
    # A non-zero neighbour is still there and prices are consistent.
    assert row["clean_price"] == row["dirty_price"]


def test_swaption_default_enum_is_present(client, data_dir):
    request = copy.deepcopy(load_json(data_dir / "swaption_request.json"))
    request["pricing"].setdefault("options", {})["swaption_pricing_details"] = True
    response = client.price("swaption", request)
    row = response["swaptions"][0]
    # Absolute is the schema default of used_strike_kind; the example strikes
    # are plain fixed rates, so the default is the real value and must be shown.
    assert row.get("used_strike_kind") == "Absolute", row
    # used_spread_from_atm is only meaningful for spread-from-ATM surfaces and
    # is therefore an optional: absent here, never a filler 0.0.
    assert "used_spread_from_atm" not in row, row


def test_swaption_conditional_scalars_follow_the_flags(client, data_dir):
    """Scalars the engine computes only under a flag are `= null` optionals:
    present exactly when computed. Always-computed scalars keep their value,
    including an exact 0.0."""
    base = copy.deepcopy(load_json(data_dir / "swaption_request.json"))
    base["pricing"].pop("options", None)
    analytic_only = ("delta", "atm_forward", "annuity")
    either_flag = ("vega", "gamma", "theta", "dv01")

    plain = client.price("swaption", base)["swaptions"][0]
    for key in analytic_only + either_flag:
        assert key not in plain, f"{key} present without any greek flag: {plain}"
    assert "npv" in plain

    rebump = copy.deepcopy(base)
    rebump["pricing"]["options"] = {"swaption_pricing_rebump": True}
    row = client.price("swaption", rebump)["swaptions"][0]
    for key in either_flag:
        assert key in row, f"rebump did not report {key}: {row}"
    for key in analytic_only:
        assert key not in row, f"{key} is analytic-only but present in rebump mode: {row}"

    details = copy.deepcopy(base)
    details["pricing"]["options"] = {"swaption_pricing_details": True}
    row = client.price("swaption", details)["swaptions"][0]
    for key in analytic_only + either_flag:
        assert key in row, f"details did not report {key}: {row}"
    # ATM diagnostics come from the surface; a constant vol has none.
    for key in ("used_atm_forward", "used_cube_node_atm"):
        assert key not in row, f"{key} present for a surface without ATM levels: {row}"
