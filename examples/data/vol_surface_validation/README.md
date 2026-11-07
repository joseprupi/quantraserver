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
| `scotia_ndx_grid.json` | The grid (`BlackVolSpec`, `CubicStrikeLinearTime`) of method `SviPerExpiry` | fit, arbitrage | fit pass: 189 of 189 quotes inside the bid/ask; arbitrage pass: 5025 of 5025 scan points |
| `scotia_ndx_svi.json` | The `SviVolSpec` payload of the same calibration | fit, arbitrage, simulation (2048 Sobol paths, 252 steps per year) | fit and arbitrage pass; simulation warn: 179 of 189 quotes within 3.5 standard errors of their bid/ask (the misses are the 16-day expiry and the deep wings, where 2048 paths are at the sampling limit) |
| `scotia_ndx_ssvi.json` | The `SsviVolSpec` payload of method `Ssvi` | fit, arbitrage | fit pass, arbitrage pass (the slices' closed-form conditions and scan clean) |
| `scotia_ndx_vendor_constant_vol.json` | A flat 20 % `BlackVolSpec` (`Constant`), as a vendor feed might supply | fit, arbitrage, simulation (exact steps), custom maturity buckets | fit fail: 0 of 189 inside, RMS vol error 11 vol points (the smile is not flat); arbitrage pass (a constant local vol); simulation fail |

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
