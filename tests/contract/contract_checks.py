"""Ported standalone contract/smoke checks for bootstrap & inflation endpoints.

These are the legacy monolith's non-parametrized scenario functions, copied
verbatim except for the def name (test_* -> check_*, so pytest does NOT collect
them directly — they return a result dict rather than asserting). The pytest
wrappers in bootstrap_test.py / inflation_test.py call these and assert on the
returned dict, preserving every original assertion.

Reference helpers (parse_date, get_*, build_curve_from_json, ql enums, and
_make_multicurve_exogenous_request) come from ql_reference.
"""

import json
from pathlib import Path

import QuantLib as ql

from ql_reference import (
    ApiClient,
    _make_multicurve_exogenous_request,
    _reference_pricing_view,
    _period_n_unit,
    get_calendar,
    get_convention,
    get_day_counter,
    get_frequency,
    get_time_unit,
    load_json,
    parse_date,
    resolve_index_from_id,
)


def check_bootstrap_curves(client: ApiClient, data_dir: Path) -> dict:
    """Test bootstrap-curves endpoint against QuantLib."""
    filepath = data_dir / "bootstrap_curves_tenor_grid.json"
    result = {
        "product": "bootstrap_curves",
        "file": str(filepath),
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }
    
    if not filepath.exists():
        result["error"] = "File not found"
        return result
    
    try:
        request = load_json(filepath)
        
        # Call API
        response = client.session.post(
            f"{client.base_url}/bootstrap-curves", 
            json=request
        ).json()
        if "results" not in response:
            result["error"] = response.get("error", "bootstrap-curves response missing 'results'")
            return result
        
        # Build QuantLib curve from request
        pricing = _reference_pricing_view(request)
        as_of = parse_date(pricing["as_of_date"])
        ql.Settings.instance().evaluationDate = as_of

        ts = pricing["curves"][0]
        quote_values = {}
        quote_types = {}
        for q in pricing.get("quotes", []):
            if "id" in q:
                quote_values[q["id"]] = q.get("value", 0.0)
                quote_types[q["id"]] = q.get("quote_type", "Curve")

        def resolve_bootstrap_rate(point: dict) -> float:
            quote_id = point.get("quote_id")
            if quote_id:
                if quote_types.get(quote_id, "Curve") != "Curve":
                    raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
                return quote_values.get(quote_id, 0.0)
            return point["rate"]
        
        # Build helpers
        helpers = []
        for point_wrapper in ts["points"]:
            point_type = point_wrapper["point_type"]
            point = point_wrapper["point"]
            
            if point_type == "DepositHelper":
                n, u = _period_n_unit(point, "tenor", 0, "Days")
                tenor = ql.Period(n, get_time_unit(u))
                helpers.append(ql.DepositRateHelper(
                    resolve_bootstrap_rate(point), tenor, point.get("fixing_days", 2),
                    get_calendar(point["calendar"]),
                    get_convention(point["business_day_convention"]),
                    True, get_day_counter(point["day_counter"])
                ))
            elif point_type == "SwapHelper":
                n, u = _period_n_unit(point, "tenor", 0, "Days")
                tenor = ql.Period(n, get_time_unit(u))
                # New schema: float_index is an IndexRef with just an id
                float_idx = point.get("float_index", {})
                idx_id = float_idx.get("id", "EUR_6M") if isinstance(float_idx, dict) else "EUR_6M"
                # Resolve index from indices definitions in the request
                index = resolve_index_from_id(idx_id, request)
                fwd_start = ql.Period(point.get("fwd_start_days", 0), ql.Days)
                helpers.append(ql.SwapRateHelper(
                    resolve_bootstrap_rate(point), tenor,
                    get_calendar(point["calendar"]),
                    get_frequency(point["sw_fixed_leg_frequency"]),
                    get_convention(point["sw_fixed_leg_convention"]),
                    get_day_counter(point["sw_fixed_leg_day_counter"]),
                    index,
                    ql.QuoteHandle(),  # spread
                    fwd_start
                ))
        
        # Bootstrap curve
        ref_date = parse_date(ts["reference_date"])
        ql_curve = ql.PiecewiseLogLinearDiscount(ref_date, helpers, 
                                                  get_day_counter(ts["day_counter"]))
        ql_curve.enableExtrapolation()
        
        # Compare discount factors
        api_result = response["results"][0]
        if "error" in api_result and api_result["error"]:
            result["error"] = api_result["error"].get("error_message", "Unknown error")
            return result
        
        # Find DF series (measure can be 0 for DF, "DF" string, or missing which defaults to DF)
        df_values = None
        for series in api_result.get("series", []):
            measure = series.get("measure")
            # If measure is missing, it defaults to 0 (DF) in FlatBuffers
            # Handle: missing (None->DF), integer 0 (DF), or string "DF"
            if measure is None or measure == "DF" or measure == 0:
                df_values = series.get("values")
                break
        
        if not df_values:
            result["error"] = "No DF series in response"
            return result
        
        grid_dates = api_result["grid_dates"]
        max_diff = 0.0
        
        for i, date_str in enumerate(grid_dates):
            ql_date = parse_date(date_str)
            q_df = df_values[i]
            ql_df = ql_curve.discount(ql_date)
            diff = abs(ql_df - q_df)
            max_diff = max(max_diff, diff)
        
        result["passed"] = max_diff < 1e-6
        result["quantra_npv"] = df_values[0] if df_values else 0
        result["quantlib_npv"] = ql_curve.discount(parse_date(grid_dates[0])) if grid_dates else 0
        result["diff"] = max_diff
        
    except Exception as e:
        import traceback
        result["error"] = f"{str(e)}\n{traceback.format_exc()}"
    
    return result


