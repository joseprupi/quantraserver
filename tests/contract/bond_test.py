"""Bond parity: fixed- and floating-rate bonds (API NPV vs QuantLib)."""

import pytest

from ql_reference import (
    api_npv,
    load_json,
    price_fixed_rate_bond_ql,
    price_floating_rate_bond_ql,
)

# (product, request file, response list key, QuantLib reference pricer)
BOND_CASES = [
    ("fixed_rate_bond", "fixed_rate_bond_request.json", "bonds", price_fixed_rate_bond_ql),
    ("floating_rate_bond", "floating_rate_bond_request.json", "bonds", price_floating_rate_bond_ql),
]


@pytest.mark.parametrize(
    "product,filename,list_key,ql_pricer",
    BOND_CASES,
    ids=[c[0] for c in BOND_CASES],
)
def test_bond_parity(client, data_dir, product, filename, list_key, ql_pricer):
    request = load_json(data_dir / filename)
    response = client.price(product, request)
    quantra = api_npv(response, list_key)
    quantlib = ql_pricer(request)
    assert abs(quantra - quantlib) < 1.0, (
        f"{product}: API NPV {quantra} vs QuantLib {quantlib}"
    )
