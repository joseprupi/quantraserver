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


def test_fra_conventions_optional_and_ignored(client, data_dir):
    """The FRA's own day_counter / calendar / business_day_convention are
    accepted-but-unused: QuantLib builds the FRA from the index. Omitting all
    three must still price (they used to be presence-required, now they are not),
    and the NPV must equal the same request with them present."""
    request = load_json(data_dir / "fra_request.json")
    with_conventions = api_npv(client.price("fra", request), "fras")

    fra = request["fras"][0]["fra"]
    for field in ("day_counter", "calendar", "business_day_convention"):
        fra.pop(field, None)

    without_conventions = api_npv(client.price("fra", request), "fras")
    assert abs(with_conventions - without_conventions) < 1e-9, (
        f"FRA NPV changed when the inert conventions were omitted: "
        f"{with_conventions} vs {without_conventions}"
    )