def check_bootstrap_curves_multicurve_exogenous(client: ApiClient) -> dict:
    """Bootstraps two curves in one call and checks exogenous discount dependency wiring."""
    result = {
        "product": "bootstrap_curves_multicurve_exogenous",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }

    try:
        request = _make_multicurve_exogenous_request()
        resp = client.session.post(
            f"{client.base_url}/bootstrap-curves",
            json=request
        )

        if resp.status_code != 200:
            result["error"] = f"HTTP {resp.status_code}: {resp.text[:300]}"
            return result

        data = resp.json()
        results = data.get("results", [])
        if len(results) != 2:
            result["error"] = f"Expected 2 curve results, got {len(results)}"
            return result

        # Basic shape checks
        ids = {r.get("id") for r in results}
        if ids != {"EUR_OIS", "EUR_6M"}:
            result["error"] = f"Unexpected curve ids: {ids}"
            return result

        by_id = {r["id"]: r for r in results}
        for cid in ["EUR_OIS", "EUR_6M"]:
            r = by_id[cid]
            if r.get("error"):
                result["error"] = f"Curve {cid} failed: {r['error']}"
                return result
            grid_dates = r.get("grid_dates") or []
            series = r.get("series") or []
            if len(grid_dates) == 0 or len(series) < 2:
                result["error"] = f"Curve {cid} missing grid/series"
                return result
            for s in series:
                vals = s.get("values") or []
                if len(vals) != len(grid_dates):
                    result["error"] = f"Curve {cid} series length mismatch"
                    return result

        # A lightweight behavioral check:
        # Discount factors should start near 1.0 and (weakly) decrease with maturity for both curves.
        def _df_series(curve_result: dict):
            for s in (curve_result.get("series") or []):
                m = s.get("measure")
                # FlatBuffers JSON might omit default DF measure=0, so treat missing as DF
                if m in (None, "DF", 0):
                    return s.get("values") or []
            return []

        df_ois = _df_series(by_id["EUR_OIS"])
        df_6m  = _df_series(by_id["EUR_6M"])

        if not df_ois or not df_6m:
            result["error"] = "Missing DF series in one or both curves"
            return result

        if not (0.95 < df_ois[0] <= 1.0001 and 0.95 < df_6m[0] <= 1.0001):
            result["error"] = f"DF at first grid point not near 1.0 (ois={df_ois[0]}, 6m={df_6m[0]})"
            return result

        if any(df_ois[i] < df_ois[i+1] - 1e-10 for i in range(len(df_ois)-1)):
            result["error"] = "OIS DF is not non-increasing"
            return result

        if any(df_6m[i] < df_6m[i+1] - 1e-10 for i in range(len(df_6m)-1)):
            result["error"] = "6M DF is not non-increasing"
            return result

        result["passed"] = True
        # Put something numeric in summary columns
        result["quantra_npv"] = float(df_6m[-1])
        result["quantlib_npv"] = float(df_ois[-1])
        result["diff"] = abs(result["quantra_npv"] - result["quantlib_npv"])
        return result

    except Exception as e:
        result["error"] = str(e)
        return result


