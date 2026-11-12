# Volatility surfaces: methods and assumptions

`POST /calibrate-vol-surface` builds an equity implied-volatility surface from
listed option quotes with one of three methods. This page states what each
method fits, what it assumes, which part is QuantLib and which part is this
engine, and where each one stops working. Field-by-field reference:
`http-api.md`, sections "Volatility surface from option quotes" and
"Validating a surface".

## Common to the three methods

| Item | What the engine does |
|---|---|
| Input | One sheet of European option quotes (bid and ask per strike, expiry and type) for one underlying on one date, the spot and a discounting curve. |
| Forwards | One forward and one discount factor per expiry, from `/calibrate-implied-forwards` (put-call parity on the same sheet) or fitted in the same call. Dividends enter only through these forwards. |
| Quotes used | The out-of-the-money side at each strike: the call at or above the forward, the put below. The mid is inverted with Black-76 on the expiry's forward. |
| Quotes dropped | Lines failing `filter` (relative bid/ask spread above `max_relative_spread`), a mid below `1e-5 x spot`, a mid outside the Black bounds. Each dropped line is reported with its reason. |
| Quote weights | One option, `weighting`, with the same meaning for the three methods. Per expiry the fit minimises `sum_i weight_i (model_vol_i - market_vol_i)^2` over the used quotes. `Uniform`: weight 1. `Vega` (default): the Black vega at the market vol. `BidAsk`: `1 / spread_i^2`, the spread being the quote's bid/ask spread in vol units, `(ask - bid) / vega`. With `BidAsk` a used quote whose ask is not above its bid is left out (reason `ask_not_above_bid`). The response echoes the value used. |
| Time | Act/365 Fixed from the as-of date to the expiry date. |
| Output | A surface spec to paste into a pricing request, the forwards used, a per-expiry report, and one `arbitrage_free` flag per returned spec. |

What none of the methods does:

- No stochastic volatility, no jumps. The surface is a static Black implied
  vol. Under a structured note it is simulated as Dupire local vol.
- No American exercise. A sheet of American quotes is rejected.
- No discrete dividends in the calibration. The forward per expiry is the
  only dividend information.
- No smoothing across dates. Each call fits the quotes of one day.

## Summary

| | `SviPerExpiry` | `Ssvi` | `AndreasenHuge` |
|---|---|---|---|
| Shape | Parametric, one slice per expiry | Parametric, one slice per expiry | Non-parametric |
| Parameters per expiry | 5 | 3 | None (piecewise local vol) |
| Minimum used quotes per expiry | 5 | 3 | `filter.min_quotes_per_expiry` |
| Fit code | QuantLib `SviInterpolation`, then this engine's refits | This engine | This engine's copy of QuantLib `AndreasenHugeVolatilityInterpl`, with weights |
| `weighting` values | `Uniform`, `Vega`, `BidAsk` | `Uniform`, `Vega`, `BidAsk` | `Vega` only |
| No butterfly arbitrage | Checked, refitted with a penalty when it fails | By construction | In the method's own model |
| No calendar arbitrage | Enforced during the fit, last expiry first | By construction | In the method's own model |
| Between expiries | Total variance linear in time at fixed forward moneyness | The three parameters interpolated in time | Total variance linear in time at fixed forward moneyness, on a sampled grid |
| Beyond the quoted strikes | The SVI formula | The eSSVI formula | Constant vol from the edge strike |
| Beyond the last expiry | Flat implied vol | Last forward variance continued | Flat implied vol |
| Local vol under a note | Numerical Dupire (QuantLib `LocalVolSurface`) | Closed form | Numerical Dupire on the sampled grid |
| Returned payload | Grid and SVI slices (`SviVolSpec`) | Grid and eSSVI slices (`SsviVolSpec`) | Grid |

## SVI per expiry

Each expiry gets its own raw SVI slice of total variance:

```
w(k) = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)),   k = ln(K / F)
```

**QuantLib part.** `SviInterpolation` fits the five parameters by
Levenberg-Marquardt on weighted vol errors, from several fixed starting
points. It offers vega weights (`weighting: Vega`) and equal weights
(`Uniform`).

