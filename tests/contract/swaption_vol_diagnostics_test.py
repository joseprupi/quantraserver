"""Swaption vol diagnostics period-unit serialization.

Regression guard: the diagnostics builder used to cast a QuantLib TimeUnit
straight into the schema's TimeUnit enum. Those two enums do NOT share an
ordering (the schema enum is alphabetical), so a 1-Year expiry came back
labelled "Milliseconds". This drives /calibrate-swaption-vol with a known
1Y/2Y x 5Y/10Y expiry/tenor grid and asserts the returned Period units read
"Years" (never "Milliseconds").
"""

from ql_reference import load_json


def _units(periods):
    return [p.get("unit") for p in periods]


def test_diagnostics_period_units_are_years(client, data_dir):
    request = load_json(data_dir / "vol" / "sabrcal_eur_2x2_5strike_roundtrip.json")
    response = client.price("calibrate_swaption_vol", request)

    diagnostics = response["diagnostics"]
    expiry_units = _units(diagnostics["expiries"])
    tenor_units = _units(diagnostics["tenors"])

    # Grid is [1Y, 2Y] expiries x [5Y, 10Y] tenors.
    assert expiry_units == ["Years", "Years"], (
        f"expiry units mislabelled: {expiry_units}"
    )
    assert tenor_units == ["Years", "Years"], (
        f"tenor units mislabelled: {tenor_units}"
    )
    assert "Milliseconds" not in expiry_units + tenor_units
