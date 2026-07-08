"""Functional parity catalog — single source of truth.

Every entry describes one "POST JSON to the server, compare the result against
an independent QuantLib reference" case. The same list drives:

  * tests/functional/test_functional_parity.py — the pytest suite that runs
    inside the gate (suite 3 of tests/run_all_tests.sh),
  * tests/functional/generate_catalog.py — the generator that renders the
    browsable CATALOG.md and catalog.html.

Field reference (all keys required unless noted):

  id          Stable, unique, kebab/snake identifier. Used as the pytest id.
  product     Product key from src/common/product_catalog.h (picks the HTTP
              route, e.g. "vanilla_swap" -> POST /vanilla-swap).
  family      Catalog grouping, e.g. "IR Swaps". New families just work.
  title       One-line human name shown in the catalog.
  description What real-world instrument the case represents.
  request     Request JSON path relative to examples/data/.
  list_key    Response list holding the compared payload ("swaps", "bonds",
              ... for NPV cases; "results" for /bootstrap-curves series cases).
  ql_pricer   Name of the reference pricer function in
              tests/contract/ql_reference.py (e.g. "price_vanilla_swap_ql").
  tolerance   Max allowed abs(api - quantlib). Default 0.01 for NPV cases (one
              cent on notionals of millions — the server and the reference
              build the same QuantLib objects, so parity is near machine
              precision). Series cases use SERIES_TOLERANCE (see below).
              Omitted (and forbidden) for compare="exact" cases, which have
              no tolerance at all.
  exercises   Short tags describing what the case covers; rendered in the
              catalog's "What it exercises" column.
  compare     Optional. "npv" (default): the reference returns a single float
              compared against list_key[0].npv. "series": the reference
              returns the sampled curve values ({series_name: [floats]} keyed
              like the response's series[].measure, or a flat list for a
              single series) and every point is compared element-wise.
              "exact": the reference returns the expected response value
              verbatim — a list of ISO date strings (calendar business days /
              holidays, list_key "dates") or a single date string (calendar
              advance, list_key "advanced_date") — and the response must
              equal it exactly (order and length included).

To add a case: drop the request JSON under examples/data/ (IR swaps live in
examples/data/ir_swaps/, bonds in examples/data/bonds/, FRAs in
examples/data/fra/, caps/floors in examples/data/cap_floor/, swaptions in
examples/data/swaption/, CDS in examples/data/cds/, curve sampling in
examples/data/curves/, calendar utilities in examples/data/calendar/,
inflation swaps in examples/data/inflation/, equity options in
examples/data/equity/),
append a dict here, regenerate the catalog
(python3 tests/functional/generate_catalog.py inside the test image) and run
the gate. See tests/functional/README.md for the full walkthrough.
"""

DEFAULT_TOLERANCE = 0.01

# Tolerance for curve-series cases (zero rates, discount factors and forward
# rates are O(0.01-1.0), so the NPV default of 0.01 would be meaningless).
# Both sides construct identical QuantLib objects; the residual differences
# are bounded by the ~12 significant digits FlatBuffers prints response
# doubles with and by the two bootstrap accuracies (1e-15 server-side,
# QuantLib's 1e-12 default in the Python reference), i.e. ~1e-12. 1e-9 keeps
# three orders of margin over that noise floor while still being far below
# any economically meaningful gap (1e-9 in a rate is 1e-5 of a basis point).
SERIES_TOLERANCE = 1e-9

