# Cross-version diff

`quantra_version_diff.py` sends the same request files to two running Quantra
servers and reports which response fields differ, and by how much.

It answers one question: **"if I move from server version A to server version
B, which of my results change?"** Typical uses:

- a new Quantra release moves to a newer QuantLib, and you want to see the
  effect on your own requests before switching;
- maintainers checking what a dependency upgrade changes across the
  functional catalog.

The repository's parity tests cannot answer this: they compare the server with
a QuantLib reference of the *same* version, so both sides move together.

The script needs Python 3 and `requests`. It does not need QuantLib, a build
of the server, or anything else from this repository (the repository is only
read when `--catalog` is used).

## 1. Run two server versions side by side

Start one container per version, each on its own host port (the HTTP/JSON API
listens on `8080` inside the image):

```bash
docker run -d --rm --name quantra-old -p 8091:8080 ghcr.io/joseprupi/quantra-server:<old-tag>
docker run -d --rm --name quantra-new -p 8093:8080 ghcr.io/joseprupi/quantra-server:<new-tag>

curl -s http://localhost:8091/meta   # api_version and dependencies.quantlib of each server
curl -s http://localhost:8093/meta
```

Stop them when you are done: `docker stop quantra-old quantra-new`.

## 2. Run the diff

With your own requests:

```bash
python3 tools/version_diff/quantra_version_diff.py \
  --old http://localhost:8091 --new http://localhost:8093 \
  --requests my_requests/ \
  --md-out diff.md --json-out diff.json
```

With the repository's functional catalog (every case in
`tests/functional/manifest.py`, run from a checkout):

```bash
python3 tools/version_diff/quantra_version_diff.py \
  --old http://localhost:8091 --new http://localhost:8093 \
  --catalog --md-out diff.md --json-out diff.json
```

`--catalog` and `--requests` can be combined.

**Check the noise floor first.** Point `--old` and `--new` at the *same*
server. Every request should come back "unchanged"; if not, the responses
contain something that varies from call to call, and you can exclude those
fields with `--ignore-path REGEX`.

### The `--requests` directory convention

The first-level sub-directory is the endpoint; every `*.json` file below it
(at any depth) is one request body, POSTed verbatim to that endpoint:

```
my_requests/
  price-vanilla-swap/
    desk_a/swap_001.json      -> POST /price-vanilla-swap
    swap_002.json             -> POST /price-vanilla-swap
  price-fixed-rate-bond/
    bond_001.json             -> POST /price-fixed-rate-bond
  bootstrap-curves/
    eur_curve.json            -> POST /bootstrap-curves
```

Endpoint names are the routes listed by `GET /meta` (`endpoints`), without the
leading slash. JSON files placed directly in the top directory have no
endpoint: they are skipped and listed in the report. Each request is
identified in the report by its path relative to the top directory.

### Options

| Option | Meaning |
|---|---|
| `--old URL`, `--new URL` | Base URLs of the two servers (required). |
| `--catalog [REPO]` | Use the functional catalog of the checkout (default: the one containing the script). |
| `--requests DIR` | Use your own request files (convention above). |
| `--rel-tol`, `--abs-tol` | Noise thresholds, both default `1e-12` (see below). |
| `--timeout SECONDS` | Per-request timeout, default 60. |
| `--top N` | Changed fields listed per case in the Markdown report, default 10. The JSON report always holds all of them. |
| `--ignore-path REGEX` | Skip response paths matching the regular expression (repeatable). |
| `--json-out FILE`, `--md-out FILE` | Where to write the reports. |
| `--fail-on-change` | Exit with status 1 if any field changed, any HTTP status differs, or any request could not be compared. Without it the exit status is 0. |
| `--quiet` | No per-request progress on stderr. |

## 3. Read the report

Each response is flattened to `path -> value`, with list indexes in the path
(`swaps[0].legs[1].npv`). Every path is put in one category:

| Category | Meaning |
|---|---|
| identical | Same value. Numbers are compared exactly: the same decimal text gives the same double. |
| noise | Two numbers that differ by at most `--abs-tol`, or by at most `--rel-tol` relative to the larger of the two magnitudes. Recorded, not counted as a change. |
| changed | Two numbers that differ by more than that. Reported with old, new, absolute and relative difference. |
| value changed | A non-numeric value (string, date, boolean, null) differs, or the type differs. |
| only in old / only in new | The field exists in one response only. |

Each request then gets one outcome:

- **unchanged** — every field identical;
- **noise only** — differences, all within the noise thresholds;
- **changed** — at least one field changed, value changed, or present on one
  side only;
- **status changed** — the two servers returned different HTTP statuses (for
  example `200` on one and `422` on the other); both bodies are quoted;
- **failed to compare** — at least one server did not answer (connection
  refused, timeout) or the request file could not be read. The run continues
  with the next request.

The Markdown report has: a header with both servers' API and QuantLib versions
(from `GET /meta`), the corpus size and the thresholds; one table row per
endpoint with the count of each outcome and the largest absolute and relative
difference seen; one section per changed request listing its changed fields
as old → new, largest relative difference first, capped at `--top` with a
count of the rest; a one-line list of the noise-only requests; then status
changes and requests that could not be compared. Output order is fixed (endpoint, then request id), so two reports
can be compared with a plain text diff apart from the timestamp line.

Requests that get the same error status from both servers (a bad payload, an
unknown endpoint) are still compared body against body, but they say nothing
about results; the report lists them in their own section so they can be
fixed.

The JSON report (`format_version` 1) holds the same information without any
cap: `old` / `new` (URL and the full `/meta` document), `tolerances`,
`corpus`, `summary`, `endpoints`, and `cases` with every noise, changed,
value-changed and one-sided field. Non-finite numbers are written as strings
(`"nan"`, `"inf"`) so the file is strict JSON.

## What the report does and does not tell you

- "Unchanged" is a statement about **the requests that were sent**. A request
  with a different shape — another convention, product feature, date or
  market-data layout — can still be affected by the same upgrade. The report is
  evidence for your corpus, not a guarantee for every request; the closer the
  corpus is to what you send in production, the more it tells you.
- The comparison is on the JSON the server returns. A difference smaller than
  the precision at which a field is serialized cannot be seen.
- The relative threshold is applied per field. A value that is the small
  difference of two large ones (an NPV that is the sum of two offsetting legs)
  can cross it while its components stay within noise; look at the absolute
  difference as well before drawing conclusions.
- To attribute differences to one cause (for example a QuantLib upgrade), the
  two servers must differ in nothing else: same Quantra source, same
  configuration, same request files.
- The tool reports differences; it does not explain them.