def check_bootstrap_curves_missing_dependency_fails(client: ApiClient) -> dict:
    """Requests a curve that references an exogenous discount curve that is not provided; should fail cleanly."""
    result = {
        "product": "bootstrap_curves_missing_dependency",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }

    try:
        request = _make_multicurve_exogenous_request()
        # Drop the discount curve so EUR_6M references a missing EUR_OIS
        request["pricing"]["rates"]["curves"] = [request["pricing"]["rates"]["curves"][1]]
        request["queries"] = [request["queries"][1]]

        resp = client.session.post(
            f"{client.base_url}/bootstrap-curves",
            json=request
        )

        # Accept either:
        # - HTTP-level failure (4xx) OR
        # - 200 with per-curve error populated
        if resp.status_code != 200:
            result["passed"] = True
            result["quantra_npv"] = 1.0
            result["quantlib_npv"] = 1.0
            result["diff"] = 0.0
            return result

        data = resp.json()
        results = data.get("results", [])
        if len(results) != 1:
            result["error"] = f"Expected 1 curve result, got {len(results)}"
            return result

        err = results[0].get("error")
        if not err:
            result["error"] = "Expected an error for missing dependency but got success"
            return result

        result["passed"] = True
        result["quantra_npv"] = 1.0
        result["quantlib_npv"] = 1.0
        result["diff"] = 0.0
        return result

    except Exception as e:
        result["error"] = str(e)
        return result


