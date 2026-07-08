# Functional parity catalog

A manifest-driven set of end-to-end cases: each case is a complete JSON
request that is POSTed to the running Quantra server, and the response is
asserted to equal an independent QuantLib reference (computed by
`tests/contract/ql_reference.py`) within a tight tolerance. Pricing cases
compare a single NPV (default tolerance **0.01**); curve- and vol-surface-
sampling cases compare every point of every returned series (tolerance
**1e-9**); calendar date-utility cases must match the reference **exactly**;
calibration cases compare every calibrated parameter and fit statistic
(tolerance **1e-7**) — see "Comparison modes". The server and the reference
are pinned to the same QuantLib version, so the observed differences are at
machine precision — a real behavioural drift on either side fails the gate
immediately.

Browse the catalog:

* [`CATALOG.md`](CATALOG.md) — rendered by GitHub, grouped by product family.
* [`catalog.html`](catalog.html) — the same catalog as a single
  self-contained HTML file; open it locally in any browser.

Both files are **generated** from `manifest.py` — never edit them by hand.
`test_functional_parity.py::test_catalog_in_sync` fails whenever the
committed catalog drifts from the manifest.

## Layout

| Piece | Role |
|---|---|
| `manifest.py` | Single source of truth: one dict per case (id, product, family, title, description, request path, response list key, reference pricer, tolerance, exercise tags). |
| `test_functional_parity.py` | Pytest driver: parametrizes over the manifest, POSTs each request, prices the same JSON with the named `ql_reference` pricer, asserts `abs(api - ql) < tolerance`. Also validates the manifest shape and catalog freshness. |
| `generate_catalog.py` | Renders `CATALOG.md` + `catalog.html` from the manifest (computes each case's QuantLib NPV, so it needs the QuantLib Python package — run it inside the test image). |
| `conftest.py` | Reuses the contract-suite harness: puts `tests/contract` on `sys.path` and provides the same `client` / `data_dir` fixtures (`--url`, `--data-dir`). |
| `../../examples/data/ir_swaps/` | The curated request JSONs for the IR-swap family. |
| `../../examples/data/bonds/` | The curated request JSONs for the Bonds family. |
| `../../examples/data/fra/` | The curated request JSONs for the FRA family. |
| `../../examples/data/cap_floor/` | The curated request JSONs for the Cap/Floor family. |
| `../../examples/data/swaption/` | The curated request JSONs for the Swaption family. |
| `../../examples/data/cds/` | The curated request JSONs for the CDS family. |
| `../../examples/data/curves/` | The curated request JSONs for the Curves family. |
| `../../examples/data/calendar/` | The curated request JSONs for the Calendars family. |
| `../../examples/data/inflation/` | The curated request JSONs for the Inflation family. |
| `../../examples/data/equity/` | The curated request JSONs for the Equity family. |
| `../../examples/data/vol/` | The curated request JSONs for the Vol / Calibration family. |

## Comparison modes

The manifest's optional `compare` field selects how a case is asserted;
existing rows omit it and keep the original behaviour.

* `compare: "npv"` (default) — the response holds one instrument under
  `list_key` and the reference returns a float:
  `abs(response[list_key][0].npv - ql) < tolerance`.
* `compare: "series"` — for endpoints that return sampled value series:
  `list_key` names the response list (`results`) and the reference returns
  the expected values, either `{series_name: [floats]}` keyed like the
  response's `series[].measure` (`DF`/`ZERO`/`FWD` on `/bootstrap-curves`)
  or a flat list when there is exactly one series —
  `/sample-vol-surfaces`' flat `results[0].vols` list is compared as that
  single series. The driver asserts the series sets match, the lengths
  match, and `abs(api[i] - ql[i]) < tolerance` for **every** point,
  reporting the maximum gap on failure.
* `compare: "exact"` — for the calendar date-utility endpoints
  (`/calendar-business-days`, `/calendar-holidays`, `/calendar-advance`),
  which return exact dates rather than floats. The reference returns the
  expected value verbatim — a list of ISO date strings under `list_key`
  `dates` (business days / holidays) or a single date string under
  `advanced_date` (advance) — and the response must equal it **exactly**:
  same length, same order, same strings. There is no tolerance; exact cases
  carry no `tolerance` field in the manifest (the driver forbids one).
* `compare: "fields"` — for the calibration endpoints
  (`/calibrate-swaption-model`, `/calibrate-swaption-vol`), whose response
  is a flat result object rather than an instrument list. The reference
  returns `{dotted.response.path: expected}` — e.g. `hw_sigma` or
  `diagnostics.alpha_per_node` — where each expected value is a float or a
  list of floats, and the driver asserts every named field is present and
  matches within the case tolerance (element-wise and length-checked for
  lists), reporting the worst gap on failure. Fields cases carry no
  `list_key` (the driver forbids one).

Series cases use a much tighter tolerance than NPV cases (1e-9, set per case
in the manifest) because the compared quantities — discount factors, zero
rates, forward rates, volatilities — are of order 0.01-1.0. Both sides build
identical QuantLib objects, so the residual noise is bounded by the ~12
significant digits FlatBuffers prints response doubles with plus the two
bootstrap accuracies (~1e-12 combined); 1e-9 keeps three orders of margin
over that floor while staying far below anything economically meaningful
(1e-9 in a rate is 1e-5 of a basis point). Never loosen a series tolerance
to make a case pass — fix the mismatch instead.

Calibration fields use 1e-7: the compared quantities (Hull-White a/sigma,
SABR alpha/beta/rho/nu, fit RMSEs) come out of the same QuantLib
Levenberg-Marquardt code on both sides, and every cataloged calibration is
deliberately well-conditioned — sharp minimum, parameters well inside their
bounds — where the measured cross-side gaps are below 3e-12. The extra
headroom over 1e-9 absorbs optimizer stopping-point wobble across future
compiler/QuantLib-build changes without ever approaching an economically
meaningful difference (1e-7 in a vol is a thousandth of a basis point).
The same honesty rule applies: a case that needs a looser tolerance to pass
is reporting a real difference — investigate it, never widen the tolerance.

In the generated catalog, series cases show a summary in the value column
(e.g. `DF/ZERO series, 10 points`), and calibration cases show the scalar
fields plus a per-node grid count, instead of an NPV.

## How it runs in the gate

Suite 3 of `tests/run_all_tests.sh` invokes pytest on `tests/contract` and
`tests/functional` together, against the servers the runner just started, so
these cases are part of the standard gate:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
    bash -lc './scripts/build.sh Release && bash tests/run_all_tests.sh'
```

## Running just this suite

With a server already listening (`build/jsonserver/json_server localhost:50051 8080`
in front of `build/server/sync_server 50051`), from the repo root inside the
test image:

```bash
python3 -m pytest tests/functional -q \
    --url http://localhost:8080 --data-dir examples/data
```

## Adding a case

1. **Write the request JSON** under `examples/data/` — IR-swap cases live in
   `examples/data/ir_swaps/`, bond cases in `examples/data/bonds/`, FRA
   cases in `examples/data/fra/`, cap/floor cases in
   `examples/data/cap_floor/`, swaption cases in `examples/data/swaption/`,
   CDS cases in `examples/data/cds/`, curve-sampling cases in
   `examples/data/curves/`, calendar date-utility cases in
   `examples/data/calendar/`, inflation-swap cases in
   `examples/data/inflation/`, equity-option cases in
   `examples/data/equity/`, vol-sampling and calibration cases in
   `examples/data/vol/`.
   Start from an existing case with the same product. Keep the request
   self-contained (indices, curves and the trade in one file) and prefer
   explicit fields over schema defaults.
2. **Check the reference supports it.** The manifest's `ql_pricer` names a
   function in `tests/contract/ql_reference.py`; that pricer must genuinely
   honour every field your case varies. If the variation needs a reference
   feature that does not exist yet, do not add a weakened case — put it on
   the "planned" list below instead.
3. **Append a dict to `CASES` in `manifest.py`** (the docstring there
   documents every field). Give it a stable unique `id`, a real-world
   `description` and honest `exercises` tags.
4. **Regenerate the catalog** inside the test image:

   ```bash
   docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
       bash -lc 'python3 tests/functional/generate_catalog.py'
   ```

5. **Run the full gate** (command above) — your case must pass within
   tolerance, and `test_catalog_in_sync` must see the regenerated files.

## Current coverage (IR Swaps)

* Vanilla fixed-vs-IBOR: payer/receiver, fixed 30/360 annual and Act/365F
  semiannual, Euribor 6M and 3M, Following + Backward schedule conventions,
  floating spread, and OIS-discounted multicurve.
* Tenor spectrum: 1Y money-market end, standard 4Y-7Y, and a 30Y long-dated
  swap on a curve bootstrapped out to the 30Y par quote; plus a 1Y-forward
  starting 5Y swap (all coupons projected, deferred effective date).
* Index / frequency variations: Euribor 1M with a monthly floating leg
  (36 coupons, 1M-indexed par-swap curve) alongside the 3M/6M cases.
* Schedule mechanics: end-of-month rolls (end_of_month=true from a Feb-28
  effective date) and IMM-dated schedules (ThirdWednesday generation on both
  legs, futures-strip style).
* Calendars / currencies: TARGET, US government bond (SIFMA), and a GBP swap
  on the UnitedKingdom calendar with same-day fixings and Act/365F
  semiannual fixed conventions.
* OIS: compounded ESTR (payer/receiver, annual/quarterly, overnight-leg
  spread, 2-day payment lag) and compounded SOFR on the US government bond
  calendar, with curves bootstrapped from OIS par quotes.
* Tenor basis: Euribor 3M + spread vs Euribor 6M, single-curve and fully
  multicurve (per-leg projection curves + OIS discounting).

## IR-swap coverage checklist

Legend: ✅ covered · ◐ partial · ☐ planned (expressible, not yet added) ·
⛔ deferred (needs reference/engine work).

**Legs & structure**
- ✅ Vanilla fixed vs Ibor float · ✅ payer / receiver · ✅ OIS (compounded
  overnight) · ✅ tenor basis (float vs float) · ✅ forward-starting
- ☐ Stub periods (short/long first & last coupon) · ☐ zero-coupon swap
- ⛔ CMS leg · ⛔ amortizing / step-up notional · ⛔ in-arrears / lookback /
  averaged OIS · ⛔ geared / capped / floored floater

**Conventions**
- ◐ Fixed day counts (✅ 30/360, Act/365F; ☐ Act/360, Act/Act on fixed leg)
- ☐ Float-leg day-count sweep
- ◐ Frequencies (✅ annual / semi / quarterly / monthly across cases; ☐ full matrix)
- ◐ Business-day conventions (✅ ModifiedFollowing, Following; ☐ Preceding,
  ModifiedPreceding)
- ✅ Date generation (Forward, Backward, ThirdWednesday/IMM) · ✅ end-of-month rolls
- ◐ Calendars (✅ TARGET, UK, US government bond; ☐ Japan, combined/joint)
- ✅ Payment lag · ☐ settlement-days variations

**Curves & discounting**
- ✅ Single curve · ✅ OIS-discounted multicurve (independently bootstrapped)
- ◐ Index tenors (✅ 1M / 3M / 6M; ☐ 12M)
- ⛔ deps-linked exogenous discounting fidelity · ⛔ cross-currency

**Rates / market / edge**
- ✅ Positive rates · ✅ short-dated (1Y) · ✅ long-dated (30Y)
- ☐ Negative rates · ☐ flat / steep / inverted curve shapes · ☐ at-par (NPV≈0)
- ☐ Seasoned swap with past fixings

## Current coverage (Bonds)

* Fixed-rate bonds: coupon at/above/below the curve (par, premium, discount);
  annual, semiannual and quarterly coupon frequencies; 30/360, Act/Act (Bond
  basis) and Act/365F accrual day counts; a 0%-coupon (zero-coupon) bullet;
  the tenor spectrum from a 2Y short note to a 30Y long-dated issue on a
  curve bootstrapped out to the 30Y par quote.
* Conventions / mechanics: T+1, T+2 and T+3 settlement; a non-par redemption
  (101.5) with Preceding payment rolls on an Unadjusted schedule; a
  seasoned USD Treasury-style bond (past coupon already paid) on the US
  government bond calendar with a Backward Unadjusted schedule.
* Curves: deposit+swap bootstrapped discounting and an explicit zero-rate
  curve.
* Floating-rate bonds: Euribor 6M semiannual and Euribor 3M quarterly
  coupons; zero and +40bp spreads; OIS-discounted multicurve
  (forwarding != discounting); Act/365F bond accrual against the index's
  Act/360; a bond-level 0-fixing-days override (same-day fixing).

## Planned / not yet covered (Bonds)

These need reference-pricer or engine features that do not exist yet (the
bond wire schemas carry a single rate, a single redemption and no option
schedule); they are deliberately not half-implemented:

* **Callable / putable bonds** — no call/put schedule on the wire and no
  reference pricer for embedded options.
* **Amortizing / sinking-fund redemption schedules** — the schemas carry one
  bullet redemption per bond.
* **Step-up / multi-rate coupon schedules** — `FixedRateBond.rate` is a
  single number on the wire.
* **Inflation-linked bonds** — no product endpoint in this catalog format.
* **Ex-coupon periods** — not on the wire.
* **Capped / floored / geared floaters** — the server builds its floaters
  with unit gearing and no caps/floors; the fields are not on the wire.
* **In-arrears floaters** — expressible on the wire (`in_arrears`), but the
  reference call path has not been validated for the convexity adjustment,
  so no case asserts it yet.
* **Seasoned floaters with historical fixings** — `IndexDef.fixings` exists
  on the wire and both sides apply them, but no catalog case pins the
  past-fixing path yet.

## Current coverage (FRA)

* Directions: the same 3x6 trade bought (long) and sold (short), pinning the
  position-type sign convention.
* Forward periods: 1x4, 3x6, 6x9 and 9x12 on Euribor 3M (the standard
  quarterly strip, including a weekend-rolled Modified Following maturity),
  plus a 6x12 on Euribor 6M (6-month accrual, index-tenor variation).
* Strike vs forward: clearly below (positive NPV), clearly above (negative
  NPV) and struck at the curve's implied forward (NPV within cents of zero).
* Conventions: euro-market Act/360 2-day-fixing indices on TARGET, and a
  GBP-style Act/365F same-day-fixing index on the UnitedKingdom calendar
  against a mildly inverted deposit curve.
* Curves: deposit-bootstrapped single-curve pricing and a multicurve case
  where the forward projects off the Euribor curve while the payoff
  discounts on a separate lower ESTR-style curve
  (discounting_curve != forwarding_curve).

## Planned / not yet covered (FRA)

These need reference-pricer or engine behaviour that is not there yet; they
are deliberately not half-implemented:

* **FRA valued on/after its fixing date (historical fixing)** —
  `IndexDef.fixings` exists on the wire and the server applies past fixings
  to its indices, but the reference index builder does not, so no case pins
  the fixed-rate path yet (same caveat as seasoned floaters in the Bonds
  family).
* **Trade-level convention overrides** — the FRA wire table carries its own
  `day_counter`, `calendar` and `business_day_convention` fields, but the
  server prices the FRA with the index's conventions and ignores these
  trade-level fields, so no case varies them independently of the index.

## Current coverage (Cap/Floor)

* Instruments: caps and floors on quarterly Euribor 3M and semiannual
  Euribor 6M legs (constant per-trade strike, single notional).
* Moneyness: strikes bracketing the ~3.1% forwards on both sides — 2% / 5%
  caps and 2% / 4% floors around a near-the-money 3.10% baseline — so the
  NPVs span intrinsic-dominated to pure time value.
* Vol / model pairings: Black on flat 20% and 30% lognormal optionlet vols,
  Bachelier on an 80bp normal vol, and shifted Black on a 15%
  shifted-lognormal vol with a 2% displacement.
* Tenors: 2Y short end, 5Y baseline and a 10Y cap priced out to the curve's
  10Y par quote.
* Conventions: trade-level Act/365F payment accrual against the index's
  Act/360 (day-count mismatch on the caplet coupons).
* Curves: single deposit+swap curve for projection and discounting, plus an
  OIS-discounted multicurve cap (forwarding != discounting).

## Planned / not yet covered (Cap/Floor)

These need reference-pricer or engine behaviour that is not there yet; they
are deliberately not half-implemented:

* **Collars** — the `CapFloorType` enum carries `Collar` on the wire, but
  the server rejects it ("Collar not yet supported - use separate Cap and
  Floor"), so no collar case exists.
* **Volatility term structures / smiles** — the optionlet vol wire spec only
  accepts a constant vol (the server validates `shape=Constant`); caplet
  vol term structures, strike-dependent surfaces and stripped caplet
  surfaces are not expressible.
* **Quote-referenced vols** — `IrVolBaseSpec.quote_id` lets the server
  resolve the constant vol from `pricing.quotes`, but the reference pricer
  reads the inline `constant_vol` only, so no case uses a vol quote id.
* **Seasoned caps with past fixings** — the first caplet of a seasoned cap
  needs a historical index fixing; the reference index builder does not
  apply `IndexDef.fixings` (same caveat as seasoned floaters/FRAs).
* **Amortizing notionals / per-caplet strike schedules** — the wire carries
  one notional and one strike per trade.
* **Digital caps/floors and capped-floored structured coupons** — not on
  the wire.

## Current coverage (Swaption)

* Exercise styles: European payer/receiver; a Bermudan with three annual
  exercise dates and an American with a 2-year exercise window, both priced
  on a Hull-White one-factor lattice with explicit parameters (a, sigma,
  tree steps identical on both sides).
* Moneyness: strikes bracketing the ~3.16% 1Y5Y forward — 2% deep in the
  money, near-the-money 3.10%, and 4.5% out of the money — so premiums span
  intrinsic-dominated to pure time value.
* Expiry / tenor grid: 1Y into 5Y baseline, 5Y into 5Y (long expiry) and
  2Y into 10Y (long underlying) on a curve bootstrapped out to the 15Y par
  quote.
* Settlement: physical delivery and cash settlement with the par-yield
  annuity method (settlement_method=ParYieldCurve), which produces a
  measurably different premium than physical on the same trade.
* Vol / model pairings: Black on a flat 20% lognormal vol, Bachelier on
  80bp and 70bp normal vols, shifted Black on a 15% shifted-lognormal vol
  with a 2% displacement, and Black on a 3x3 ATM vol matrix interpolated at
  the trade's expiry and swap length.
* Underlyings: vanilla fixed-vs-Euribor 6M swaps and a daily-compounded
  ESTR OIS on a curve bootstrapped from OIS par quotes.

## Planned / not yet covered (Swaption)

These need reference-pricer or engine behaviour that is not there yet; they
are deliberately not half-implemented:

* **Calibrated Hull-White models (param_mode=Calibrate)** — the server
  calibrates a and sigma to the quoted ATM matrix before pricing; the
  reference pricer would have to reproduce the whole calibration routine
  (helpers, optimizer, end criteria), so Bermudan/American cases here use
  explicit parameters only.
* **Smile cubes and SABR vol surfaces** — the wire supports 3D smile cubes
  (fixed-strike and spread-from-ATM), SABR parameter grids and in-server
  SABR calibration, but the Python reference only rebuilds constant vols
  and ATM matrices, so no catalog case prices off a smile yet.
* **Quote-referenced vols** — `IrVolBaseSpec.quote_id` and per-cell ATM
  matrix `quote_ids` resolve against `pricing.quotes` on the server; the
  reference pricer handles matrix quote ids but no catalog case exercises
  the quote indirection yet.
* **Collateralized cash settlement as a distinct value** — the wire and
  both pricers accept settlement_method=CollateralizedCashPrice, but with a
  single discount curve it prices identically to physical delivery, so a
  case would not pin anything beyond the enum plumbing.
* **Multicurve swaptions (forwarding != discounting)** — the server prices
  them, but the reference swaption pricer builds its Ibor index off the
  discounting curve, so an OIS-discounted swaption case cannot be
  reference-priced yet (the other families cover multicurve parity).
* **Swaptions on seasoned underlyings (past fixings)** — the reference
  index builder does not apply `IndexDef.fixings` (same caveat as seasoned
  floaters/FRAs/caps).

## Current coverage (CDS)

* Directions: the same 5Y 100bp contract buying and selling protection
  (protection-side sign flip on identical market data).
* Credit-curve builds: bootstrapped from par spread quotes (1Y 80bp to 10Y
  130bp, LogLinear survival probabilities) and a flat 2% hazard-rate curve
  (the no-quotes branch). All cases use the LogLinear credit-curve
  interpolator — the server intentionally rejects every other value.
* Coupons / upfront: 100bp running coupon at/around the par spreads, the
  500bp standard coupon (premium-leg dominated, deeply negative buyer NPV)
  and a trade-level 2% upfront with an explicit upfront settlement date.
* Recovery: 40% baseline and a 20% distressed-name assumption (recovery
  feeds both the par-spread bootstrap and the pricing engine on both sides).
* Maturities: 3Y (coupon above the short-end par spread, negative buyer
  NPV), the 5Y baseline and a 10Y priced to the long-end pillars.
* Engines: MidPoint baseline and the ISDA standard model with its default
  settings (Taylor numerical fix, half-day accrual bias, piecewise forwards)
  including ISDA-mode curve helpers.
* Schedules / day counts: the market-standard quarterly Act/360
  TwentiethIMM premium grid and an old-style semiannual Act/365F
  Forward-generated schedule.

## Planned / not yet covered (CDS)

These need reference-pricer or engine behaviour that is not there yet; they
are deliberately not half-implemented:

* **Credit curves bootstrapped from upfront quotes** — `CdsQuote` carries
  `quote_type=Upfront` on the wire and the server builds `UpfrontCdsHelper`
  instances, but the reference pricer's upfront-helper construction has not
  been validated against the server's (the upfront settlement lag argument
  is threaded differently by the two builders), so no case bootstraps from
  upfront quotes yet.
* **Quote-referenced credit spreads** — `CdsQuote.quote_id` resolves against
  `pricing.quotes` (`quote_type=Credit`) on the server and the reference
  supports it, but the catalog cases carry inline `quoted_par_spread`
  values; the quote indirection is only exercised by
  `tests/contract/cds_test.py` (loose tolerance) today.
* **Non-LogLinear credit-curve interpolators** — the server fails closed on
  every credit-curve interpolator except LogLinear (QuantLib's
  default-probability bootstrap is survival-log-linear only), so there is
  nothing to pin beyond the error contract.
* **ISDA engine convention variations** — the flat-forwards /
  no-accrual-bias / no-numerical-fix combinations are expressible on both
  sides, but only the standard settings are pinned so far.
* **Seasoned CDS (protection start / trade date in the past)** — no case
  values a contract mid-life yet; all cases start on the valuation date.

## Current coverage (Curves)

All Curves cases use `compare: "series"`: every point of every returned
series is checked element-wise against an independently built QuantLib curve
within 1e-9 (see "Comparison modes").

* Curve builds: a EUR deposit+swap bootstrap (1M/3M/6M deposits, 1Y-10Y par
  swaps), a EUR deposit+futures bootstrap (3M deposit plus a money-market
  futures contract, IMM start, price 96.50, pinning the FuturesRateHelper
  pillar), an ESTR OIS bootstrap (1Y/2Y/5Y/10Y OIS par quotes) — all
  LogLinear discount — and an explicit zero-rate curve (six continuous
  zero points, Linear interpolation, US government bond calendar).
* Measures: discount factors, zero rates (curve-day-counter continuous and
  a re-quoted Act/360 semiannually compounded variant), period forwards
  (3M and 6M tenors, simple compounding) and approximated instantaneous
  forwards (1-day epsilon, continuous), including all three measures
  aligned in a single query.
* Grids: market tenor grids on TARGET Modified Following and on the US
  government bond calendar; a calendar-less tenor grid (plain date
  arithmetic, weekend grid dates); a dense daily RangeGrid (182 calendar
  days); and a business-days-only daily RangeGrid over one year (TARGET
  weekend/holiday filter, ~256 points) whose first date sits on the curve
  reference date, pinning the server's clamp of at-or-before-reference
  zero-rate queries to reference+1.

## Planned / not yet covered (Curves)

These need reference behaviour that `build_curve_from_json` /
`bootstrap_curves_ql` in `tests/contract/ql_reference.py` do not have yet;
they are deliberately not half-implemented:

* **Non-LogLinear / non-Discount bootstrapped curves** — the server
  bootstraps every Interpolator x BootstrapTrait combination
  (Linear/BackwardFlat/ForwardFlat/LogCubic, ZeroYield/ForwardRate traits),
  but the reference always builds `PiecewiseLogLinearDiscount` for
  helper-based curves, so only LogLinear+Discount bootstraps can be
  series-compared today (explicit zero-rate curves honour their
  interpolator field: the Linear case is covered).
* **FRA / bond-helper pillars** — `FRAHelper` is not built by the reference
  curve builder; `BondHelper` is built but has never been validated at
  series precision. (`FutureHelper` pillars are now covered — see the
  deposit+futures bootstrap above.)
* **Non-zero swap-helper forward starts** — the server threads
  `fwd_start_days` into `SwapRateHelper`; the reference builder does not,
  so the curve cases keep `fwd_start_days: 0` (NPV families tolerate the
  tiny difference at 0.01, series cases at 1e-9 cannot).
* **Exogenous-discounting dependencies (`deps.discount_curve`)** — the
  reference ignores `deps` (same caveat as the IR-swaps family); the
  dependency wiring itself is shape-checked by
  `tests/contract/bootstrap_test.py`.
* **Multi-curve / multi-query requests** — `bootstrap_curves_ql` samples
  exactly one query per request; the multi-curve build path is covered by
  the contract suite.
* **Pillar-date parity** — the response's `pillar_dates` are not asserted
  yet, only the sampled series.
* **Inflation curve sampling** (`/bootstrap-inflation-curves`) — a separate
  future family.

## Current coverage (Calendars)

All Calendars cases use `compare: "exact"`: the returned date list (business
days / holidays) or single advanced date must equal the QuantLib reference
verbatim — no tolerance (see "Comparison modes"). Every case also pins the
server's calendar-enum mapping (`CalendarToQL`), which the reference mirrors
entry for entry, including the market variants QuantLib defaults to
(UnitedKingdom -> Settlement, plain UnitedStates -> Settlement, plus the
explicit NYSE / GovernmentBond / NERC variants).

* Business days: TARGET across the Easter 2025 holidays, the US government
  bond (SIFMA) calendar around a Friday July 4, the UnitedKingdom calendar
  across the Christmas-to-New-Year boundary with both endpoints excluded
  (include_start / include_end false), and Japan through Golden Week 2025
  (substitute-holiday rules).
* Holidays: the full TARGET 2025 holiday list (weekends excluded, the
  default), the NYSE closures for 2025 H1 (including the January 9 national
  day of mourning), and the UK festive season with include_weekends=true
  across a year boundary.
* Advance: 10 business days over Easter (Days unit); +1 month from a July
  month-end landing on a Sunday under both Following (rolls into September)
  and ModifiedFollowing (rolls back into August) on the identical input;
  +1 month from Jan 31 with end_of_month=true (month-end roll to the last
  business day of February); +2 weeks landing exactly on Independence Day
  under Preceding (holiday-forced adjustment, Weeks unit); and +1 year onto
  a Saturday under Following on the UK calendar (Years unit).

## Planned / not yet covered (Calendars)

These are expressible on the wire (or nearly so) but deliberately not
half-implemented:

* **Combined / joint calendars** — the `Calendar` enum has no joint-calendar
  value, so a TARGET+UK style union cannot be requested.
* **BespokeCalendar** — the enum value exists and the server maps it, but an
  empty bespoke calendar has no holiday content to pin (and no way to add
  holidays over the wire), so the reference deliberately rejects it.
* **Negative tenors (back-shifts) on advance** — `tenor_number` accepts
  negative values on the wire and the C++ parity suite pins one; no catalog
  case exercises it yet.
* **Sub-day tenor units** — the `TimeUnit` enum carries Hours through
  Microseconds, but `Calendar::advance` is date-resolution; nothing
  meaningful to pin.
* **NullCalendar / WeekendsOnly special calendars** — mapped by both sides
  but degenerate (no holidays / weekends only); not pinned yet.
* **Remaining business-day conventions on advance** —
  HalfMonthModifiedFollowing, ModifiedPreceding, Nearest and Unadjusted are
  on the wire; the catalog pins Following, ModifiedFollowing and Preceding
  so far.

## Current coverage (Inflation)

Both inflation swap products, priced on bootstrapped inflation curves and
compared as NPVs (tolerance 0.01). Note the QuantLib direction conventions
both sides implement: a ZCIIS "Payer" pays the **inflation** leg and
receives fixed, while a YYIIS "Payer" pays the **fixed** leg and receives
the YoY leg.

* Zero-coupon inflation swaps (ZCIIS): pay/receive inflation on the same 5Y
  trade (sign-flip pin); a 10Y contract on the curve's long end; CPI
  observation interpolation both Linear (within-month interpolation of the
  monthly fixings) and AsIndex (the non-interpolated index's month fixing
  used flat); a 6M trade-level observation lag against 3M-lagged curve
  helpers (base CPI read from different past fixings); and a curve built
  from explicit end-date helpers (July anniversaries, one Saturday date
  used verbatim) instead of tenor-relative ones.
* Year-on-year inflation swaps (YYIIS): pay/receive fixed on the same 5Y
  annual trade (sign-flip pin); semiannual schedules with split day counts
  (30/360 fixed vs Act/360 YoY); and a +25bp spread on the YoY leg. The
  YoY curve helpers discount on the request's nominal curve, and the YoY
  leg is priced with the same nominal-curve coupon pricer the server uses.
* Market data: EUR HICP-style monthly index, 2M availability lag, monthly
  CPI (and YoY-rate) fixings for June-December 2024, zero curve from ZCIIS
  quotes 1Y 2.00% to 10Y 2.35%, YoY curve from YoY quotes 1Y 2.00% to 7Y
  2.25%, EUR deposit+swap nominal discounting.

## Planned / not yet covered (Inflation)

These need reference-pricer or engine behaviour that is not there yet (or
pin nothing measurable); they are deliberately not half-implemented:

* **Ratio-based YoY indices (`underlying_zero_index_id`)** — the server
  derives a YoY index as the ratio of a zero-inflation index's fixings; the
  reference index builder does not build ratio indices yet.
* **Explicit start+end dated helpers** — the server accepts helper
  `start_date`+`end_date` pairs (a dated helper constructor), but the
  QuantLib Python bindings only expose the maturity-date helper
  constructors, so the reference rejects `start_date` instead of
  approximating it. End-date-only helpers are covered.
* **`adjust_observation_dates` / inflation calendar on ZCIIS** — on the
  wire and passed to QuantLib by both sides, but numerically inert in this
  QuantLib version (the CPI flow amount is computed from the maturity and
  observation lag regardless of the adjusted observation date), so no NPV
  case can pin it.
* **Quote-referenced inflation helpers (`quote_id`)** — resolved against
  `pricing.quotes` by both the server and the reference, but no catalog
  case exercises the indirection yet.
* **Seasonality** — the server builds every inflation curve with no
  seasonality adjustment (nothing on the wire), so there is nothing to
  vary.
* **The index spec's `interpolated` flag** — carried on the wire but never
  passed into the QuantLib index constructor by the server (QuantLib 1.41
  dropped that parameter; within-month behaviour is controlled by the
  per-observation `CPIInterpolationType` instead). The catalog cases set
  it `false` to match the index actually built.
* **Inflation curve sampling** (`/bootstrap-inflation-curves`) — a future
  `compare: "series"` addition, like the nominal Curves family.

## Current coverage (Equity)

European vanilla equity options priced on a Black-Scholes-Merton process
(spot quote + dividend-yield curve + risk-free discount curve + flat Black
vol) and compared as NPVs (tolerance 0.01). Both sides build the identical
process, payoff, exercise and engine, so parity is at machine precision.

* Moneyness: calls and puts at at-the-money (100 vs 100 spot), in-the-money
  (call at 80, put at 120) and out-of-the-money (call at 120, put at 80)
  strikes, spanning intrinsic-dominated to pure-time-value premiums.
* Dividends: a zero-dividend baseline, a 2.5% continuous dividend yield
  (a flat zero curve referenced by the underlying spec's
  `dividend_yield_curve_id`, feeding the process's dividend leg), and
  discrete cash dividends (`EquityUnderlyingSpec.discrete_dividends`: two
  2.50 cash dividends before expiry) priced with the escrowed-dividend
  analytic European engine (`AnalyticDividendEuropeanEngine` over a
  `FixedDividend` schedule) layered on top of the continuous curve.
* Vol / expiry: flat 20% and 35% Black vols; 3M, 1Y and 2Y expiries.
* Engine: the analytic European (Black-Scholes-Merton) engine
  (`model_type=BlackScholesAnalytic`). See the planned list for why the
  binomial engine has no case yet.
* Trade mechanics: quantity scaling (the response NPV is per-option NPV
  times the trade's quantity; a 100-option case pins it).

## Planned / not yet covered (Equity)

These need server or reference behaviour that is not there yet; they are
deliberately not half-implemented:

* **American and Bermudan exercise** — `EquityAmericanExercise` /
  `EquityBermudanExercise` are on the wire, but the server's parser rejects
  everything except `EquityEuropeanExercise`, so there is nothing to pin
  beyond the error contract.
* **Digital payoffs (cash-or-nothing / asset-or-nothing)** — on the wire
  (`EquityCashOrNothingPayoff`, `EquityAssetOrNothingPayoff`) but the
  server's parser accepts only `EquityPlainVanillaPayoff`.
* **Binomial CRR engine (`model_type=BinomialCRR`)** — the reference
  supports it (it builds the CRR lattice with the model spec's own
  `binomial_steps`, and a manual check showed the NPVs agree at machine
  precision), but the server's binomial response is not parseable JSON:
  the binomial engine does not implement vega/rho, the server reports them
  as NaN, and the HTTP gateway serializes them as the literal token `nan`,
  which strict JSON parsers (including the test client's
  `response.json()`) reject. A case can be added once NaN greeks serialize
  as valid JSON (e.g. `null`).
* **Barrier features** — the server prices continuous-monitoring barrier
  options end to end (analytic barrier engine), but the reference pricer
  does not build barrier options yet, so no catalog case asserts them.
* **Non-constant vol surfaces** — the wire supports term/smile/price-based
  Black vol surfaces, but the reference rebuilds constant vols only; the
  equity cases keep `shape=Constant`.
* **Quote-referenced vols** — `quote_id` on the vol spec resolves against
  `pricing.quotes` on the server; the reference reads the inline
  `constant_vol` only (same caveat as the Cap/Floor family).
* **Cash settlement as a distinct value** — `settlement` is carried and
  echoed by the server but does not change the price, so a Cash case would
  pin only the enum plumbing.

## Current coverage (Vol / Calibration)

Three products: `/sample-vol-surfaces` (compare="series", every returned vol
checked within 1e-9), `/calibrate-swaption-model` and
`/calibrate-swaption-vol` (compare="fields", every calibrated parameter and
fit statistic checked within 1e-7 — see the tolerance rationale under
"Comparison modes"). All calibration cases are deliberately
well-conditioned; the measured server-vs-reference gaps are below 3e-12.

* Vol-surface sampling: a lognormal ATM swaption matrix sampled on an
  off-pillar expiry/tenor cube (genuine bilinear interpolation in option
  time and swap length, with exercise dates, spot-lagged swap starts and
  fixed-leg schedule ends regenerated from the swap index conventions on
  both sides); a normal ATM matrix queried in ExpirySlice mode (slice
  selectors + fixed slice strike); a constant swaption vol over a
  2x2x3 strike cube (pins the expiry-major/tenor/strike flattening); a
  constant 80bp normal optionlet surface on a tenor grid; and an equity
  Black term-vol curve (BlackVarianceCurve) sampled before the first pillar
  and between pillars (genuine variance interpolation).
* Hull-White calibration: a two-parameter round-trip that recovers
  a=0.05/sigma=0.008 from an ATM matrix generated by that very model (sharp
  minimum, ~1e-9 fit RMSE); a sigma-only fit (calibrate_a=false, via the
  request-level calibration override) to a market-style matrix no
  Hull-White model fits exactly (nonzero irreducible RMSE, matrix fallback
  grid); and a sigma-only fit to a flat 80bp normal vol with an explicit
  calibration grid (the Normal/price-error helper branch).
* SABR cube calibration: a 3-strike beta-fixed exact fit (3 free parameters
  per node, per-node RMSE 0); a 5-strike overdetermined round-trip whose
  smiles were generated from known interior SABR parameters at the curve's
  own forwards (the calibrator must recover them); and the same round-trip
  with vega_weighted_smile_fit=true. Compared fields: per-node forwards,
  alpha/beta/rho/nu grids, Hagan ATM vols and the per-node/overall fit
  RMSEs.

## Deferred / not yet covered (Vol / Calibration)

Honesty matters most in this family — some paths are deliberately NOT
cataloged because a faithful independent reference either does not exist yet
or cannot agree with the server more tightly than the effect being tested:

* **Free-(a, sigma) Hull-White calibration against vols the model cannot
  fit** — measured, not hypothetical: on a market-style ATM matrix the
  objective has a flat valley in `a`, and the server binary and the Python
  QuantLib wheel (same version, different compiler builds) stop at
  different, equally valid points — observed `a` gap ~1.9e-5 with objective
  values matching to 0.03%, while the same request with `a` held fixed
  agrees to 1e-13. A tolerance loose enough to absorb that valley would
  also hide real regressions, so only well-conditioned two-parameter cases
  (the round-trip) and sigma-only cases are cataloged.
* **Boundary-pinned SABR fits** — smiles that drive rho to its bound
  (rho -> 1) showed cross-build gaps up to ~1.6e-6 on the pinned nodes
  (same flat-direction effect); the cataloged smiles keep parameters well
  interior. A vega-weighted fit on such ill-fitting data is additionally
  rejected by QuantLib's global error tolerance on the server.
* **Smile-cube / SABR surface sampling** (`/sample-vol-surfaces` on
  SmileCube3D and SabrParams/SabrCalibrate surfaces, SpreadFromATM strike
  axes) — the reference rebuilds constant, ATM-matrix and equity term
  structures only; the forward-aware cube reconstruction exists for
  `/calibrate-swaption-vol` but wiring it through the sampling grid logic
  (per-mode slices, spread-axis translation) is not validated yet, same
  deferral as the Swaption pricing family's smile cases.
* **SmileSlice / TermSlice output modes and RangeGrid vol grids** — on the
  wire and served, but the reference implements Cube + ExpirySlice on
  TenorGrids only; slices/grids beyond that raise in the reference rather
  than approximating the evaluator's stepping loop untested.
* **Quote-referenced vols** (`quote_id` on vol bases / matrix cells) — the
  server resolves them against `pricing.quotes`; the reference deliberately
  raises (same caveat as the Cap/Floor and Equity families).
* **Shifted-lognormal SABR calibration** (displacement != 0) — expressible
  on the wire; the reference restricts itself to the displacement-free
  lognormal path it has validated.
* **OIS-underlying vol surfaces** (`swap_index_id` of kind OisSwapIndex) —
  the server computes OIS ATM forwards for sampling, but SABR calibrate
  rejects OIS swap indices ("not supported in v1") and the reference only
  builds the Ibor path.
* **The diagnostics `expiries`/`tenors` period fields** — a genuine server
  bug found while building this family: the diagnostics builder casts
  QuantLib's TimeUnit enum (Days=0, Weeks=1, Months=2, Years=3) straight
  into the schema's alphabetical TimeUnit enum (Days=0, Hours=1,
  Microseconds=2, Milliseconds=3, ...), so a 1Y expiry serializes as
  `{"n": 1, "unit": "Milliseconds"}` in every SwaptionVolDiagnostics block
  (`src/market/swaption_vol_diagnostics.cpp`, `writePeriod`). The catalog
  cases compare the numeric grids only; the fields can be asserted once the
  mapper uses the proper enum conversion.

## Planned / not yet covered (IR Swaps)

These need reference-pricer features that `ql_reference.py` does not have
yet; they are deliberately not half-implemented:

* **CMS legs** — the server prices vanilla swaps with a CMS leg, but there is
  no Python reference for CMS coupon pricers.
* **Amortizing / step-up notionals** — the swap schemas carry one notional
  per leg today.
* **Cross-currency swaps** — no product endpoint yet.
* **Exogenous-discounting bootstrap (`deps.discount_curve`) parity** — the
  server bootstraps projection curves discounted on another curve (see
  `examples/data/vanilla_swap_multicurve_request.json`), but
  `build_curve_from_json` in the Python reference ignores `deps`, so such
  requests cannot be reference-priced yet. The multicurve cases here use
  independently bootstrapped curves instead.
* **Averaged (non-compounded) OIS legs, lookback/lockout variations** — the
  schema and server support them; the reference call path has not been
  validated for them.
* **In-arrears floating legs, gearings ≠ 1** — untested in the reference.
* **Other families**: Bonds, FRAs, caps/floors, swaptions, CDS,
  bootstrapped-curve sampling (Curves), the calendar date utilities
  (Calendars), the inflation swaps (Inflation), the equity options
  (Equity) and the vol-surface sampling / calibration endpoints
  (Vol / Calibration) are covered above.