**Engine part.**

- `weighting: BidAsk`: QuantLib's two fits are the starting points of the
  engine's own Levenberg-Marquardt on the `BidAsk`-weighted vol errors. The
  lower objective is kept.
- The refits below use the same weights as the fit.
- Butterfly check on each slice (Durrleman's function on 201 points, and a
  wing slope `b (1 + |rho|)` of at most 2). A failing slice is refitted with a
  penalty.
- Calendar order. Slices are fitted from the last expiry back. Each slice must
  sit below the next one by a margin, `min_forward_vol^2 x (T_next - T)` in
  total variance, at every moneyness of the checked region. A slice that
  breaks this is refitted with a penalty, then lowered by what remains.
- Checked region per expiry: from the forward outwards, the distance five
  standard deviations cover at the market's implied vols, plus the strikes
  quoted on this expiry and the one before.
- Time interpolation: total variance linear in time at fixed forward
  moneyness `k = ln(K / F(t))`, with `ln F` linear in time through the spot
  and the expiry forwards.
- The slices carry the checked region as `strike_min` / `strike_max`. A note's
  local vol holds its edge value beyond them.

**Assumptions.**

- Five parameters describe the smile of one expiry.
- Between two expiries nothing happens that linear total variance at fixed
  moneyness does not capture (no event dates between quoted expiries).
- The longer expiry is the more reliable one. When two slices conflict, the
  shorter one gives way.

**Known limits.**

- Short-dated, deep out-of-the-money calls quoted at the minimum tick are
  fitted worse than by independent slices, because their upturn is not
  compatible with a time-ordered surface.
- The refit result depends on its starting points. Two are tried and the
  better objective is kept.
- Output expiries must be fitted sheet expiries.
- Arbitrage is checked on the region, not on every strike. Outside it the
  local vol is held at the edge value, not derived from the formula.

## eSSVI (`Ssvi`)

Each expiry gets three parameters, the at-the-money total variance `theta`,
the correlation `rho` and `psi`:

```
w(k) = 1/2 (theta + rho psi k + sqrt(psi^2 k^2 + 2 rho psi theta k + theta^2)),   k = ln(K / F)
```

Reference: Corbetta, Cohort, Laachir, Martini, "Robust calibration and
arbitrage-free interpolation of SSVI slices" (2019). QuantLib has no eSSVI.
The calibrator, the surface and the closed-form local vol are this engine's
code, contributed in #156.

**How it is fitted.**

- Expiries are fitted in increasing time.
- The quote nearest the money is the anchor. `theta` is solved so the slice
  reproduces it exactly.
- `rho` and `psi` minimise the weighted squared vol errors (`weighting`)
  under the no-arbitrage conditions: butterfly `psi (1 + |rho|) <= 4` and
  `psi^2 (1 + |rho|) <= 4 theta`; calendar against the previous slice
  (`theta` and `psi` non-decreasing, `|rho psi - rho_prev psi_prev| <= psi -
  psi_prev`, `psi / theta` non-increasing).
- Quotes with `|ln(K / F)|` above `ssvi.max_abs_log_moneyness` (default 1.0)
  are left out.

**Reading.** Between two slices `theta`, `psi` and `rho psi` are linear in
time. With slices that satisfy the conditions, the surface has no butterfly
or calendar arbitrage at any time or strike. The local vol is computed from
the formula's derivatives, with no numerical differentiation and no strike
limits.

**Assumptions.**

- Three parameters describe the smile of one expiry.
- The quote nearest the money is exact.

**Known limits.**

- Three parameters fit a liquid index smile less closely than SVI's five.
  The difference is largest in short-dated wings.
- When the quotes themselves cross at the money (a short expiry quoted above
  the next one in total variance), no feasible slice exists. The anchor is
  raised to the smallest feasible value, `ssvi_anchor_clamped` is set, and
  that slice misses its at-the-money quotes by the size of the crossing.
- Each slice is constrained by the one before it, so an error in an early
  slice limits the later ones.

## Andreasen-Huge

