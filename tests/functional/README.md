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
   `examples/data/ir_swaps/`. Start from an existing case with the same
   product. Keep the request self-contained (indices, curves and the trade in
   one file) and prefer explicit fields over schema defaults.
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

## Planned / not yet covered

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
* **Other families** (bonds, FRAs, caps/floors, swaptions, CDS, inflation)
  already have representative parity tests in `tests/contract/`; migrating
  them into this catalog format is future work.
