"""FRA parity (API NPV vs QuantLib)."""

from ql_reference import api_npv, load_json, price_fra_ql


def test_fra_parity(client, data_dir):
    request = load_json(data_dir / "fra/fra_request.json")
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
    request = load_json(data_dir / "fra/fra_request.json")
    with_conventions = api_npv(client.price("fra", request), "fras")

    fra = request["fras"][0]["fra"]
    for field in ("day_counter", "calendar", "business_day_convention"):
        fra.pop(field, None)

    without_conventions = api_npv(client.price("fra", request), "fras")
    assert abs(with_conventions - without_conventions) < 1e-9, (
        f"FRA NPV changed when the inert conventions were omitted: "
        f"{with_conventions} vs {without_conventions}"
    )


def test_fra_long_pays_fixed_receives_floating(client, data_dir):
    """fra_type Long is QuantLib's Position::Long: it pays the strike and
    receives the floating rate. Struck below the forward it is worth more than
    zero, struck above the forward less than zero, and the Short side of the
    same trade is worth the negative."""
    request = load_json(data_dir / "fra/fra_eur_3x6_long_strike_below_forward.json")
    fra = request["fras"][0]["fra"]
    assert fra["fra_type"] == "Long"

    long_below = client.price("fra", request)["fras"][0]
    assert fra["strike"] < long_below["forward_rate"]
    assert long_below["npv"] > 0.0, long_below

    fra["fra_type"] = "Short"
    short_below = client.price("fra", request)["fras"][0]
    assert abs(short_below["npv"] + long_below["npv"]) < 1e-9, (long_below, short_below)

    fra["fra_type"] = "Long"
    fra["strike"] = long_below["forward_rate"] + 0.005
    long_above = client.price("fra", request)["fras"][0]
    assert long_above["npv"] < 0.0, long_above


def test_fra_spot_value_is_npv(client, data_dir):
    """spot_value reports the same number as npv (the settlement amount
    discounted on the discounting curve), not the undiscounted amount."""
    request = load_json(data_dir / "fra/fra_eur_3x6_long_strike_below_forward.json")
    fra = client.price("fra", request)["fras"][0]
    assert fra["spot_value"] == fra["npv"], fra
