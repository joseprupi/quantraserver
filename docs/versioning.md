# Versioning Policy

This repository uses Semantic Versioning: `MAJOR.MINOR.PATCH`.

## Rules

- `MAJOR`: breaking API, schema, or contract changes
- `MINOR`: backward-compatible features and endpoint additions
- `PATCH`: backward-compatible fixes and internal improvements

While the project is in `0.x` (pre-stable), semver permits breaking changes on
a `MINOR` bump; each one is documented below. `1.0.0` is reserved for the
first release that promises wire stability.

## Contract Discipline

- OpenAPI is treated as an API contract
- breaking contract changes require a major version bump
- non-breaking additions require a minor version bump
- fixes and docs-only updates require a patch bump

## Release Flow

1. Merge changes into `master`
2. Bump `VERSION`
3. Create tag `vX.Y.Z`
4. Publish release notes

## Version 0.12.0 (October 2026) — Structured notes on index baskets (Monte Carlo)

`v0.12.0` is a backward-compatible MINOR release: one new endpoint, one new
market-data block and four new enums. Nothing that existed in 0.11.0
changes shape, value or status code; a 0.11.0 request is a valid 0.12.0
request with the same response. QuantLib stays at 1.44 (no numerical
change to any existing product).

**New endpoint: `POST /price-structured-note`** (gRPC `PriceStructuredNote`,
request `PriceStructuredNoteRequest`, response `PriceStructuredNoteResponse`).
Notes on baskets of equity indices priced by Monte Carlo through one
declarative payoff: a strip of `ObservationEvent`s (each with an optional
`autocall_trigger`, an optional `coupon_trigger` and a `coupon`; both
triggers absent = an accrual-only period), a `CouponRule` (`memory`,
`pay_only_on_call`), a `CapitalProtection` block (barrier, `European` or
`DailyClose` monitoring, geared put via `put_strike` / `participation`,
`knocked_in` for a seasoned daily barrier), an optional
`UpsideParticipation`, a `performance_rule` (`WorstOf` / `BestOf` /
`Average`), a funding spread over the note's discounting curve and a
`MonteCarloSpec` (paths, seed, `Sobol` or `PseudoRandom`, antithetic,
Brownian bridge, `steps_per_year`). Classic / snowball autocallables, low
hurdles, step-downs, Phoenix coupons with or without memory, reverse
convertibles, bonus and twin-win shapes are configurations of these fields,
never products of their own. Underlyings in another currency than the note
are quanto'd through a `QuantoSpec` (local rate curve, FX spot, FX vol,
underlying / FX correlation) on QuantLib's `QuantoTermStructure`; past
observations are resolved from `historical_levels` before anything is
simulated, and a note the past already decided (`Called` / `Matured`) is
priced from its known payment without simulation.

**New market-data block: `pricing.correlations`** (`CorrelationMarketData`
with `CorrelationMatrixSpec {id, labels, values}`), validated at parse
(symmetric, unit diagonal, entries in `[-1, 1]`, positive semi-definite —
a `400` naming the pair or the smallest eigenvalue otherwise) and shared
by any basket product. Additive on `Pricing`; requests without it are
unchanged. New enums in `enums.fbs`: `BasketPerformanceRule`,
`CapitalBarrierMonitoring`, `RandomSequence`, `StructuredNoteStatus`.

**Response.** `npv`, `npv_pct_of_notional`, `standard_error`, `paths`,
`seed`, `status`; `legs` (`bond_redemption`, `coupons`, `capital_loss`,
each with its standard error, summing to the headline);
`event_probabilities` (`p_call` / `p_coupon` per simulated event);
`p_barrier_breach`; `expected_life_years`; and always the `terms` echo
(every booked field with defaults filled and each event flagged
`accrual_only` / `final` / `resolved_from_history`), the `seasoning`
resolution (per past event the levels, performances, outcomes, coupon paid
and memory carried) and the `model` echo (vol shape and local-vol flag per
underlying, quanto drift, grid, sequence dimension) — a mis-read factsheet
is visible before the price is trusted.

