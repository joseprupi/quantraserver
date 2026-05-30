"""FRA parity (API NPV vs QuantLib)."""

from ql_reference import api_npv, load_json, price_fra_ql


def test_fra_parity(client, data_dir):
    request = load_json(data_dir / "fra_request.json")
    response = client.price("fra", request)
    quantra = api_npv(response, "fras")
    quantlib = price_fra_ql(request)
    assert abs(quantra - quantlib) < 1.0, (
        f"fra: API NPV {quantra} vs QuantLib {quantlib}"
    )
