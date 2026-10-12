"""Inapplicable response diagnostics are ABSENT, not sentinels.

Several response fields used to carry a -1.0 / -1 sentinel (or a has_x flag
pair) to mean "not applicable". They are now optional and simply omitted from
the response JSON when they do not apply, matching how equity greeks and CDS
fair_spread already behave. These tests pin that behaviour end-to-end:

  - a swaption priced with a non-Hull-White model carries no `used_hw_*` keys;
  - a plain vanilla (IBOR) swap carries no `used_cms_*` keys and no
    `cms_swap_rate` / `has_cms_swap_rate` keys on any flow (`legs[i].flows`);
  - a swap reports exactly the fair values its shape defines: `fair_rate`
    iff exactly one leg is fixed, `fair_spread` iff a leg is floating (both
    on a fixed-vs-floating swap, only `fair_spread` on a basis swap).
"""

import json

from ql_reference import load_json

HW_KEYS = [
    "used_hw_a",
    "used_hw_sigma",
    "used_hw_rmse",
    "used_hw_num_helpers",
    "used_hw_grid_rows",
    "used_hw_grid_cols",
    "used_hw_grid_points",
]

CMS_KEYS = [
    "used_cms_mean_reversion",
    "used_cms_hagan_lower_limit",
    "used_cms_hagan_upper_limit",
    "used_cms_hagan_precision",
    "used_cms_hagan_hard_upper_limit",
]


def test_non_hw_swaption_omits_hw_diagnostics(client, data_dir):
    # The example swaption uses a Black model, so no Hull-White diagnostics apply.
    request = load_json(data_dir / "swaption_request.json")
    response = client.price("swaption", request)
    rows = response.get("swaptions")
    assert isinstance(rows, list) and rows, f"no swaptions in response: {response}"
    for row in rows:
        for key in HW_KEYS:
            assert key not in row, (
                f"non-Hull-White swaption unexpectedly carries {key!r}: {row}"
            )


def test_plain_vanilla_swap_omits_cms_diagnostics(client, data_dir):
    request = load_json(data_dir / "vanilla_swap_request.json")
    # Request per-leg flows so the coupon-level cms_swap_rate omission is exercised
    # too (a plain IBOR swap has no CMS coupons).
    request = json.loads(json.dumps(request))
    request["include_flows"] = True
    response = client.price("vanilla_swap", request)
    swaps = response.get("swaps")
    assert isinstance(swaps, list) and swaps, f"no swaps in response: {response}"
    for swap in swaps:
        for key in CMS_KEYS:
            assert key not in swap, (
                f"plain vanilla swap unexpectedly carries {key!r}: {swap}"
            )
        # One result per leg in request order (fixed_leg, floating_leg), each
        # carrying its flows under `legs[i].flows`.
        assert len(swap["legs"]) == 2, f"expected two leg results: {swap}"
        for leg in swap["legs"]:
            assert leg.get("flows"), f"include_flows=true but a leg has no flows: {leg}"
            for flow in leg["flows"]:
                assert "cms_swap_rate" not in flow, (
                    f"non-CMS coupon unexpectedly carries cms_swap_rate: {flow}"
                )
                assert "has_cms_swap_rate" not in flow, (
                    f"has_cms_swap_rate must be removed from the schema: {flow}"
                )
        # A fixed-vs-floating swap solves for both fair quantities: the
        # fixed rate (one fixed leg) and the floating leg's spread.
        assert "fair_rate" in swap, f"vanilla swap response lacks fair_rate: {swap}"
        assert "fair_spread" in swap, f"vanilla swap response lacks fair_spread: {swap}"


def test_basis_swap_solves_fair_spread_not_fair_rate(client, data_dir):
    """A float-vs-float basis swap carries `fair_spread` and no `fair_rate`;
    its `legs` list holds leg1 then leg2 (request order)."""
    request = load_json(data_dir / "basis_swap_request.json")
    response = client.price("basis_swap", request)
    swaps = response.get("swaps")
    assert isinstance(swaps, list) and swaps, f"no swaps in response: {response}"
    for swap in swaps:
        assert len(swap["legs"]) == 2, f"expected two leg results: {swap}"
        assert "fair_spread" in swap, f"basis swap response lacks fair_spread: {swap}"
        assert "fair_rate" not in swap, (
            f"basis swap response unexpectedly carries fair_rate: {swap}"
        )
