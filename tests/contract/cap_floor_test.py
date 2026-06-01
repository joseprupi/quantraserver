"""Cap/Floor parity (API NPV vs QuantLib).

Tolerance is 100 (not 1.0) — matching the legacy monolith, which loosened the
band for the Black-vol-priced products (cap_floor and swaption).
"""

from ql_reference import api_npv, load_json, price_cap_floor_ql


def test_cap_floor_parity(client, data_dir):
    request = load_json(data_dir / "cap_floor_request.json")
    response = client.price("cap_floor", request)
    quantra = api_npv(response, "cap_floors")
    quantlib = price_cap_floor_ql(request)
    assert abs(quantra - quantlib) < 100, (
        f"cap_floor: API NPV {quantra} vs QuantLib {quantlib}"
    )
