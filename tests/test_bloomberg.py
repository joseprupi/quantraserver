"""
Call Quantra JSON API with OISRateHelper curve construction
and rebump analytics enabled to mirror Claude's setup.
"""

import json
import requests

API_URL = "http://localhost:8080/price-swaption"

curve_data = [
    ("1W",  5.33440),
    ("2W",  5.33509),
    ("3W",  5.33938),
    ("1M",  5.34440),
    ("2M",  5.21415),
    ("3M",  5.12540),
    ("4M",  5.03610),
    ("5M",  4.91730),
    ("6M",  4.80250),
    ("7M",  4.72188),
    ("8M",  4.62320),
    ("9M",  4.53830),
    ("10M", 4.45405),
    ("11M", 4.37650),
    ("12M", 4.29980),
    ("18M", 3.94167),
    ("2Y",  3.74750),
    ("3Y",  3.52740),
    ("4Y",  3.42450),
    ("5Y",  3.38055),
    ("6Y",  3.36510),
    ("7Y",  3.36140),
    ("8Y",  3.36680),
    ("9Y",  3.37600),
    ("10Y", 3.38780),
    ("12Y", 3.41870),
    ("15Y", 3.45420),
    ("20Y", 3.45530),
    ("25Y", 3.38780),
    ("30Y", 3.30880),
    ("40Y", 3.11350),
    ("50Y", 2.92100),
]


def tenor_unit(tenor: str) -> str:
    unit = tenor[-1].upper()
    mapping = {"W": "Weeks", "M": "Months", "Y": "Years"}
    return mapping[unit]


def tenor_number(tenor: str) -> int:
    return int(tenor[:-1])


points = []
for tenor, rate_pct in curve_data:
    points.append({
        "point_type": "OISHelper",
        "point": {
            "rate": rate_pct / 100.0,
            "tenor_number": tenor_number(tenor),
            "tenor_time_unit": tenor_unit(tenor),
            "overnight_index": {"id": "USD_SOFR"},
            "settlement_days": 2,
            "calendar": "UnitedStatesGovernmentBond",
            "fixed_leg_frequency": "Annual",
            "fixed_leg_convention": "ModifiedFollowing",
            "fixed_leg_day_counter": "Actual360"
        }
    })


payload = {
    "pricing": {
        "as_of_date": "2024-08-14",
        "swaption_pricing_rebump": True,
        "curves": [
            {
                "id": "USD_SOFR",
                "day_counter": "Actual360",
                "interpolator": "BackwardFlat",
                "reference_date": "2024-08-14",
                "bootstrap_trait": "FwdRate",
                "points": points
            }
        ],
        "vol_surfaces": [
            {
                "id": "usd_sofr_swo_vol",
                "payload_type": "SwaptionVolSpec",
                "payload": {
                    "payload_type": "SwaptionVolConstantSpec",
                    "payload": {
                        "base": {
                            "reference_date": "2024-08-14",
                            "calendar": "UnitedStatesGovernmentBond",
                            "business_day_convention": "ModifiedFollowing",
                            "day_counter": "Actual365Fixed",
                            "shape": "Constant",
                            "volatility_type": "Normal",
                            "displacement": 0.0,
                            "constant_vol": 0.010267
                        }
                    }
                }
            }
        ],
        "models": [
            {
                "id": "bachelier_model",
                "payload_type": "SwaptionModelSpec",
                "payload": {"model_type": "Bachelier"}
            }
        ],
        "indices": [
            {
                "id": "USD_SOFR",
                "name": "SOFR",
                "index_type": "Overnight",
                "tenor_number": 0,
                "tenor_time_unit": "Days",
                "fixing_days": 0,
                "calendar": "UnitedStatesGovernmentBond",
                "business_day_convention": "Following",
                "day_counter": "Actual360",
                "currency": "USD"
            }
        ]
    },
    "swaptions": [
        {
            "swaption": {
                "exercise_type": "European",
                "settlement_type": "Cash",
                "settlement_method": "ParYieldCurve",
                "exercise_date": "2024-09-16",
                "underlying_type": "OisSwap",
                "underlying": {
                    "swap_type": "Payer",
                    "fixed_leg": {
                        "notional": 1000000.0,
                        "schedule": {
                            "calendar": "UnitedStatesGovernmentBond",
                            "effective_date": "2024-09-18",
                            "termination_date": "2034-09-18",
                            "frequency": "Annual",
                            "convention": "ModifiedFollowing",
                            "termination_date_convention": "ModifiedFollowing",
                            "date_generation_rule": "Forward"
                        },
                        "rate": 0.03367463,
                        "day_counter": "Actual360",
                        "payment_convention": "ModifiedFollowing"
                    },
                    "overnight_leg": {
                        "notional": 1000000.0,
                        "schedule": {
                            "calendar": "UnitedStatesGovernmentBond",
                            "effective_date": "2024-09-18",
                            "termination_date": "2034-09-18",
                            "frequency": "Annual",
                            "convention": "ModifiedFollowing",
                            "termination_date_convention": "ModifiedFollowing",
                            "date_generation_rule": "Forward"
                        },
                        "index": {"id": "USD_SOFR"},
                        "spread": 0.0,
                        "day_counter": "Actual360",
                        "payment_convention": "ModifiedFollowing",
                        "payment_calendar": "UnitedStatesGovernmentBond",
                        "payment_lag": 2,
                        "averaging_method": "Compound",
                        "lookback_days": -1,
                        "lockout_days": 0,
                        "apply_observation_shift": False,
                        "telescopic_value_dates": False
                    }
                }
            },
            "discounting_curve": "USD_SOFR",
            "forwarding_curve": "USD_SOFR",
            "volatility": "usd_sofr_swo_vol",
            "model": "bachelier_model"
        }
    ]
}


def main() -> None:
    resp = requests.post(API_URL, json=payload, timeout=30)
    resp.raise_for_status()
    data = resp.json()
    print(json.dumps(data, indent=2))


if __name__ == "__main__":
    main()