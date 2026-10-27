"""Per-request state isolation and integer input guards (HTTP contract).

Three concerns that share a theme — a request must see only its own inputs:

1. Index fixings are process-global QuantLib state. The server resets the
   whole fixing store at the start of every request, so a request that needs
   a past fixing and does not supply one must fail the same way whether or
   not an earlier request happened to supply that fixing. The gate runs a
   single worker, so sequential requests here hit the same process — the
   B / A / B sequence below is exactly the leak scenario.

2. Day-count integers (fixing_days / settlement_days / cash_settlement_days)
   feed unsigned QuantLib parameters. A negative value must be a named 400,
   never a silent wrap into a multi-billion-day offset.

3. The per-query allow_extrapolation option of /bootstrap-curves and
   /sample-vol-surfaces applies to that query only. With the engine caches
   on (as in the container) the curve and the calibrated SABR cube a query
   reads are shared by later requests, so the option must never be left
   set on them. The B / A / B sequences take the client to run against: the
   cache-ON suite (tests/caching) runs them again on the server where those
   objects are shared.
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
    req = _load(data_dir, "ir_swaps/vanilla_swap_multicurve_request.json")
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
    req = _load(data_dir, "ir_swaps/vanilla_swap_multicurve_request.json")
    for idx in req["pricing"]["rates"]["indices"]:
        if idx["id"] == "EUR_6M":
            idx["fixing_days"] = -1
    return "vanilla_swap", req, "IndexDef.fixing_days"


def _neg_bond_settlement_days(data_dir):
    req = _load(data_dir, "bonds/fixed_rate_bond_request.json")
    req["bonds"][0]["fixed_rate_bond"]["settlement_days"] = -1
    return "fixed_rate_bond", req, "FixedRateBond.settlement_days"


def _neg_ois_helper_settlement_days(data_dir):
    req = _load(data_dir, "ir_swaps/vanilla_swap_multicurve_request.json")
    hit = False
    for curve in req["pricing"]["rates"]["curves"]:
        for p in curve.get("points", []):
            if p.get("point_type") == "OISHelper":
                p["point"]["settlement_days"] = -1
                hit = True
    assert hit, "example has no OISHelper point"
    return "vanilla_swap", req, "OISHelper.settlement_days"


def _neg_cds_cash_settlement_days(data_dir):
    req = _load(data_dir, "cds/cds_request.json")
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


# ---------------------------------------------------------------------------
# Per-query extrapolation options
# ---------------------------------------------------------------------------


def _tenor_grid(*tenors):
    return {
        "grid_type": "TenorGrid",
        "grid": {
            "tenors": [{"n": n, "unit": unit} for n, unit in tenors],
            "calendar": "TARGET",
            "business_day_convention": "ModifiedFollowing",
        },
    }


def _results_without_errors(status, body, what):
    assert status == 200, f"{what} should answer 200, got {status} :: {body[:300]}"
    results = json.loads(body).get("results")
    assert results, f"{what}: no results :: {body[:300]}"
    for r in results:
        assert not r.get("error"), f"{what}: query failed :: {r.get('error')}"


def run_curve_extrapolation_sequence(client, data_dir):
    """Bond past its curve's last pillar: 422; a /bootstrap-curves query on
    that curve without options (allow_extrapolation defaults to true): 200;
    the bond again: the same 422."""
    bond = _load(data_dir, "bonds/fixed_rate_bond_beyond_pillar_request.json")
    curve_id = bond["bonds"][0]["discounting_curve"]
    sample = {
        "pricing": copy.deepcopy(bond["pricing"]),
        "queries": [{
            "curve_id": curve_id,
            "measures": ["DF"],
            "grid": _tenor_grid((1, "Months")),
        }],
    }

    first = _post(client, "fixed_rate_bond", bond)
    assert first[0] == 422 and "past max curve time" in first[1], \
        f"bond past the last pillar should be a 422 :: {first[0]} {first[1][:300]}"

    _results_without_errors(*_post(client, "bootstrap_curves", sample), "bootstrap-curves")

    again = _post(client, "fixed_rate_bond", bond)
    assert again == first, \
        f"the query's extrapolation option leaked into the curve: {again[0]} {again[1][:300]}"


def run_swaption_vol_extrapolation_sequence(client, data_dir):
    """SABR-calibrated swaption on a 12Y underlying, past the cube's last
    tenor (10Y), priced on the cube's own flat extrapolation: 200; a
    /sample-vol-surfaces query on that cube with allow_extrapolation false:
    200; the swaption again: 200 with the same NPV."""
    swaption = _load(data_dir, "vol/swaption_sabr_calibrate_request.json")
    trade = swaption["swaptions"][0]
    underlying = trade["swaption"]["underlying"]
    for leg in ("fixed_leg", "floating_leg"):
        underlying[leg]["schedule"]["termination_date"] = "2038-01-17"
    curve_id = underlying["fixed_leg"]["discounting_curve"]
    sample = {
        "pricing": copy.deepcopy(swaption["pricing"]),
        "queries": [{
            "vol_id": trade["volatility"],
            "surface_type": "Swaption",
            "expiry_grid": _tenor_grid((1, "Years")),
            "tenor_grid": _tenor_grid((5, "Years")),
            "strike_grid": {"axis": "AbsoluteStrike", "strikes": [0.03]},
            "discounting_curve_id": curve_id,
            "forwarding_curve_id": curve_id,
            "output_mode": "Cube",
            "options": {"allow_extrapolation": False},
        }],
    }

    status, body = _post(client, "swaption", swaption)
    assert status == 200, f"12Y underlying should price: HTTP {status} :: {body[:300]}"
    npv = json.loads(body)["swaptions"][0]["npv"]

    _results_without_errors(*_post(client, "sample_vol_surfaces", sample), "sample-vol-surfaces")

    status, body = _post(client, "swaption", swaption)
    assert status == 200, \
        f"the query's extrapolation option leaked into the cube: HTTP {status} :: {body[:300]}"
    assert json.loads(body)["swaptions"][0]["npv"] == npv


def test_curve_extrapolation_option_does_not_leak(client, data_dir):
    run_curve_extrapolation_sequence(client, data_dir)


def test_swaption_vol_extrapolation_option_does_not_leak(client, data_dir):
    run_swaption_vol_extrapolation_sequence(client, data_dir)
