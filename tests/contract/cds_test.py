"""CDS parity across the simple and complex request shapes (API NPV vs QuantLib).

Also: a seasoned CDS (effective date before the valuation date) prices. Its
upfront settles on trade date + cash settlement days, already past, so
QuantLib has no fair upfront for it; the response leaves fair_upfront out
instead of failing the request.
"""

import pytest

from ql_reference import api_npv, load_json, price_cds_ql

CDS_FILES = [
    "cds/cds_request.json",
    "cds/cds_complex_request.json",
]

# The catalog's NPV tolerance for CDS (tests/functional/manifest.py).
SEASONED_NPV_TOL = 0.01


@pytest.mark.parametrize("filename", CDS_FILES)
def test_cds_parity(client, data_dir, filename):
    request = load_json(data_dir / filename)
    response = client.price("cds", request)
    quantra = api_npv(response, "cds_list")
    quantlib = price_cds_ql(request)
    assert abs(quantra - quantlib) < 1.0, (
        f"cds[{filename}]: API NPV {quantra} vs QuantLib {quantlib}"
    )


@pytest.mark.parametrize("filename", [
    "cds/cds_request.json",                    # MidPoint engine
    "cds/cds_eur_5y_buyer_isda_engine.json",   # ISDA engine
])
def test_seasoned_cds_prices_without_fair_upfront(client, data_dir, filename):
    request = load_json(data_dir / filename)
    cds = request["cds_list"][0]["cds"]
    cds["schedule"]["effective_date"] = "2024-06-03"  # valuation date 2025-01-15
    for field in ("protection_start", "trade_date"):
        if field in cds:
            cds[field] = "2024-06-03"

    row = client.price("cds", request)["cds_list"][0]
    assert "fair_upfront" not in row, row
    quantlib = price_cds_ql(request)
    assert abs(row["npv"] - quantlib) < SEASONED_NPV_TOL, (
        f"cds[{filename}] seasoned: API NPV {row['npv']} vs QuantLib {quantlib}"
    )
    for key in ("fair_spread", "default_leg_npv", "premium_leg_npv"):
        assert key in row, row