No smile formula. A local volatility, piecewise in strike, is calibrated
expiry by expiry to the option prices with a one-step implicit
finite-difference scheme (Andreasen and Huge, "Volatility Interpolation",
2010).

**QuantLib part.** The method and its numerical scheme are QuantLib 1.44's
`AndreasenHugeVolatilityInterpl` (cubic-spline interpolation, call/put
calibration, 500 grid points, Levenberg-Marquardt).

**Engine part.**

- The calibration is an engine-side copy of that class with one change, the
  cost function. The residual of a quote is
  `sqrt(weight_i) (model_price_i - market_price_i) / vega_i`: the price error
  turned into a vol error, then weighted. The vega is floored at 1 % of the
  expiry's at-the-money vega. QuantLib's class minimises plain price errors,
  which in vol terms is a weight of vega squared.
- The Black vol is the model price inverted, as QuantLib's
  `AndreasenHugeVolatilityAdapter` does.
- The model's Black vols are sampled on the model's own strike mesh and
  returned as a grid.
- A grid point is inverted only where the model's out-of-the-money price is
  at least `1e-4 x spot`. Below that the price is within the scheme's
  numerical noise. Those points take the vol of the nearest inverted strike
  of the same expiry and are counted in `grid_points_filled`.

**Assumptions.**

- The quoted prices of each expiry are themselves free of arbitrage. The
  model has one parameter per used quote and tries to match every quote.
  The weights only decide where the error goes when it cannot.

**Known limits.**

- `weighting` accepts `Vega` only. Under `Uniform` or `BidAsk` weights the
  calibration, started from a flat local vol, does not settle on a close fit
  on every sheet, so the two values are a `400`.
- On an expiry the model cannot match, the optimiser stops at its evaluation
  limit. The result is then the best point found, not a converged minimum.
- The method is free of arbitrage in its own one-step model. What is returned
  is a sampled grid of implied vols, read back with a cubic spline in strike
  and linear total variance in time. The Dupire local vol of that grid is not
  the model's local vol and can fail the arbitrage scan.
- The filled wings are flat in implied vol.
- For structured notes use `SviPerExpiry` or `Ssvi`. Use an Andreasen-Huge
  grid for Black-vol reads, and for a note only if it passes
  `/validate-vol-surface`.

## How a surface is read when pricing

| Use | What is read |
|---|---|
| Equity option | The Black vol at the option's expiry and strike. |
| Structured note, grid or SVI slices | Dupire local vol from QuantLib's `LocalVolSurface`, which differentiates the implied surface numerically in strike and time. |
| Structured note, eSSVI slices | Local vol from the closed-form derivatives of the eSSVI formula. |

- Dupire local vol does not exist where total variance decreases in time or
  where the local variance is negative. QuantLib raises an error there
  ("decreasing variance", "negative local vol^2") and the note returns a
  `400` with that message.
  `monte_carlo.local_vol_floor` replaces such reads with a floor and reports
  how many reads were floored.
- Beyond the per-expiry strike range of a grid or of SVI slices, the local
  vol is held at its value on the edge.
- Beyond the last expiry, grids and SVI slices from
  `/calibrate-vol-surface` hold the local vol of the last expiry
  (`local_vol_time_extrapolation: FlatLocalVol`). eSSVI slices continue the
  last forward variance with `rho` and `psi` held.
- Price on the discounting curve the quotes were fitted against and on the
  dividend curve `/calibrate-implied-forwards` returned. With other forwards
  the strikes sit at a different moneyness than in the market.

## Checking a surface: `POST /validate-vol-surface`

The endpoint takes a surface and a sheet of option quotes and returns up to
three reports. It fits nothing and changes nothing. The surface can come
from `/calibrate-vol-surface` or from anywhere else, for example a vendor
grid.

What goes in:

| Input | Meaning |
|---|---|
| The surface | Any equity vol in `pricing.volatility.vol_surfaces`, named by `volatility_id`. |
| The market | The spot, the dividend curve of the underlying and a discounting curve. |
| The quotes | The same sheet format and filter as `/calibrate-vol-surface`. |
| `forwards` | Optional. See "Sheet forwards or the note's curves" below. |
| `checks` | Which of the three tests to run. Fit and arbitrage are on by default, simulation is off. |
| `simulation` | Monte Carlo settings, required for the simulation test. |

