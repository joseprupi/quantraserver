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
examples/data/fra/, caps/floors in examples/data/cap_floor/, swaptions in
examples/data/swaption/, CDS in examples/data/cds/), append a dict here,
regenerate the catalog
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
]