def check_bootstrap_inflation_curves_smoke(client: ApiClient) -> dict:
    """Smoke-test inflation bootstrap endpoint exposure and response shape."""
    result = {
        "product": "bootstrap_inflation_curves",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None
    }
    try:
        request = json.loads(r"""
{
    "pricing": {
        "as_of_date": "2025-01-15",
        "rates": {
            "indices": [
                {
                    "id": "EUR_6M",
                    "name": "Euribor",
                    "index_type": "Ibor",
                    "fixing_days": 2,
                    "calendar": "TARGET",
                    "business_day_convention": "ModifiedFollowing",
                    "day_counter": "Actual360",
                    "currency": "EUR",
                    "tenor": {
                        "n": 6,
                        "unit": "Months"
                    }
                }
            ],
            "curves": [
                {
                    "id": "DISC",
                    "reference_date": "2025-01-15",
                    "day_counter": "Actual365Fixed",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "DepositHelper",
                            "point": {
                                "rate": 0.03,
                                "fixing_days": 2,
                                "calendar": "TARGET",
                                "business_day_convention": "ModifiedFollowing",
                                "day_counter": "Actual360",
                                "tenor": {
                                    "n": 6,
                                    "unit": "Months"
                                }
                            }
                        }
                    ]
                }
            ]
        },
        "inflation": {
            "inflation_indices": [
                {
                    "id": "EUHICP",
                    "family_name": "EU HICP",
                    "currency": "EUR",
                    "calendar": "TARGET",
                    "day_counter": "Actual365Fixed",
                    "frequency": "Monthly",
                    "availability_lag": {
                        "n": 2,
                        "unit": "Months"
                    },
                    "observation_lag": {
                        "n": 3,
                        "unit": "Months"
                    },
                    "interpolated": true,
                    "revised": false,
                    "kind": "ZeroInflation",
                    "fixings": [
                        {
                            "date": "2024-10-01",
                            "value": 100.0
                        },
                        {
                            "date": "2024-11-01",
                            "value": 100.2
                        },
                        {
                            "date": "2024-12-01",
                            "value": 100.4
                        }
                    ]
                }
            ],
            "inflation_curves": [
                {
                    "id": "HICP_ZC",
                    "reference_date": "2025-01-15",
                    "calendar": "TARGET",
                    "business_day_convention": "ModifiedFollowing",
                    "day_counter": "Actual365Fixed",
                    "interpolator": "Linear",
                    "bootstrap_accuracy": 1e-12,
                    "kind": "ZeroInflation",
                    "index_id": "EUHICP",
                    "discount_curve_id": "DISC",
                    "allow_extrapolation": true,
                    "points": [
                        {
                            "point_type": "ZeroCouponInflationSwapHelper",
                            "point": {
                                "quote_value": 0.02,
                                "swap_observation_lag": {
                                    "n": 3,
                                    "unit": "Months"
                                },
                                "tenor": {
                                    "n": 1,
                                    "unit": "Years"
                                },
                                "calendar": "TARGET",
                                "payment_convention": "ModifiedFollowing",
                                "day_counter": "Actual365Fixed",
                                "observation_interpolation": "Linear"
                            }
                        },
                        {
                            "point_type": "ZeroCouponInflationSwapHelper",
                            "point": {
                                "quote_value": 0.021,
                                "swap_observation_lag": {
                                    "n": 3,
                                    "unit": "Months"
                                },
                                "tenor": {
                                    "n": 2,
                                    "unit": "Years"
                                },
                                "calendar": "TARGET",
                                "payment_convention": "ModifiedFollowing",
                                "day_counter": "Actual365Fixed",
                                "observation_interpolation": "Linear"
                            }
                        }
                    ]
                }
            ]
        }
    },
    "queries": [
        {
            "curve_id": "HICP_ZC",
            "measures": [
                "ZeroRate"
            ],
            "grid": {
                "grid_type": "TenorGrid",
                "grid": {
                    "tenors": [
                        {
                            "n": 1,
                            "unit": "Years"
                        },
                        {
                            "n": 2,
                            "unit": "Years"
                        }
                    ],
                    "calendar": "TARGET",
                    "business_day_convention": "ModifiedFollowing"
                }
            }
        }
    ]
}
""")

        response = client.price("bootstrap_inflation_curves", request)
        results = response.get("results", [])
        if len(results) != 1:
            result["error"] = f"Expected 1 result, got {len(results)}"
            return result
        if results[0].get("error"):
            result["error"] = str(results[0]["error"])
            return result

        series = results[0].get("series", [])
        if not series or not series[0].get("values"):
            result["error"] = "Missing sampled inflation series values"
            return result
        result["passed"] = True
        result["quantra_npv"] = float(series[0]["values"][0])
        result["quantlib_npv"] = float(series[0]["values"][0])
        result["diff"] = 0.0
        return result
    except Exception as e:
        result["error"] = str(e)
        return result


