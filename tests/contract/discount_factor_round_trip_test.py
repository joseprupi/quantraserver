"""Discount factors above 1 (negative rates) round-trip through the API.

A curve bootstrapped from negative overnight-swap quotes has discount factors
above 1. QuantLib's InterpolatedDiscountCurve takes them (it requires only
positive values and exactly 1.0 on the first point), so the same numbers sent
back as DiscountFactorPoints must rebuild the same curve:

  1. bootstrap an OIS curve (Discount trait, LogLinear) from negative quotes
     and read its discount factors on a tenor grid that starts at the
     reference date;
  2. send those dates and factors back as an InterpolatedDiscount curve
     (LogLinear) and read the same grid;
  3. the second response is HTTP 200 and equals the first within 1e-12.

The quotes are illustrative, not market data.
"""

import copy

from ql_reference import load_json

# The ESTR OIS example supplies the request structure; its index, helpers and
# query are replaced below by a SARON-like index on the Switzerland calendar.
OIS_EXAMPLE = "curves/curves_eur_ois_estr_df_zero_tenor_grid.json"
INDEX_ID = "CHF_SARON"
OIS_CURVE_ID = "CHF_OIS"
DF_CURVE_ID = "CHF_DF"
CALENDAR = "Switzerland"

# (tenor, par rate): illustrative negative overnight-swap quotes.
OIS_QUOTES = [
    ((3, "Months"), -0.0072),
    ((6, "Months"), -0.0073),
    ((1, "Years"), -0.0073),
    ((2, "Years"), -0.0070),
    ((3, "Years"), -0.0064),
    ((5, "Years"), -0.0050),
    ((7, "Years"), -0.0034),
    ((10, "Years"), -0.0012),
]

# Tenors off the curve reference date. The zero tenor is the reference date
# itself, where the factor is exactly 1.0 and the first point must sit.
GRID_TENORS = [(0, "Days"), (1, "Months"), (3, "Months"), (6, "Months"),
               (9, "Months"), (1, "Years"), (18, "Months"), (2, "Years"),
               (3, "Years"), (4, "Years"), (5, "Years"), (7, "Years"),
               (10, "Years")]

TOLERANCE = 1e-12


def _post(client, request):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS['bootstrap_curves']}", json=request)


def _df_query(curve_id):
    return {
        "curve_id": curve_id,
        "measures": ["DF"],
        "grid": {
            "grid_type": "TenorGrid",
            "grid": {
                "tenors": [{"n": n, "unit": unit} for n, unit in GRID_TENORS],
                "calendar": CALENDAR,
                "business_day_convention": "ModifiedFollowing",
            },
        },
    }


def _ois_request(data_dir):
    req = load_json(data_dir / OIS_EXAMPLE)
    rates = req["pricing"]["rates"]
    rates["indices"][0].update(id=INDEX_ID, name="SARON", currency="CHF",
                               calendar=CALENDAR)
    curve = rates["curves"][0]
    curve.update(id=OIS_CURVE_ID, bootstrap_trait="Discount", interpolator="LogLinear")
    template = curve["points"][0]
    points = []
    for (n, unit), rate in OIS_QUOTES:
        point = copy.deepcopy(template)
        point["point"].update(rate=rate, tenor={"n": n, "unit": unit},
                              calendar=CALENDAR)
        point["point"]["overnight_index"]["id"] = INDEX_ID
        points.append(point)
    curve["points"] = points
    req["queries"] = [_df_query(OIS_CURVE_ID)]
    return req


def _discount_factor_request(source_curve, dates, discount_factors):
    """The sampled dates and factors as an InterpolatedDiscount curve with the
    source curve's reference date and day counter."""
    points = [{"point_type": "DiscountFactorPoint",
               "point": {"date": d, "discount_factor": df}}
              for d, df in zip(dates, discount_factors)]
    return {
        "pricing": {
            "as_of_date": source_curve["reference_date"],
            "rates": {
                "indices": [],
                "curves": [{
                    "id": DF_CURVE_ID,
                    "reference_date": source_curve["reference_date"],
                    "day_counter": source_curve["day_counter"],
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "InterpolatedDiscount",
                    "points": points,
                }],
            },
        },
        "queries": [_df_query(DF_CURVE_ID)],
    }


def _sampled(response):
    """(reference_date, grid_dates, discount factors) of the single curve."""
    results = response["results"]
    assert len(results) == 1, response
    entry = results[0]
    assert not entry.get("error"), entry["error"]
    # The measure is omitted from the JSON when it is the enum default (DF).
    series = [s for s in entry["series"] if s.get("measure", "DF") == "DF"]
    assert len(series) == 1, entry["series"]
    return entry["reference_date"], entry["grid_dates"], series[0]["values"]


def test_negative_rate_curve_discount_factors_round_trip(client, data_dir):
    ois = _ois_request(data_dir)
    first = _post(client, ois)
    assert first.status_code == 200, first.text[:400]
    reference_date, dates, dfs = _sampled(first.json())

    # The grid starts at the reference date, where the factor is exactly 1.0.
    assert dates[0] == reference_date
    assert dfs[0] == 1.0
    assert len(dates) == len(GRID_TENORS)
    assert any(df > 1.0 for df in dfs), dfs

    source = ois["pricing"]["rates"]["curves"][0]
    assert reference_date == source["reference_date"]
    second = _post(client, _discount_factor_request(source, dates, dfs))
    assert second.status_code == 200, second.text[:400]
    reference_date_2, dates_2, dfs_2 = _sampled(second.json())

    assert reference_date_2 == reference_date
    assert dates_2 == dates
    assert len(dfs_2) == len(dfs)
    for date, a, b in zip(dates, dfs, dfs_2):
        assert abs(a - b) <= TOLERANCE, (date, a, b)
