"""Swaption parity across four request shapes (API NPV vs QuantLib).

Covers the four swaption example files the legacy monolith looped over:
the vanilla-underlying request, the OIS-underlying request, the OIS request
fed by Bloomberg zero-rate curves, and the ATM vol-matrix request. Tolerance
is 100 — matching the legacy band for Black-vol-priced products.
"""

import pytest

from ql_reference import api_npv, load_json, price_swaption_ql

SWAPTION_FILES = [
    "swaption_request.json",
    "swaption_ois_request.json",
    "swaption_ois_bbg_zerorate_request.json",
    "swaption_atm_matrix_request.json",
]


@pytest.mark.parametrize("filename", SWAPTION_FILES)
def test_swaption_parity(client, data_dir, filename):
    request = load_json(data_dir / filename)
    response = client.price("swaption", request)
    quantra = api_npv(response, "swaptions")
    quantlib = price_swaption_ql(request)
    assert abs(quantra - quantlib) < 100, (
        f"swaption[{filename}]: API NPV {quantra} vs QuantLib {quantlib}"
    )