### Test 1, fit: does the surface price the quotes?

For each usable quote the engine reads the surface vol at the quote's expiry
and strike, prices the option with Black-76 on that expiry's forward and
discount factor, and compares the price with the bid and the ask.

| Reported | Meaning |
|---|---|
| `inside_spread` per quote | The surface's price is between the bid and the ask. |
| `vol_error` per quote | Surface vol minus the vol of the market mid. Positive: the surface is above the market. |
| `bid_vol`, `ask_vol` per quote | The bid and the ask in vol units. They show how wide the spread is at that strike. |
| `bands[]` | Count, inside count and RMS vol error per cell of moneyness (`K / F`) and maturity. |
| `atm_error_per_expiry[]` | The vol error of the quote nearest the money, per expiry. |

How to read it:

- The share inside the spread is the headline. The band table says where
  the misses are: which wing, which maturities.
- A large vol error in a wing can still be inside the spread. Wing spreads
  are wide in vol units. Compare `vol_error` with `bid_vol` and `ask_vol`.
- The at-the-money errors are checked one by one. A single expiry above
  0.5 vol point prevents a `pass`.

Common causes of a poor fit:

- The surface was built on another date or on other forwards than the ones
  used here.
- The method has too few parameters for the smile (eSSVI in short wings).
- The filter here keeps quotes that the calibration dropped.

### Test 2, arbitrage: can a note simulate the surface?

A structured note does not read implied vols. It simulates paths with a
local vol derived from the surface (Dupire). At each time step it reads the
local vol at the level each path has reached. If one read cannot be
computed, the note returns a `400`.

This test does the same reads ahead of time, on a fixed scan instead of on
random paths:

- Times: every expiry, just before and just after each expiry, the midpoint
  between expiries, and just before the last expiry.
- Strikes: 201 per time, over the region paths are expected to reach. That
  region is five standard deviations either side of the forward at the
  market's implied vols, plus the quoted strikes.
- When the surface carries strike limits, the edges of those limits are
  scanned too.

A point where the local vol cannot be computed is a refusal. There are two
causes:

| QuantLib message | Cause |
|---|---|
| `decreasing variance` | Total variance at that strike is lower at the later time than at the earlier one. This is calendar arbitrage. |
| `negative local vol^2` | The smile at that time implies a negative probability density at that strike. This is butterfly arbitrage. |

| Reported | Meaning |
|---|---|
| `points`, `refused` | Points scanned and points refused. |
| `first_refusal` | QuantLib's message at the first refused point, with its time and strike. |
| `min_local_variance` and its time and strike | The lowest local variance among the points that evaluated. A value near zero is a point close to a refusal. |
| `svi_slices[]`, `ssvi_slices[]` | For slice payloads, the slice-by-slice conditions (butterfly, wing slope, order in time). |
| `clamp_edges[]` | The strike range a note holds the local vol to, per expiry. |
| `local_vol_grid` | With `checks.arbitrage_grid`: every scanned point with its local vol. For plotting. |

How to read it:

- `refused = 0`: every scanned point can be simulated. The scan is finite,
  so this is not a proof for strikes outside the scanned region.
- `refused > 0`: a note whose paths reach those points returns a `400`.
  `first_refusal` gives the time and strike to look at.

What to do after a refusal:

- Refit. For SVI, raise `svi.min_forward_vol`. Or use `Ssvi`.
- Check the forwards. See the next section.
- Price the note with `monte_carlo.local_vol_floor`. The note then prices
  and reports the share of reads that took the floor, which shows how much
  of the price rests on it.

### Test 3, simulation: do simulated paths reprice the quotes?

Tests 1 and 2 look at the surface. This test runs the chain a note uses:
local vol, time grid, path generation. It simulates the underlying to every
expiry, prices each quote as the discounted average payoff, and compares it
with the bid and the ask.

A Monte Carlo price has sampling noise, reported as its standard error. A
quote counts as `within_noise` when