def check_bootstrap_inflation_curves_rejects_legacy_payload(client: ApiClient) -> dict:
    """Breaking-change guard: old generic inflation pillar payload must be rejected."""
    result = {
        "product": "bootstrap_inflation_curves_legacy_payload_rejected",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": 0.0,
        "quantlib_npv": 0.0,
        "diff": 0.0,
        "error": None,
    }
    request = json.loads(r"""
{
    "pricing": {
        "as_of_date": "2025-01-15",
        "rates": {
            "curves": [
                {
                    "id": "DISC",
                    "reference_date": "2025-01-15",
                    "day_counter": "Actual365Fixed",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "DepositHelper",
                            "point": {
                                "rate": 0.03,
                                "tenor": {
                                    "n": 6,
                                    "unit": "Months"
                                },
                                "fixing_days": 2,
                                "calendar": "TARGET",
                                "business_day_convention": "ModifiedFollowing",
                                "day_counter": "Actual360"
                            }
                        }
                    ]
                }
            ]
        },
        "inflation": {
            "inflation_indices": [
                {
                    "id": "EUHICP",
                    "family_name": "EU HICP",
                    "currency": "EUR",
                    "calendar": "TARGET",
                    "day_counter": "Actual365Fixed",
                    "frequency": "Monthly",
                    "availability_lag": {
                        "n": 2,
                        "unit": "Months"
                    },
                    "observation_lag": {
                        "n": 3,
                        "unit": "Months"
                    },
                    "interpolated": true,
                    "revised": false,
                    "kind": "ZeroInflation"
                }
            ],
            "inflation_curves": [
                {
                    "base": {
                        "id": "HICP_ZC",
                        "reference_date": "2025-01-15",
                        "calendar": "TARGET",
                        "business_day_convention": "ModifiedFollowing",
                        "day_counter": "Actual365Fixed",
                        "kind": "ZeroInflation",
                        "index_id": "EUHICP"
                    },
                    "payload_type": "ZeroInflationCurveFromZcSwapsSpec",
                    "payload": {
                        "pillars": [
                            {
                                "maturity": {
                                    "n": 1,
                                    "unit": "Years"
                                },
                                "quote_value": 0.02
                            }
                        ]
                    }
                }
            ]
        }
    },
    "queries": [
        {
            "curve_id": "HICP_ZC",
            "measures": [
                "ZeroRate"
            ],
            "grid": {
                "grid_type": "TenorGrid",
                "grid": {
                    "tenors": [
                        {
                            "n": 1,
                            "unit": "Years"
                        }
                    ]
                }
            }
        }
    ]
}
""")

    try:
        client.price("bootstrap_inflation_curves", request)
        result["error"] = "Legacy payload unexpectedly accepted"
    except Exception as e:
        msg = str(e)
        if "ZeroInflationCurveFromZcSwapsSpec" in msg or "JSON parse error" in msg:
            result["passed"] = True
        else:
            result["error"] = msg
    return result


