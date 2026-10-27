"""Bond cash flows on the wire (pricing.options.bond_pricing_flows = true).

Each flow is a `FlowsWrapper`: `flow_type` names the table, `flow` holds it.

1. Past coupons are `FlowPastInterest` tables, so a reader of that table sees
   the realized `rate` of a fixed-rate bond's paid coupons.
2. A floating coupon's `fixing_date` is the coupon's ISO fixing date. It must
   not depend on the fixing value: a past fixing of 1.0 or more must still
   price (HTTP 200) with every fixing date readable.
"""

import datetime
import json
import re

import pytest

_ISO_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _post(client, product, request):
    endpoint = client.ENDPOINTS[product]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    return r.status_code, r.text


def _load(data_dir, filename):
    with open(data_dir / filename) as fh:
        return json.load(fh)


def _flows(body, flow_type):
    bonds = json.loads(body).get("bonds") or []
    assert bonds, f"no bonds in response :: {body[:300]}"
    flows = bonds[0].get("flows") or []
    return [w["flow"] for w in flows if w.get("flow_type") == flow_type]


def _is_iso_date(value):
    if not isinstance(value, str) or not _ISO_DATE.match(value):
        return False
    try:
        datetime.date.fromisoformat(value)
    except ValueError:
        return False
    return True


def test_fixed_rate_bond_past_coupons_carry_their_rate(client, data_dir):
    # As of 2008-09-15, the 2007-11-15 and 2008-05-15 coupons of the
    # 2007-05-15 semiannual bond are paid.
    req = _load(data_dir, "bonds/fixed_rate_bond_request.json")
    req["pricing"].setdefault("options", {})["bond_pricing_flows"] = True
    coupon_rate = req["bonds"][0]["fixed_rate_bond"]["rate"]

    status, body = _post(client, "fixed_rate_bond", req)
    assert status == 200, f"HTTP {status} :: {body[:300]}"

    past = _flows(body, "FlowPastInterest")
    assert len(past) == 2, f"expected 2 past coupons :: {past}"
    assert past[0].get("accrual_start_date") == "2007-05-15", past[0]
    assert past[0].get("accrual_end_date") == "2007-11-15", past[0]
    for flow in past:
        assert flow.get("rate") == pytest.approx(coupon_rate, rel=1e-12), (
            f"past coupon without its rate :: {flow}")


def _seasoned_frn(data_dir, fixing):
    """The FRN example moved one year back. As of 2025-01-15 its first coupon
    (fixed 2024-01-15) is paid and its second (fixed 2024-07-15) is running,
    so both fixings are past and come from the request."""
    req = _load(data_dir, "bonds/floating_rate_bond_request.json")
    bond = req["bonds"][0]["floating_rate_bond"]
    bond["issue_date"] = "2024-01-17"
    bond["schedule"]["effective_date"] = "2024-01-17"
    bond["schedule"]["termination_date"] = "2029-01-17"
    req["pricing"]["options"] = {"bond_pricing_flows": True}
    for idx in req["pricing"]["rates"]["indices"]:
        if idx["id"] == bond["index"]["id"]:
            idx["fixings"] = [
                {"date": "2024-01-15", "value": fixing},
                {"date": "2024-07-15", "value": fixing},
            ]
            return req
    raise AssertionError("bond index not found in example request")


def test_floating_rate_bond_flows_carry_fixing_dates(client, data_dir):
    fixing = 2.9
    req = _seasoned_frn(data_dir, fixing)
    spread = req["bonds"][0]["floating_rate_bond"]["spread"]

    status, body = _post(client, "floating_rate_bond", req)
    assert status == 200, f"HTTP {status} :: {body[:300]}"

    past = _flows(body, "FlowPastInterest")
    running = _flows(body, "FlowInterest")
    assert len(past) == 1, f"expected 1 past coupon :: {past}"
    assert len(running) == 9, f"expected 9 unpaid coupons :: {running}"
    for flow in past + running:
        assert _is_iso_date(flow.get("fixing_date")), (
            f"coupon without a readable fixing_date :: {flow}")

    first = past[0]
    assert first.get("accrual_start_date") == "2024-01-17", first
    assert first.get("fixing_date") == "2024-01-15", first
    assert first.get("rate") == pytest.approx(fixing + spread, rel=1e-12), first

    second = running[0]
    assert second.get("accrual_start_date") == "2024-07-17", second
    assert second.get("fixing_date") == "2024-07-15", second
