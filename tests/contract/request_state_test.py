"""Per-request state isolation and integer input guards (HTTP contract).

Two concerns that share a theme — a request must see only its own inputs:

1. Index fixings are process-global QuantLib state. The server resets the
   whole fixing store at the start of every request, so a request that needs
   a past fixing and does not supply one must fail the same way whether or
   not an earlier request happened to supply that fixing. The gate runs a
   single worker, so sequential requests here hit the same process — the
   B / A / B sequence below is exactly the leak scenario.

2. Day-count integers (fixing_days / settlement_days / cash_settlement_days)
   feed unsigned QuantLib parameters. A negative value must be a named 400,
   never a silent wrap into a multi-billion-day offset.
"""

import copy
import json

import pytest


def _post(client, product, request):
    endpoint = client.ENDPOINTS[product]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    return r.status_code, r.text


def _load(data_dir, filename):
    with open(data_dir / filename) as fh:
        return json.load(fh)


# ---------------------------------------------------------------------------
# Fixings isolation
# ---------------------------------------------------------------------------

_FIXING_DATE = "2024-01-12"
_FIXING_VALUE = 0.039


def _swap_needing_fixing(data_dir):
    """Vanilla swap whose first floating coupon fixes before the as-of date.

    Marking 2024-01-16 a TARGET holiday pushes the first fixing back to
    2024-01-12 (two business days before the 2024-01-17 effective date, with
    the 15th being the as-of date and the 16th now closed), so the floating
    leg needs a historical Euribor6M fixing. No index carries `fixings`.
    """
    req = _load(data_dir, "vanilla_swap_multicurve_request.json")
    req["pricing"]["calendar_overrides"] = [
        {"calendar": "TARGET", "added_holidays": ["2024-01-16"]}
    ]
    for idx in req["pricing"]["rates"]["indices"]:
        idx.pop("fixings", None)
    return req


def _with_fixing(req):
    out = copy.deepcopy(req)
    for idx in out["pricing"]["rates"]["indices"]:
        if idx["id"] == "EUR_6M":
            idx["fixings"] = [{"date": _FIXING_DATE, "value": _FIXING_VALUE}]
            return out
    raise AssertionError("EUR_6M index not found in example request")


def _assert_missing_fixing(status, body):
    assert status == 422, f"expected 422 for a missing fixing, got {status} :: {body[:300]}"
    assert "fixing" in body.lower(), f"error should name the missing fixing :: {body[:300]}"


def test_fixings_do_not_leak_between_requests(client, data_dir):
    without = _swap_needing_fixing(data_dir)
    with_fixing = _with_fixing(without)

    # B: needs the fixing, supplies none -> missing-fixing error.
    _assert_missing_fixing(*_post(client, "vanilla_swap", without))

    # A: same trade, fixing supplied on the index -> prices.
    status, body = _post(client, "vanilla_swap", with_fixing)
    assert status == 200, f"request with the fixing should price: HTTP {status} :: {body[:300]}"
    priced = json.loads(body)
    assert priced.get("swaps"), f"no swaps in priced response :: {body[:200]}"

    # B again: A's fixing must not have survived in the process-wide store.
    _assert_missing_fixing(*_post(client, "vanilla_swap", without))


# ---------------------------------------------------------------------------
# Negative day-count integers
# ---------------------------------------------------------------------------


def _neg_index_fixing_days(data_dir):
    req = _load(data_dir, "vanilla_swap_multicurve_request.json")
    for idx in req["pricing"]["rates"]["indices"]:
        if idx["id"] == "EUR_6M":
            idx["fixing_days"] = -1
    return "vanilla_swap", req, "IndexDef.fixing_days"


def _neg_bond_settlement_days(data_dir):
    req = _load(data_dir, "fixed_rate_bond_request.json")
    req["bonds"][0]["fixed_rate_bond"]["settlement_days"] = -1
    return "fixed_rate_bond", req, "FixedRateBond.settlement_days"


def _neg_ois_helper_settlement_days(data_dir):
    req = _load(data_dir, "vanilla_swap_multicurve_request.json")
    hit = False
    for curve in req["pricing"]["rates"]["curves"]:
        for p in curve.get("points", []):
            if p.get("point_type") == "OISHelper":
                p["point"]["settlement_days"] = -1
                hit = True
    assert hit, "example has no OISHelper point"
    return "vanilla_swap", req, "OISHelper.settlement_days"


def _neg_cds_cash_settlement_days(data_dir):
    req = _load(data_dir, "cds_request.json")
    req["cds_list"][0]["cds"]["cash_settlement_days"] = -1
    return "cds", req, "CDS.cash_settlement_days"


@pytest.mark.parametrize(
    "builder",
    [
        _neg_index_fixing_days,
        _neg_bond_settlement_days,
        _neg_ois_helper_settlement_days,
        _neg_cds_cash_settlement_days,
    ],
    ids=["index_fixing_days", "bond_settlement_days",
         "ois_helper_settlement_days", "cds_cash_settlement_days"],
)
def test_negative_day_count_is_a_named_400(client, data_dir, builder):
    product, req, field = builder(data_dir)
    status, body = _post(client, product, req)
    assert status == 400, f"{field}=-1 should be a 400, got {status} :: {body[:300]}"
    assert field in body, f"error should name {field} :: {body[:300]}"
    assert "non-negative" in body, f"error should say non-negative :: {body[:300]}"
