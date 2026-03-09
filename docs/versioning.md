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