**Opt-in blocks (request flags).** `greeks` = bump-and-reprice
sensitivities on common random numbers (every bumped run on the same seed,
paths, grid and sequence): per underlying `delta` (per +1 % spot, central),
`gamma`, `vega` (per vol point, the Black vol rebuilt with every value
shifted; it also enters the quanto drift), `correlation_sensitivity` (per
+0.01 on every off-diagonal entry, the bumped matrix repaired by eigenvalue
clipping when it is no longer positive semi-definite — `correlation_bump_repaired`
and the matrix used are returned), `funding_dv01` and `rho` (per bp; the
rate bump moves both the processes' drift and the discounting), each with
the standard error of its per-path difference; cost `1 + 2N + N + 3`
simulations. `diagnostics` = `market` (the market as used: forwards,
implied yields and rates, ATM and barrier vols, quanto drift, discount
factors, the correlation matrix), `decomposition` (PV per event, survival /
breach / loss-given-breach), `checks` (running means, the zero-vol forward
walk, the correlation-one and correlation-zero limit prices, the bond-only
value), `distribution` (basket-performance and payout quantiles, a payout
histogram, `sample_paths` individual paths), `timing` (milliseconds per
stage). Every block is computed from the same paths as the price.

**Examples.** `examples/data/structured_note/`: the two IDAD factsheet
transcriptions (BNP Paribas Quad Index Classic Autocall June 2025,
XS2741509801; Santander Quad Index Low Hurdle Autocall August 2025,
XS3109492564) with PLACEHOLDER market data (the README there maps every
factsheet line to a field and lists every placeholder), the BNP request
with every diagnostic flag on, the BNP request with `greeks` on, a
single-underlying reverse convertible and a synthetic two-asset basket.

**Verification.** Three layers: (1) C++ collapse tests against closed
forms — a coupon-only event is a cash-or-nothing digital
(`AnalyticEuropeanEngine`), capital-only is a put plus a digital put, a
two-asset worst-of is the bivariate normal, the quanto forward is
`S exp((r_local - q - rho sigma_S sigma_FX) T)`, zero vol reproduces the
snowball arithmetic, the Greeks of the digital match the analytic engine's
delta / gamma / vega, the correlation sensitivity of the worst-of digital
matches the bivariate normal at `rho + 0.01`, `rho` and `funding_dv01` of a
bond-like note match `sum CF_i DF_i (exp(-b t_i) - 1) / b` exactly; (2) a
Python QuantLib reference (`tests/contract/ql_reference.py`,
`structured_note_ql`) that draws the engine's own random stream
(`SobolBrownianBridgeRsg`, Sobol JoeKuoD7 or Mersenne Twister, time-major,
antithetic pairs averaged into one sample) and reproduces the market
set-up, the grid, the walk, every diagnostic block and every Greek from the
request JSON — the price, its standard error, the buckets, the
probabilities, the diagnostic blocks and the Greeks agree with the server
to the 12 decimals the gateway prints on the two factsheet requests and
every synthetic case (flat, term and surface vol, quanto, Phoenix + daily
barrier, pseudo-random, Sobol without bridge, correlation repair); (3) the
three example files as functional-catalog cases (family "Structured
Notes") plus a convergence test (standard error ∝ 1 / sqrt(paths)).

**Known limits** (`docs/http-api.md`, "Structured notes"): composite
(non-quanto) foreign underlyings are not supported (the FX rate is not
simulated); the funding spread is flat; best-of / average baskets have no
weights; issuer-callable notes, lookbacks, coupon floors, FX-settled notes
and scripted payoffs are out of scope; paths are not shared across the
notes of one request; the correlation sensitivity is one uniform
off-diagonal bump (no per-pair Greeks); the `DailyClose` barrier is tested
on the simulated closes only (no continuity correction). A 4-underlying
note with `greeks` on is 16 simulations (about 21 s of engine time at
65 536 paths, beyond the JSON gateway's 10 s RPC deadline; the engine
checks the request budget before every extra run): size the paths of a
Greeks request to the deadline (the shipped example uses 16 384).

**Migration.** None: nothing existing changed. The OpenAPI document gains
the endpoint and its components; `/meta` reports `0.12.0`.

## Version 0.11.0 (October 2026) — QuantLib 1.41 → 1.44

`v0.11.0` is a backward-compatible MINOR release with **no API change**: every
endpoint, request and response schema, status code and error body is the same
as in 0.10.0. The only change is the pricing library underneath — the engine is
rebuilt on QuantLib 1.44 (from 1.41) — and `/meta` reports `quantlib: 1.44`.
Some results move; every numerical change, with its upstream cause and the
requests it affects, is documented in
[`docs/quantlib-upgrades/1.41-to-1.44.md`](quantlib-upgrades/1.41-to-1.44.md).
The cross-version diff tool (`tools/version_diff/`, see its README) replays
your own requests against a 0.10.0 and a 0.11.0 server and lists every field
that differs, so the effect on your book can be measured before adopting the
release.

**What moves (summary; the page has the numbers).** The comparison corpus
was 291 requests (the 208 functional-catalog cases plus 83 targeted probes),
sent to a 1.41 and a 1.44 server built from the same source: 215 unchanged,
2 noise-only, 72 changed, 2 status changes; 28 of the 208 catalog values
changed. The changes cluster as: cross-currency swaps and the collateral
curves built from `CrossCcyBasisHelper` pillars (a compounded overnight leg on
a schedule calendar other than the index calendar under-accrued across index
holidays on 1.41; 1.44 prices it at par, leg NPVs move by about 3 bp of
notional); CDS on bootstrapped credit curves with `settlement_days: 0` and a
non-`CDS` date rule (helper trade-date convention, up to 1.4% of NPV);
overnight coupons with `lookback_days > 0` and `apply_observation_shift`
(1.41 under-stated them); inflation swaps and caps/floors (helper pillar
placement; ZCIIS fixed leg on contract dates); joint Hull-White `a`/`sigma`
calibration (a different point on a flat objective); American options whose
exercise window starts after the valuation date (1.41 ignored the start);
bond yield/duration/convexity with `bond_pricing_details` (≤ 6e-9 in yield);
holiday lists of eleven calendars. Two requests that returned 200 now return
422: a swap valued on a holiday of the helpers' calendar with
`DepositHelper.fixing_days > 0` (a historical fixing the API cannot supply;
value on a business day or use `fixing_days: 0`), and an overnight coupon
whose `payment_convention` / `payment_calendar` put the payment before the
accrual end (1.41 returned `inf`). Full detail, worked numbers and remedies:
[`quantlib-upgrades/1.41-to-1.44.md`](quantlib-upgrades/1.41-to-1.44.md).

### Quantra version → QuantLib version

| Quantra | QuantLib |
|---|---|
| 0.1.0 – 0.10.0 | 1.41 |
| 0.11.0 | 1.44 |

Release notes for the upgrades this project tracked (1.42, 1.43 and 1.44) are
summarised on the upgrade page above; a QuantLib upgrade ships as its own
release containing nothing else.

## Version 0.10.0 (October 2026) — one swap leg model for every swap

`v0.10.0` is a **wire-breaking** MINOR release under the 0.x rules: the four
swap endpoints and the swaption request change shape. Numbers do not change:
every 0.9.0 swap and swaption request, converted with the shipped tool,
returns the same values (11,430 of 11,478 captured values identical; the
remaining 48 are the CMS `fair_rate` and `fair_spread`, which 0.9.0 reported
as `0.0` and 0.10.0 now computes). A converter and a field-by-field guide
are included.

### Changed — the `SwapLeg` model

Every swap endpoint (`price-vanilla-swap`, `price-ois-swap`,
`price-basis-swap`, `price-cross-currency-swap`) and the swaption's
underlying now describe each leg with one shared table, `SwapLeg`:

```
SwapLeg {
  pay_receive          Pay | Receive          (replaces swap_type; one leg Pay, the other Receive)
  currency             ISO code               (must match a floating index's IndexDef.currency)
  notional | notionals[]
  schedule, day_counter, payment_convention
  payment_calendar, payment_lag               (optional; required on both legs of an OIS)
  coupons_type + coupons                      FixedCoupons { rate }
                                              IborCoupons { index, spread, fixing_days, in_arrears }
                                              OvernightCoupons { index, spread, averaging_method, lookback_days,
                                                                 lockout_days, apply_observation_shift,
                                                                 telescopic_value_dates }
                                              CmsCoupons { swap_index_id, swap_tenor, swaption_vol_id, gear,
                                                           spread, cap, floor, pricer, fixing_days }
  discounting_curve, forwarding_curve         (curves live on the leg, not on the request wrapper)
  initial_exchange, final_exchange            (required on the cross-currency endpoint;
                                               absent = false on the single-currency endpoints)
}
```

The swap tables keep their names and their role-named fields, so a request
still reads as the instrument it is: `VanillaSwap { fixed_leg, floating_leg }`
(a CMS swap is a vanilla swap whose floating leg carries `CmsCoupons`; the
`cms_leg` field is gone), `OisSwap { fixed_leg, overnight_leg }`,
`BasisSwap { leg1, leg2 }`, `CrossCurrencySwap { leg1, leg2 }`. Each endpoint
checks that the leg in a role carries the coupon kind the role requires
(`400` naming the field), that the two legs are one `Pay` and one `Receive`,
and — on the single-currency endpoints — that both legs share one currency.
The request wrappers carry the swap only: `discounting_curve`,
`forwarding_curve`, `forwarding_curve_leg1/2` and `swap_type` are gone.

Responses report `legs:[SwapLegResult]` in field order (`fixed_leg` then
`floating_leg` / `overnight_leg`; `leg1` then `leg2`), each with `npv` in the
leg's currency signed from the reported party's view, `npv_reporting`, and
`flows` with `include_flows` (each flow tagged `Coupon` or
`NotionalExchange`). The per-leg fields `fixed_leg_npv`, `floating_leg_npv`,
`overnight_leg_npv`, `leg1_npv`, `leg2_npv`, the `*_bps` fields and the
per-leg `*_flows` lists are removed. One rule on every swap endpoint:
`fair_rate` is present iff exactly one leg carries `FixedCoupons` and
`fair_spread` iff a leg carries `IborCoupons`, `OvernightCoupons` or
`CmsCoupons` — on vanilla and OIS the floating / overnight leg's spread
(QuantLib's `fairSpread`, as in 0.9.0), on basis the leg that carries the
request's spread (both zero → `leg2`), on cross-currency the floating leg of
a fixed/float swap (new: 0.9.0 reported it only for float/float) or `leg2`
of a float/float one. Both are `= null` optionals; basis
`fair_spread_leg1/2` are removed. The vanilla `used_cms_*` diagnostics are
present only for a CMS leg.

Swaptions: `PriceSwaption.discounting_curve` / `forwarding_curve` are gone;
the underlying's legs carry the curves, and both legs must name the same
`discounting_curve`.

### Fixed

- CMS swaps report their real `fair_rate` and `fair_spread` (0.9.0 wrote
  `0.0` for both; the spread is solved on the CMS coupons' annuity with a
  `cap` / `floor` held at its strike).
- An overnight index under `IborCoupons` is rejected with a `400` naming
  `OvernightCoupons` on every swap endpoint; 0.9.0's `price-vanilla-swap` /
  `price-basis-swap` silently priced it as daily IBOR coupons (#150).
- `IborCoupons.fixing_days` is optional: absent means the index's own fixing
  days (0.9.0 carried a literal default of 2 that the vanilla path ignored).

### Migrating

- `tools/migrate_swap_requests/migrate_swap_requests.py` rewrites 0.9.0
  vanilla, OIS, basis, cross-currency and swaption request JSON into the
  0.10.0 shape (`--out DIR` or `--in-place`; idempotent; refuses anything it
  cannot map rather than guessing). `--check OLD_URL NEW_URL` prices each
  original against a 0.9.0 server and its conversion against a 0.10.0 server
  and compares `npv`, fair quantities and per-leg NPVs.
- The field-by-field tables, per endpoint, are in
  `tools/migrate_swap_requests/README.md`; the leg model is documented in
  `docs/http-api.md` § "Swap legs".
- Responses: read `legs[i].npv` where you read `<leg>_npv`; `legs[i].flows`
  where you read `<leg>_flows`; `fair_spread` where you read
  `fair_spread_leg1/2`.

Unchanged: every other endpoint, including `price-fx-forward`, the
zero-coupon and inflation swaps, bonds, caps/floors, CDS, equity, curves,
calendars. Still on QuantLib 1.41.

---

## Version 0.9.0 (October 2026) — multi-currency market, FX forwards and cross-currency swaps

`v0.9.0` is a backward-compatible MINOR release: every request that worked on
0.8.x works unchanged and returns the same numbers. It adds FX market data,
two FX-implied curve helpers and two products.

### Added

- **FX spot market data.** `pricing.fx.spots[]` (`FxSpotSpec`): a currency
  pair as two ISO codes (`base_currency`, `quote_currency`), the spot `rate`
  (or a `quote_id` naming a quote with the new `quote_type` `Fx`), the spot lag
  (`spot_days`), the settlement calendars (joined) and a business-day
  convention. The rate is the market spot for delivery on the spot date; where
  the engine needs an as-of rate it uses `S · DF_quote(spot) / DF_base(spot)`
  from the two discount curves. Spot lag and calendars are per-pair facts the
  request states; the engine keeps no convention table.
- **FX swap helper** (`FxSwapHelper`, QuantLib `FxSwapRateHelper`): bootstraps
  the discount curve of the non-collateral currency of a pair from FX forward
  points, given the spot and the collateral currency's discount curve.
- **Cross-currency basis helper** (`CrossCcyBasisHelper`, QuantLib
  `ConstNotionalCrossCurrencyBasisSwapRateHelper`): extends the same curve
  from constant-notional basis swaps between two floating legs (IBOR or
  overnight), with explicit forwarding and discount curves per leg.
  Together the two helpers build the classic collateral-adjusted curve
  (FX swaps to 1Y, basis swaps beyond); both take part in the curve
  dependency ordering and in the curve cache key.
- **FX forward** (`POST /price-fx-forward`): NPV in a chosen reporting
  currency (base or quote), fair forward rate, forward points, per-currency
  leg PVs, spot and adjusted maturity dates. Spot-date and maturity rules are
  QuantLib's FX-swap-helper rules, so a forward struck on an FX swap pillar
  reprices spot + points.
- **Cross-currency swap** (`POST /price-cross-currency-swap`): two legs in
  two currencies, each discounted on its own curve, with explicit
  `initial_exchange` / `final_exchange` flags, converted to the reporting
  currency at the as-of rate. Returns the total NPV, each leg's NPV in its
  own currency and in the reporting currency (`legs[]`, signed from the
  reported party's view), `fair_rate` (exactly one fixed leg) or
  `fair_spread` (no fixed leg), the spot used, the as-of rate and, with
  `include_flows`, each leg's flows tagged `Coupon` or `NotionalExchange`
  (new `SwapLegFlow.kind`, absent on the other swap endpoints).
- **`SwapLeg`, the shared swap-leg table** (`swap_leg.fbs`): `pay_receive`
  (new `PayReceive` enum, per leg, replacing a swap-level side), `currency`,
  `coupons` (union `SwapCoupons` of `FixedCoupons`, `IborCoupons`,
  `OvernightCoupons`, `CmsCoupons`; written in JSON as `coupons_type` +
  `coupons`), `discounting_curve`, `forwarding_curve`, `initial_exchange`,
  `final_exchange`; `SwapLegResult` (`npv`, `npv_reporting`, `flows`). The
  cross-currency swap is its first user; from 0.10.0 the vanilla, OIS and
  basis swaps keep their endpoints and take `SwapLeg` legs too. An
  `OvernightCoupons` leg is built by the same code as `price-ois-swap`'s
  overnight leg; `IborCoupons` on an overnight index (and the reverse) is a
  `400` naming the table to use; `CmsCoupons` is a `400` on the
  cross-currency swap in this version.
- `FxDirection`, `SwapFlowKind` and `PayReceive` enums; `fx_forward.fbs`,
  `swap_leg.fbs`, `cross_currency_swap.fbs` and their request/response
  schemas; Python client `price_fx_forwards` / `price_cross_currency_swaps`
  (exports `SwapLegT`, `SwapCoupons`, `PayReceive`, `OvernightCouponsT`,
  `RateAveragingType`); OpenAPI "FX" tag.

### Changed

- `FxSwapHelper` and `CrossCcyBasisHelper` tables are redefined. Both were
  schema-only and returned `501` with any payload, so no working request is
  affected. `HelperDependencies.projection_curve_2` and
  `HelperDependencies.fx_spot_quote_id`, which no helper ever read, are
  removed (last two slots; earlier fields keep their positions).
- **Coupon tables renamed** (generated class names only; every field name,
  the JSON and the binary wire of the existing endpoints are unchanged):
  `SwapFixedLeg` -> `FixedCoupons`, `SwapFloatingLeg` -> `IborCoupons`,
  `OisFloatingLeg` -> `OvernightCoupons`, `SwapCmsLeg` -> `CmsCoupons`. C++
  and Python object-API users rename the types (`FixedCouponsT`,
  `IborCouponsBuilder`, ...); JSON users change nothing. Error messages of
  the existing endpoints keep their wording.
- The vanilla, OIS, basis and cross-currency swap evaluators share one
  swap-leg builder (the OIS legs are now built through it and priced as a
  generic `Swap`, exactly QuantLib's `OvernightIndexedSwap` chain). Results
  of the existing endpoints are unchanged (pinned by the parity suites and
  a byte-for-byte comparison of every catalog case); a basis-swap IBOR leg
  marked `in_arrears` now carries the explicit zero-volatility coupon pricer,
  which is a no-op for in-advance coupons.

### Rejections added (all `400` naming the field, `404` for unknown ids)

See `docs/http-api.md` sections "FX spot market data", "FX swap helper",
"Cross-currency basis helper", "FX forward" and "Cross-currency swap" for the
complete lists: malformed pairs and currencies, `rate`/`quote_id` or
`spread`/`quote_id` both or neither, non-`Fx` quote behind a spot, missing
spot lag / calendars / convention, collateral currency outside the pair,
both or neither discount curve on a basis helper, mark-to-market reset
requested, a forward or swap whose currencies do not match the spot (no
implicit pair inversion), a reporting currency outside the pair, two legs in
one currency, both legs on the same side, a coupon table that does not match
its index kind, `CmsCoupons`, per-period notionals.

### Still not supported (planned)

Mark-to-market resetting cross-currency swaps, FX sensitivities (spot delta,
curve rebump) for the two products, FX options, reporting in a third
currency, `TenorBasisSwapHelper` (still `501`).

---

## Version 0.8.2 (October 2026) — rebump greeks bump curves once

`v0.8.2` is a patch release: no schema change, no change to any plain pricing
result.

### Fixes

- **Rebump greeks shift each curve quote exactly once.** With
  `swaption_pricing_rebump`, the engine rebuilds the curves with every quote
  moved by ±1bp and reprices. A curve pillar written with `quote_id` received
  that shift twice: once when the request's `quotes[]` were loaded and again
  when the pillar resolved its quote. Curves written entirely with `quote_id`
  pillars therefore reported a rebump `dv01` about twice and a `gamma` about
  four times the true value, and a curve mixing inline and `quote_id` pillars
  got a lopsided shock (only the `quote_id` pillars moved 2bp). `vega` and
  `theta`, plain pricing, `swaption_pricing_details` and curves written with
  inline rates were never affected. Every pillar is now shifted once, so the
  two spellings of the same market produce identical greeks.

---

## Version 0.8.1 (October 2026) — request isolation and input guards

`v0.8.1` is a patch release: no schema change, no change to any correctly
formed request's results.

### Fixes

- **Index fixings no longer leak between requests.** QuantLib keeps past
  fixings in a process-global store, and the server only cleared an index's
  fixings when the request supplied `fixings`. A request that supplied none
  inherited the previous request's fixings on that worker and could price with
  them. The store is now cleared at the start and end of every request.
  Requests that relied on this by accident (omitting fixings a seasoned trade
  needs) will now get the missing-fixing error they should always have had.
- **Negative day counts are rejected.** `fixing_days`, `settlement_days`,
  `cash_settlement_days` and `spot_days` across indices, bonds, deposit / FRA /
  OIS / bond helpers, swap floating legs, CDS and constant optionlet volatility
  used to be passed to QuantLib's unsigned parameters, so a negative value
  became a day count of about four billion (a 500, or a silently wrong date).
  They now return a `400` naming the field.

### Internal

- Dead code removed (unused flow builders, unreachable swaption helpers, a
  duplicate guard); global `using namespace` directives removed from headers.
- A `.clang-format` now encodes the existing code style and was applied once
  to all hand-written C++ sources; `scripts/format.sh` reproduces it.

## Version 0.8.0 (October 2026) — response fixes: zero values kept, swaption greeks corrected

`v0.8.0` fixes three response defects. Requests are unchanged; what changes is
what comes back, so this is a MINOR bump under the 0.x rules. Every change is
listed here so a client can tell what to expect.

### Zero-valued fields are no longer dropped

A response number that was exactly zero (`npv`, `fair_spread`, a holiday
`count`, `theta`, …) used to disappear from the JSON because the FlatBuffer did
not store scalars equal to their schema default. Responses now carry every
scalar, so a computed zero is returned as `0` / `0.0`. Fields declared optional
(`= null`) are unaffected: they are still present only when set.

Clients that treated a missing field as zero are unaffected. Clients that
treated a missing field as "not applicable" should rely on the optional fields,
which keep that meaning.

### Swaption `swaption_pricing_details` greeks: units corrected

The analytic greeks were written in the raw units of QuantLib's Black and
Bachelier calculators, and `theta` was computed with the swap annuity passed
where the calculator expects a discount factor, which made the number
meaningless. All details greeks are now in the same units as the rebump
greeks:

| Field | Unit (both modes) | Change from 0.7.0 (details mode) |
| --- | --- | --- |
| `delta` | currency per 1 bp move of the forward | divided by 10,000 |
| `gamma` | currency per bp² | divided by 1e8 |
| `vega` | currency per 1 bp of volatility | divided by 10,000 |
| `theta` | currency per calendar day | recomputed: time decay of the option premium with forward, volatility and annuity held fixed |
| `dv01` | currency per 1 bp | unchanged (equals `delta`) |

The values match native QuantLib computed with the same definitions.

### Swaption `swaption_pricing_rebump` theta: the roll now moves the whole market

Rebump `theta` is `NPV(tomorrow) − NPV(today)` with the market rolled one
calendar day. In 0.7.0 the roll moved the evaluation date but left explicit
curve and volatility `reference_date`s where they were, so a request whose
curves carry a `reference_date` (the common shape) reported `theta` as exactly
zero, which was then dropped, and helper-curve requests produced a partial
roll without option time decay. The roll now shifts every explicit reference
date and re-references the volatility entry, so `theta` includes both the
curve roll and the option's time decay. Rebump theta values therefore change
for every request. Known limit: SABR-calibrated surfaces roll curves only.

### Swaption greek fields are optional

`delta`, `annuity`, `atm_forward`, `vega`, `gamma`, `theta`, `dv01`,
`implied_volatility` and the `used_*` diagnostics are now `= null` optionals,
present only when the request flag that produces them is set. When both flags
are set, the rebump values of `dv01`, `gamma`, `vega` and `theta` take
precedence. Units, definitions and precedence are documented in
[`docs/http-api.md`](http-api.md).

### Migration (0.7.0 → 0.8.0)

No request changes. If you consume swaption greeks, re-read the units above;
if you parse responses strictly, expect scalars that were previously absent at
zero to be present.

## Version 0.7.0 (October 2026) — per-request calendar holiday overrides, backward-compatible

`v0.7.0` adds one feature, requested during an external validation POC: a
request can carry its own holiday corrections, so a short-notice or one-off
holiday no longer needs a new server image. Every 0.6.0 request prices
identically on 0.7.0: the change is one new optional field, and a request
without it behaves — and is cached — exactly as before. No migration is needed.

### Calendar holiday overrides

- New optional `calendar_overrides` list: one entry per calendar, with
  `added_holidays` (dates that must be holidays) and `removed_holidays` (dates
  that must be business days).
- It goes inside `pricing`, beside `as_of_date`, on every endpoint that has a
  `pricing` block (pricing, curve bootstrapping, calibration, sampling), and at
  the top level of `/calendar-holidays`, `/calendar-advance` and
  `/calendar-business-days`.
- Overrides apply for the duration of that one request and to every use of the
  calendar in it (indices, curve helpers, schedules, query grids). Nothing is
  stored on the server.
- An override that is already true is accepted with no effect, so a client's
  override list keeps working if a later server version already includes that
  holiday.
- Rejected with `400`, naming the field path: an entry without a calendar, an
  unparseable date, a date in both lists, a duplicate date, the same calendar
  in two entries (`UnitedStates` and `UnitedStatesSettlement` are the same
  calendar), `BespokeCalendar` / `NullCalendar`, and a weekend date in
  `removed_holidays`.
- Cached curves and calibrations are kept per override set: requests with
  different overrides never share a cached result.

The intended workflow is to fetch the server's holidays from
`/calendar-holidays`, reconcile them against your own calendar client-side, and
send only the differences. Full reference:
[`docs/http-api.md`](http-api.md#calendar-holiday-overrides).

## Version 0.6.0 (July 2026) — BREAKING: explicit OIS conventions

`v0.6.0` closes a gap reported by an external validation POC: the OIS curve
helpers could not be given the market-standard payment lag (or the other
overnight-coupon conventions), so a curve bootstrapped from OIS quotes
silently assumed a zero-day lag. The per-coupon effect is tiny but compounds
along the bootstrap into a measurable long-end discount-factor shift.

### OIS helpers and the OIS swap leg: conventions are now explicit and required

`OISHelper` and `DatedOISHelper` gain five required fields, mirroring the
priced `OisFloatingLeg` field-for-field:

- `payment_lag` — business days between accrual end and coupon payment
  (US SOFR market standard: 2). Must be >= 0.
- `averaging_method` — `Compound` or `Simple` averaging of the overnight
  fixings.
- `lookback_days` — observation lookback; `0` = no lookback. Must be >= 0.
- `lockout_days` — end-of-period fixing lockout; `0` = no lockout. Must
  be >= 0.
- `apply_observation_shift` — whether lookback uses the shifted date's
  day-count weight.

On the priced `OisFloatingLeg`, the same fields (plus `payment_convention`
and `payment_calendar`) are now required rather than silently defaulted, and
the old `lookback_days = -1` "none" sentinel is gone — `0` means no lookback.
Internally a wire `0` maps to QuantLib's null lookback (measurably different
from a literal zero-day lookback on indices with fixing days).

Two behavior corrections ride along:

- The helper fields `fixed_leg_convention`, `fixed_leg_frequency` and
  `calendar` were previously accepted but **ignored** (QuantLib used
  Following / Annual / the index calendar regardless). They are now honored —
  they feed QuantLib's payment convention/frequency/calendar — and the
  migrated examples state the values the engine previously applied, so
  results are unchanged for migrated requests. If your payloads stated
  different values, your curves will now (correctly) reflect them.
- `fixed_leg_day_counter` on both helpers and `day_counter` on the overnight
  leg are deprecated: no QuantLib overload consumes them (the day count comes
  from the overnight index). They are accepted-but-ignored and documented as
  such.

### Migration (0.5.0 → 0.6.0)

Add the five fields to every OIS helper point and the required conventions to
every OIS swap leg. A standard US SOFR curve is:

```json
"payment_lag": 2,
"averaging_method": "Compound",
"lookback_days": 0,
"lockout_days": 0,
"apply_observation_shift": false
```

To reproduce pre-0.6.0 behavior exactly, state `payment_lag: 0` and, on
helper points, `fixed_leg_convention: Following`, `fixed_leg_frequency:
Annual` (the values the engine silently used). The functional catalog
contains worked SOFR-style examples, including a payment-lag-2 curve, its
zero-lag twin (documenting the long-end divergence between them), and a
lookback/lockout curve, each verified against QuantLib.

## Version 0.5.0 (July 2026) — BREAKING: no implicit values, richer curves

`v0.5.0` completes the "an omitted field is an error, never a silent default"
theme across the whole request surface, redesigns curve construction around an
explicit trait, and cleans up response-side sentinels. It is **wire-breaking**:
requests that omitted a field and relied on its default, and gRPC/FlatBuffers
consumers reading response fields whose accessor type changed, must migrate.
Numbers are unchanged — a request made fully explicit prices exactly as before.

### Curve construction is now trait-driven (six explicit families)

`TermStructure.bootstrap_trait` is **required** and selects the curve family;
the server no longer guesses from the point types, and each trait validates the
points it receives (mismatch → 400):

- `Discount` / `ZeroRate` / `FwdRate` — bootstrap a `PiecewiseYieldCurve` from
  rate helpers (deposits, swaps, …).
- `InterpolatedZero` — interpolate zero rates directly from `ZeroRatePoint`s.
- `InterpolatedDiscount` — interpolate discount factors from the new
  `DiscountFactorPoint`s.
- `InterpolatedFwd` — interpolate instantaneous forwards from the new
  `ForwardRatePoint`s (Linear / BackwardFlat / ForwardFlat only — QuantLib
  cannot integrate a log-interpolated forward).

The three interpolated families are genuinely distinct curves (interpolating
zeros, discount factors or forwards off the same points yields different values
between nodes). **Explicit zero-rate curves that previously sent
`bootstrap_trait: Discount` must send `InterpolatedZero`.** Zero-rate points in
one curve must share one `compounding`/`frequency` (a mixed set was silently
mis-built before).

### Every request value and convention must be explicit

- **Values** (were silent zeros): notionals, `base_nominal`, `face_amount`,
  `redemption`, strikes and digital `cash`, `QuoteSpec.value`, `coupon_rate`,
  optionlet `volatility`, inflation helper quotes, and `CdsQuote.quote_type`.
  Amounts must be positive; rates may be negative but must be present and finite.
- **Conventions** (were hard defaults): calendars, day counters, frequencies,
  business-day conventions, settlement/fixing days, `recovery_rate`,
  `end_of_month`, `Period` unit/count, curve interpolator, and the CDS ISDA
  accrual flags, across the index, swap-index, curve-helper, inflation and
  credit-curve specs. Omission → 400 naming the field.
- **Duplicate ids** in a curves / indices / vol-surfaces / models /
  credit-curves list are rejected (were silently first-wins).

### Response changes

- Swaption Hull-White diagnostics, swaption implied volatility and the
  vanilla-swap CMS diagnostics are now **omitted when inapplicable** instead of
  reporting a `-1.0` / `-1` sentinel (which was ambiguous for a legitimate
  negative value). The `has_cms_swap_rate` flag is removed — the `cms_swap_rate`
  field alone carries presence. JSON consumers: those keys are simply absent;
  the only visible removal is `has_cms_swap_rate`. gRPC/FlatBuffers consumers:
  the accessors are now optional.

### Behavior fixes

- `in_arrears` / `fixing_days` on a floating swap leg are now honored on the
  vanilla-swap path (an in-arrears swap was silently priced in advance); the
  swaption underlying rejects `in_arrears` (QuantLib's `VanillaSwap` cannot
  express it) rather than mispricing it.
- The unused `FRA` day-counter/calendar/convention fields are no longer
  required (accepted-but-ignored, deprecated); discrete equity-barrier
  monitoring is rejected with a clear message (no native QuantLib engine).
- The shipped container now enables the Hull-White calibration cache
  (`QUANTRA_HW_CACHE_ENABLED=1`). In the 0.3.0 and 0.4.0 images it was
  available but off unless set explicitly.

### Migration

Make every request fully explicit — the ~130 payloads under `examples/data/`
are all valid, fully-explicit 0.5.0 requests and serve as templates. The
functional catalog (`tests/functional/CATALOG.md`) maps each to its expected
QuantLib result. `bootstrap_trait` and the presence rules are runtime contracts
the OpenAPI schema cannot express, so this section is authoritative.

## Version 0.4.0 (July 2026) — product expansion, backward-compatible

`v0.4.0` widens the product catalog. Every 0.3.0 request prices identically on
0.4.0: all schema changes are additive (new optional fields, new tables, new
endpoints), with one exception noted below that only affects previously
rejected inputs.

### New products (four new endpoints)

- **Zero-coupon bond** (`/price-zero-coupon-bond`) and **zero-coupon swap**
  (`/price-zero-coupon-swap`). The swap's fixed side is either an explicit
  `fixed_payment` or a `fixed_rate` + day-counter pair (exactly one form;
  QuantLib's rate form compounds annually by definition, so there are no
  compounding/frequency fields).
- **Year-on-year inflation cap/floor/collar**
  (`/price-year-on-year-inflation-cap-floor`), priced under Black,
  unit-displaced Black or Bachelier engines over a constant year-on-year
  optionlet volatility spec (a strike/tenor surface form is a planned
  additive extension).
- **Callable/puttable fixed-rate bond**
  (`/price-callable-fixed-rate-bond`): call/put schedule of dated clean
  prices, priced on the tree-based Hull-White engine; the model is referenced
  by id exactly like swaptions (explicit or calibrated, calibration cache
  applies). Response is npv + clean/dirty price; option-adjusted spread and
  risk measures are a planned optional extension.

### Widened existing products (all additive)

- **Amortizing/step-up notionals**: optional `notionals` vector (one entry
  per coupon period) on vanilla-swap legs and fixed/floating bonds. Paths
  that do not support it yet (OIS fixed leg, basis legs, swaption
  underlyings) reject it explicitly rather than ignoring it.
- **CMS capped/floored coupons**: the CMS leg's `cap`/`floor` now price
  (previously rejected as unsupported). These two fields changed from a
  negative-sentinel convention to optional-with-presence — the one
  non-additive change, affecting only requests that were rejected before.
- **Stub periods**: optional `first_date` / `next_to_last_date` on the shared
  `Schedule`, enabling short/long first and last coupons on every
  schedule-carrying product.

### Verification

- CMS legs gained full independent parity coverage against native QuantLib
  (matched to ~1e-7); every new product/feature above ships with functional
  parity cases and error-contract tests. The suite grew 537 → 624 cases.

## Version 0.3.0 (July 2026) — features + minor behavior changes

`v0.3.0` adds engine introspection, broader equity coverage, and
robustness/performance hardening. Requests that were valid and priced
correctly on 0.2.0 price identically on 0.3.0; the behavior changes below
affect only degenerate cases (no-op flags, requests slower than the new edge
timeout).

### New

- **gRPC `Meta` RPC** on the engine: `api_version` (the `VERSION` file
  verbatim), `backend_version`, `git_sha`, `build_time_utc`, `products[]`,
  `rpc_methods[]`, and `dependencies{quantlib, grpc, flatbuffers}`. gRPC-only;
  the JSON gateway keeps its richer `GET /meta`.
- **Standard `grpc.health.v1.Health`** (Check/Watch) served by the engine;
  Envoy now health-checks workers over gRPC instead of TCP, distinguishing a
  dead worker from a busy one.
- **Equity options: American and Bermudan exercise, digital payoffs.**
  American/Bermudan vanilla via finite-difference, European digitals
  analytically, American digitals via the analytic at-hit engine.
  Combinations QuantLib does not natively support (e.g. discrete cash
  dividends with American/Bermudan or digital payoffs) return a 400 naming
  the combination. Greeks an engine cannot compute are omitted from the
  response instead of serializing as invalid JSON.
- **Hull-White calibration cache** (`QUANTRA_HW_CACHE_ENABLED`, off by
  default; on in the shipped container from 0.5.0): repeat calibrations
  ~150ms → ~47ms, bit-for-bit transparent (gated by the cache-correctness
  suite).

### Behavior changes (degenerate cases only)

- **Requests are bounded end-to-end.** Envoy route timeout is 30s (was
  unlimited; configurable), client `grpc-timeout` is capped at 60s, and the
  engine abandons work when the caller's deadline has already expired —
  mid-request, at per-curve/per-trade checkpoints (`DEADLINE_EXCEEDED`,
  HTTP 504). Calibration iteration knobs from the wire are clamped
  (`max_iterations` ≤ 1000, `function_evaluations` ≤ 5000).
- **ZCIIS `adjust_observation_dates=true` is rejected** (400). The flag is
  numerically inert in the pinned QuantLib — it never changed any NPV — so it
  now errors instead of implying an adjustment that does not happen.
- Load balancing is least-request (was round-robin); worker count defaults to
  `min(cores, 8)` (was fixed 4). `QUANTRA_WORKERS` still overrides.

No schema fields were removed or renamed; no request that priced on 0.2.0
prices differently on 0.3.0.

## Version 0.2.0 (July 2026) — BREAKING

> **Why 0.2.0 and not a major bump:** the only published releases are the
> `v0.1.x` tags. The `2.0.0` previously recorded in `VERSION` was an internal
> development number that never shipped as a tag or container. While the wire
> contract is still stabilizing the project stays in `0.x`, where semver allows
> breaking changes on a minor bump.

`v0.2.0` bundles two coordinated wire-hardening batches with one theme: **an
omitted field no longer silently selects a default — it is an error.**
Previously, on the FlatBuffers wire a missing scalar/enum was indistinguishable
from its zero value, so forgetting a field could silently mis-price (an omitted
calendar meant Argentina; an omitted CDS side meant protection Buyer). Now the
server returns `400 INVALID_ARGUMENT` naming the missing field.

### Migration guide (0.1.x → 0.2.0)

To migrate a client, make every request fully explicit:

1. **State every convention.** Schedules (`calendar`, `frequency`,
   `convention`, `termination_date_convention`, `date_generation_rule`), curve
   helpers (calendars, business-day conventions, day counters, and the term
   structure's `day_counter`/`interpolator`), volatility specs, swap legs
   (`day_counter`, `payment_convention`), bonds (`accrual_day_counter`,
   `payment_convention`), the `Yield` spec (`day_counter`, `compounding`,
   `frequency`), FRA/cap-floor/CDS conventions, and the coupon-pricer's
   optionlet volatility conventions are all mandatory. Omission → 400 naming
   the field, e.g. `Schedule.calendar is required`.
2. **State the product discriminators.** `FRA.fra_type`,
   `CapFloor.cap_floor_type` and `CDS.side` are mandatory (previously an
   omitted `side` silently priced as protection Buyer — a sign flip).
3. **Give every curve helper an explicit quote.** `rate` / `spread` /
   `fx_points` / `price` are presence-based; supply the value or a `quote_id`.
   Omitting all of them → 400. The old sentinel-zero conventions are gone,
   which also makes a genuine 0 value representable.
4. **CDS quote fields are presence-based.** `flat_hazard_rate`,
   `running_coupon` and `upfront` must be supplied where the trade needs them;
   an empty credit curve is an error (the server no longer invents a hazard
   rate). In responses, `fair_spread` is omitted when QuantLib cannot express
   it (e.g. zero-running-coupon trades) instead of reporting 0.
5. **Use ISO-8601 dates.** All date strings are exactly `YYYY-MM-DD`. Slash
   formats and impossible dates (`2024-02-30`) → 400
   `Invalid date '<value>': expected YYYY-MM-DD`. Response dates are ISO too.
6. **Bond/future helper prices are explicit fields** (from the v2 batch below,
   also first published in 0.2.0): use `price` / `futures_price` instead of
   the old price-in-`rate` sentinels.

Every JSON under `examples/data/` is a valid, fully-explicit 0.2.0 request and
doubles as a migration reference; the browsable catalog
(`tests/functional/CATALOG.md`) maps each to its expected QuantLib result. The
OpenAPI spec cannot express runtime presence rules for scalars, so this prose
is the authoritative list of newly-required fields.

Interoperability notes: the served API version is visible at `/meta`, in the
OpenAPI `info.version`, and echoed on every response in the
`X-Quantra-Api-Version` header (all sourced from the `VERSION` file). The
internal curve-cache key namespace moved to `yc:v3:` so entries from older
builds cannot collide.

### Schema hardening batch v2 (June 2026) — BREAKING (first published in 0.2.0)

One coordinated wire-breaking FlatBuffers schema batch. Breaking parts:

- **Flow precision (breaking, responses):** `FlowInterest`/`FlowPastInterest`/
  `FlowNotional` fields `discount`, `rate`, `price` widened `float` → `double`.
  Response payload layout changes and these numbers gain precision digits;
  NPVs are unaffected (always computed in double).
- **Presence-based helper quotes (breaking, requests):** `BondHelper.price`/
  `.rate` and `FutureHelper.futures_price`/`.rate` are now optional scalars
  (`= null`). Field selection is by presence (price wins over rate), replacing
  the old sentinel-zero conventions ("if price==0 use rate" / "if
  futures_price!=0, rate ignored"). Omitting all of price/rate/quote_id is now
  a 400 INVALID_ARGUMENT instead of a silent zero quote. In JSON, omitted =
  absent. The internal curve-cache key serialization includes presence and its
  version moved `yc:v1:` → `yc:v2:` so old keys cannot collide.

Non-breaking parts shipped in the same batch:

- Every enum enumerator now carries its explicit numeric value (locks the
  existing wire values against future reordering; no value changed).
- Removed dead, never-referenced tables `FlowInterestFloat` and
  `FlowPastInterestFloat`.
