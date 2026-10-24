"""Bootstrap-curves contract tests.

Wraps the ported legacy checks (contract_checks.check_*) and asserts on the
returned result dict, preserving every original assertion:

  * test_bootstrap_curves           — DF parity vs an independently built
    QuantLib PiecewiseLogLinearDiscount curve (max_diff < 1e-6).
  * test_bootstrap_curves_multicurve_exogenous — 2-curve build + exogenous
    discount wiring + DF shape (near 1.0, non-increasing).
  * test_bootstrap_curves_missing_dependency_fails — missing exogenous
    discount curve fails cleanly (HTTP non-200 or per-curve error).
"""

from contract_checks import (
    check_bootstrap_curves,
    check_bootstrap_curves_missing_dependency_fails,
    check_bootstrap_curves_multicurve_exogenous,
)


def test_bootstrap_curves(client, data_dir):
    result = check_bootstrap_curves(client, data_dir)
    assert result["passed"], result["error"]


def test_bootstrap_curves_multicurve_exogenous(client):
    result = check_bootstrap_curves_multicurve_exogenous(client)
    assert result["passed"], result["error"]


def test_bootstrap_curves_missing_dependency_fails(client):
    result = check_bootstrap_curves_missing_dependency_fails(client)
    assert result["passed"], result["error"]


def _flat_zero_curve_request(reference_date: str, grid: dict) -> dict:
    """A flat 4.5% continuously compounded InterpolatedZero curve (two
    ZeroRatePoints of the same rate) sampled with the ZERO measure."""
    point = {
        "zero_rate": 0.045,
        "calendar": "TARGET",
        "business_day_convention": "Following",
        "compounding": "Continuous",
        "frequency": "Annual",
    }
    return {
        "pricing": {
            "as_of_date": "2025-01-15",
            "rates": {
                "curves": [
                    {
                        "id": "FLAT",
                        "reference_date": reference_date,
                        "day_counter": "Actual365Fixed",
                        "interpolator": "Linear",
                        "bootstrap_trait": "InterpolatedZero",
                        "points": [
                            {"point_type": "ZeroRatePoint",
                             "point": dict(point, date=reference_date)},
                            {"point_type": "ZeroRatePoint",
                             "point": dict(point, tenor={"n": 10, "unit": "Years"})},
                        ],
                    }
                ]
            },
        },
        "queries": [
            {
                "curve_id": "FLAT",
                "measures": ["ZERO"],
                "grid": grid,
                "zero": {"compounding": "Continuous", "use_curve_day_counter": True},
            }
        ],
    }


def _post_bootstrap(client, request: dict) -> dict:
    response = client.session.post(f"{client.base_url}/bootstrap-curves", json=request)
    assert response.status_code == 200, response.text
    body = response.json()
    assert "results" in body, body
    return body["results"][0]


def test_zero_sample_at_the_reference_date_is_the_first_pillar(client):
    """A ZERO sample on the reference date (tenor 0D) is the t -> 0 limit of
    the zero rate: on a flat curve it equals the first pillar's zero within
    1e-10, the same value as every later grid point."""
    grid = {
        "grid_type": "TenorGrid",
        "grid": {
            "tenors": [{"n": 0, "unit": "Days"}, {"n": 1, "unit": "Years"}],
            "calendar": "TARGET",
            "business_day_convention": "Following",
        },
    }
    result = _post_bootstrap(client, _flat_zero_curve_request("2025-01-15", grid))
    assert result["grid_dates"][0] == "2025-01-15"
    series = result["series"][0]
    assert series["measure"] == "ZERO"
    assert len(series["values"]) == 2
    assert abs(series["values"][0] - 0.045) < 1e-10, series["values"]
    assert abs(series["values"][1] - 0.045) < 1e-10, series["values"]


def test_zero_sample_before_the_reference_date_is_the_first_pillar(client):
    """A RangeGrid starting before the curve's reference date (the curve is
    anchored two days after the as-of date) samples dates before the
    reference date: they return the same t -> 0 limit, not a one-day rate."""
    grid = {
        "grid_type": "RangeGrid",
        "grid": {
            "start_date": "2025-01-15",
            "end_date": "2025-01-20",
            "step_number": 1,
            "step_time_unit": "Days",
        },
    }
    result = _post_bootstrap(client, _flat_zero_curve_request("2025-01-17", grid))
    assert result["reference_date"] == "2025-01-17"
    assert result["grid_dates"][0] == "2025-01-15"
    values = result["series"][0]["values"]
    assert len(values) == len(result["grid_dates"])
    for date, value in zip(result["grid_dates"], values):
        assert abs(value - 0.045) < 1e-10, (date, value)
