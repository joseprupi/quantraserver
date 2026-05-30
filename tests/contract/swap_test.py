"""Swap parity: vanilla, OIS and basis swaps (API NPV vs QuantLib)."""

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
