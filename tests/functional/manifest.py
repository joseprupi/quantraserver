"""Functional parity catalog — single source of truth.

Every entry describes one "POST JSON to the server, compare the NPV against an
independent QuantLib reference pricer" case. The same list drives:

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
  list_key    Response list holding the instrument NPV ("swaps", "bonds", ...).
  ql_pricer   Name of the reference pricer function in
              tests/contract/ql_reference.py (e.g. "price_vanilla_swap_ql").
  tolerance   Max allowed abs(api_npv - quantlib_npv). Default 0.01 (one cent
              on notionals of millions — the server and the reference build
              the same QuantLib objects, so parity is near machine precision).
  exercises   Short tags describing what the case covers; rendered in the
              catalog's "What it exercises" column.

To add a case: drop the request JSON under examples/data/ (IR swaps live in
examples/data/ir_swaps/, bonds in examples/data/bonds/, FRAs in
examples/data/fra/), append a dict here, regenerate the catalog
(python3 tests/functional/generate_catalog.py inside the test image) and run
the gate. See tests/functional/README.md for the full walkthrough.
"""

DEFAULT_TOLERANCE = 0.01

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
]
