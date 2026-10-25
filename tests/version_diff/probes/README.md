# Targeted probes for the cross-version diff

One small request per QuantLib release-note item that may change a number or
turn a working request into an error between QuantLib 1.41 (Quantra 0.10.0)
and 1.44. Where an item needs a trigger and a control, both are here. The
corpus is read by `tools/version_diff/quantra_version_diff.py --requests
tests/version_diff/probes`: the first-level directory is the endpoint, every
`.json` below it is one request body.

Every probe returns HTTP 200 on the published 0.10.0 image
(`ghcr.io/joseprupi/quantra-server:0.10.0`, QuantLib 1.41). The 0.10.0-vs-0.10.0
run over this corpus plus the functional catalog is 291/291 unchanged (noise
baseline: zero differences).

Upstream references are pull requests of lballabio/QuantLib (`#NNNN`), taken
from the release notes of 1.42, 1.43 and 1.44.

## Probes by release-note item

| Probe (under `tests/version_diff/probes/`) | Item | Request feature that triggers it | Response fields that would move |
|---|---|---|---|
| `price-ois-swap/ois_helpers_fixed_leg_convention_preceding.json`, `.../ois_helpers_fixed_leg_convention_modified_preceding.json` | 1.44 #2649: the OIS fixed leg now applies the payment adjustment it is given | `OISHelper.fixed_leg_convention` other than `Following` (`Preceding`, `ModifiedPreceding`) on the helpers of the discount curve | every pillar of the bootstrapped curve, hence `swaps[].npv`, `fair_rate`, `fair_spread`, `legs[].npv`, `flows[].discount` of anything priced on it |
| `price-ois-swap/ois_helpers_fixed_leg_convention_following_control.json` | control for #2649 | same trade, helpers with `Following` (the QuantLib default) | none expected |
| `price-ois-swap/ois_payment_lag_2_helpers_and_legs.json` | 1.44 #2649 (payment lag path) | `payment_lag: 2` on the OIS helpers and on both swap legs | curve pillars; `npv`, `fair_rate`, `flows[].payment_date` / `discount` |
| `price-ois-swap/ois_payment_lag_2_helpers_preceding.json` | 1.44 #2649, lag and convention together | helpers and legs with `payment_lag: 2` and `Preceding` payment convention | as above |
| `price-vanilla-swap/vanilla_as_of_on_target_holiday.json` | 1.44 #2653: the swap spot date is now derived from the actual evaluation date even when it is a holiday | `as_of_date` 2025-05-01 (TARGET holiday); swap helpers on the curve start from that date | curve pillars from the swap helpers; `npv`, `fair_rate`, `legs[].npv` (since 0.13.1 the deposit pillars with `fixing_days: 2` make this a 400 naming the remedy; also used by the error-contract suite) |
| `price-vanilla-swap/vanilla_as_of_on_business_day_control.json` | control for #2653 | same trade and curve, `as_of_date` 2025-04-30 | none expected |
| `price-vanilla-swap/vanilla_swap_helpers_uk_calendar_index_target.json` | 1.43 #2611: swap-helper spot date computed on the index fixing calendar | `SwapHelper.calendar: UnitedKingdom` with an index on TARGET, as of 2025-04-30 (UK spot 2025-05-02, TARGET spot 2025-05-05) | swap-helper pillars; `npv`, `fair_rate`, `legs[].npv` |
| `price-vanilla-swap/vanilla_swap_helpers_target_calendar_control.json` | control for #2611 | same, helpers on TARGET | none expected |
| `bootstrap-curves/curve_swap_helpers_spot_on_month_end_index_eom.json` | 1.42 #2459: end-of-month handling in the swap helpers' schedules | `as_of_date` 2025-01-29 (spot 2025-01-31, a month end), index `end_of_month: true` | `results[].values` (DF, ZERO, 6M FWD) at every grid point |
| `bootstrap-curves/curve_swap_helpers_spot_on_month_end_index_eom_false_control.json` | control for #2459 | same, index `end_of_month: false` | none expected |
| `bootstrap-curves/curve_ois_helpers_spot_on_month_end_index_eom.json` | 1.42 #2459: end-of-month handling in the OIS helpers' schedules | OIS helpers from spot 2025-01-31, overnight index `end_of_month: true` | `results[].values` (DF, ZERO) |
| `bootstrap-curves/curve_ois_helpers_spot_on_month_end_index_eom_false_control.json` | control for #2459 | same, index `end_of_month: false` | none expected |
| `bootstrap-curves/curve_dated_ois_helpers_with_settlement_days.json` | 1.42 #2481: an OIS built with both an effective date and settlement days is now rejected | `DatedOISHelper` entries (explicit dates) carrying `settlement_days: 2` | status (200 to error) if the dated helper reaches that check; otherwise `results[].values` |
| `bootstrap-curves/curve_discount_trait_linear_interpolation.json` | 1.42 #2491: log interpolation for discount curves (scope to verify) | `bootstrap_trait: Discount` with `interpolator: Linear`, sampled between pillars | `results[].values` (DF, ZERO, FWD) off-pillar |
| `bootstrap-curves/curve_discount_trait_loglinear_interpolation_control.json` | control for #2491 | same with `LogLinear` | none expected |
| `price-ois-swap/ois_seasoned_mid_period_simple_averaging.json` | 1.42 #2427: accrued amount of simple-averaging overnight coupons | OIS started 2024-10-17, valued 2025-01-15 inside the first coupon, `averaging_method: Simple`, ESTR fixings supplied | `legs[1].flows[0].amount` / `rate`, `legs[1].npv`, `npv`, `fair_rate`, `fair_spread` (the accrued amount itself is not a response field; the probe shows whether the running coupon moves) |
| `price-ois-swap/ois_seasoned_mid_period_compound_averaging_control.json` | control for #2427 | same trade with `Compound` averaging | none expected from #2427 (1.43 #2556 reworked the compounding pricer: may move) |
| `price-ois-swap/ois_overnight_payment_date_before_accrual_end.json` | 1.42 #2421: an overnight coupon paid before its accrual end is now rejected | overnight leg on a TARGET schedule ending 2025-07-04 (US holiday) with `payment_calendar: UnitedStatesGovernmentBond` and `payment_convention: Preceding`, so the payment date is 2025-07-03 | status: 200 on 1.41 (the body carries non-finite `inf` values for that coupon and the swap `npv`) to an error on 1.42 and later |
| `price-ois-swap/ois_overnight_payment_date_after_accrual_end_control.json` | control for #2421 | same trade, `Following` (payment 2025-07-07) | none expected |
| `price-ois-swap/ois_lookback2_lockout2_observation_shift.json` | 1.43 #2556: compounding overnight coupon pricer reworked | overnight leg with `lookback_days: 2`, `lockout_days: 2`, `apply_observation_shift: true`, 5 bp spread | `legs[1].flows[].rate` / `amount`, `legs[1].npv`, `npv`, `fair_rate`, `fair_spread` |
| `price-vanilla-swap/vanilla_backward_schedule_short_first_period.json` | 1.42 #2511: first-period regularity check in backward schedule generation | `date_generation_rule: Backward` with a short first period (2025-01-17 to the first regular date) | `legs[].flows[].accrual_start_date` / `accrual_end_date` / `amount`, `npv`, `fair_rate` |
| `price-vanilla-swap/vanilla_matured_before_as_of_zero_annuity.json`, `price-ois-swap/ois_matured_before_as_of_zero_annuity.json`, `price-basis-swap/basis_matured_before_as_of_zero_annuity.json`, `price-cross-currency-swap/xccy_matured_before_as_of_zero_annuity.json` | 1.44 #2678 / #2690: zero-BPS guards in fair rate / fair spread | every flow before `as_of_date`, so both legs' BPS are zero | on 0.10.0: `npv: 0.0`, no `fair_rate` / `fair_spread` (the server computes them itself and drops non-finite values); a status change would show if the guard is reached |
| `price-basis-swap/basis_3m_vs_6m_fair_spread_control.json`, `price-cross-currency-swap/xccy_sofr_vs_estr_fair_spread_control.json` | control for #2690 | live basis and cross-currency swaps with a reported `fair_spread` | none expected |
| `price-zero-coupon-inflation-swap/zciis_3y_as_index_observation.json`, `.../zciis_5y_linear_observation.json`, `.../zciis_5y_flat_observation.json`, `.../zciis_5y_6m_observation_lag.json`, `.../zciis_5y_dated_helpers.json` | 1.42 #2451 (fixed-leg year fraction on contract dates) and #2456 (inflation bootstrap pillar dates for linear observation) | `observation_interpolation` AsIndex / Linear / Flat on the swap and on the curve helpers; a 6M lag; dated helpers | `swaps[].npv`, `fair_rate`, `fixed_leg_bps`, leg NPVs (already moved on 1.43: see plan 14 section 5.3) |
| `price-year-on-year-inflation-swap/yyiis_5y_linear_observation.json`, `.../yyiis_5y_as_index_observation.json` | 1.42 #2456; 1.44 #2692 (zero-BPS guard in the YoY swap fair rate) | year-on-year swap with Linear vs AsIndex observation on the swap and the helpers | `npv`, `fair_rate`, `fair_spread`, leg NPVs |
| `price-year-on-year-inflation-cap-floor/yoy_cap_5y_black.json`, `.../yoy_cap_5y_bachelier.json`, `.../yoy_floor_5y_black.json` | 1.42 #2456 (same YoY curve) | cap / floor on the YoY curve built from Linear helpers | `cap_floors[].npv`, `atm_rate` |
| `price-callable-fixed-rate-bond/callable_call_date_on_coupon_date.json` | 1.44 #2748: a coupon was lost when a callability fell on a coupon date | call date 2029-01-17 = a coupon date | `bonds[].npv`, `clean_price`, `dirty_price` |
| `price-callable-fixed-rate-bond/callable_call_date_between_coupon_dates_control.json` | control for #2748; also 1.43 #2615 (trinomial tree branching) | call date 2029-07-17, between coupons | tree-related moves only |
| `price-callable-fixed-rate-bond/callable_multi_call_step_down.json`, `.../puttable_put_98.json` | 1.43 #2615, 1.44 #2745 (tree stepback) | several call dates; a put | `bonds[].npv`, `clean_price`, `dirty_price` |
| `price-fixed-rate-bond/frb_5y_at_par_yield_duration_convexity_bps.json`, `.../frb_7y_premium_semiannual_yield_duration_convexity_bps.json`, `.../frb_5y_deep_discount_1pct_coupon_yield_solver.json` | 1.43 #2589 (yield solver derivative), 1.44 #2769 (`basisPointValue` convexity term) | `bond_pricing_details: true`, yield `Compounded` Annual / Semiannual; par, premium and deep-discount bonds | `yield`, `macaulay_duration`, `modified_duration`, `convexity`; `bps` (note: `bps` is the curve-based BPS, not the yield-based `basisPointValue`, so #2769 is not expected to reach it) |
| `price-floating-rate-bond/frn_5y_yield_duration_convexity.json` | 1.43 #2589 | floating-rate bond with `bond_pricing_details: true` | `yield`, durations, `convexity`, `bps` |
| `price-zero-coupon-bond/zcb_1y_yield_duration_convexity.json`, `.../zcb_10y_yield_duration_convexity.json` | 1.43 #2589 (moved on 1.43 by about 5e-9 in yield) | zero-coupon bonds with details | `yield`, `macaulay_duration`, `modified_duration`, `convexity`, `bps` |
| `price-cds/cds_5y_par_spread_helpers_midpoint.json`, `.../cds_5y_par_spread_helpers_isda.json`, `.../cds_5y_upfront_quotes.json`, `.../cds_5y_helpers_trade_date_before_as_of.json` | 1.44 #2698: CDS helpers use the default trade-date calculation | credit curve bootstrapped from par-spread or upfront quotes (`helper_model` MidPoint / ISDA); a trade dated before the as-of | `cds_list[].npv`, `fair_spread`, `fair_upfront`, leg NPVs, `default_probability` fields |
| `price-cds/cds_5y_flat_hazard_control.json` | control for #2698 | flat hazard rate, no helpers | none expected |
| `price-swaption/swaption_bermudan_hull_white_lattice_explicit.json`, `.../swaption_american_hull_white_lattice_explicit.json` | 1.43 #2615 (tree branching), #2592 (Hull-White r0 on relink), 1.44 #2745 | `HullWhiteLattice` model with explicit a / sigma, Bermudan and American exercise | `swaptions[].npv` |
| `price-swaption/swaption_bermudan_hull_white_lattice_calibrated.json` | 1.43 #2592, #2615; 1.44 #2664 (Jamshidian root finding, used by the calibration helpers) | `param_mode: Calibrate` on an ATM matrix | `npv`, `used_hw_a`, `used_hw_sigma`, `used_hw_rmse` |
| `price-swaption/swaption_european_black_atm_matrix_control.json` | control (no tree, no calibration) | European swaption on Black | none expected |
| `calibrate-swaption-model/hull_white_calibration_a_and_sigma.json`, `.../hull_white_calibration_sigma_only.json` | 1.43 #2592; 1.44 #2664 | Hull-White calibration to an ATM matrix | `hw_a`, `hw_sigma`, `rmse` |
| `calibrate-swaption-vol/sabr_cube_beta_fixed.json`, `.../sabr_cube_vega_weighted.json` | 1.44 #2656: SABR swaption-cube calibration preserves ATM vols | `SwaptionSabrCalibrateSpec` on a 2x2x5 cube, beta fixed / vega-weighted fit | calibrated SABR parameters and errors per node (`diagnostics`) |
| `price-equity-option/equity_american_put_fd_exercise_starts_after_as_of.json` | 1.42 #2449: finite-difference engines respect the earliest American exercise date | American exercise `start_date` 2025-07-15, six months after the as-of (the server prices American and Bermudan options on the finite-difference Black-Scholes engine) | `options[].npv`, `delta`, `gamma`, `vega`, `theta`, `rho` |
| `price-equity-option/equity_american_put_fd_control.json`, `.../equity_bermudan_call_fd.json` | control for #2449 | exercise window starting at the as-of; Bermudan dates | none expected |
| `price-equity-option/equity_american_call_fd_zero_rate.json`, `.../equity_american_put_fd_negative_rate.json`, `.../equity_american_put_fd_tiny_vol.json` | 1.44 American-option edge cases (zero / negative rate, vanishing vol) | risk-free zero rate 0.0, -0.5%, constant vol 0.1% | `options[].npv` and greeks (the upstream fixes target the analytic approximations, which the API does not expose; these show the finite-difference engine on the same inputs) |
| `calendar-holidays/holidays_hong_kong_2025_2027.json`, `holidays_indonesia_2025_2027.json`, `holidays_south_korea_2025_2027.json`, `holidays_south_africa_2025_2027.json`, `holidays_saudi_arabia_2025_2027.json`, `holidays_italy_2025_2027.json`, `holidays_singapore_2025_2027.json`, `holidays_china_2025_2027.json`, `holidays_taiwan_2025_2027.json`, `holidays_india_2025_2027.json`, `holidays_israel_2025_2027.json` | calendar updates: 1.42 China 2026, Taiwan 2025-26, Singapore 2026; 1.43 Israel, South Korea 2025, India 2026; 1.44 Hong Kong 2026, Indonesia, South Korea, South Africa, Saudi Arabia, Italy, Singapore 2027 | `calendar-holidays` 2025-01-01 to 2027-12-31 per calendar | `dates` (holidays added or removed) |
| `calendar-holidays/holidays_target_2025_2027.json` | control | TARGET, same window | none expected |
| `calendar-holidays/holidays_hong_kong_2026_control.json` | 1.44 Hong Kong 2026 | Hong Kong, 2026 only (on 1.41 the 2026 list has no Lunar New Year days) | `dates` |
| `calendar-holidays/holidays_hong_kong_2026_override_adds_lunar_new_year.json`, `calendar-advance/advance_hong_kong_2026_02_16_plus_1_day_override_lunar_new_year.json` | holiday overrides after an upgrade that adds the holiday natively | `calendar_overrides` adding 2026-02-17/18/19 to Hong Kong | none expected: the override is a no-op once the calendar carries the dates, so these answers stay the same while the un-overridden `..._control` requests change |
| `calendar-advance/advance_hong_kong_2026_02_16_plus_1_day_control.json` | 1.44 Hong Kong 2026 | advance one business day over the (not yet native) Lunar New Year | `advanced_date` |

