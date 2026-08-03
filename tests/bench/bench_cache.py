#!/usr/bin/env python3
"""
Quantra Curve Cache Benchmark

Modes:
    bond     - 1 curve, 8 helpers (3 deposits + 5 bonds)
    swap     - 2 EUR curves, 24 helpers, OIS dep chain
    hwcalib  - Hull-White swaption-model calibration, 1 curve (9 helpers)
               against a 6x4 -> 24-node ATM vol grid (24 calibration helpers)
"""

import json, time, argparse, statistics, requests

# --- BOND: 1 curve, 8 helpers (3 deposits + 5 bonds) ---
BOND_REQUEST = {
    "pricing": {
        "as_of_date": "2008-09-15", "settlement_date": "2008-09-18",
        "rates": {"curves": [{
            "id": "depos_curve", "day_counter": "ActualActualISDA", "interpolator": "LogLinear", "reference_date": "2008-09-18", "bootstrap_trait": "Discount",
            "points": [
                {"point_type": "DepositHelper", "point": {"rate": 0.0096, "tenor": {"n": 3, "unit": "Months"}, "fixing_days": 3, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed"}},
                {"point_type": "DepositHelper", "point": {"rate": 0.0145, "tenor": {"n": 6, "unit": "Months"}, "fixing_days": 3, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed"}},
                {"point_type": "DepositHelper", "point": {"rate": 0.0194, "tenor": {"n": 12, "unit": "Months"}, "fixing_days": 3, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed"}},
                {"point_type": "BondHelper", "point": {"price": 100.390625, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2005-03-15", "termination_date": "2010-08-31", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.02375, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2005-03-15"}},
                {"point_type": "BondHelper", "point": {"price": 106.21875, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2005-06-15", "termination_date": "2011-08-31", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.04625, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2005-06-15"}},
                {"point_type": "BondHelper", "point": {"price": 100.59375, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2006-06-30", "termination_date": "2013-08-31", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.03125, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2006-06-30"}},
                {"point_type": "BondHelper", "point": {"price": 101.6875, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2002-11-15", "termination_date": "2018-08-15", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.04, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "2002-11-15"}},
                {"point_type": "BondHelper", "point": {"price": 102.140625, "settlement_days": 3, "face_amount": 100, "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "1987-05-15", "termination_date": "2038-05-15", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}, "coupon_rate": 0.045, "day_counter": "ActualActualBond", "business_day_convention": "Unadjusted", "redemption": 100, "issue_date": "1987-05-15"}},
            ]
        }]}
    },
    "bonds": [{
        "fixed_rate_bond": {
            "settlement_days": 3, "face_amount": 100, "rate": 0.045,
            "accrual_day_counter": "ActualActualBond", "payment_convention": "ModifiedFollowing",
            "redemption": 100, "issue_date": "2007-05-15",
            "schedule": {"calendar": "UnitedStatesGovernmentBond", "effective_date": "2007-05-15", "termination_date": "2017-05-15", "frequency": "Semiannual", "convention": "Unadjusted", "termination_date_convention": "Unadjusted", "date_generation_rule": "Backward"}
        },
        "discounting_curve": "depos_curve",
        "yield": {"day_counter": "Actual360", "compounding": "Compounded", "frequency": "Annual"}
    }]
}

# --- SWAP: EUR multicurve, 24 helpers (OIS + Euribor 6M with dep chain) ---
SWAP_REQUEST = {
    "pricing": {
        "as_of_date": "2024-01-15",
        "rates": {"indices": [
            {"id": "EUR_6M", "name": "Euribor", "index_type": "Ibor", "tenor": {"n": 6, "unit": "Months"}, "fixing_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual360", "end_of_month": True, "currency": "EUR"},
            {"id": "EUR_ESTR", "name": "ESTR", "index_type": "Overnight", "tenor": {"n": 0, "unit": "Days"}, "fixing_days": 0, "calendar": "TARGET", "business_day_convention": "Following", "day_counter": "Actual360", "currency": "EUR"}
        ],
        "curves": [
            {
                "id": "EUR_OIS", "day_counter": "Actual365Fixed", "interpolator": "LogLinear", "bootstrap_trait": "Discount",
                "points": [
                    {"point_type": "DepositHelper", "point": {"rate": 0.0390, "tenor": {"n": 1, "unit": "Days"}, "fixing_days": 0, "calendar": "TARGET", "business_day_convention": "Following", "day_counter": "Actual360"}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0385, "tenor": {"n": 1, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0365, "tenor": {"n": 2, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0352, "tenor": {"n": 3, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0343, "tenor": {"n": 4, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0335, "tenor": {"n": 5, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0325, "tenor": {"n": 7, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0315, "tenor": {"n": 10, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0305, "tenor": {"n": 15, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0298, "tenor": {"n": 20, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                    {"point_type": "OISHelper", "point": {"rate": 0.0288, "tenor": {"n": 30, "unit": "Years"}, "overnight_index": {"id": "EUR_ESTR"}, "settlement_days": 2, "calendar": "TARGET", "fixed_leg_frequency": "Annual", "fixed_leg_convention": "Following", "payment_lag": 0, "averaging_method": "Compound", "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": False}},
                ]
            },
            {
                "id": "EUR_6M_CURVE", "day_counter": "Actual365Fixed", "interpolator": "LogLinear", "bootstrap_trait": "Discount",
                "points": [
                    {"point_type": "DepositHelper", "point": {"rate": 0.0395, "tenor": {"n": 6, "unit": "Months"}, "fixing_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual360"}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0370, "tenor": {"n": 2, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0358, "tenor": {"n": 3, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0348, "tenor": {"n": 4, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0340, "tenor": {"n": 5, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0335, "tenor": {"n": 6, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0330, "tenor": {"n": 7, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0326, "tenor": {"n": 8, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0320, "tenor": {"n": 10, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0316, "tenor": {"n": 12, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0312, "tenor": {"n": 15, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0308, "tenor": {"n": 20, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                    {"point_type": "SwapHelper", "point": {"rate": 0.0302, "tenor": {"n": 30, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "float_index": {"id": "EUR_6M"}, "deps": {"discount_curve": {"id": "EUR_OIS"}}}},
                ]
            }
        ]}
    },
    "swaps": [{
        "vanilla_swap": {
            "swap_type": "Payer",
            "fixed_leg": {
                "schedule": {"calendar": "TARGET", "effective_date": "2024-01-17", "termination_date": "2034-01-17", "frequency": "Annual", "convention": "ModifiedFollowing", "termination_date_convention": "ModifiedFollowing", "date_generation_rule": "Forward", "end_of_month": False},
                "notional": 10000000, "rate": 0.032, "day_counter": "Thirty360", "payment_convention": "ModifiedFollowing"
            },
            "floating_leg": {
                "schedule": {"calendar": "TARGET", "effective_date": "2024-01-17", "termination_date": "2034-01-17", "frequency": "Semiannual", "convention": "ModifiedFollowing", "termination_date_convention": "ModifiedFollowing", "date_generation_rule": "Forward", "end_of_month": False},
                "notional": 10000000, "index": {"id": "EUR_6M"}, "spread": 0.0, "day_counter": "Actual360", "payment_convention": "ModifiedFollowing"
            }
        },
        "discounting_curve": "EUR_OIS",
        "forwarding_curve": "EUR_6M_CURVE"
    }]
}

# --- HWCALIB: Hull-White swaption-model calibration ---
# Derived from examples/data/calibrate_swaption_model_request.json but enriched
# so the calibration is representative rather than a trivial 2x2: the discount
# curve carries 9 helpers (out to 30Y) and the ATM swaption vol surface is a
# 6x4 grid (expiries 1/2/3/5/7/10Y x tenors 2/5/7/10Y) => 24 SwaptionHelpers.
# The market-style lognormal vols are not Hull-White-consistent, so the full
# two-parameter Levenberg-Marquardt fit runs to a genuine minimum (the same
# uncached cost the calibration cache will later target). Longest node is
# 10Y-into-10Y (20Y), comfortably inside the 30Y curve pillar.
HWCALIB_REQUEST = {
    "pricing": {
        "as_of_date": "2025-01-15",
        "rates": {
            "indices": [
                {"id": "EUR_6M", "name": "Euribor", "index_type": "Ibor", "tenor": {"n": 6, "unit": "Months"}, "fixing_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual360", "end_of_month": False, "currency": "EUR"}
            ],
            "swap_indices": [
                {"id": "EUR_SWAP_6M", "kind": "IborSwapIndex", "spot_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "end_of_month": False,
                 "fixed_leg": {"fixed_frequency": "Annual", "fixed_day_counter": "Thirty360", "fixed_calendar": "TARGET", "fixed_bdc": "ModifiedFollowing", "fixed_term_bdc": "ModifiedFollowing", "fixed_date_rule": "Forward", "fixed_eom": False},
                 "float_index_id": "EUR_6M",
                 "float_leg": {"float_tenor": {"n": 6, "unit": "Months"}, "float_calendar": "TARGET", "float_bdc": "ModifiedFollowing", "float_term_bdc": "ModifiedFollowing", "float_date_rule": "Forward", "float_eom": False}}
            ],
            "curves": [
                {
                    "id": "discount", "day_counter": "Actual365Fixed", "interpolator": "LogLinear", "bootstrap_trait": "Discount", "reference_date": "2025-01-15",
                    "points": [
                        {"point_type": "DepositHelper", "point": {"rate": 0.0295, "tenor": {"n": 6, "unit": "Months"}, "fixing_days": 2, "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual360"}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0300, "tenor": {"n": 1, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0305, "tenor": {"n": 2, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0310, "tenor": {"n": 3, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0320, "tenor": {"n": 5, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0328, "tenor": {"n": 7, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0335, "tenor": {"n": 10, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0342, "tenor": {"n": 15, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0346, "tenor": {"n": 20, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                        {"point_type": "SwapHelper", "point": {"rate": 0.0350, "tenor": {"n": 30, "unit": "Years"}, "calendar": "TARGET", "sw_fixed_leg_frequency": "Annual", "sw_fixed_leg_convention": "ModifiedFollowing", "sw_fixed_leg_day_counter": "Thirty360", "spread": 0.0, "fwd_start_days": 0, "float_index": {"id": "EUR_6M"}}},
                    ]
                }
            ]
        },
        "volatility": {
            "vol_surfaces": [
                {
                    "id": "swaption_atm", "payload_type": "SwaptionVolSpec",
                    "payload": {
                        "swap_index_id": "EUR_SWAP_6M", "payload_type": "SwaptionVolAtmMatrixSpec",
                        "payload": {
                            "base": {"reference_date": "2025-01-15", "calendar": "TARGET", "business_day_convention": "ModifiedFollowing", "day_counter": "Actual365Fixed", "volatility_type": "Lognormal", "displacement": 0.0},
                            "expiries": [{"n": 1, "unit": "Years"}, {"n": 2, "unit": "Years"}, {"n": 3, "unit": "Years"}, {"n": 5, "unit": "Years"}, {"n": 7, "unit": "Years"}, {"n": 10, "unit": "Years"}],
                            "tenors": [{"n": 2, "unit": "Years"}, {"n": 5, "unit": "Years"}, {"n": 7, "unit": "Years"}, {"n": 10, "unit": "Years"}],
                            "vols": {
                                "n_rows": 6, "n_cols": 4,
                                "values": [
                                    0.240, 0.230, 0.222, 0.210,
                                    0.235, 0.225, 0.217, 0.206,
                                    0.228, 0.219, 0.212, 0.202,
                                    0.218, 0.210, 0.204, 0.196,
                                    0.208, 0.201, 0.196, 0.190,
                                    0.196, 0.191, 0.187, 0.183,
                                ]
                            }
                        }
                    }
                }
            ],
            "models": [
                {
                    "id": "hw_model", "payload_type": "SwaptionModelSpec",
                    "payload": {
                        "model_type": "HullWhiteLattice", "lattice_steps": 50, "param_mode": "Calibrate",
                        "hw_calibration": {
                            "swaption_vol_id": "swaption_atm", "discount_curve_id": "discount", "forwarding_curve_id": "discount", "swap_index_id": "EUR_SWAP_6M",
                            "calibrate_a": True, "calibrate_sigma": True, "a_init": 0.03, "sigma_init": 0.01,
                            "max_iterations": 200, "function_evaluations": 1000, "end_criteria_eps": 1e-08
                        }
                    }
                }
            ]
        }
    },
    "model_id": "hw_model"
}


def _hwcalib_result(r: dict):
    """Pull the calibrated Hull-White a / sigma from a calibrate-swaption-model
    response (top-level hw_a / hw_sigma; falls back to a nested model object)."""
    src = r if "hw_a" in r else r.get("model", r.get("models", [{}])[0] if isinstance(r.get("models"), list) else {})
    return f"a={src.get('hw_a')}, sigma={src.get('hw_sigma')}, rmse={src.get('rmse')}"


# Mode config
MODES = {
    "bond":    {"payload": BOND_REQUEST,    "endpoint": "price-fixed-rate-bond",       "npv_path": lambda r: r.get("bonds", [{}])[0].get("npv"), "desc": "1 curve, 8 helpers"},
    "swap":    {"payload": SWAP_REQUEST,    "endpoint": "price-vanilla-swap",          "npv_path": lambda r: r.get("swaps", [{}])[0].get("npv"), "desc": "EUR 2 curves, 24 helpers, OIS dep chain"},
    "hwcalib": {"payload": HWCALIB_REQUEST, "endpoint": "calibrate-swaption-model",     "npv_path": _hwcalib_result,                              "desc": "Hull-White calibration, 1 curve/9 helpers, 6x4=24 vol grid"},
}


# =============================================================================
# Benchmark runner
# =============================================================================

def run_benchmark(url: str, mode: str, n_requests: int = 100, warmup: int = 3) -> dict:
    cfg = MODES[mode]
    session = requests.Session()
    session.headers.update({"Content-Type": "application/json"})
    endpoint = f"{url.rstrip('/')}/{cfg['endpoint']}"
    payload = json.dumps(cfg["payload"])

    try:
        r = session.get(f"{url.rstrip('/')}/health", timeout=5)
        assert r.status_code == 200
    except Exception as e:
        print(f"ERROR: Cannot reach server at {url}: {e}")
        return None

    print(f"  Mode: {mode} ({cfg['desc']})")
    print(f"  Warming up ({warmup} requests)...")
    for i in range(warmup):
        r = session.post(endpoint, data=payload, timeout=30)
        if r.status_code != 200:
            print(f"  ERROR on warmup {i}: {r.status_code} - {r.text[:300]}")
            return None

    r = session.post(endpoint, data=payload, timeout=30)
    baseline_result = r.json()
    try:
        print(f"  Result: {cfg['npv_path'](baseline_result)}")
    except Exception:
        pass

    print(f"  Running {n_requests} requests...")
    latencies = []
    errors = 0

    for i in range(n_requests):
        t0 = time.perf_counter()
        r = session.post(endpoint, data=payload, timeout=30)
        t1 = time.perf_counter()
        if r.status_code == 200:
            latencies.append((t1 - t0) * 1000)
        else:
            errors += 1

    if not latencies:
        print("  ERROR: All requests failed")
        return None

    return {
        "n_requests": n_requests,
        "errors": errors,
        "latencies_ms": latencies,
        "mean_ms": statistics.mean(latencies),
        "median_ms": statistics.median(latencies),
        "p95_ms": sorted(latencies)[int(len(latencies) * 0.95)],
        "p99_ms": sorted(latencies)[int(len(latencies) * 0.99)],
        "min_ms": min(latencies),
        "max_ms": max(latencies),
        "stdev_ms": statistics.stdev(latencies) if len(latencies) > 1 else 0,
        "total_s": sum(latencies) / 1000,
        "throughput_rps": len(latencies) / (sum(latencies) / 1000),
        "result_sample": baseline_result,
    }


def print_results(tag: str, results: dict):
    print(f"\n{'='*60}")
    print(f"  {tag}")
    print(f"{'='*60}")
    print(f"  Requests:    {results['n_requests']} ({results['errors']} errors)")
    print(f"  Mean:        {results['mean_ms']:.2f} ms")
    print(f"  Median:      {results['median_ms']:.2f} ms")
    print(f"  P95:         {results['p95_ms']:.2f} ms")
    print(f"  P99:         {results['p99_ms']:.2f} ms")
    print(f"  Min:         {results['min_ms']:.2f} ms")
    print(f"  Max:         {results['max_ms']:.2f} ms")
    print(f"  Stdev:       {results['stdev_ms']:.2f} ms")
    print(f"  Throughput:  {results['throughput_rps']:.1f} req/s")
    print(f"  Total time:  {results['total_s']:.2f} s")
    print(f"{'='*60}")


def main():
    parser = argparse.ArgumentParser(description="Quantra curve cache benchmark")
    parser.add_argument("--url", default="http://localhost:8080")
    parser.add_argument("--tag", default="benchmark")
    parser.add_argument("-n", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--mode", choices=list(MODES.keys()), default="bond",
                        help="bond=1 curve/8 helpers, swap=EUR multicurve 24 helpers")
    args = parser.parse_args()

    print(f"\n--- {args.tag} ({args.url}) ---")
    results = run_benchmark(args.url, args.mode, args.n, args.warmup)
    if results:
        print_results(args.tag, results)
        outfile = f"bench_{args.tag.replace(' ', '_')}.json"
        with open(outfile, 'w') as f:
            json.dump({"tag": args.tag, "mode": args.mode,
                        "latencies_ms": results["latencies_ms"],
                        "mean_ms": results["mean_ms"],
                        "median_ms": results["median_ms"],
                        "p95_ms": results["p95_ms"]}, f, indent=2)
        print(f"\n  Raw data saved to: {outfile}")


if __name__ == "__main__":
    main()