def check_price_zero_coupon_inflation_swap_smoke(client: ApiClient) -> dict:
    result = {
        "product": "zero_coupon_inflation_swap",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None,
    }
    try:
        request = {
            "pricing": {
                "as_of_date": "2025-01-15",
                "rates": {
                    "indices": [{
                        "id": "EUR_6M", "name": "Euribor", "index_type": "Ibor",
                        "tenor": {"n": 6, "unit": "Months"},
                        "fixing_days": 2, "calendar": "TARGET",
                        "business_day_convention": "ModifiedFollowing",
                        "day_counter": "Actual360", "currency": "EUR"
                    }],
                    "curves": [{
                        "id": "DISC", "reference_date": "2025-01-15",
                        "day_counter": "Actual365Fixed", "interpolator": "LogLinear",
                        "bootstrap_trait": "Discount",
                        "points": [
                            {
                                "point_type": "DepositHelper",
                                "point": {
                                    "rate": 0.03,
                                    "tenor": {"n": 3, "unit": "Months"},
                                    "fixing_days": 2,
                                    "calendar": "TARGET",
                                    "business_day_convention": "ModifiedFollowing",
                                    "day_counter": "Actual360"
                                }
                            },
                            {
                                "point_type": "DepositHelper",
                                "point": {
                                    "rate": 0.03,
                                    "tenor": {"n": 6, "unit": "Months"},
                                    "fixing_days": 2,
                                    "calendar": "TARGET",
                                    "business_day_convention": "ModifiedFollowing",
                                    "day_counter": "Actual360"
                                }
                            },
                            {
                                "point_type": "DepositHelper",
                                "point": {
                                    "rate": 0.03,
                                    "tenor": {"n": 1, "unit": "Years"},
                                    "fixing_days": 2,
                                    "calendar": "TARGET",
                                    "business_day_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed"
                                }
                            },
                            {
                                "point_type": "SwapHelper",
                                "point": {
                                    "rate": 0.03,
                                    "calendar": "TARGET",
                                    "sw_fixed_leg_frequency": "Annual",
                                    "sw_fixed_leg_convention": "ModifiedFollowing",
                                    "sw_fixed_leg_day_counter": "Thirty360",
                                    "float_index": {"id": "EUR_6M"},
                                    "spread": 0.0,
                                    "fwd_start_days": 0,
                                    "tenor": {"n": 5, "unit": "Years"}
                                }
                            },
                            {
                                "point_type": "SwapHelper",
                                "point": {
                                    "rate": 0.03,
                                    "calendar": "TARGET",
                                    "sw_fixed_leg_frequency": "Annual",
                                    "sw_fixed_leg_convention": "ModifiedFollowing",
                                    "sw_fixed_leg_day_counter": "Thirty360",
                                    "float_index": {"id": "EUR_6M"},
                                    "spread": 0.0,
                                    "fwd_start_days": 0,
                                    "tenor": {"n": 10, "unit": "Years"}
                                }
                            }
                        ]
                    }]
                },
                "inflation": {
                    "inflation_indices": [{
                        "id": "EUHICP", "family_name": "EU HICP", "currency": "EUR",
                        "calendar": "TARGET", "day_counter": "Actual365Fixed",
                        "frequency": "Monthly",
                        "availability_lag": {"n": 2, "unit": "Months"},
                        "observation_lag": {"n": 3, "unit": "Months"},
                        "interpolated": True, "revised": False, "kind": "ZeroInflation",
                        "fixings": [
                            {"date": "2024-10-01", "value": 100.0},
                            {"date": "2024-11-01", "value": 100.2},
                            {"date": "2024-12-01", "value": 100.4}
                        ]
                    }],
                    "inflation_curves": [{
                        "id": "HICP_ZC",
                        "reference_date": "2025-01-15",
                        "calendar": "TARGET",
                        "business_day_convention": "ModifiedFollowing",
                        "day_counter": "Actual365Fixed",
                        "interpolator": "Linear",
                        "bootstrap_accuracy": 1.0e-12,
                        "kind": "ZeroInflation",
                        "index_id": "EUHICP",
                        "discount_curve_id": "DISC",
                        "allow_extrapolation": True,
                        "points": [
                            {
                                "point_type": "ZeroCouponInflationSwapHelper",
                                "point": {
                                    "quote_value": 0.0200,
                                    "swap_observation_lag": {"n": 3, "unit": "Months"},
                                    "tenor": {"n": 1, "unit": "Years"},
                                    "calendar": "TARGET",
                                    "payment_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed",
                                    "observation_interpolation": "Linear"
                                }
                            },
                            {
                                "point_type": "ZeroCouponInflationSwapHelper",
                                "point": {
                                    "quote_value": 0.0210,
                                    "swap_observation_lag": {"n": 3, "unit": "Months"},
                                    "tenor": {"n": 2, "unit": "Years"},
                                    "calendar": "TARGET",
                                    "payment_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed",
                                    "observation_interpolation": "Linear"
                                }
                            },
                            {
                                "point_type": "ZeroCouponInflationSwapHelper",
                                "point": {
                                    "quote_value": 0.0220,
                                    "swap_observation_lag": {"n": 3, "unit": "Months"},
                                    "tenor": {"n": 5, "unit": "Years"},
                                    "calendar": "TARGET",
                                    "payment_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed",
                                    "observation_interpolation": "Linear"
                                }
                            }
                        ]
                    }]
                }
            },
            "swaps": [{
                "zero_coupon_inflation_swap": {
                    "swap_type": "Payer",
                    "notional": 1000000.0,
                    "start_date": "2025-01-15",
                    "maturity_date": "2030-01-15",
                    "fixed_calendar": "TARGET",
                    "fixed_convention": "ModifiedFollowing",
                    "day_counter": "Actual365Fixed",
                    "fixed_rate": 0.0217,
                    "inflation_index_id": "EUHICP",
                    "observation_lag": {"n": 3, "unit": "Months"},
                    "observation_interpolation": "Linear",
                    "adjust_observation_dates": False,
                    "inflation_calendar": "NullCalendar",
                    "inflation_convention": "Following"
                },
                "discounting_curve": "DISC",
                "inflation_curve": "HICP_ZC"
            }],
            "include_flows": True
        }

        response = client.price("zero_coupon_inflation_swap", request)
        swaps = response.get("swaps", [])
        if len(swaps) != 1:
            result["error"] = f"Expected 1 swap, got {len(swaps)}"
            return result
        swap = swaps[0]
        if not isinstance(swap.get("npv"), (int, float)):
            result["error"] = "Missing numeric npv"
            return result
        if not isinstance(swap.get("fair_rate"), (int, float)):
            result["error"] = "Missing numeric fair_rate"
            return result
        if len(swap.get("fixed_leg_flows", [])) != 1 or len(swap.get("inflation_leg_flows", [])) != 1:
            result["error"] = "Unexpected ZCIIS flow counts"
            return result
        result["passed"] = True
        result["quantra_npv"] = float(swap["npv"])
        result["quantlib_npv"] = float(swap["npv"])
        result["diff"] = 0.0
        return result
    except Exception as e:
        result["error"] = str(e)
        return result


