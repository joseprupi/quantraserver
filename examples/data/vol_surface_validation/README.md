# Validating a volatility surface — examples

`POST /validate-vol-surface` requests (see `docs/http-api.md`, "Validating a
surface"). **Nothing here is market data**: the quote sheet is the synthetic
Scotia NDX sheet (`tests/tools/make_synthetic_quote_sheet.py`, preset
`svi_scotia_ndx`, also shipped as
`examples/data/vol_surface/synthetic_scotia_ndx_sheet.json`), written on the
Scotia note example's own Treasury curve and NDX dividend curve, which the
requests carry as their market (no `forwards` pasted: the checks read the
curves, as a structured note does). The surfaces are the ones
`/calibrate-vol-surface` returns on that sheet; a contract test asserts each
file still carries today's output.

| File | Surface | Checks | Result |
|---|---|---|---|
| `scotia_ndx_grid.json` | The grid (`BlackVolSpec`, `CubicStrikeLinearTime`) of method `SviPerExpiry` | fit, arbitrage | fit pass: 189 of 189 quotes inside the bid/ask; arbitrage pass: 6825 of 6825 scan points |
| `scotia_ndx_svi.json` | The `SviVolSpec` payload of the same calibration | fit, arbitrage, simulation (2048 Sobol paths, 252 steps per year) | fit and arbitrage pass; simulation warn: 178 of 189 quotes within 3.5 standard errors of their bid/ask (the misses are calls more than 15 % out of the money on expiries under four months, where 2048 paths are at the sampling limit) |
| `scotia_ndx_ssvi.json` | The `SsviVolSpec` payload of method `Ssvi` | fit, arbitrage | fit pass, arbitrage pass (the slices' closed-form conditions and scan clean; the payload carries no strike range, so the local vol is read unclamped and `clamp_edges` is empty) |
| `scotia_ndx_vendor_constant_vol.json` | A flat 20 % `BlackVolSpec` (`Constant`), as a vendor feed might supply | fit, arbitrage, simulation (exact steps), custom maturity buckets | fit fail: 0 of 189 inside, RMS vol error 11 vol points (the smile is not flat); arbitrage pass (a constant local vol); simulation fail |

Every fit quote also carries `bid_vol` / `ask_vol` (the spread in vol units),
and `arbitrage.clamp_edges` the strike range the note's local vol is held to
per expiry (from the grid's `local_vol_strike_min` / `_max` or the slices'
`strike_min` / `strike_max`: at each expiry its range intersected with the
next one's, moved inside by 1e-3 in log-strike; empty for the constant vol and
the eSSVI payload). Add `"arbitrage_grid": true` to `checks` to get every
scanned local vol point (`arbitrage.local_vol_grid`, 33 rows of 201 strikes
plus up to 6 points at the clamp's edges here).

The band table of the fit (`fit.bands`) shows where a surface misses: on the
flat vol the short-dated put wing (moneyness below 0.8, under three months)
is 28 vol points off, the at-the-money cell 3.5.

Run one:

```bash
curl -s -X POST http://localhost:8080/validate-vol-surface \
  -H 'Content-Type: application/json' \
  --data-binary @examples/data/vol_surface_validation/scotia_ndx_svi.json \
  | jq '{fit: .fit.verdict, arbitrage: .arbitrage.verdict, simulation: .simulation.verdict}'
```
