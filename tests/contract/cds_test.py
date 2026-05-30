"""CDS parity across the simple and complex request shapes (API NPV vs QuantLib)."""

import pytest

from ql_reference import api_npv, load_json, price_cds_ql

CDS_FILES = [
    "cds_request.json",
    "cds_complex_request.json",
]


@pytest.mark.parametrize("filename", CDS_FILES)
def test_cds_parity(client, data_dir, filename):
    request = load_json(data_dir / filename)
    response = client.price("cds", request)
    quantra = api_npv(response, "cds_list")
    quantlib = price_cds_ql(request)
    assert abs(quantra - quantlib) < 1.0, (
        f"cds[{filename}]: API NPV {quantra} vs QuantLib {quantlib}"
    )
