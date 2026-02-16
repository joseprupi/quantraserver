# Versioning Policy

This repository uses Semantic Versioning (`MAJOR.MINOR.PATCH`).

## Rules
- `MAJOR`: breaking API, schema, or contract changes.
- `MINOR`: backward-compatible features and endpoint additions.
- `PATCH`: backward-compatible fixes and internal improvements.

## Contract Discipline
- OpenAPI is treated as API contract.
- Breaking contract changes require a major version bump.
- Non-breaking additions require a minor version bump.
- Fixes and docs-only updates require a patch bump.

## Release Flow
1. Merge PRs into `master`.
2. Bump `VERSION`.
3. Create tag `vX.Y.Z`.
4. Publish GitHub Release notes.