CASES = [
    # ------------------------------------------------------------------
    # IR Swaps — vanilla fixed vs IBOR
    # ------------------------------------------------------------------
    {
        "id": "irs_eur_5y_payer_fixed_30360_vs_euribor6m",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer swap, fixed 30/360 annual vs Euribor 6M",
        "description": (
            "Standard EUR interest rate swap: pay 3.20% fixed (annual, "
            "30/360) and receive Euribor 6M flat (semiannual, Act/360) on "
            "10m notional, TARGET calendar, Modified Following. Single "
            "deposit+swap curve used for discounting and projection."
        ),
        "request": "ir_swaps/irs_eur_5y_payer_fixed_30360_vs_euribor6m.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "fixed 30/360 annual", "Euribor6M",
                      "single curve", "TARGET"],
    },
    {
        "id": "irs_eur_5y_receiver_fixed_30360_vs_euribor6m",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y receiver swap, fixed 30/360 annual vs Euribor 6M",
        "description": (
            "The receiver side of the baseline trade: receive 3.40% fixed "
            "(annual, 30/360), pay Euribor 6M flat. Fixed rate is above the "
            "5Y par rate, so the NPV is positive for the receiver."
        ),
        "request": "ir_swaps/irs_eur_5y_receiver_fixed_30360_vs_euribor6m.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["receiver", "fixed 30/360 annual", "Euribor6M",
                      "single curve"],
    },
    {
        "id": "irs_eur_7y_payer_fixed_act365_semiannual",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 7Y payer swap, fixed Act/365F semiannual vs Euribor 6M",
        "description": (
            "7Y swap with a semiannual Act/365 Fixed coupon on the fixed leg "
            "(both legs pay on the same semiannual dates). Exercises a "
            "different fixed-leg day count and payment frequency than the "
            "market-standard annual 30/360."
        ),
        "request": "ir_swaps/irs_eur_7y_payer_fixed_act365_semiannual.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "fixed Act/365F semiannual", "Euribor6M",
                      "7Y tenor"],
    },
    {
        "id": "irs_eur_4y_payer_vs_euribor3m_quarterly",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 4Y payer swap vs Euribor 3M quarterly",
        "description": (
            "4Y swap whose floating leg fixes on Euribor 3M and pays "
            "quarterly. Exercises resolving a 3M IndexDef (instead of the "
            "6M default) and a quarterly floating schedule."
        ),
        "request": "ir_swaps/irs_eur_4y_payer_vs_euribor3m_quarterly.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "Euribor3M", "quarterly floating leg",
                      "index tenor variation"],
    },
    {
        "id": "irs_eur_5y_payer_spread25bp_following_backward",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer swap, +25bp floating spread, Following/Backward",
        "description": (
            "5Y swap paying 3.30% fixed against Euribor 6M + 25bp. Schedules "
            "use the Following business-day convention and Backward date "
            "generation, exercising non-default schedule conventions and a "
            "non-zero floating spread."
        ),
        "request": "ir_swaps/irs_eur_5y_payer_spread25bp_following_backward.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "floating spread +25bp", "Following convention",
                      "Backward date generation"],
    },
    {
        "id": "irs_eur_1y_payer_vs_euribor6m",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 1Y payer swap, money-market tenor",
        "description": (
            "Short-dated 1Y swap: pay 2.90% fixed (a single annual 30/360 "
            "coupon) against two semiannual Euribor 6M coupons. The curve's "
            "short end (deposits + 1Y par swap) dominates, exercising the "
            "shortest tenor a fixed-vs-IBOR swap is quoted at."
        ),
        "request": "ir_swaps/irs_eur_1y_payer_vs_euribor6m.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "1Y tenor", "single fixed coupon",
                      "Euribor6M", "curve short end"],
    },
    {
        "id": "irs_eur_30y_receiver_fixed_30360_vs_euribor6m",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 30Y receiver swap, long-dated",
        "description": (
            "Long-dated 30Y swap: receive 3.50% fixed (annual, 30/360), pay "
            "Euribor 6M flat, on a curve bootstrapped out to the 30Y par "
            "quote (2Y-30Y swaps). Exercises long-end bootstrapping and "
            "discounting over 60 semiannual floating periods."
        ),
        "request": "ir_swaps/irs_eur_30y_receiver_fixed_30360_vs_euribor6m.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["receiver", "30Y tenor", "long-end curve quotes",
                      "Euribor6M"],
    },
    {
        "id": "irs_eur_5y_payer_1y_forward_start",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer swap, 1Y forward starting",
        "description": (
            "Forward-starting trade: a 5Y payer swap whose schedules begin "
            "one year after the valuation date (effective 2026-01-19 vs "
            "as-of 2025-01-15), so every coupon, including the first "
            "floating fixing, is projected off the curve. The classic "
            "pre-hedge / delayed-start structure."
        ),
        "request": "ir_swaps/irs_eur_5y_payer_1y_forward_start.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "forward starting", "deferred effective date",
                      "Euribor6M", "all fixings projected"],
    },
    {
        "id": "irs_eur_3y_payer_vs_euribor1m_monthly",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 3Y payer swap vs Euribor 1M monthly",
        "description": (
            "3Y swap whose floating leg fixes on Euribor 1M and pays "
            "monthly (36 floating coupons), with the curve bootstrapped "
            "from 1M-indexed par swaps. Exercises the shortest IBOR index "
            "tenor and a monthly floating schedule."
        ),
        "request": "ir_swaps/irs_eur_3y_payer_vs_euribor1m_monthly.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "Euribor1M", "monthly floating leg",
                      "index tenor variation"],
    },
    {
        "id": "irs_eur_5y_payer_end_of_month_schedules",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer swap, end-of-month schedule rolls",
        "description": (
            "5Y swap effective on 2025-02-28 (the last day of February) "
            "with end_of_month=true on both legs, so coupon dates roll to "
            "month-ends (Aug 31, Feb 28/29) instead of keeping the 28th. "
            "Exercises the schedule end-of-month rule end to end."
        ),
        "request": "ir_swaps/irs_eur_5y_payer_end_of_month_schedules.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "end-of-month rule", "month-end effective date",
                      "Euribor6M"],
    },
    {
        "id": "irs_eur_2y_payer_imm_third_wednesday",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 2Y payer IMM swap (ThirdWednesday dates)",
        "description": (
            "IMM-dated 2Y swap running from the March 2025 IMM date "
            "(2025-03-19) to the March 2027 IMM date, with ThirdWednesday "
            "date generation on both legs so every coupon date is a third "
            "Wednesday. Floating leg fixes Euribor 3M quarterly, matching "
            "the futures-strip style schedule."
        ),
        "request": "ir_swaps/irs_eur_2y_payer_imm_third_wednesday.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "IMM dates", "ThirdWednesday date generation",
                      "Euribor3M", "quarterly floating leg"],
    },
    {
        "id": "irs_gbp_5y_payer_uk_calendar_act365_semiannual",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "GBP 5Y payer swap, UK calendar, Act/365F semiannual",
        "description": (
            "GBP-market-style 5Y swap: pay 4.35% fixed (semiannual, "
            "Act/365F) against a same-day-fixing 6M GBP IBOR-style index, "
            "both legs on the UnitedKingdom calendar. The curve is "
            "bootstrapped from UK-calendar deposits and semiannual Act/365F "
            "par swaps. Exercises a non-TARGET/non-US calendar, zero fixing "
            "days and GBP fixed-leg conventions."
        ),
        "request": "ir_swaps/irs_gbp_5y_payer_uk_calendar_act365_semiannual.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "GBP", "UnitedKingdom calendar",
                      "fixed Act/365F semiannual", "zero fixing days"],
    },
    {
        "id": "irs_eur_5y_payer_ois_discounted_multicurve",
        "product": "vanilla_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer swap, OIS-discounted (multicurve)",
        "description": (
            "Post-2008 market-standard setup: coupons are projected off a "
            "Euribor 6M curve (deposit + swap quotes) while cash flows are "
            "discounted on a separate ESTR OIS curve. Exercises "
            "discounting_curve != forwarding_curve on a vanilla swap."
        ),
        "request": "ir_swaps/irs_eur_5y_payer_ois_discounted_multicurve.json",
        "list_key": "swaps",
        "ql_pricer": "price_vanilla_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "multicurve", "OIS discounting",
                      "separate projection curve", "Euribor6M"],
    },

    # ------------------------------------------------------------------
    # IR Swaps — OIS (compounded overnight)
    # ------------------------------------------------------------------
    {
        "id": "ois_eur_5y_payer_estr",
        "product": "ois_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer OIS vs ESTR (compounded)",
        "description": (
            "5Y overnight indexed swap: pay 2.70% fixed (annual, Act/360), "
            "receive daily-compounded ESTR. The curve is bootstrapped from "
            "OIS par quotes, so the same instrument family prices the curve "
            "and the trade."
        ),
        "request": "ir_swaps/ois_eur_5y_payer_estr.json",
        "list_key": "swaps",
        "ql_pricer": "price_ois_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "OIS", "ESTR compounded",
                      "curve from OIS quotes"],
    },
    {
        "id": "ois_eur_2y_receiver_estr_spread10bp_quarterly",
        "product": "ois_swap",
        "family": "IR Swaps",
        "title": "EUR 2Y receiver OIS vs ESTR + 10bp, quarterly",
        "description": (
            "2Y OIS from the receiver side with quarterly payments on both "
            "legs and a +10bp spread over compounded ESTR on the overnight "
            "leg. Exercises payment frequency and overnight-leg spread."
        ),
        "request": "ir_swaps/ois_eur_2y_receiver_estr_spread10bp_quarterly.json",
        "list_key": "swaps",
        "ql_pricer": "price_ois_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["receiver", "OIS", "ESTR compounded",
                      "overnight spread +10bp", "quarterly payments"],
    },
    {
        "id": "ois_eur_5y_payer_estr_payment_lag2",
        "product": "ois_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y payer OIS vs ESTR, 2-day payment lag",
        "description": (
            "5Y ESTR OIS paying 2.68% fixed with a 2-business-day payment "
            "lag on the TARGET payment calendar, the standard settlement "
            "delay for cleared OIS. Coupon payment dates land two business "
            "days after each accrual period ends on both legs."
        ),
        "request": "ir_swaps/ois_eur_5y_payer_estr_payment_lag2.json",
        "list_key": "swaps",
        "ql_pricer": "price_ois_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "OIS", "ESTR compounded",
                      "payment lag 2 days"],
    },
    {
        "id": "ois_usd_3y_payer_sofr",
        "product": "ois_swap",
        "family": "IR Swaps",
        "title": "USD 3Y payer OIS vs SOFR (compounded)",
        "description": (
            "3Y USD SOFR OIS: pay 4.05% fixed (annual, Act/360), receive "
            "daily-compounded SOFR, on the US government bond (SIFMA) "
            "calendar. Exercises a non-TARGET calendar and a USD overnight "
            "index."
        ),
        "request": "ir_swaps/ois_usd_3y_payer_sofr.json",
        "list_key": "swaps",
        "ql_pricer": "price_ois_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["payer", "OIS", "SOFR compounded", "USD",
                      "US government bond calendar"],
    },

    # ------------------------------------------------------------------
    # IR Swaps — tenor basis (float vs float)
    # ------------------------------------------------------------------
    {
        "id": "basis_eur_5y_euribor3m_plus5bp_vs_euribor6m",
        "product": "basis_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y tenor basis: Euribor 3M + 5bp vs Euribor 6M",
        "description": (
            "5Y tenor basis swap paying Euribor 3M + 5bp quarterly and "
            "receiving Euribor 6M flat semiannually, both projected and "
            "discounted on one curve. Exercises the float-vs-float product "
            "with a basis spread."
        ),
        "request": "ir_swaps/basis_eur_5y_euribor3m_plus5bp_vs_euribor6m.json",
        "list_key": "swaps",
        "ql_pricer": "price_basis_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["tenor basis", "Euribor3M vs Euribor6M",
                      "basis spread +5bp", "single curve"],
    },
    {
        "id": "basis_eur_5y_3m_vs_6m_ois_discounted_multicurve",
        "product": "basis_swap",
        "family": "IR Swaps",
        "title": "EUR 5Y tenor basis, per-leg curves, OIS-discounted",
        "description": (
            "Fully multicurve tenor basis: the 3M leg projects off a 3M "
            "curve, the 6M leg off a 6M curve, and both legs discount on an "
            "ESTR OIS curve (three curves in one request). Exercises "
            "per-leg forwarding curves plus OIS discounting."
        ),
        "request": "ir_swaps/basis_eur_5y_3m_vs_6m_ois_discounted_multicurve.json",
        "list_key": "swaps",
        "ql_pricer": "price_basis_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["tenor basis", "multicurve", "per-leg projection curves",
                      "OIS discounting", "three curves"],
    },

    # ------------------------------------------------------------------
    # Bonds — fixed-rate
    # ------------------------------------------------------------------
    {
        "id": "frb_eur_5y_at_par_annual_30360",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 5Y fixed-rate bond at the curve (annual 30/360)",
        "description": (
            "New-issue 5Y EUR benchmark-style bond: 3.10% annual coupon "
            "(30/360) on 1m face, TARGET calendar, Modified Following. The "
            "coupon sits at the curve's 5Y par swap quote, so the bond "
            "prices close to par. Discounted on a deposit+swap curve."
        ),
        "request": "bonds/frb_eur_5y_at_par_annual_30360.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["fixed coupon at the curve", "annual 30/360",
                      "TARGET", "deposit+swap curve"],
    },
    {
        "id": "frb_eur_7y_premium_semiannual_actactbond",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 7Y premium bond, 5.25% semiannual Act/Act",
        "description": (
            "Seasoned-style high-coupon issue: 5.25% paid semiannually "
            "(ActualActual Bond basis) against a ~3.1% curve, so the bond "
            "trades well above par. Exercises a semiannual fixed schedule "
            "and the Act/Act bond-basis accrual day count."
        ),
        "request": "bonds/frb_eur_7y_premium_semiannual_actactbond.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["premium bond", "semiannual coupon",
                      "ActualActualBond accrual", "7Y maturity"],
    },
    {
        "id": "frb_eur_5y_discount_quarterly_act365f",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 5Y discount bond, 1.25% quarterly Act/365F",
        "description": (
            "Low-coupon bond issued in a lower-rate era: 1.25% paid "
            "quarterly (Act/365F) against a ~3.1% curve, so it prices at a "
            "clear discount to par. Exercises a quarterly fixed schedule "
            "and Act/365F accrual."
        ),
        "request": "bonds/frb_eur_5y_discount_quarterly_act365f.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["discount bond", "quarterly coupon",
                      "Act/365F accrual"],
    },
    {
        "id": "frb_eur_10y_zero_coupon",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 10Y zero-coupon bond (0% rate, bullet redemption)",
        "description": (
            "Pure discount instrument: a 10Y bond with a 0% coupon rate, so "
            "the only cash flow is the 100 redemption at maturity and the "
            "NPV is the 10Y discount factor times face. Exercises the "
            "rate=0 edge and redemption-only pricing."
        ),
        "request": "bonds/frb_eur_10y_zero_coupon.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["zero coupon (rate 0)", "redemption-only cash flow",
                      "10Y maturity"],
    },
    {
        "id": "frb_eur_2y_settlement_t3_annual_30360",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 2Y short bond, T+3 settlement",
        "description": (
            "Short-dated 2Y note paying 3.05% annually (30/360) with "
            "3-business-day settlement instead of the usual T+2, so the "
            "bond's own settlement date drives the NPV anchor. Exercises "
            "the settlement-days field and the curve's short end."
        ),
        "request": "bonds/frb_eur_2y_settlement_t3_annual_30360.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["2Y maturity", "settlement days T+3",
                      "annual 30/360", "curve short end"],
    },
    {
        "id": "frb_eur_30y_long_dated_annual_30360",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 30Y long-dated bond, 3.50% annual",
        "description": (
            "Long-end issue: 3.50% annual coupon (30/360) maturing in 30 "
            "years, discounted on a curve bootstrapped out to the 30Y par "
            "swap quote (15Y/20Y/30Y pillars). Exercises long-end "
            "bootstrapping and discounting of 30 coupon dates."
        ),
        "request": "bonds/frb_eur_30y_long_dated_annual_30360.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["30Y maturity", "long-end curve quotes",
                      "annual 30/360"],
    },
    {
        "id": "frb_usd_10y_treasury_semiannual_zero_curve",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "USD 10Y Treasury-style bond on a zero curve",
        "description": (
            "US Treasury-style note: 4.25% semiannual coupon (Act/Act Bond "
            "basis) issued 2024-02-15 and maturing 2034-02-15, so it is "
            "seasoned at the 2025-01-15 valuation date (one coupon already "
            "paid). US government bond calendar, Unadjusted backward "
            "schedule, T+1 settlement, discounted on an explicit "
            "zero-rate curve. Exercises a non-TARGET calendar, a seasoned "
            "schedule with past coupons and the zero-curve path."
        ),
        "request": "bonds/frb_usd_10y_treasury_semiannual_zero_curve.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["USD", "US government bond calendar", "seasoned bond",
                      "zero-rate curve", "Backward Unadjusted schedule",
                      "T+1 settlement"],
    },
    {
        "id": "frb_eur_6y_redemption_101_5_preceding_payments",
        "product": "fixed_rate_bond",
        "family": "Bonds",
        "title": "EUR 6Y bond, 101.5 redemption, Preceding payments",
        "description": (
            "6Y 3.30% annual bond redeemed at 101.5% of face (make-whole "
            "style premium redemption) whose coupon dates stay unadjusted "
            "on the 17th but whose payments roll backward (Preceding) when "
            "that is a weekend — Jan 17 falls on a Saturday in 2026 and a "
            "Sunday in 2027. Exercises a non-par redemption and a "
            "non-default payment convention."
        ),
        "request": "bonds/frb_eur_6y_redemption_101_5_preceding_payments.json",
        "list_key": "bonds",
        "ql_pricer": "price_fixed_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["non-par redemption 101.5", "Preceding payment convention",
                      "Unadjusted schedule"],
    },

    # ------------------------------------------------------------------
    # Bonds — floating-rate
    # ------------------------------------------------------------------
    {
        "id": "frn_eur_5y_euribor6m_flat_semiannual",
        "product": "floating_rate_bond",
        "family": "Bonds",
        "title": "EUR 5Y FRN on Euribor 6M flat",
        "description": (
            "Plain-vanilla floater: semiannual coupons fixing on Euribor 6M "
            "flat (Act/360) on 1m face, projected and discounted on the "
            "same deposit+swap curve, so the bond prices close to par. The "
            "baseline floating-rate case."
        ),
        "request": "bonds/frn_eur_5y_euribor6m_flat_semiannual.json",
        "list_key": "bonds",
        "ql_pricer": "price_floating_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["FRN", "Euribor6M", "zero spread", "single curve"],
    },
    {
        "id": "frn_eur_4y_euribor3m_quarterly_spread40bp",
        "product": "floating_rate_bond",
        "family": "Bonds",
        "title": "EUR 4Y FRN, Euribor 3M + 40bp quarterly",
        "description": (
            "Credit-spread floater: quarterly coupons paying Euribor 3M "
            "plus a 40bp margin, with the curve bootstrapped from "
            "3M-indexed par swaps. Exercises the 3M index tenor, a "
            "quarterly schedule and a non-zero coupon spread."
        ),
        "request": "bonds/frn_eur_4y_euribor3m_quarterly_spread40bp.json",
        "list_key": "bonds",
        "ql_pricer": "price_floating_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["FRN", "Euribor3M", "spread +40bp",
                      "quarterly coupons"],
    },
    {
        "id": "frn_eur_5y_euribor6m_ois_discounted_multicurve",
        "product": "floating_rate_bond",
        "family": "Bonds",
        "title": "EUR 5Y FRN, OIS-discounted (multicurve)",
        "description": (
            "Post-2008 multicurve setup on a floater: coupons project off a "
            "Euribor 6M deposit+swap curve while cash flows discount on a "
            "separate ESTR OIS curve, so the bond prices above par (OIS "
            "discounting below the projected coupons). Exercises "
            "forwarding_curve != discounting_curve on a bond."
        ),
        "request": "bonds/frn_eur_5y_euribor6m_ois_discounted_multicurve.json",
        "list_key": "bonds",
        "ql_pricer": "price_floating_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["FRN", "multicurve", "OIS discounting",
                      "separate projection curve", "Euribor6M"],
    },
    {
        "id": "frn_eur_3y_euribor6m_act365f_accrual",
        "product": "floating_rate_bond",
        "family": "Bonds",
        "title": "EUR 3Y FRN, Act/365F accrual vs Act/360 index",
        "description": (
            "3Y floater whose coupons accrue on Act/365F while the "
            "underlying Euribor 6M index stays on its market-standard "
            "Act/360, so the coupon amounts pick up the day-count "
            "mismatch. Exercises the bond-level accrual day count "
            "independently of the index convention."
        ),
        "request": "bonds/frn_eur_3y_euribor6m_act365f_accrual.json",
        "list_key": "bonds",
        "ql_pricer": "price_floating_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["FRN", "Act/365F accrual", "index day count mismatch",
                      "Euribor6M"],
    },
    {
        "id": "frn_eur_2y_euribor6m_same_day_fixing",
        "product": "floating_rate_bond",
        "family": "Bonds",
        "title": "EUR 2Y FRN, same-day fixing (0 fixing days)",
        "description": (
            "2Y floater whose bond-level fixing_days is 0, overriding the "
            "index's 2-day fixing lag so every coupon fixes on its accrual "
            "start date. Exercises the bond-level fixing-days override on "
            "the floating coupons."
        ),
        "request": "bonds/frn_eur_2y_euribor6m_same_day_fixing.json",
        "list_key": "bonds",
        "ql_pricer": "price_floating_rate_bond_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["FRN", "zero fixing days", "same-day fixing",
                      "Euribor6M"],
    },
    # ------------------------------------------------------------------
    # FRA — forward rate agreements
    # ------------------------------------------------------------------
    {
        "id": "fra_eur_3x6_long_strike_below_forward",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 3x6 long FRA, strike below the forward",
        "description": (
            "Baseline FRA: buy (go long) the 3x6 period on Euribor 3M at a "
            "2.75% strike against a ~3.13% implied forward, on 1m notional. "
            "Accrues 2025-04-17 to 2025-07-17 (spot+3M, TARGET-adjusted), "
            "so the long side locks a below-market rate and the NPV is "
            "positive. Deposit-bootstrapped upward-sloping curve used for "
            "both projection and discounting."
        ),
        "request": "fra/fra_eur_3x6_long_strike_below_forward.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "3x6", "Euribor3M", "strike below forward",
                      "positive NPV", "single curve"],
    },
    {
        "id": "fra_eur_3x6_short_strike_below_forward",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 3x6 short FRA, same trade sold",
        "description": (
            "The sold side of the baseline trade: short the same 3x6 "
            "Euribor 3M FRA at 2.75%. Everything else is identical, so the "
            "NPV is the exact negative of the long case, pinning the "
            "position-type sign convention end to end."
        ),
        "request": "fra/fra_eur_3x6_short_strike_below_forward.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["short", "3x6", "Euribor3M", "sign flip",
                      "negative NPV"],
    },
    {
        "id": "fra_eur_1x4_long_euribor3m",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 1x4 long FRA, front of the curve",
        "description": (
            "Short-dated 1x4 FRA on Euribor 3M struck at 3.00%, accruing "
            "2025-02-17 to 2025-05-19 (the 3M anniversary lands on a "
            "Saturday and rolls Modified Following to Monday). The nearest "
            "quoted FRA period, dominated by the curve's 1M-3M deposits."
        ),
        "request": "fra/fra_eur_1x4_long_euribor3m.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "1x4", "Euribor3M", "curve short end",
                      "weekend-rolled maturity"],
    },
    {
        "id": "fra_eur_6x9_long_strike_above_forward",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 6x9 long FRA, strike above the forward",
        "description": (
            "Mid-curve 6x9 FRA on Euribor 3M struck at 3.60%, well above "
            "the ~3.24% implied forward, so the long side is locking an "
            "above-market rate and the NPV is clearly negative. Accrues "
            "2025-07-17 to 2025-10-17."
        ),
        "request": "fra/fra_eur_6x9_long_strike_above_forward.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "6x9", "Euribor3M", "strike above forward",
                      "negative NPV"],
    },
    {
        "id": "fra_eur_9x12_long_euribor3m",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 9x12 long FRA, furthest standard period",
        "description": (
            "9x12 FRA on Euribor 3M struck at 3.30%, accruing 2025-10-17 to "
            "2026-01-19 — the furthest of the standard quarterly FRA strip, "
            "priced off the 9M-1Y segment of the deposit curve."
        ),
        "request": "fra/fra_eur_9x12_long_euribor3m.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "9x12", "Euribor3M", "curve 9M-1Y segment"],
    },
    {
        "id": "fra_eur_6x12_long_euribor6m",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 6x12 long FRA on Euribor 6M",
        "description": (
            "6x12 FRA whose underlying is Euribor 6M instead of the 3M "
            "index: a single 6-month accrual period from 2025-07-17 to "
            "2026-01-19, struck at 3.20%. Exercises resolving a 6M IndexDef "
            "and the longer accrual fraction in the FRA payoff."
        ),
        "request": "fra/fra_eur_6x12_long_euribor6m.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "6x12", "Euribor6M", "index tenor variation",
                      "6M accrual period"],
    },
    {
        "id": "fra_eur_3x6_long_at_forward",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 3x6 long FRA struck at the forward (NPV ~ 0)",
        "description": (
            "The baseline 3x6 trade struck at the curve's own implied "
            "forward (3.1319%), so the FRA is at market and the NPV is "
            "within a few cents of zero — the fair-value edge case where "
            "any sign or day-count slip on either side shows up immediately."
        ),
        "request": "fra/fra_eur_3x6_long_at_forward.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "3x6", "at-the-money strike", "NPV near zero",
                      "Euribor3M"],
    },
    {
        "id": "fra_gbp_3x6_long_act365f_same_day_fixing",
        "product": "fra",
        "family": "FRA",
        "title": "GBP 3x6 long FRA, Act/365F, same-day fixing",
        "description": (
            "GBP-market-style 3x6 FRA: the index fixes same-day "
            "(0 fixing days) on the UnitedKingdom calendar and accrues "
            "Act/365F instead of the euro-market Act/360, struck at 4.25% "
            "against a mildly inverted GBP deposit curve. Accrues "
            "2025-04-15 to 2025-07-15."
        ),
        "request": "fra/fra_gbp_3x6_long_act365f_same_day_fixing.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "3x6", "GBP", "UnitedKingdom calendar",
                      "Act/365F index day count", "zero fixing days",
                      "inverted curve"],
    },
    {
        "id": "fra_eur_3x6_long_ois_discounted",
        "product": "fra",
        "family": "FRA",
        "title": "EUR 3x6 long FRA, OIS-discounted (multicurve)",
        "description": (
            "The baseline 3x6 Euribor 3M FRA priced multicurve: the forward "
            "projects off the Euribor deposit curve while the payoff "
            "discounts on a separate, lower ESTR-style curve. Exercises "
            "discounting_curve != forwarding_curve on the FRA endpoint."
        ),
        "request": "fra/fra_eur_3x6_long_ois_discounted.json",
        "list_key": "fras",
        "ql_pricer": "price_fra_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["long", "3x6", "multicurve", "OIS discounting",
                      "separate projection curve", "Euribor3M"],
    },

    # ------------------------------------------------------------------
    # Cap/Floor — caps and floors on IBOR legs, constant optionlet vols
    # ------------------------------------------------------------------
    {
        "id": "cap_eur_5y_atm_quarterly_euribor3m",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y cap on Euribor 3M, struck near the money",
        "description": (
            "Baseline interest rate cap: quarterly caplets on Euribor 3M "
            "struck at 3.10%, right at the curve's 5Y par level, on 1m "
            "notional (TARGET, Act/360, Modified Following). Priced with "
            "the Black engine on a flat 20% lognormal optionlet vol, "
            "projected and discounted on one deposit+swap curve."
        ),
        "request": "cap_floor/cap_eur_5y_atm_quarterly_euribor3m.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "near-the-money strike", "Euribor3M",
                      "quarterly caplets", "Black lognormal 20%",
                      "single curve"],
    },
    {
        "id": "cap_eur_5y_itm_strike_2pct",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y cap struck at 2%, deep in the money",
        "description": (
            "The baseline 5Y quarterly Euribor 3M cap struck at 2.00%, "
            "well below the ~3.1% forwards, so nearly every caplet is in "
            "the money and the premium is dominated by intrinsic value. "
            "Pins the high-NPV end of the strike spectrum."
        ),
        "request": "cap_floor/cap_eur_5y_itm_strike_2pct.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "in-the-money strike", "intrinsic-dominated",
                      "Euribor3M", "Black lognormal 20%"],
    },
    {
        "id": "cap_eur_5y_otm_strike_5pct",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y cap struck at 5%, out of the money",
        "description": (
            "The baseline 5Y quarterly Euribor 3M cap struck at 5.00%, "
            "far above the ~3.1% forwards, so the premium is pure time "
            "value and small relative to notional. Pins the low-NPV tail "
            "where a vol or day-count slip is proportionally largest."
        ),
        "request": "cap_floor/cap_eur_5y_otm_strike_5pct.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "out-of-the-money strike", "time-value only",
                      "Euribor3M", "Black lognormal 20%"],
    },
    {
        "id": "floor_eur_5y_itm_strike_4pct",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y floor struck at 4%, in the money",
        "description": (
            "5Y quarterly floor on Euribor 3M struck at 4.00%, well above "
            "the ~3.1% forwards, so the floorlets pay in most scenarios — "
            "the classic minimum-yield guarantee a floating-rate investor "
            "buys. Exercises the Floor instrument type on the same market "
            "data as the cap cases."
        ),
        "request": "cap_floor/floor_eur_5y_itm_strike_4pct.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["floor", "in-the-money strike", "Euribor3M",
                      "quarterly floorlets", "Black lognormal 20%"],
    },
    {
        "id": "floor_eur_5y_otm_strike_2pct",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y floor struck at 2%, out of the money",
        "description": (
            "5Y quarterly floor on Euribor 3M struck at 2.00%, well below "
            "the forwards, so it is cheap disaster protection against "
            "rates collapsing. Together with the 4% floor it brackets the "
            "floor moneyness spectrum."
        ),
        "request": "cap_floor/floor_eur_5y_otm_strike_2pct.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["floor", "out-of-the-money strike", "Euribor3M",
                      "Black lognormal 20%"],
    },
    {
        "id": "cap_eur_5y_bachelier_normal_vol_80bp",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y cap on normal (Bachelier) vols, 80bp",
        "description": (
            "The baseline 5Y quarterly Euribor 3M cap quoted the "
            "post-2015 market way: a flat 80bp normal (Bachelier) "
            "optionlet vol priced with the Bachelier engine instead of "
            "Black. Exercises the Normal volatility type and the "
            "model/vol-type pairing end to end."
        ),
        "request": "cap_floor/cap_eur_5y_bachelier_normal_vol_80bp.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "Bachelier engine", "normal vol 80bp",
                      "Euribor3M", "vol-type variation"],
    },
    {
        "id": "cap_eur_5y_shifted_black_displacement_2pct",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y cap on shifted-lognormal vols (2% shift)",
        "description": (
            "The baseline 5Y quarterly Euribor 3M cap priced with the "
            "shifted Black model: a flat 15% shifted-lognormal optionlet "
            "vol with a 2% displacement, the standard quoting convention "
            "from the negative-rate era. Exercises the ShiftedLognormal "
            "vol type and a non-zero displacement flowing into the Black "
            "engine."
        ),
        "request": "cap_floor/cap_eur_5y_shifted_black_displacement_2pct.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "shifted Black", "displacement 2%",
                      "shifted-lognormal vol 15%", "Euribor3M"],
    },
    {
        "id": "cap_eur_10y_euribor6m_semiannual",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 10Y cap on Euribor 6M, semiannual",
        "description": (
            "Long-dated 10Y cap with semiannual caplets on Euribor 6M "
            "struck at 3.50%, priced out to the curve's 10Y par quote. "
            "Exercises the 6M index tenor, a semiannual caplet schedule "
            "and 20 caplets across the full curve."
        ),
        "request": "cap_floor/cap_eur_10y_euribor6m_semiannual.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "10Y maturity", "Euribor6M",
                      "semiannual caplets", "index tenor variation"],
    },
    {
        "id": "floor_eur_2y_act365f_vol_30pct",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 2Y floor, Act/365F accrual, 30% vol",
        "description": (
            "Short-dated 2Y quarterly floor on Euribor 3M struck at "
            "3.00%, accruing Act/365F at the trade level while the index "
            "stays on its market-standard Act/360, priced on a higher "
            "30% lognormal vol. Exercises the trade-level payment day "
            "count independently of the index convention, a second vol "
            "level and the curve's short end."
        ),
        "request": "cap_floor/floor_eur_2y_act365f_vol_30pct.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["floor", "2Y maturity", "Act/365F accrual",
                      "index day count mismatch", "lognormal vol 30%",
                      "Euribor3M"],
    },
    {
        "id": "cap_eur_5y_euribor6m_ois_discounted_multicurve",
        "product": "cap_floor",
        "family": "Cap/Floor",
        "title": "EUR 5Y cap, OIS-discounted (multicurve)",
        "description": (
            "Post-2008 multicurve setup on a cap: semiannual Euribor 6M "
            "caplets struck at 3.20% project off a deposit+swap Euribor "
            "curve while the premium discounts on a separate, lower ESTR "
            "OIS curve. Exercises discounting_curve != forwarding_curve "
            "on the cap/floor endpoint."
        ),
        "request": "cap_floor/cap_eur_5y_euribor6m_ois_discounted_multicurve.json",
        "list_key": "cap_floors",
        "ql_pricer": "price_cap_floor_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["cap", "multicurve", "OIS discounting",
                      "separate projection curve", "Euribor6M",
                      "semiannual caplets"],
    },

    # ------------------------------------------------------------------
    # Swaption — options on vanilla and OIS swaps
    # ------------------------------------------------------------------
    {
        "id": "swpt_eur_1y5y_payer_near_atm_physical",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption, struck near the money, physical",
        "description": (
            "Baseline European swaption: the right to pay 3.10% fixed "
            "(annual, 30/360) vs Euribor 6M on 1m notional for 5 years, "
            "exercisable in one year, physically settled. The strike sits "
            "just below the ~3.16% forward par rate, so the option is close "
            "to at the money. Priced with the Black engine on a flat 20% "
            "lognormal vol, one deposit+swap curve for projection and "
            "discounting."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_near_atm_physical.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "1Y into 5Y",
                      "near-the-money strike", "physical settlement",
                      "Black lognormal 20%", "single curve"],
    },
    {
        "id": "swpt_eur_1y5y_receiver_near_atm",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y receiver swaption, same strike",
        "description": (
            "The receiver twin of the baseline trade: the right to receive "
            "3.10% fixed for 5 years, exercisable in one year. With the "
            "strike below the ~3.16% forward the receiver is slightly out "
            "of the money, so its premium sits below the payer's — pinning "
            "the payer/receiver option-type flag end to end."
        ),
        "request": "swaption/swpt_eur_1y5y_receiver_near_atm.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "receiver", "1Y into 5Y",
                      "option-type flip", "Black lognormal 20%"],
    },
    {
        "id": "swpt_eur_1y5y_payer_itm_strike_2pct",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption struck at 2%, deep in the money",
        "description": (
            "The baseline 1Y5Y payer struck at 2.00%, more than 100bp below "
            "the ~3.16% forward, so exercise is close to certain and the "
            "premium is dominated by the intrinsic value of paying "
            "below-market fixed. Pins the high-premium end of the strike "
            "spectrum."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_itm_strike_2pct.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "in-the-money strike",
                      "intrinsic-dominated", "Black lognormal 20%"],
    },
    {
        "id": "swpt_eur_1y5y_payer_otm_strike_4_5pct",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption struck at 4.5%, out of the money",
        "description": (
            "The baseline 1Y5Y payer struck at 4.50%, well above the ~3.16% "
            "forward, so the premium is pure time value and small relative "
            "to notional — the tail where a vol or day-count slip is "
            "proportionally largest."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_otm_strike_4_5pct.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "out-of-the-money strike",
                      "time-value only", "Black lognormal 20%"],
    },
    {
        "id": "swpt_eur_5y5y_payer",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 5Y5Y payer swaption, long expiry",
        "description": (
            "The classic 5Y-into-5Y structure: a payer swaption struck at "
            "3.30% (about 12bp below the ~3.42% forward) exercisable in "
            "five years into a 5Y swap. Exercises a long option expiry and "
            "the curve segment out to 10 years."
        ),
        "request": "swaption/swpt_eur_5y5y_payer.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "5Y into 5Y", "long expiry",
                      "Black lognormal 20%"],
    },
    {
        "id": "swpt_eur_2y10y_payer",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 2Y10Y payer swaption, long underlying",
        "description": (
            "A 2Y option into a 10Y swap struck at 3.30%, right at the "
            "~3.35% forward, on a curve bootstrapped out to the 15Y par "
            "quote. Exercises a long underlying tenor (20 semiannual "
            "floating coupons after exercise) and the 12-year curve "
            "horizon."
        ),
        "request": "swaption/swpt_eur_2y10y_payer.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "2Y into 10Y",
                      "long underlying tenor", "Black lognormal 20%"],
    },
    {
        "id": "swpt_eur_1y5y_payer_cash_par_yield",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption, cash-settled (par-yield annuity)",
        "description": (
            "The baseline 1Y5Y payer settled in cash under the classic "
            "pre-2019 EUR convention: the payoff is the swap rate "
            "difference times a par-yield cash annuity instead of the "
            "curve-discounted swap annuity, so the premium differs "
            "measurably from the physical case. Exercises "
            "settlement_type=Cash with settlement_method=ParYieldCurve."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_cash_par_yield.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "cash settlement",
                      "ParYieldCurve annuity", "Black lognormal 20%"],
    },
    {
        "id": "swpt_eur_1y5y_payer_bachelier_normal_80bp",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption on normal (Bachelier) vols, 80bp",
        "description": (
            "The baseline 1Y5Y payer quoted the post-2015 market way: a "
            "flat 80bp normal (Bachelier) vol priced with the Bachelier "
            "engine instead of Black. Exercises the Normal volatility type "
            "and the model/vol-type pairing on the swaption endpoint."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_bachelier_normal_80bp.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "Bachelier engine",
                      "normal vol 80bp", "vol-type variation"],
    },
    {
        "id": "swpt_eur_1y5y_payer_shifted_black_2pct",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption on shifted-lognormal vols (2% shift)",
        "description": (
            "The baseline 1Y5Y payer priced with the shifted Black model: "
            "a flat 15% shifted-lognormal vol with a 2% displacement, the "
            "standard quoting convention from the negative-rate era. "
            "Exercises the ShiftedLognormal vol type and a non-zero "
            "displacement flowing through the swaption Black engine."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_shifted_black_2pct.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "shifted Black",
                      "displacement 2%", "shifted-lognormal vol 15%"],
    },
    {
        "id": "swpt_eur_1y5y_payer_atm_matrix_vol",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption on an ATM vol matrix",
        "description": (
            "The baseline 1Y5Y payer priced off a 3x3 ATM lognormal vol "
            "matrix (expiries 1Y/2Y/5Y x tenors 2Y/7Y/10Y, 18-22%) instead "
            "of a single constant vol. The trade's ~5Y swap length falls "
            "between the 2Y and 7Y tenor pillars, so the priced vol is a "
            "genuine surface interpolation, not a grid-node read. "
            "Exercises the matrix vol input end to end."
        ),
        "request": "swaption/swpt_eur_1y5y_payer_atm_matrix_vol.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "ATM vol matrix 3x3",
                      "surface interpolation", "Black engine"],
    },
    {
        "id": "swpt_eur_ois_1y5y_payer_estr",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR 1Y5Y payer swaption on an ESTR OIS underlying",
        "description": (
            "A European payer swaption whose underlying is a 5Y ESTR OIS "
            "(annual fixed Act/360 at 2.65%, close to the ~2.70% forward, "
            "vs daily-compounded ESTR), on a curve bootstrapped from OIS "
            "par quotes. Cash-settled par-yield, priced with the Bachelier "
            "engine on a 70bp normal vol — the post-LIBOR market-standard "
            "combination. Exercises the OIS underlying type."
        ),
        "request": "swaption/swpt_eur_ois_1y5y_payer_estr.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "payer", "OIS underlying",
                      "ESTR compounded", "Bachelier engine",
                      "normal vol 70bp", "cash settlement"],
    },
    {
        "id": "swpt_eur_bermudan_3ex_into_5y_hw_explicit",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR Bermudan payer swaption, 3 annual exercises, Hull-White",
        "description": (
            "Bermudan payer swaption on the 5Y underlying (3.10% fixed vs "
            "Euribor 6M) exercisable on three annual dates (2026/2027/"
            "2028), priced on a Hull-White one-factor lattice with "
            "explicit parameters (a=0.03, sigma=0.01, 100 tree steps) on "
            "both sides. Exercises the Bermudan exercise schedule and the "
            "tree engine."
        ),
        "request": "swaption/swpt_eur_bermudan_3ex_into_5y_hw_explicit.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["Bermudan", "payer", "3 exercise dates",
                      "Hull-White explicit", "tree engine 100 steps"],
    },
    {
        "id": "swpt_eur_american_2y_window_hw_explicit",
        "product": "swaption",
        "family": "Swaption",
        "title": "EUR American payer swaption, 2Y exercise window, Hull-White",
        "description": (
            "American payer swaption exercisable continuously from the "
            "valuation date until January 2027 into a 5Y swap paying 3.10% "
            "fixed, priced on the same explicit-parameter Hull-White "
            "lattice (a=0.03, sigma=0.01, 100 steps). Exercises the "
            "American exercise window, which both sides anchor at the "
            "evaluation date."
        ),
        "request": "swaption/swpt_eur_american_2y_window_hw_explicit.json",
        "list_key": "swaptions",
        "ql_pricer": "price_swaption_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["American", "payer", "2Y exercise window",
                      "Hull-White explicit", "tree engine 100 steps"],
    },

    # ------------------------------------------------------------------
    # CDS — credit default swaps (LogLinear credit-curve bootstrap only;
    # the server rejects every other credit-curve interpolator)
    # ------------------------------------------------------------------
    {
        "id": "cds_eur_5y_buyer_100bp_spread_curve",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS, buy protection at 100bp, par-spread curve",
        "description": (
            "Baseline single-name CDS: buy protection on 10m notional "
            "paying a 100bp running coupon (quarterly, Act/360, "
            "TwentiethIMM grid, TARGET). The credit curve is bootstrapped "
            "from par spread quotes (1Y 80bp to 10Y 130bp, 40% recovery, "
            "LogLinear survival) on a deposit+swap discount curve; the 5Y "
            "par spread of ~110bp sits above the coupon, so the protection "
            "buyer's NPV is positive. MidPoint engine."
        ),
        "request": "cds/cds_eur_5y_buyer_100bp_spread_curve.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "100bp running coupon",
                      "par-spread bootstrap", "recovery 40%",
                      "MidPoint engine", "IMM grid"],
    },
    {
        "id": "cds_eur_5y_seller_100bp_spread_curve",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS, same trade selling protection",
        "description": (
            "The sold side of the baseline trade: sell protection on the "
            "same 5Y 100bp contract against the same par-spread credit "
            "curve. Everything else is identical, so the NPV is the exact "
            "negative of the buy case, pinning the protection-side sign "
            "convention end to end."
        ),
        "request": "cds/cds_eur_5y_seller_100bp_spread_curve.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["sell protection", "sign flip",
                      "100bp running coupon", "par-spread bootstrap"],
    },
    {
        "id": "cds_eur_5y_buyer_500bp_running_coupon",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS at the 500bp standard coupon",
        "description": (
            "The baseline trade paying the high standard coupon: 500bp "
            "running against a curve whose 5Y par spread is ~110bp, so the "
            "premium leg dwarfs the protection leg and the buyer's NPV is "
            "deeply negative (about -1.8m on 10m). In the market this "
            "contract would trade with a large upfront paid to the buyer; "
            "here the coupon is deliberately left unbalanced to pin the "
            "premium-leg scaling."
        ),
        "request": "cds/cds_eur_5y_buyer_500bp_running_coupon.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "500bp standard coupon",
                      "premium-leg dominated", "negative NPV"],
    },
    {
        "id": "cds_eur_5y_buyer_100bp_upfront_2pct",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS with a 2% upfront payment",
        "description": (
            "The baseline 100bp trade with a trade-level upfront: the "
            "protection buyer also pays 2% of notional (200k) settling on "
            "2025-01-20. Exercises the upfront-bearing instrument "
            "constructor and the upfront date on the wire — the NPV drops "
            "by roughly the upfront amount relative to the baseline case."
        ),
        "request": "cds/cds_eur_5y_buyer_100bp_upfront_2pct.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "upfront 2%", "upfront date",
                      "100bp running coupon"],
    },
    {
        "id": "cds_eur_5y_buyer_flat_hazard_2pct",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS on a flat 2% hazard rate",
        "description": (
            "The baseline trade priced on a flat-hazard-rate credit curve "
            "instead of a bootstrapped one: a constant 2% default "
            "intensity (Act/365F) with 40% recovery implies an expected "
            "loss rate of ~120bp/yr against the 100bp coupon, so the "
            "buyer's NPV is positive. Exercises the flat-hazard branch of "
            "the credit-curve build (no quotes on the wire)."
        ),
        "request": "cds/cds_eur_5y_buyer_flat_hazard_2pct.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "flat hazard rate 2%",
                      "no credit quotes", "recovery 40%"],
    },
    {
        "id": "cds_eur_5y_buyer_recovery_20pct",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS with 20% recovery (distressed-debt assumption)",
        "description": (
            "The baseline trade with the recovery assumption dropped from "
            "40% to 20%, as quoted for subordinated or distressed names. "
            "Recovery enters twice — the par-spread bootstrap and the "
            "pricing engine — and because the same par quotes re-anchor "
            "the curve, the NPV moves only second-order vs the 40% case; "
            "the case pins that both sides apply the same recovery in "
            "both places."
        ),
        "request": "cds/cds_eur_5y_buyer_recovery_20pct.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "recovery 20%",
                      "recovery in bootstrap and engine"],
    },
    {
        "id": "cds_eur_3y_buyer_100bp_spread_curve",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 3Y CDS, coupon above the short-end par spread",
        "description": (
            "A 3Y contract on the baseline curve: the interpolated 3Y par "
            "spread (~92bp) sits below the 100bp coupon, so unlike the 5Y "
            "case the protection buyer's NPV is negative. Exercises a "
            "shorter maturity and the front end of the credit curve, where "
            "the 1Y 80bp quote dominates."
        ),
        "request": "cds/cds_eur_3y_buyer_100bp_spread_curve.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "3Y maturity",
                      "credit curve short end", "negative NPV"],
    },
    {
        "id": "cds_eur_10y_buyer_100bp_spread_curve",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 10Y CDS, long-dated protection",
        "description": (
            "A 10Y contract on the baseline curve, priced out to the 10Y "
            "130bp par-spread pillar with the discount curve bootstrapped "
            "to its 12Y swap quote. The wide long-end spreads against the "
            "100bp coupon make the buyer's NPV strongly positive. "
            "Exercises the long end of both the credit and discount "
            "curves (~40 quarterly coupons)."
        ),
        "request": "cds/cds_eur_10y_buyer_100bp_spread_curve.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "10Y maturity",
                      "credit curve long end", "long-dated discounting"],
    },
    {
        "id": "cds_eur_5y_buyer_isda_engine",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS priced with the ISDA standard-model engine",
        "description": (
            "The baseline trade priced with the ISDA engine instead of "
            "MidPoint, with the standard settings (Taylor numerical fix, "
            "half-day accrual bias, piecewise forwards, settlement-date "
            "flows included) and the curve helpers also built in ISDA "
            "mode. The premium differs measurably from the MidPoint case "
            "on the same market data, pinning the engine selection end to "
            "end."
        ),
        "request": "cds/cds_eur_5y_buyer_isda_engine.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "ISDA engine",
                      "ISDA helper model", "standard ISDA settings"],
    },
    {
        "id": "cds_eur_5y_buyer_semiannual_act365f",
        "product": "cds",
        "family": "CDS",
        "title": "EUR 5Y CDS, old-style semiannual Act/365F premium leg",
        "description": (
            "A pre-big-bang style contract: the 100bp premium accrues "
            "semiannually on Act/365F with a Forward-generated schedule "
            "from the trade date instead of the quarterly Act/360 "
            "TwentiethIMM grid. Exercises the premium-leg frequency, day "
            "count and date-generation fields independently of the "
            "market-standard conventions."
        ),
        "request": "cds/cds_eur_5y_buyer_semiannual_act365f.json",
        "list_key": "cds_list",
        "ql_pricer": "price_cds_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["buy protection", "semiannual premium leg",
                      "Act/365F accrual", "Forward date generation",
                      "non-IMM schedule"],
    },

    # ------------------------------------------------------------------
    # Curves — /bootstrap-curves sampled series (compare="series": every
    # grid point of every returned series is checked element-wise against
    # an independently bootstrapped QuantLib curve, tolerance 1e-9)
    # ------------------------------------------------------------------
    {
        "id": "curves_eur_depo_swap_df_zero_tenor_grid",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR deposit+swap curve: discount factors and zero rates",
        "description": (
            "Baseline curve query: a EUR curve bootstrapped from 1M/3M/6M "
            "deposits and 1Y-10Y par swaps (LogLinear discount) sampled at "
            "ten market tenors (1W to 10Y, TARGET Modified Following) for "
            "both discount factors and continuously compounded zero rates "
            "on the curve's own Act/365F day counter."
        ),
        "request": "curves/curves_eur_depo_swap_df_zero_tenor_grid.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["deposit+swap bootstrap", "LogLinear discount",
                      "discount factors", "zero rates continuous",
                      "tenor grid", "TARGET"],
    },
    {
        "id": "curves_eur_depo_swap_zero_act360_compounded_semiannual",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR curve zero rates re-expressed Act/360, semiannual",
        "description": (
            "The baseline curve's zero rates re-quoted under non-default "
            "conventions: use_curve_day_counter=false with an Act/360 day "
            "counter and semiannually compounded rates instead of the "
            "curve-day-counter continuous default. Exercises the zero-query "
            "day-count/compounding/frequency overrides end to end."
        ),
        "request": "curves/curves_eur_depo_swap_zero_act360_compounded_semiannual.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["zero rates", "day-count override Act/360",
                      "Compounded/Semiannual", "quote-convention conversion"],
    },
    {
        "id": "curves_eur_depo_swap_fwd_3m_period",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR curve 3M period forward rates",
        "description": (
            "Simple-compounded 3-month forward rates F(d, d+3M) off the "
            "baseline curve at twelve grid tenors from 1M to 9Y, with the "
            "3M advance done on the grid's TARGET Modified Following "
            "calendar — the forward strip a money-market desk reads off a "
            "curve. Exercises the Period forward type and the tenor "
            "advance."
        ),
        "request": "curves/curves_eur_depo_swap_fwd_3m_period.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["forward rates", "Period 3M", "Simple compounding",
                      "grid-calendar advance"],
    },
    {
        "id": "curves_eur_depo_swap_fwd_instantaneous_plain_grid",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR curve instantaneous forwards, calendar-less grid",
        "description": (
            "Approximated instantaneous forwards (1-day period, continuous "
            "compounding) on a tenor grid that names no calendar, so grid "
            "dates are plain reference-date + tenor arithmetic (some land "
            "on weekends). Exercises the Instantaneous forward type, the "
            "1-day epsilon and the no-calendar grid branch."
        ),
        "request": "curves/curves_eur_depo_swap_fwd_instantaneous_plain_grid.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["forward rates", "Instantaneous 1D epsilon",
                      "Continuous compounding", "calendar-less tenor grid"],
    },
    {
        "id": "curves_eur_ois_estr_df_zero_tenor_grid",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR ESTR OIS curve: discount factors and zero rates",
        "description": (
            "An ESTR OIS curve bootstrapped from 1Y/2Y/5Y/10Y OIS par "
            "quotes (daily-compounded overnight index, LogLinear discount) "
            "sampled for discount factors and continuous zero rates at "
            "eight tenors. Exercises OIS-helper bootstrapping through the "
            "curve-sampling endpoint."
        ),
        "request": "curves/curves_eur_ois_estr_df_zero_tenor_grid.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["OIS bootstrap", "ESTR", "discount factors",
                      "zero rates continuous", "inverted short end"],
    },
    {
        "id": "curves_eur_depo_swap_df_daily_range_grid",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR curve discount factors on a dense daily grid",
        "description": (
            "Discount factors sampled every calendar day for six months "
            "(2025-01-15 to 2025-07-15, 182 points) off the baseline "
            "curve. Exercises the RangeGrid daily stepping path and dense "
            "sampling across the deposit-dominated short end, including "
            "the DF=1 point at the curve's reference date."
        ),
        "request": "curves/curves_eur_depo_swap_df_daily_range_grid.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["RangeGrid daily", "182 grid points",
                      "discount factors", "curve short end"],
    },
    {
        "id": "curves_eur_depo_swap_zero_business_days_range_grid",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR curve zero rates, business days only, one year",
        "description": (
            "Continuous zero rates sampled on every TARGET business day "
            "for a year (weekends and TARGET holidays such as Good "
            "Friday, Easter Monday, May 1 and Christmas filtered out — "
            "~256 points). The first grid date equals the curve reference "
            "date, exercising the server's clamp of at-or-before-reference "
            "zero-rate queries to reference+1."
        ),
        "request": "curves/curves_eur_depo_swap_zero_business_days_range_grid.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["RangeGrid business days only", "TARGET holiday filter",
                      "zero rates", "reference-date clamp"],
    },
    {
        "id": "curves_usd_zero_curve_linear_df_zero",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "USD explicit zero-rate curve (Linear), DF and zero rates",
        "description": (
            "A curve defined directly by six explicit continuously "
            "compounded zero-rate points (no bootstrap) with Linear "
            "interpolation on the US government bond calendar, sampled for "
            "discount factors and zero rates at seven tenors. Exercises "
            "the ZeroRatePoint curve branch and a non-TARGET grid "
            "calendar."
        ),
        "request": "curves/curves_usd_zero_curve_linear_df_zero.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["explicit zero-rate curve", "Linear interpolation",
                      "US government bond calendar", "discount factors",
                      "zero rates"],
    },
    {
        "id": "curves_eur_depo_swap_all_measures_tenor_grid",
        "product": "bootstrap_curves",
        "family": "Curves",
        "title": "EUR curve, all three measures in one query",
        "description": (
            "One query returning three aligned series — discount factors, "
            "continuous zero rates and simple 6M period forwards — at nine "
            "tenors off the baseline curve. Exercises multi-measure series "
            "alignment in a single response."
        ),
        "request": "curves/curves_eur_depo_swap_all_measures_tenor_grid.json",
        "list_key": "results",
        "ql_pricer": "bootstrap_curves_ql",
        "tolerance": SERIES_TOLERANCE,
        "compare": "series",
        "exercises": ["DF + zero + forward in one query",
                      "multi-series alignment", "Period 6M forwards"],
    },

    # ------------------------------------------------------------------
    # Calendars — date-utility endpoints (compare="exact": the returned
    # date list / advanced date must equal the QuantLib reference verbatim,
    # no tolerance; each case pins the CalendarToQL enum mapping too)
    # ------------------------------------------------------------------
    {
        "id": "cal_bd_target_easter_2025",
        "product": "calendar_business_days",
        "family": "Calendars",
        "title": "TARGET business days across Easter 2025",
        "description": (
            "The business dates from 2025-04-14 to 2025-04-25 on the TARGET "
            "calendar, a two-week window containing Good Friday (Apr 18) "
            "and Easter Monday (Apr 21) plus a weekend — 8 of the 12 days "
            "survive. Both endpoints included (the defaults)."
        ),
        "request": "calendar/cal_bd_target_easter_2025.json",
        "list_key": "dates",
        "ql_pricer": "calendar_business_days_ql",
        "compare": "exact",
        "exercises": ["business days", "TARGET", "Good Friday",
                      "Easter Monday", "inclusive endpoints"],
    },
    {
        "id": "cal_bd_usgovbond_july4_2025",
        "product": "calendar_business_days",
        "family": "Calendars",
        "title": "US government bond business days around July 4, 2025",
        "description": (
            "The business dates from 2025-06-30 to 2025-07-11 on the US "
            "government bond (SIFMA) calendar: Independence Day falls on a "
            "Friday, so the two-week window keeps 9 of its 10 weekdays. "
            "Pins the UnitedStatesGovernmentBond enum mapping on a "
            "date-utility endpoint."
        ),
        "request": "calendar/cal_bd_usgovbond_july4_2025.json",
        "list_key": "dates",
        "ql_pricer": "calendar_business_days_ql",
        "compare": "exact",
        "exercises": ["business days", "US government bond calendar",
                      "Independence Day", "single-holiday week"],
    },
    {
        "id": "cal_bd_uk_year_end_exclusive_ends",
        "product": "calendar_business_days",
        "family": "Calendars",
        "title": "UK year-end business days, both endpoints excluded",
        "description": (
            "The business dates strictly between 2025-12-24 and 2026-01-02 "
            "on the UnitedKingdom calendar (include_start and include_end "
            "both false). Christmas, Boxing Day and New Year's Day drop "
            "out, and because both excluded endpoints are themselves "
            "business days, only Dec 29-31 remain — pinning the endpoint "
            "flags and a year boundary in one case."
        ),
        "request": "calendar/cal_bd_uk_year_end_exclusive_ends.json",
        "list_key": "dates",
        "ql_pricer": "calendar_business_days_ql",
        "compare": "exact",
        "exercises": ["business days", "UnitedKingdom calendar",
                      "exclusive endpoints", "year boundary",
                      "Christmas/Boxing Day/New Year"],
    },
    {
        "id": "cal_bd_japan_golden_week_2025",
        "product": "calendar_business_days",
        "family": "Calendars",
        "title": "Japan business days through Golden Week 2025",
        "description": (
            "The business dates from 2025-04-28 to 2025-05-09 on the Japan "
            "calendar: Showa Day (Apr 29), Constitution/Greenery/Children's "
            "Day (May 3-5) and the May 6 substitute holiday all drop out. "
            "Exercises a calendar with substitute-holiday rules that no "
            "pricing case in the catalog touches."
        ),
        "request": "calendar/cal_bd_japan_golden_week_2025.json",
        "list_key": "dates",
        "ql_pricer": "calendar_business_days_ql",
        "compare": "exact",
        "exercises": ["business days", "Japan calendar", "Golden Week",
                      "substitute holiday"],
    },
    {
        "id": "cal_hol_target_2025",
        "product": "calendar_holidays",
        "family": "Calendars",
        "title": "TARGET holidays for calendar year 2025",
        "description": (
            "The holiday list for the whole of 2025 on the TARGET calendar "
            "with weekends excluded (the default): New Year's Day, Good "
            "Friday, Easter Monday, Labour Day, Christmas and St. "
            "Stephen's Day — six dates. The baseline holidays query."
        ),
        "request": "calendar/cal_hol_target_2025.json",
        "list_key": "dates",
        "ql_pricer": "calendar_holidays_ql",
        "compare": "exact",
        "exercises": ["holidays", "TARGET", "full calendar year",
                      "weekends excluded"],
    },
    {
        "id": "cal_hol_usnyse_2025_h1",
        "product": "calendar_holidays",
        "family": "Calendars",
        "title": "NYSE holidays, first half of 2025",
        "description": (
            "The New York Stock Exchange holiday list from 2025-01-01 to "
            "2025-06-30: New Year, the January 9 national day of mourning, "
            "Martin Luther King Day, Washington's Birthday, Good Friday, "
            "Memorial Day and Juneteenth — seven weekday closures. Pins "
            "the UnitedStatesNYSE enum mapping (a different holiday set "
            "than the SIFMA government bond calendar)."
        ),
        "request": "calendar/cal_hol_usnyse_2025_h1.json",
        "list_key": "dates",
        "ql_pricer": "calendar_holidays_ql",
        "compare": "exact",
        "exercises": ["holidays", "NYSE calendar", "exchange closures",
                      "US market variant mapping"],
    },
    {
        "id": "cal_hol_uk_festive_include_weekends",
        "product": "calendar_holidays",
        "family": "Calendars",
        "title": "UK festive-season non-business days, weekends included",
        "description": (
            "Every non-business day from 2025-12-20 to 2026-01-05 on the "
            "UnitedKingdom calendar with include_weekends=true: the "
            "December and January weekends interleaved with Christmas, "
            "Boxing Day and New Year's Day — nine dates across a year "
            "boundary. Exercises the include_weekends flag."
        ),
        "request": "calendar/cal_hol_uk_festive_include_weekends.json",
        "list_key": "dates",
        "ql_pricer": "calendar_holidays_ql",
        "compare": "exact",
        "exercises": ["holidays", "UnitedKingdom calendar",
                      "include_weekends", "year boundary"],
    },
    {
        "id": "cal_adv_target_10bd_over_easter",
        "product": "calendar_advance",
        "family": "Calendars",
        "title": "TARGET: advance 10 business days across Easter",
        "description": (
            "2025-04-14 advanced by 10 business days on TARGET: the count "
            "skips two weekends plus Good Friday and Easter Monday, "
            "landing on 2025-04-30. Business-day stepping is "
            "convention-independent, so this pins the Days unit and the "
            "holiday skips themselves."
        ),
        "request": "calendar/cal_adv_target_10bd_over_easter.json",
        "list_key": "advanced_date",
        "ql_pricer": "calendar_advance_ql",
        "compare": "exact",
        "exercises": ["advance", "10 business days", "TARGET",
                      "Easter holidays skipped"],
    },
    {
        "id": "cal_adv_target_1m_following_crosses_month",
        "product": "calendar_advance",
        "family": "Calendars",
        "title": "TARGET: +1 month, Following rolls into September",
        "description": (
            "2025-07-31 plus one month lands on Sunday 2025-08-31; under "
            "the Following convention the result rolls forward out of the "
            "month to Monday 2025-09-01. Paired with the ModifiedFollowing "
            "case on the identical input, this pins the convention "
            "difference at a month end."
        ),
        "request": "calendar/cal_adv_target_1m_following_crosses_month.json",
        "list_key": "advanced_date",
        "ql_pricer": "calendar_advance_ql",
        "compare": "exact",
        "exercises": ["advance", "1 month", "Following",
                      "weekend landing", "rolls past month end"],
    },
    {
        "id": "cal_adv_target_1m_modfollowing_rolls_back",
        "product": "calendar_advance",
        "family": "Calendars",
        "title": "TARGET: +1 month, ModifiedFollowing stays in August",
        "description": (
            "The same 2025-07-31 plus one month onto Sunday 2025-08-31, "
            "but under ModifiedFollowing: rolling forward would leave "
            "August, so the date rolls back to Friday 2025-08-29 instead. "
            "The counterpart of the Following case — same input, different "
            "convention, different month."
        ),
        "request": "calendar/cal_adv_target_1m_modfollowing_rolls_back.json",
        "list_key": "advanced_date",
        "ql_pricer": "calendar_advance_ql",
        "compare": "exact",
        "exercises": ["advance", "1 month", "ModifiedFollowing",
                      "weekend landing", "rolls back within month"],
    },
    {
        "id": "cal_adv_target_eom_jan31_1m",
        "product": "calendar_advance",
        "family": "Calendars",
        "title": "TARGET: +1 month from Jan 31 with end-of-month rule",
        "description": (
            "2025-01-31 (the last business day of January) plus one month "
            "with end_of_month=true: the end-of-month rule maps it to the "
            "last business day of February, 2025-02-28, rather than a "
            "plain day-of-month shift. Exercises the end_of_month flag on "
            "the wire."
        ),
        "request": "calendar/cal_adv_target_eom_jan31_1m.json",
        "list_key": "advanced_date",
        "ql_pricer": "calendar_advance_ql",
        "compare": "exact",
        "exercises": ["advance", "1 month", "end-of-month rule",
                      "month-end roll", "TARGET"],
    },
    {
        "id": "cal_adv_usgovbond_2w_preceding_july4",
        "product": "calendar_advance",
        "family": "Calendars",
        "title": "US government bond: +2 weeks, Preceding off July 4",
        "description": (
            "2025-06-20 plus two weeks lands exactly on Independence Day "
            "(Friday 2025-07-04); under the Preceding convention the "
            "result rolls back to Thursday 2025-07-03. A holiday (not "
            "weekend) landing that forces an adjustment, on the Weeks "
            "unit and the SIFMA calendar."
        ),
        "request": "calendar/cal_adv_usgovbond_2w_preceding_july4.json",
        "list_key": "advanced_date",
        "ql_pricer": "calendar_advance_ql",
        "compare": "exact",
        "exercises": ["advance", "2 weeks", "Preceding",
                      "holiday landing", "US government bond calendar"],
    },
    {
        "id": "cal_adv_uk_1y_weekend_following",
        "product": "calendar_advance",
        "family": "Calendars",
        "title": "UK: +1 year onto a Saturday, Following",
        "description": (
            "2025-08-22 plus one year on the UnitedKingdom calendar lands "
            "on Saturday 2026-08-22 and rolls Following to Monday "
            "2026-08-24 (the August bank holiday is the following Monday, "
            "Aug 31, so the 24th stands). Exercises the Years unit and a "
            "weekend adjustment on a non-TARGET calendar."
        ),
        "request": "calendar/cal_adv_uk_1y_weekend_following.json",
        "list_key": "advanced_date",
        "ql_pricer": "calendar_advance_ql",
        "compare": "exact",
        "exercises": ["advance", "1 year", "Following", "weekend landing",
                      "UnitedKingdom calendar"],
    },
    # ------------------------------------------------------------------
    # Inflation — zero-coupon (ZCIIS) and year-on-year (YYIIS) inflation
    # swaps priced on bootstrapped zero/YoY inflation curves. Note the
    # QuantLib direction conventions both sides implement: a ZCIIS "Payer"
    # pays the inflation leg and receives fixed, while a YYIIS "Payer" pays
    # the fixed leg and receives the YoY leg.
    # ------------------------------------------------------------------
    {
        "id": "zciis_eur_5y_payer_linear_obs",
        "product": "zero_coupon_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y ZCIIS, pay inflation vs 2.10% fixed, Linear CPI",
        "description": (
            "Baseline zero-coupon inflation swap: pay realized HICP "
            "inflation compounded over 5 years, receive 2.10% fixed "
            "compounded, on 1m notional (TARGET, Act/365F, 3M observation "
            "lag, linearly interpolated monthly CPI observations). The zero "
            "inflation curve is bootstrapped from ZCIIS quotes (1Y 2.00% to "
            "10Y 2.35%) on the EUR deposit+swap nominal curve; the 5Y "
            "breakeven of ~2.20% sits above the 2.10% fixed leg, so the "
            "payer of inflation is underwater."
        ),
        "request": "inflation/zciis_eur_5y_payer_linear_obs.json",
        "list_key": "swaps",
        "ql_pricer": "price_zero_coupon_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["ZCIIS", "pay inflation", "CPI Linear observation",
                      "3M observation lag", "zero-inflation bootstrap"],
    },
    {
        "id": "zciis_eur_5y_receiver_linear_obs",
        "product": "zero_coupon_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y ZCIIS, same trade receiving inflation",
        "description": (
            "The other side of the baseline trade: receive realized HICP "
            "inflation, pay 2.10% fixed, on identical market data. "
            "Everything else is unchanged, so the NPV is the exact negative "
            "of the payer case, pinning the ZCIIS direction convention end "
            "to end."
        ),
        "request": "inflation/zciis_eur_5y_receiver_linear_obs.json",
        "list_key": "swaps",
        "ql_pricer": "price_zero_coupon_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["ZCIIS", "receive inflation", "sign flip",
                      "CPI Linear observation"],
    },
    {
        "id": "zciis_eur_10y_payer_linear_obs",
        "product": "zero_coupon_inflation_swap",
        "family": "Inflation",
        "title": "EUR 10Y ZCIIS, pay inflation vs 2.40% fixed",
        "description": (
            "Long-dated zero-coupon inflation swap out to the curve's 10Y "
            "2.35% pillar: pay 10 years of compounded HICP inflation, "
            "receive 2.40% fixed compounded. The fixed leg sits above the "
            "10Y breakeven, so the payer of inflation is ahead. Exercises "
            "the long end of the zero-inflation bootstrap and a decade of "
            "compounding on both legs."
        ),
        "request": "inflation/zciis_eur_10y_payer_linear_obs.json",
        "list_key": "swaps",
        "ql_pricer": "price_zero_coupon_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["ZCIIS", "pay inflation", "10Y maturity",
                      "curve long end", "positive NPV"],
    },
    {
        "id": "zciis_eur_3y_payer_asindex_obs",
        "product": "zero_coupon_inflation_swap",
        "family": "Inflation",
        "title": "EUR 3Y ZCIIS, AsIndex (flat monthly) CPI observation",
        "description": (
            "A 3Y zero-coupon inflation swap whose CPI observations use "
            "AsIndex interpolation on both the trade and the curve helpers: "
            "the index is built non-interpolated, so each observation reads "
            "the fixing of the observation month as-is instead of "
            "interpolating within the month. Pays inflation vs 2.05% fixed "
            "against a ~2.10% 3Y breakeven."
        ),
        "request": "inflation/zciis_eur_3y_payer_asindex_obs.json",
        "list_key": "swaps",
        "ql_pricer": "price_zero_coupon_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["ZCIIS", "CPI AsIndex observation",
                      "flat monthly fixing", "3Y maturity"],
    },
    {
        "id": "zciis_eur_5y_payer_6m_observation_lag",
        "product": "zero_coupon_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y ZCIIS with a 6M observation lag",
        "description": (
            "The baseline 5Y trade with the trade-level observation lag "
            "widened from 3 to 6 months (the curve helpers keep their 3M "
            "lag), so both the base CPI (July/August 2024 fixings) and the "
            "final observation shift back a quarter and the breakeven drops "
            "to ~2.17%. Exercises the trade observation lag independently "
            "of the curve's."
        ),
        "request": "inflation/zciis_eur_5y_payer_6m_observation_lag.json",
        "list_key": "swaps",
        "ql_pricer": "price_zero_coupon_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["ZCIIS", "6M observation lag",
                      "trade lag != helper lag", "past CPI fixings"],
    },
    {
        "id": "zciis_eur_5y_payer_dated_helpers",
        "product": "zero_coupon_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y ZCIIS on a curve with explicit-date helpers",
        "description": (
            "The baseline 5Y trade priced on a zero-inflation curve whose "
            "helpers carry explicit end dates (July anniversaries, "
            "2026-07-15 to 2035-07-15, one landing on a Saturday and used "
            "verbatim) instead of tenors, so every pillar sits half a year "
            "off the trade's dates and the 5Y observation is a genuine "
            "interpolation between the 2030 and 2032 pillars. Exercises "
            "the end_date branch of the helper date resolution."
        ),
        "request": "inflation/zciis_eur_5y_payer_dated_helpers.json",
        "list_key": "swaps",
        "ql_pricer": "price_zero_coupon_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["ZCIIS", "explicit end-date helpers",
                      "off-grid pillars", "weekend helper maturity"],
    },
    {
        "id": "yyiis_eur_5y_receiver_annual",
        "product": "year_on_year_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y YoY inflation swap, receive 2.15% fixed",
        "description": (
            "Baseline year-on-year inflation swap: receive 2.15% fixed "
            "annually (Act/365F), pay the annual year-on-year HICP rate, "
            "on 1m notional (TARGET, Modified Following, 3M observation "
            "lag, Linear interpolation). The YoY curve is bootstrapped "
            "from YoY swap quotes (1Y 2.00% to 7Y 2.25%) whose helpers "
            "discount on the nominal curve; the 5Y fair rate of ~2.20% "
            "sits above the fixed leg, so the fixed receiver is behind."
        ),
        "request": "inflation/yyiis_eur_5y_receiver_annual.json",
        "list_key": "swaps",
        "ql_pricer": "price_year_on_year_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["YYIIS", "receive fixed", "annual YoY coupons",
                      "YoY-inflation bootstrap", "3M observation lag"],
    },
    {
        "id": "yyiis_eur_5y_payer_annual",
        "product": "year_on_year_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y YoY inflation swap, same trade paying fixed",
        "description": (
            "The other side of the baseline YoY trade: pay 2.15% fixed, "
            "receive the annual year-on-year HICP rate, on identical "
            "market data. The NPV is the exact negative of the receiver "
            "case, pinning the YYIIS direction convention (a QuantLib "
            "YoY-swap Payer pays the fixed leg) end to end."
        ),
        "request": "inflation/yyiis_eur_5y_payer_annual.json",
        "list_key": "swaps",
        "ql_pricer": "price_year_on_year_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["YYIIS", "pay fixed", "sign flip",
                      "annual YoY coupons"],
    },
    {
        "id": "yyiis_eur_3y_receiver_semiannual_30360",
        "product": "year_on_year_inflation_swap",
        "family": "Inflation",
        "title": "EUR 3Y YoY swap, semiannual, 30/360 fixed vs Act/360 YoY",
        "description": (
            "A 3Y year-on-year swap with semiannual schedules on both legs "
            "and split day counts: the 2.10% fixed leg accrues 30/360 while "
            "the YoY leg accrues Act/360, so each semiannual YoY coupon "
            "observes the year-on-year rate at its own lagged date on the "
            "annually-quoted curve. Exercises non-annual YoY periods and "
            "per-leg day-count overrides."
        ),
        "request": "inflation/yyiis_eur_3y_receiver_semiannual_30360.json",
        "list_key": "swaps",
        "ql_pricer": "price_year_on_year_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["YYIIS", "semiannual schedules", "fixed 30/360",
                      "YoY Act/360", "day-count split"],
    },
    {
        "id": "yyiis_eur_5y_payer_spread25bp",
        "product": "year_on_year_inflation_swap",
        "family": "Inflation",
        "title": "EUR 5Y YoY swap, +25bp spread on the YoY leg",
        "description": (
            "The 5Y YoY trade paying 2.20% fixed against the year-on-year "
            "HICP rate plus a 25bp spread, so the effective fair fixed "
            "rate rises to ~2.45% and the fixed payer is well ahead. "
            "Exercises the YoY-leg spread field end to end."
        ),
        "request": "inflation/yyiis_eur_5y_payer_spread25bp.json",
        "list_key": "swaps",
        "ql_pricer": "price_year_on_year_inflation_swap_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["YYIIS", "pay fixed", "YoY spread +25bp",
                      "positive NPV"],
    },

    # ------------------------------------------------------------------
    # Equity — European vanilla options (Black-Scholes-Merton)
    # ------------------------------------------------------------------
    {
        "id": "eqopt_eur_call_atm_1y",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European call, at the money",
        "description": (
            "Baseline equity option: a European call on a 100.00 spot "
            "struck at 100.00, expiring in one year, 20% flat Black vol, "
            "3% continuous risk-free zero curve, zero dividend yield. "
            "Priced with the analytic Black-Scholes-Merton (European) "
            "engine on both sides."
        ),
        "request": "equity/eqopt_eur_call_atm_1y.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "call", "at-the-money strike",
                      "analytic BSM engine", "flat 20% vol",
                      "zero dividend yield"],
    },
    {
        "id": "eqopt_eur_put_atm_1y",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European put, at the money",
        "description": (
            "The put twin of the baseline trade: same 100.00 spot, 100.00 "
            "strike, 1Y expiry, 20% vol and 3% risk-free curve. With zero "
            "dividends the call and put premiums differ by exactly the "
            "put-call parity gap, pinning the option-type flag end to end."
        ),
        "request": "equity/eqopt_eur_put_atm_1y.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "put", "at-the-money strike",
                      "option-type flip", "analytic BSM engine"],
    },
    {
        "id": "eqopt_eur_call_itm_k80_1y",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European call struck at 80, deep in the money",
        "description": (
            "The baseline 1Y call struck at 80.00 against the 100.00 spot, "
            "so exercise is close to certain and the premium is dominated "
            "by intrinsic value (spot minus discounted strike). Pins the "
            "high-premium end of the strike spectrum."
        ),
        "request": "equity/eqopt_eur_call_itm_k80_1y.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "call", "in-the-money strike",
                      "intrinsic-dominated"],
    },
    {
        "id": "eqopt_eur_call_otm_k120_1y",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European call struck at 120, out of the money",
        "description": (
            "The baseline 1Y call struck at 120.00, 20% above spot, so the "
            "premium is pure time value and small relative to spot — the "
            "tail where a vol or day-count slip is proportionally largest."
        ),
        "request": "equity/eqopt_eur_call_otm_k120_1y.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "call", "out-of-the-money strike",
                      "time-value only"],
    },
    {
        "id": "eqopt_eur_put_itm_k120_1y",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European put struck at 120, in the money",
        "description": (
            "1Y put struck at 120.00 against the 100.00 spot — the "
            "protective-put side of the moneyness spectrum, with the "
            "premium dominated by the intrinsic value of selling 20% above "
            "market."
        ),
        "request": "equity/eqopt_eur_put_itm_k120_1y.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "put", "in-the-money strike",
                      "intrinsic-dominated"],
    },
    {
        "id": "eqopt_eur_put_otm_k80_1y",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European put struck at 80, out of the money",
        "description": (
            "1Y put struck at 80.00, 20% below spot — cheap crash "
            "protection whose premium is pure time value. Together with "
            "the 120 put it brackets the put moneyness spectrum."
        ),
        "request": "equity/eqopt_eur_put_otm_k80_1y.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "put", "out-of-the-money strike",
                      "time-value only"],
    },
    {
        "id": "eqopt_eur_call_atm_1y_div_yield_2_5pct",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European call with a 2.5% dividend yield",
        "description": (
            "The baseline at-the-money 1Y call on an underlying paying a "
            "2.5% continuous dividend yield: the underlying spec's "
            "dividend curve is a flat 2.5% zero curve feeding the "
            "Black-Scholes-Merton process, so the forward drops below "
            "spot and the call premium falls versus the zero-dividend "
            "baseline. Pins the dividend-yield leg of the process."
        ),
        "request": "equity/eqopt_eur_call_atm_1y_div_yield_2_5pct.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "call", "continuous dividend yield 2.5%",
                      "BSM dividend leg"],
    },
    {
        "id": "eqopt_eur_put_atm_2y_vol35",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 2Y European put, 35% vol",
        "description": (
            "Longer-dated, higher-vol variation: an at-the-money put "
            "expiring in two years on a flat 35% Black vol. Exercises a "
            "second vol level and a second expiry on the same market "
            "data skeleton."
        ),
        "request": "equity/eqopt_eur_put_atm_2y_vol35.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "put", "2Y expiry", "flat 35% vol",
                      "vol/expiry variation"],
    },
    {
        "id": "eqopt_eur_call_atm_3m_short_dated",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 3M European call, short-dated",
        "description": (
            "Short-dated variation: the at-the-money call expiring in "
            "three months (2025-04-15), where the premium is small and "
            "most sensitive to the day-count handling of the short "
            "time-to-expiry."
        ),
        "request": "equity/eqopt_eur_call_atm_3m_short_dated.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "call", "3M expiry", "short-dated",
                      "small premium"],
    },
    {
        "id": "eqopt_eur_call_atm_1y_qty100",
        "product": "equity_option",
        "family": "Equity",
        "title": "EUR 1Y European call, 100 options",
        "description": (
            "The baseline at-the-money 1Y call with quantity 100: the "
            "response NPV is the per-option premium scaled by the trade's "
            "quantity on both sides, pinning the quantity field end to "
            "end (exactly 100x the single-option baseline)."
        ),
        "request": "equity/eqopt_eur_call_atm_1y_qty100.json",
        "list_key": "options",
        "ql_pricer": "price_equity_option_ql",
        "tolerance": DEFAULT_TOLERANCE,
        "exercises": ["European", "call", "quantity 100",
                      "quantity scaling"],
    },
]
