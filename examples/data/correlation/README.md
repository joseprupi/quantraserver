# Correlation from price histories — examples

`POST /calibrate-correlation` requests: price histories in, a
`CorrelationMatrixSpec` out, ready to paste under
`pricing.correlations.matrices` and to reference from a structured note's
`correlation_id`. The engine never fetches closes; every value is in the
request.

Source of the closes: index closes, Yahoo Finance history pages (Nasdaq-100
`^NDX`, Russell 2000 `^RUT`, S&P 500 `^GSPC`), daily, as published. The
series ids are the `EquityUnderlyingSpec.id`s the structured-note examples
use (`NDX`, `RTY` for the Russell 2000, `SPX`), so the returned labels match
those notes' underlyings as they are.

| File | Content |
|---|---|
| `ndx_rut_closes_2025-09-29_to_2026-09-30.json` | NDX and RTY, 253 daily closes each, one year to the Scotia note's as-of date (2026-09-30). `shrinkage: None` (with two series the Ledoit-Wolf target is the sample itself, so shrinkage does nothing). Reproduces the 0.747 the Scotia example is booked with. |
| `ndx_rty_spx_closes_2025-10-17_to_2026-10-19.json` | NDX, RTY (to 2026-10-19) and SPX (from 2025-10-17), `window_days: 365` back from the JPMorgan note's as-of date (2026-10-19), `LedoitWolf` shrinkage. 251 common closes inside the window; the 15 NDX / RTY closes before 2025-10-20 and the SPX close of 2025-10-17 are reported as dropped. |

## What comes out

On the NDX / RTY file (one year of daily log returns, Pearson sample
correlation, no shrinkage):

```
sample / spec:      NDX-RTY  0.747207075688
realised vols:      NDX 0.199  RTY 0.187   (Act/252, unbiased)
observations_used:  253, returns_used 252, 2025-09-29 to 2026-09-30
smallest eigenvalue 0.2528 (= 1 - rho); floor not applied
```

The same closes with `shrinkage: LedoitWolf` give the same matrix and
`shrinkage_intensity_used: 0`: with two series the constant-correlation
target equals the sample correlation.

On the three-series file (251 common closes, daily log returns) the sample
correlations are NDX-RTY 0.7450, NDX-SPX 0.9243, RTY-SPX 0.8297 (the
JPMorgan example is booked with 0.746 / 0.925 / 0.831, an estimate on a
window one day longer), the constant-correlation target is their mean
0.8330, and the Ledoit-Wolf intensity on 250 returns is 0.2257, so the
returned matrix is NDX-RTY 0.7649, NDX-SPX 0.9037, RTY-SPX 0.8304: each
pair pulled about a fifth of the way toward the average. Realised vols
19.8 % / 18.2 % / 12.7 %. `sample_correlation` is always returned next to
the spec, so both are at hand; `shrinkage: None` returns the sample as the
spec.

`estimator_echo` is the estimator as resolved (an explicit `start_date` /
`end_date` in place of `window_days`); posting the same series with it as
`estimator` reproduces the response.

## Using the spec

```json
"pricing": {"correlations": {"matrices": [ <correlation_spec as returned> ]}},
"notes": [{"underlyings": [{"underlying_id": "NDX", ...}, {"underlying_id": "RTY", ...}],
           "correlation_id": "NDX_RTY", "correlation_markup": 0.05, ...}]
```

`correlation_markup` is the desk's uniform shift on every off-diagonal
entry (clipped to `[-1, 1]`, repaired when the shift breaks positive
semi-definiteness, echoed in `terms` and in the `market` diagnostic). A
historical estimate is an input the desk judges, not a market price: the
mark-up is where that judgment goes. See "Correlation from price
histories" and "Structured notes" in `docs/http-api.md`, and the workflow
in `examples/structured_note_workflow/` (`--closes`).

## Running

```bash
curl -X POST http://localhost:8080/calibrate-correlation \
  -H "Content-Type: application/json" \
  -d @examples/data/correlation/ndx_rut_closes_2025-09-29_to_2026-09-30.json
```