## Release-note items the API does not reach

These items have no probe because no request shape can trigger them on the
0.10.0 wire; they are expected to be invisible to clients.

| Item | Why it cannot be reached |
|---|---|
| 1.42 #2480: `MakeVanillaSwap` fixed-tenor inference with an explicit termination date | swap helpers are built from a tenor (`SwapHelper.tenor`); no termination date is passed |
| 1.42 #2455: callable bond call price accrued during an ex-coupon period | `CallableFixedRateBond` has no ex-coupon fields |
| 1.42 #2495: initial guess for spread discount curves | no spread-over-curve term structure in the API |
| 1.42 #2421 via a negative payment lag | `payment_lag` must be >= 0 on every leg and helper (400 naming the field); the trigger used instead is a `Preceding` payment calendar, see the probe above |
| 1.44 #2671: callable bond effective duration / convexity on the dirty price | the callable bond response has `npv`, `clean_price`, `dirty_price`, `settlement_date` only |
| 1.44 #2667: `SimpleThenCompounded` / `CompoundedThenSimple` yields | on 0.10.0 every yield convention other than `Compounded` is a 422 (`compounded rate required`: the Macaulay duration needs a compounded yield), so no 200 request exists; `CompoundedThenSimple` is not in the `Compounding` enum |
| 1.44 #2726: overnight-index futures accrual at a holiday maturity | `FutureHelper` builds IBOR futures only |
| 1.44 #2771: jumps on probability curves | `CreditCurveSpec` has no jump dates / values |
| 1.44 American-option approximation fixes (Barone-Adesi-Whaley, Bjerksund-Stensland, Ju) | American and Bermudan equity options price on the finite-difference Black-Scholes engine; no approximation engine is exposed |
| 1.44 new calendars Malaysia, Philippines | not in the `Calendar` enum |
| 1.44 #2692 for a zero-coupon inflation swap entirely in the past | a matured inflation swap needs historical index fixings on both sides; not probed |

## Regenerating

The probes are plain JSON files; edit them in place. Dates are in the era of
`examples/data` (as of 2025-01-15 unless the item needs another date) so no
fixings are needed except where the probe supplies them.