```
bid - 3.5 x standard_error <= mc_price <= ask + 3.5 x standard_error
```

| Reported | Meaning |
|---|---|
| `mc_price`, `standard_error` per quote | The simulated price and its noise. |
| `within_noise` per quote | The condition above. |
| `bands[]` | The same moneyness and maturity cells as the fit. |

How to read it:

- A surface that passes the fit and fails here is priced correctly by the
  formula but not by the simulation. Check the number of time steps per
  year, and whether the strike limits hold the local vol where paths go
  (`clamp_edges`).
- The standard error falls with the square root of the number of paths.
  Short-dated, deep out-of-the-money quotes fail first at low path counts:
  few paths end in the money, so both the price and its standard error are
  poorly estimated. In the shipped SVI example, 2,048 paths put 178 of 189
  quotes within the noise. The 11 misses are all calls more than 15 % out
  of the money on expiries under four months.
- Run it with the path count and `steps_per_year` of the note you are about
  to price.
- If the local vol refuses along a path, the whole request returns a `400`.
  Run the arbitrage test alone first, or set `simulation.local_vol_floor`.

### Sheet forwards or the note's curves

Fit and simulation need a forward and a discount factor per expiry. There
are two choices:

| `forwards` | Forward per expiry | Answers |
|---|---|---|
| Absent | From the spot, the dividend curve and the discounting curve of the request. This is what a note simulates. | Will a note on these curves price, and does it reprice the quotes? |
| Present (pasted from `/calibrate-implied-forwards`) | The forwards implied by the sheet. This is what the surface was fitted on. | Did the fit work? |

The two runs agree when the note uses the dividend curve that
`/calibrate-implied-forwards` returned and the same discounting curve. They
differ when the note uses its own dividend or rate assumptions. A strike
then sits at a different distance from the forward than it did in the
market, so the fit is worse and the arbitrage scan covers other strikes. A
surface that is clean on the sheet's forwards can refuse on the note's
curves.

The run without `forwards` is the one that says whether the note will
price.

### Verdicts

Each test returns a `verdict` text that starts with `pass`, `warn` or `fail`
and then gives the numbers behind it. The arbitrage test has no `warn`. The
thresholds are fixed defaults of this engine, not a market standard:

| Test | `pass` | `fail` |
|---|---|---|
| Fit | At least 90 % of quotes inside bid/ask and every at-the-money error within 0.5 vol point | Fewer than 50 % inside |
| Arbitrage | No refused point and no slice condition broken | Otherwise |
| Simulation | At least 95 % of quotes within the noise | Fewer than 80 % |

For fit and simulation, anything in between is `warn`. The per-quote
numbers are always returned, so a caller can apply other thresholds.

### What the endpoint does not tell you

- Whether the quotes are right. Stale or one-sided quotes are only removed
  by the filter.
- Anything about strikes and expiries with no quotes. Fit and simulation
  test quoted points only.
- Whether local vol is the right model for the product. Two models that
  reprice the same vanilla options can give different prices for a
  path-dependent note.
- Anything about correlation. The endpoint tests one underlying.

### Running it

```bash
curl -s -X POST http://localhost:8080/validate-vol-surface \
  -H 'Content-Type: application/json' \
  --data-binary @examples/data/vol_surface_validation/scotia_ndx_svi.json \
  | jq '{fit: .fit.verdict, arbitrage: .arbitrage.verdict, simulation: .simulation.verdict}'
```

`examples/data/vol_surface_validation/` has one request per surface type on
a synthetic sheet, and a flat 20 % vol that fails the fit on purpose.
`examples/structured_note_workflow/price_note_from_quotes.py` runs
calibration, validation and the note in sequence.

## Choosing a method

| Need | Method |
|---|---|
| Closest fit to the quotes, surface used for vanilla options and notes | `SviPerExpiry` |
| No arbitrage at any strike and time, closed-form local vol, fewer parameters | `Ssvi` |
| A non-parametric surface for Black-vol reads | `AndreasenHuge` |

Run `/validate-vol-surface` on the result in every case.
