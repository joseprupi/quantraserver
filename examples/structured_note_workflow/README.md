# Pricing a structured note: three levels of market data

A worked, runnable example of how a worst-of autocallable goes through
`POST /price-structured-note`, depending on what market data is at hand.
The note is the Bank of Nova Scotia autocallable on the Nasdaq-100 and the
Russell 2000 (`examples/data/structured_note/scotia_..._us06419jme72.json`,
terms from its SEC pricing supplement; see the README there). The request
and response fields named below are defined in `docs/http-api.md`:
["Structured notes"](../../docs/http-api.md#structured-notes),
["Diagnostics"](../../docs/http-api.md#diagnostics),
["Greeks"](../../docs/http-api.md#greeks),
["Implied forwards and dividends from option quotes"](../../docs/http-api.md#implied-forwards-and-dividends-from-option-quotes),
["Volatility surface from option quotes"](../../docs/http-api.md#volatility-surface-from-option-quotes)
and ["Correlation from price histories"](../../docs/http-api.md#correlation-from-price-histories).

| File | What |
|---|---|
| `price_note_from_quotes.py` | Level 2 end to end: price histories → `calibrate-correlation` (with `--closes`), quote sheets → `calibrate-implied-forwards` → `calibrate-vol-surface` → `price-structured-note` on the grid and on the SVI payload, with the repricing diagnostic; prints a table and writes every request and response body. Python 3 + `requests`. |
| `price_note_from_quotes.sh` | The same calls as raw HTTP (`curl` + `jq`), on the bodies the Python script wrote or on the shipped example requests. |
| `../data/vol_surface/synthetic_scotia_{ndx,rty}_sheet.json` | The two quote sheets the script reads by default: **synthetic**, generated on the note's own market by `tests/tools/make_synthetic_quote_sheet.py` (`svi_scotia_ndx` / `svi_scotia_rty`). |
| `../data/correlation/ndx_rut_closes_2025-09-29_to_2026-09-30.json` | The price histories `--closes` reads by default: one year of daily NDX and RTY closes (public index closes; see the README there). Real data, unlike the sheets. |
| `../data/structured_note/scotia_..._with_synthetic_surface.json`, `..._with_synthetic_svi.json` | The level-2 result as a single request each (grid / SVI payload), ready to post. |

`tests/contract/structured_note_workflow_test.py` runs the Python script in
the test gate, so the example keeps working as the API moves.

Nothing in the synthetic sheets is market data, so the prices below are not
valuations of the note; the issuer's estimated value is $973.52 per $1,000.
For real quotes, `tools/option_quotes/cboe_eod_to_quote_sheet.py` turns a
Cboe DataShop end-of-day file into a sheet of the same shape
(`--sheet-ndx` / `--sheet-rty` accept its output as written). Licensed
vendor files, and anything derived from them, are never committed.

## Level 1: no option data (flat vols, dividend yields, a correlation)

What the IDAD factsheet transcriptions and the two SEC notes under
`examples/data/structured_note/` do. One request, no calibration.

Fields: `pricing.quotes` (a spot per index), `pricing.rates.curves` (the
discounting curve, here Treasury zeros as `USD_TSY`; a flat dividend curve
per index, `InterpolatedZero`, two points), `pricing.volatility.vol_surfaces`
(a `BlackVolSpec` of `shape: Constant` per index, e.g. the VXN / RVX closes),
`pricing.correlations.matrices[0]` (labels in basket order, row-major values,
positive semi-definite), the note's `underlyings[]` (`underlying_id`,
`volatility_id`, `initial_level`), `events[]`, `capital`, `upside`,
`discounting_curve`, `funding_spread_bp`, `correlation_id` and
`monte_carlo` (`paths`, `seed`, `sequence`, `antithetic`, `brownian_bridge`;
no `steps_per_year`, the grid is the observation dates).

```bash
curl -sS -X POST http://localhost:8080/price-structured-note \
  -H 'Content-Type: application/json' \
  -d @examples/data/structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72.json | jq '.notes[0]'
```

The response: `npv` and `standard_error` (per `notional`, so per 1,000
here), `status` (`Alive` / `Called` / `Matured` after the seasoning),
`legs` (`bond_redemption`, `coupons`, `capital_loss`, each a PV with its
standard error; they add up to `npv`), `event_probabilities[]` (`p_call` /
`p_coupon` per observation), `p_barrier_breach`, `expected_life_years`, the
`terms` and `model` echoes (what was parsed, `vol_shape` per underlying,
`time_steps`), the `seasoning` block (past observations resolved from
`historical_levels`). Add `diagnostics` flags for the market as used, the
decomposition, the correlation-limit checks, the payout distribution and the
timing, and `greeks` for delta / gamma / vega per underlying, the correlation
sensitivity, `funding_dv01` and `rho` (every bump a full simulation on the
same random stream; see the structured-note README for the cost).

Caveats, in the words of that README: a 30-day volatility index is a proxy
for a one- or two-year at-the-money vol, a flat vol carries no skew (so the
75 % trigger put is under-priced), and a trailing dividend yield is not the
forward the market trades. The result is indicative: 965.6 ± 0.4 at
65,536 paths against the filing's 973.52.

## Level 2: option quotes → forwards → surface → the note

Two sheets of listed European option quotes (one per index, the as-of date
of the note) are the new input, and optionally the two indices' price
histories; the chain is three calibration endpoints and the pricer, and the
script runs it:

```bash
python3 examples/structured_note_workflow/price_note_from_quotes.py --url http://localhost:8080 --flat --closes
# own sheets, own note:
python3 examples/structured_note_workflow/price_note_from_quotes.py \
  --sheet-ndx ndx_eod_sheet.json --sheet-rty rut_eod_sheet.json --note my_note.json \
  --paths 4096 --steps-per-year 252 --method SviPerExpiry --out-dir /tmp/note_bodies
bash examples/structured_note_workflow/price_note_from_quotes.sh /tmp/note_bodies   # the same calls with curl
```

Steps, each printed with a one-line summary:

- (a) The note is loaded; its spots, as-of date and discounting curve are
  the market the calibrations run on (a sheet whose `spot` or `as_of_date`
  differs from the note's is reported).
- (b) With `--closes [FILE]`, `POST /calibrate-correlation` on the price
  histories (the shipped NDX / RTY closes by default; `FILE` is any
  `calibrate-correlation` request whose series are the note's underlyings):
  the `series` of `{date, value}` closes, the `estimator` (window, log or
  simple returns, daily or weekly, shrinkage) and the note's
  `correlation_id`. The response gives `correlation_spec` (pasted into
  `pricing.correlations.matrices` in place of the booked matrix),
  `sample_correlation` next to it, the shrinkage target and intensity, the
  smallest eigenvalue before and after the floor, the closes used and
  dropped per series and their realised vols. The script prints the matrix
  used and the sample side by side. Without `--closes` the booked matrix is
  reported and kept. On the shipped closes the estimate is 0.7472, the
  0.747 the note is booked with; with two series the Ledoit-Wolf target is
  the sample itself (intensity 0), so the file asks for `shrinkage: None`.
- (c) `POST /calibrate-implied-forwards` per index: `pricing` (the as-of
  date, the spot quote, the discounting curve, a flat 0 % placeholder
  dividend curve the fit does not read), the `sheet`, the
  `discounting_curve`, the `filter` and the `dividend_curve_id` wanted.
  The response gives, per expiry, the `forward`, the
  `implied_discount_factor` against the curve's, the `implied_funding_bp`,
  the `implied_dividend_yield`, the quotes used and dropped with reasons and
  the fit residual, plus `dividend_curve_spec`: the `InterpolatedZero` curve
  that reproduces those forwards, pasted into `pricing.rates.curves` with
  the underlying's `dividend_yield_curve_id` pointed at it. The script
  prints that table.
- (d) `POST /calibrate-vol-surface` per index: the same `pricing` and
  `sheet`, the step-(b) `expiries` array as `forwards` (so the surface is
  fitted on exactly the forwards the note will simulate), `method`
  (`SviPerExpiry` here; `AndreasenHuge` is the default) and
  `vol_surface_id` = the note's `volatility_id` for that index. The response
  gives the per-expiry report (`quotes_used`, `rmse_vol` and
  `max_abs_error_vol` in vol units, the fitted `svi` parameters, the
  butterfly and calendar findings and what was done about them),
  `vol_surface_spec` (the grid: a `BlackVolSpec` `SmileCube3D`, 8 expiries x
  41 strikes, read `CubicStrikeLinearTime`) and, with `SviPerExpiry`,
  `vol_surface_spec_svi` (the same surface as an `SviVolSpec`: the fitted
  slices with their forwards), and `vanilla_repricing`: every used quote
  repriced on the returned grid, the surface's own acceptance check. The
  script prints the report and keeps both specs.
- (e) `POST /price-structured-note` twice: the note with the correlation
  matrix of (b), the two dividend curves and the two surfaces pasted in
  (grid, then SVI), `monte_carlo.steps_per_year` set (required with a surface: the simulation is Dupire
  local vol on a time grid; 252 = one step per trading day), and
  `diagnostics: {vanilla_repricing: true, vanilla_repricing_sheets: [the two
  sheets], vanilla_repricing_filter: the same filter}`.
- (f) With `--flat`: a third pricing with a `Constant` vol per index equal
  to the Black-76 implied vol of the sheet's at-the-money quote nearest one
  year, no `steps_per_year`: the level-1 request built from the same sheets.
- (g) The table.

Run on the synthetic sheets at the defaults (2,048 Sobol paths, 252 steps
per year) with `--flat` and the booked correlation; the 507-step local-vol
simulation and its repricing run take about 2 s each in the engine at this
path count (with `--closes` the matrix is 0.7472 instead of 0.747 and the
rows move within the noise):

```
    run    price      +/- SE   status  p_call  p_breach      bond   coupons   cap.loss  repricing inside/within noise/total  max vol err
    grid     951.91     6.19   Alive   0.535    0.201      982.7      75.3     -106.1               43 /  370 /  383   15.99 pts
    svi      951.57     6.26   Alive   0.535    0.200      982.8      75.3     -106.6               45 /  370 /  383   16.01 pts
    flat     957.03     4.57   Alive   0.411    0.224      975.0      57.9      -75.8                7 /   67 /  383   25.33 pts
```

How to read it:

- `price +/- SE`: `npv` and `standard_error` per 1,000. Grid and SVI are
  the same surface carried two ways on the same random stream: the grid
  nodes sit on the slices and only the spline's error between nodes
  separates them, so the two prices agree to the Monte Carlo noise.
- `p_call`, `p_breach`: `event_probabilities[0].p_call` and
  `p_barrier_breach`; `bond`, `coupons`, `cap.loss`: the three `legs`.
- `repricing inside / within noise / total`: of the `vanilla_repricing`
  block, `inside_bid_ask` (model price strictly inside the quote's bid/ask),
  the number of options whose model price sits no further than 3.5
  standard errors outside its band (the gate's criterion: at a few thousand
  paths the standard error of an at-the-money option is a few percent of
  its price against a 1 % bid/ask, so "inside" is sampling-limited and
  "within noise" is the honest count), and `count` (every usable quote of
  both sheets, the same quotes step (c) used). `max vol err` is
  `max_abs_vol_error`, the worst `model_vol - market_vol` over the quotes
  for which both Black-76 vols solve; it is dominated by the deep wings of
  the 16-day expiry, where the Euler steps of the local vol and the
  sampling limit meet. The block also carries `note_npv` /
  `note_standard_error`, the note walked on the repricing paths (a different
  stream, the same model: a second estimate of the price), per-expiry
  forwards and discount factors, every option with its `model_price`,
  `standard_error`, `price_error` and `vol_error`, and the dropped lines
  with reasons.
- The `flat` row is what level 1 gives from the same sheets. Its price
  lands near the surfaces' (957 against 952, one standard error apart at
  this path count) but not for the same reasons: without the skew the call
  at year one is less likely (0.41 against 0.54), the coupon leg smaller
  (58 against 75) and the capital-loss leg smaller too (−76 against −106),
  and the two differences happen to offset on this note. The repricing
  block tells the two apart: 7 of 383 listed options inside their band and
  67 within the noise against 370, vol errors of up to 25 points in the
  wings. A local-vol run reprices the listed options; a flat vol reprices
  the one quote it was read from.

The request bodies the script sends (`correlation_request.json` with
`--closes`, `implied_forwards_<ID>_request.json`,
`vol_surface_<ID>_request.json`, `price_note_{grid,svi,flat}_request.json`)
and the responses are written to `--out-dir` (a temp dir by default; the
path is printed). Any HTTP error ends the run with the engine's message and
a non-zero exit.

Cost: each pricing is a 252-steps-per-year local-vol simulation of two
indices over two years, and the repricing diagnostic is a second one on a
finer grid; together about 1 s per 1,000 paths in the engine, against the
JSON gateway's 10 s RPC deadline (a `504` beyond it). 4,096 paths with the
diagnostic fit; `--no-repricing` halves the cost (8,192 paths fit) and
drops the repricing columns; more paths than that go through the gRPC
server directly or a deployment with a longer deadline. At 8,192 paths the
grid and the SVI prices are 950.8 ± 3.1 and 950.7 ± 3.1.

`--method AndreasenHuge` returns the grid only (no closed-form slices;
`vol_surface_spec_svi_note` says so) and the script prices the grid alone.
On the synthetic sheets that surface is refused by the local-vol simulation
(the deep wings are filled flat, `grid_points_filled`, and the cubic spline
bends where the fill starts; the engine answers `400` naming the vol ids
and the strike, and the script ends with that message); the SVI slices do
not have the problem, see "Volatility surface from option quotes".

## Level 3: your own surfaces, forwards, correlation and funding

When the desk already has the market, paste it; nothing is calibrated.

- Vols: a `BlackVolSpec` `SmileCube3D` grid (`expiries` as periods,
  `strikes`, `surface_vols` row-major, `surface_interpolator:
  CubicStrikeLinearTime` — `Bilinear` loses the smile's curvature between
  nodes and reprices 2 to 3 vol points high, see `docs/versioning.md`
  § 0.15.0) or an `SviVolSpec` (per expiry `forward, a, b, sigma, rho, m`,
  the raw SVI total variance in log-moneyness against that forward). The
  two committed Scotia files are the shapes. `monte_carlo.steps_per_year`
  is required with either.
- Forwards: a dividend curve per index (`InterpolatedZero` zero rates, or
  the `dividend_curve_spec` of step (b) on your own sheet) under the
  underlying's `dividend_yield_curve_id`; the note's forward at `T` is
  `spot x D_div(T) / D_discount(T)`, and the `market` diagnostic echoes the
  forwards and implied yields at every observation date, so a desk forward
  can be checked against it before pricing.
- Correlation: the matrix the desk marks (`pricing.correlations.matrices`),
  or the historical estimate of `/calibrate-correlation` with the desk's
  view as `correlation_markup` on the note (a uniform shift of every
  off-diagonal entry, clipped and repaired, echoed in `terms` and in the
  `market` diagnostic); the `checks` diagnostic prices the correlation-one
  and correlation-zero limits on the same stream, and
  `greeks.correlation_sensitivity` the slope.
- Funding: `funding_spread_bp` over the `discounting_curve` (the issuer's
  level; `greeks.funding_dv01` is the sensitivity per bp), or a funding
  curve as the discounting curve itself with the spread at zero.
- Keep `diagnostics.vanilla_repricing` with the sheets the surfaces came
  from: it is the one check that the pasted surface, the pasted forwards and
  the simulation agree on the listed options.
