"""Swap parity: vanilla, OIS and basis swaps (API NPV vs QuantLib)."""

import copy

import pytest

from ql_reference import (
    api_npv,
    load_json,
    price_basis_swap_ql,
    price_ois_swap_ql,
    price_vanilla_swap_ql,
)

# (product, request file, response list key, QuantLib reference pricer)
SWAP_CASES = [
    ("vanilla_swap", "vanilla_swap_request.json", "swaps", price_vanilla_swap_ql),
    ("ois_swap", "ois_swap_request.json", "swaps", price_ois_swap_ql),
    ("basis_swap", "basis_swap_request.json", "swaps", price_basis_swap_ql),
]


@pytest.mark.parametrize(
    "product,filename,list_key,ql_pricer",
    SWAP_CASES,
    ids=[c[0] for c in SWAP_CASES],
)
def test_swap_parity(client, data_dir, product, filename, list_key, ql_pricer):
    request = load_json(data_dir / filename)
    response = client.price(product, request)
    quantra = api_npv(response, list_key)
    quantlib = ql_pricer(request)
    assert abs(quantra - quantlib) < 1.0, (
        f"{product}: API NPV {quantra} vs QuantLib {quantlib}"
    )


def test_vanilla_swap_in_arrears_differs_and_matches_ql(client, data_dir):
    """An in-arrears floating leg must (a) price differently from the same swap
    fixed in advance and (b) match a QuantLib reference built with inArrears."""
    in_arrears = load_json(data_dir / "ir_swaps/irs_eur_5y_payer_euribor6m_in_arrears.json")
    assert in_arrears["swaps"][0]["vanilla_swap"]["floating_leg"]["in_arrears"] is True

    # In-advance twin: identical request with the in-arrears flag turned off.
    in_advance = copy.deepcopy(in_arrears)
    in_advance["swaps"][0]["vanilla_swap"]["floating_leg"]["in_arrears"] = False

    npv_arrears = api_npv(client.price("vanilla_swap", in_arrears), "swaps")
    npv_advance = api_npv(client.price("vanilla_swap", in_advance), "swaps")

    # The shifted fixing dates read different forwards off the sloped curve, so
    # the two NPVs must be materially different (not a silent no-op).
    assert abs(npv_arrears - npv_advance) > 100.0, (
        f"in-arrears NPV {npv_arrears} vs in-advance NPV {npv_advance} "
        "should differ; in_arrears appears to be ignored"
    )

    # And the in-arrears NPV must match the QuantLib reference built with inArrears.
    ref = price_vanilla_swap_ql(in_arrears)
    assert abs(npv_arrears - ref) < 1.0, (
        f"in-arrears: API NPV {npv_arrears} vs QuantLib {ref}"
    )
