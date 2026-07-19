# Contributing

## Branching
- `master` is always releasable.
- Create short-lived branches:
  - `feat/<topic>`
  - `fix/<topic>`
  - `chore/<topic>`
  - `docs/<topic>`
- No direct pushes to `master`; use Pull Requests.

## Running the gate locally
The build dependencies (gRPC, QuantLib, `flatc`, the pinned Python) exist only
in the `quantraserver:test` Docker image — a bare host cannot build or test the
project. Run the same gate CI runs before you open a PR:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace quantraserver:test \
    bash -lc './scripts/build.sh Release && bash tests/run_all_tests.sh'
```

It passes only when it exits 0 and the summary reports no failed suites. See
`docs/testing.md` for what each suite covers.

## Pull Requests
- Keep PRs focused and small.
- Ensure the gate above and CI pass before merge.
- Update docs when behavior or API changes.
- Mark API contract impact clearly (`breaking` or `non-breaking`).

## Merge Policy
- Prefer squash merge for clean history.
