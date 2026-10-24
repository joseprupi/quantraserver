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


COLLAR_EXAMPLE = "cap_floor/collar_eur_5y_cap_4pct_floor_2pct_euribor3m.json"


def test_collar_is_cap_minus_floor(client, data_dir):
    """A collar is long the cap at `strike` and short the floor at
    `floor_strike` on the same leg: its NPV equals the cap's NPV minus the
    floor's NPV from two separate requests on the same market, and matches
    the QuantLib reference (ql.Collar)."""
    request = load_json(data_dir / COLLAR_EXAMPLE)
    cf = request["cap_floors"][0]["cap_floor"]
    assert cf["cap_floor_type"] == "Collar" and cf["floor_strike"] < cf["strike"]
    collar = api_npv(client.price("cap_floor", request), "cap_floors")

    cap_req = load_json(data_dir / COLLAR_EXAMPLE)
    cap_cf = cap_req["cap_floors"][0]["cap_floor"]
    cap_cf["cap_floor_type"] = "Cap"
    cap_cf.pop("floor_strike")
    cap = api_npv(client.price("cap_floor", cap_req), "cap_floors")

    floor_req = load_json(data_dir / COLLAR_EXAMPLE)
    floor_cf = floor_req["cap_floors"][0]["cap_floor"]
    floor_cf["cap_floor_type"] = "Floor"
    floor_cf["strike"] = floor_cf.pop("floor_strike")
    floor = api_npv(client.price("cap_floor", floor_req), "cap_floors")

    assert cap > 0.0 and floor > 0.0
    assert abs(collar - (cap - floor)) < 1e-6, (
        f"collar {collar} vs cap - floor {cap} - {floor} = {cap - floor}"
    )
    quantlib = price_cap_floor_ql(request)
    assert abs(collar - quantlib) < 1e-6, f"collar: API NPV {collar} vs QuantLib {quantlib}"
