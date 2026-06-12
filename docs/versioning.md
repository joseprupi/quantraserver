# Versioning Policy

This repository uses Semantic Versioning: `MAJOR.MINOR.PATCH`.

## Rules

- `MAJOR`: breaking API, schema, or contract changes
- `MINOR`: backward-compatible features and endpoint additions
- `PATCH`: backward-compatible fixes and internal improvements

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

## Pending Breaking Changes

### Schema hardening batch v2 (June 2026) — BREAKING, requires a MAJOR bump

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
