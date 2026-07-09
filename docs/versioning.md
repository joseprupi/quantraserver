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
