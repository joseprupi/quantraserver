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
- **Hull-White calibration cache** (`QUANTRA_HW_CACHE_ENABLED`, on in the
  shipped container): repeat calibrations ~150ms → ~47ms, bit-for-bit
  transparent (gated by the cache-correctness suite).

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
