# Functional parity catalog

A manifest-driven set of end-to-end pricing cases: each case is a complete
JSON request that is POSTed to the running Quantra server, and the returned
NPV is asserted to equal an independent QuantLib reference price (computed by
`tests/contract/ql_reference.py`) within a tight tolerance (default **0.01**).
The server and the reference are pinned to the same QuantLib version, so the
observed differences are at machine precision — a real behavioural drift on
either side fails the gate immediately.

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
   `examples/data/cap_floor/`. Start
   from an existing case with the same product. Keep the request
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
* **Other families** (swaptions, CDS, inflation) already have
  representative parity tests in `tests/contract/`; migrating them into
  this catalog format is future work. Bonds, FRAs and caps/floors are
  covered above.
