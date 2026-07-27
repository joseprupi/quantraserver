"""Inapplicable response diagnostics are ABSENT, not sentinels.

Several response fields used to carry a -1.0 / -1 sentinel (or a has_x flag
pair) to mean "not applicable". They are now optional and simply omitted from
the response JSON when they do not apply, matching how equity greeks and CDS
fair_spread already behave. These tests pin that behaviour end-to-end:

  - a swaption priced with a non-Hull-White model carries no `used_hw_*` keys;
  - a plain vanilla (IBOR) swap carries no `used_cms_*` keys and no
    `cms_swap_rate` / `has_cms_swap_rate` keys on any flow.
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
        for leg_key in ("fixed_leg_flows", "floating_leg_flows"):
            for flow in swap.get(leg_key, []):
                assert "cms_swap_rate" not in flow, (
                    f"non-CMS coupon unexpectedly carries cms_swap_rate: {flow}"
                )
                assert "has_cms_swap_rate" not in flow, (
                    f"has_cms_swap_rate must be removed from the schema: {flow}"
                )
