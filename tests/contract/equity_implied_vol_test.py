"""Equity option implied volatility inverts the NPV with the pricing inputs.

The examples price on a constant Black volatility, so inverting their own NPV
must return that volatility. With discrete cash dividends on the underlying the
inversion has to use the same dividends as the pricing engine; without them it
would answer a different question (the vol of a dividend-free option with the
same price).
"""

import pytest

from ql_reference import load_json

# The inversion solves to well within this; a wrong dividend treatment is off
# by several vol points.
IMPLIED_VOL_TOL = 1e-6


@pytest.mark.parametrize("filename", [
    "equity/eqopt_eur_call_atm_1y_discrete_div.json",
    "equity/eqopt_eur_call_atm_1y.json",
])
def test_implied_volatility_recovers_the_pricing_vol(client, data_dir, filename):
    request = load_json(data_dir / filename)
    vol_id = request["options"][0]["volatility"]
    surface = next(v for v in request["pricing"]["volatility"]["vol_surfaces"]
                   if v["id"] == vol_id)
    vol = surface["payload"]["base"]["constant_vol"]

    row = client.price("equity_option", request)["options"][0]
    assert abs(row["implied_volatility"] - vol) < IMPLIED_VOL_TOL, (
        f"{filename}: implied_volatility {row['implied_volatility']} vs pricing vol {vol}"
    )