def check_price_year_on_year_inflation_swap_smoke(client: ApiClient) -> dict:
    result = {
        "product": "year_on_year_inflation_swap",
        "file": "<inline>",
        "passed": False,
        "quantra_npv": None,
        "quantlib_npv": None,
        "diff": None,
        "error": None,
    }
    try:
        request = {
            "pricing": {
                "as_of_date": "2025-01-15",
                "rates": {
                    "indices": [{
                        "id": "EUR_6M", "name": "Euribor", "index_type": "Ibor",
                        "tenor": {"n": 6, "unit": "Months"},
                        "fixing_days": 2, "calendar": "TARGET",
                        "business_day_convention": "ModifiedFollowing",
                        "day_counter": "Actual360", "currency": "EUR"
                    }],
                    "curves": [{
                        "id": "DISC", "reference_date": "2025-01-15",
                        "day_counter": "Actual365Fixed", "interpolator": "LogLinear",
                        "bootstrap_trait": "Discount",
                        "points": [
                            {
                                "point_type": "DepositHelper",
                                "point": {
                                    "rate": 0.03,
                                    "tenor": {"n": 3, "unit": "Months"},
                                    "fixing_days": 2,
                                    "calendar": "TARGET",
                                    "business_day_convention": "ModifiedFollowing",
                                    "day_counter": "Actual360"
                                }
                            },
                            {
                                "point_type": "DepositHelper",
                                "point": {
                                    "rate": 0.03,
                                    "tenor": {"n": 6, "unit": "Months"},
                                    "fixing_days": 2,
                                    "calendar": "TARGET",
                                    "business_day_convention": "ModifiedFollowing",
                                    "day_counter": "Actual360"
                                }
                            },
                            {
                                "point_type": "DepositHelper",
                                "point": {
                                    "rate": 0.03,
                                    "tenor": {"n": 1, "unit": "Years"},
                                    "fixing_days": 2,
                                    "calendar": "TARGET",
                                    "business_day_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed"
                                }
                            },
                            {
                                "point_type": "SwapHelper",
                                "point": {
                                    "rate": 0.03,
                                    "calendar": "TARGET",
                                    "sw_fixed_leg_frequency": "Annual",
                                    "sw_fixed_leg_convention": "ModifiedFollowing",
                                    "sw_fixed_leg_day_counter": "Thirty360",
                                    "float_index": {"id": "EUR_6M"},
                                    "spread": 0.0,
                                    "fwd_start_days": 0,
                                    "tenor": {"n": 5, "unit": "Years"}
                                }
                            },
                            {
                                "point_type": "SwapHelper",
                                "point": {
                                    "rate": 0.03,
                                    "calendar": "TARGET",
                                    "sw_fixed_leg_frequency": "Annual",
                                    "sw_fixed_leg_convention": "ModifiedFollowing",
                                    "sw_fixed_leg_day_counter": "Thirty360",
                                    "float_index": {"id": "EUR_6M"},
                                    "spread": 0.0,
                                    "fwd_start_days": 0,
                                    "tenor": {"n": 10, "unit": "Years"}
                                }
                            }
                        ]
                    }]
                },
                "inflation": {
                    "inflation_indices": [{
                        "id": "EUHICP_YY", "family_name": "EU HICP YoY", "currency": "EUR",
                        "calendar": "TARGET", "day_counter": "Actual365Fixed",
                        "frequency": "Monthly",
                        "availability_lag": {"n": 2, "unit": "Months"},
                        "observation_lag": {"n": 3, "unit": "Months"},
                        "interpolated": True, "revised": False, "kind": "YoYInflation",
                        "fixings": [
                            {"date": "2024-10-01", "value": 0.0180},
                            {"date": "2024-11-01", "value": 0.0190}
                        ]
                    }],
                    "inflation_curves": [{
                        "id": "HICP_YY",
                        "reference_date": "2025-01-15",
                        "calendar": "TARGET",
                        "business_day_convention": "ModifiedFollowing",
                        "day_counter": "Actual365Fixed",
                        "interpolator": "Linear",
                        "bootstrap_accuracy": 1.0e-12,
                        "kind": "YoYInflation",
                        "index_id": "EUHICP_YY",
                        "discount_curve_id": "DISC",
                        "allow_extrapolation": True,
                        "points": [
                            {
                                "point_type": "YearOnYearInflationSwapHelper",
                                "point": {
                                    "quote_value": 0.0200,
                                    "swap_observation_lag": {"n": 3, "unit": "Months"},
                                    "tenor": {"n": 1, "unit": "Years"},
                                    "calendar": "TARGET",
                                    "payment_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed",
                                    "observation_interpolation": "Linear",
                                    "nominal_curve_id": "DISC"
                                }
                            },
                            {
                                "point_type": "YearOnYearInflationSwapHelper",
                                "point": {
                                    "quote_value": 0.0210,
                                    "swap_observation_lag": {"n": 3, "unit": "Months"},
                                    "tenor": {"n": 2, "unit": "Years"},
                                    "calendar": "TARGET",
                                    "payment_convention": "ModifiedFollowing",
                                    "day_counter": "Actual365Fixed",
                                    "observation_interpolation": "Linear",
                                    "nominal_curve_id": "DISC"
                                }
                            }
                        ]
                    }]
                }
            },
            "swaps": [{
                "year_on_year_inflation_swap": {
                    "swap_type": "Receiver",
                    "notional": 1000000.0,
                    "fixed_schedule": {
                        "effective_date": "2025-01-15",
                        "termination_date": "2027-01-15",
                        "calendar": "TARGET",
                        "frequency": "Annual",
                        "convention": "ModifiedFollowing",
                        "termination_date_convention": "ModifiedFollowing",
                        "date_generation_rule": "Forward"
                    },
                    "fixed_rate": 0.0204,
                    "fixed_day_counter": "Actual365Fixed",
                    "yoy_schedule": {
                        "effective_date": "2025-01-15",
                        "termination_date": "2027-01-15",
                        "calendar": "TARGET",
                        "frequency": "Annual",
                        "convention": "ModifiedFollowing",
                        "termination_date_convention": "ModifiedFollowing",
                        "date_generation_rule": "Forward"
                    },
                    "inflation_index_id": "EUHICP_YY",
                    "observation_lag": {"n": 3, "unit": "Months"},
                    "observation_interpolation": "Linear",
                    "spread": 0.0002,
                    "yoy_day_counter": "Actual365Fixed",
                    "payment_calendar": "TARGET",
                    "payment_convention": "ModifiedFollowing"
                },
                "discounting_curve": "DISC",
                "inflation_curve": "HICP_YY"
            }],
            "include_flows": True
        }

        response = client.price("year_on_year_inflation_swap", request)
        swaps = response.get("swaps", [])
        if len(swaps) != 1:
            result["error"] = f"Expected 1 swap, got {len(swaps)}"
            return result
        swap = swaps[0]
        for field in ("npv", "fair_rate", "fair_spread"):
            if not isinstance(swap.get(field), (int, float)):
                result["error"] = f"Missing numeric {field}"
                return result
        if len(swap.get("fixed_leg_flows", [])) != 2 or len(swap.get("yoy_leg_flows", [])) != 2:
            result["error"] = "Unexpected YYIIS flow counts"
            return result
        result["passed"] = True
        result["quantra_npv"] = float(swap["npv"])
        result["quantlib_npv"] = float(swap["npv"])
        result["diff"] = 0.0
        return result
    except Exception as e:
        result["error"] = str(e)
        return result
