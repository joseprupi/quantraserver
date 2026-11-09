# Versioning Policy

This repository uses Semantic Versioning: `MAJOR.MINOR.PATCH`.

## Rules

- `MAJOR`: breaking API, schema, or contract changes
- `MINOR`: backward-compatible features and endpoint additions
- `PATCH`: backward-compatible fixes and internal improvements

While the project is in `0.x` (pre-stable), semver permits breaking changes on
a `MINOR` bump; each one is documented below. `1.0.0` is reserved for the
first release that promises wire stability.

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

## Version 0.16.9 (November 2026) — the arbitrage scan covers what the simulation reads

`v0.16.9` closes a gap between the arbitrage scan of
`/validate-vol-surface` (and of `/calibrate-vol-surface`) and the local
vol a structured note actually reads: a surface could pass the scan with
no refusal and the note on it could still be a `400` ("decreasing
variance at strike ... between time ... and ..."). No wire field is added
or removed; QuantLib stays at 1.44.

**What was wrong.** Two things combined, found on a real index sheet with
daily expiries:

* The scan read the local vol at every expiry ± 2e-4 years and at the
  midpoints, over the strike region of the next expiry only, and never at
  an expiry itself. A simulation's time grid steps exactly on the
  expiries, and the time stencil of a read there (t ± 1e-4) reaches into
  both neighbouring intervals.
* The per-expiry local-vol strike ranges (`BlackVolSpec.local_vol_strike_min`
  / `_max`, `SviSlice.strike_min` / `strike_max` with
  `local_vol_strike_range: PerExpiry`) were applied one at a time: at time
  `t`, the range of the first expiry at or after `t`. When an expiry's
  range reached lower than the next one's (one daily expiry quoted down
  to a strike the next was not), a step landing on the first expiry read
  a level inside its range, and the stencil evaluated the next slice
  there, on its unquoted wing. Where that wing crossed under the earlier
  slice the variance decreased in time and QuantLib refused; the scan,
  reading the next expiry's range just after the node, never saw it.

**The region rule.** A per-expiry range still applies to the interval
that its expiry closes (between two expiries the later one's: the range
over which `/calibrate-vol-surface` orders both slices). Within 2e-4 years
of an expiry (twice the stencil) the clamp is now the intersection of the
ranges of the two intervals around it, so the edge cannot change inside
the stencil of a read and a step landing on an expiry never reads the
next slice beyond the next range (at the time actually read: `T_last −
0.001` with `FlatLocalVol`). Ranges that do not intersect fall back to the
range of the interval the time lies in. The rule lives where the note
reads the surface, so it applies the same way to the ranges
`/calibrate-vol-surface` writes (the per-expiry regions, unchanged) and to
ranges a caller supplies. Two alternatives were tried and not taken:
widening each range to its neighbours' reads a slice further out on its
unchecked wing, exactly where the refusal came from; intersecting the
ranges of both bracketing expiries at every time throws away the later
expiry's checked wing over whole intervals (a deep put of the synthetic
NDX test sheet moved by about 3 %).

**The scan.** The expiry itself is now one of the scan times (next to
± 2e-4, the midpoints and `T_last − 0.001`). At each time the 201 strikes
span the region of the next expiry together with the following one's
within 2e-4 of an expiry, and when the local vol is clamped the scan adds
the clamp's two edges at that time and the strikes 1e-3 in log-strike
either side of each (up to 6 more points per time:
`local_vol_check.points` and the rows of `local_vol_grid` grow
accordingly; `strikes` stays 201). A payload read unclamped is scanned
over regions no longer capped at the quoted strikes. `clamp_edges` entries
with scope `PerExpiry` report the edges a step at that expiry reads at
(its range intersected with the next one's). A test runs the note's own
local vol reads (its time grid and levels across and around the edges)
through the same object and requires every refusal there to be a refusal
of the scan.

**eSSVI unclamped.** The eSSVI formula is valid at every strike and its
no-arbitrage conditions are checked when the payload is read, so a strike
clamp only changed the model. The `SsviVolSpec` that `/calibrate-vol-surface`
returns no longer carries `strike_min` / `strike_max` on its slices nor
`local_vol_strike_range: PerExpiry`, and its `arbitrage_free_ssvi` is
checked unclamped; `clamp_edges` is empty for it. Ranges a caller puts on
an `SsviVolSpec` are still honoured (by the rule above).

**What may change for an existing request.** A spec without per-expiry
ranges reads exactly as before (bit for bit). A spec with them (the grid
and SVI payloads `/calibrate-vol-surface` returns since 0.16.5, or a
caller's) is clamped, within 2e-4 years of each expiry, to the
intersection of the two ranges around it: a step on an expiry beyond the
narrower of the two now reads the local vol at that edge, so a note's NPV
can move slightly, and a note refused at an expiry before may now price.
An eSSVI payload saved from an earlier
`/calibrate-vol-surface` still carries its ranges and is read with them;
recalibrate, or remove them, to read it unclamped. The scan is stricter:
a surface that passed before can now report refusals where a note would
have refused (its `arbitrage_free` flags follow), and the SVI refit loop
of `/calibrate-vol-surface` sees the same scan, so its slices can differ
slightly where it now refits.

## Version 0.16.8 (November 2026) — container build: one verified Envoy binary

`v0.16.8` changes nothing in the engine, the API or the wire: every 0.16.7
request returns the same result. It fixes how the container image is built.

The `production` stage of the Dockerfile downloaded Envoy a second time,
independently of the `deps` stage that downloads and checks it, with a
`curl` that saved whatever the server answered. On 2026-11-09 the release
CDN answered a 92-byte "504 Gateway Time-out" page and the published
`0.16.6` image shipped it as `/usr/local/bin/envoy`; the container exited
at start with "Exec format error". The production stage now copies the
binary the `deps` stage verified (`COPY --from=deps`) and runs `envoy
--version` again; the download itself fails on an HTTP error (`curl -f`)
and retries a transient one. The `0.16.6` and `0.16.7` images were rebuilt
from their own tags and checked by running them.

## Version 0.16.7 (November 2026) — surface validation as its own endpoint

`v0.16.7` separates checking a volatility surface from building it and from
pricing with it. A new endpoint, `POST /validate-vol-surface`, tests any
equity Black vol surface against a sheet of listed option quotes; the fit
statistics, the full arbitrage checks and the Monte Carlo repricing that
`/calibrate-vol-surface` and `/price-structured-note` used to carry move
there. **This changes the responses of two endpoints and a request of one**
(the 0.x rule: documented here, with the migration). QuantLib stays at 1.44;
no price, surface or fitted parameter changes.

Migration in one line: read the fit and the arbitrage checks from
`/validate-vol-surface` (the returned payload in `pricing`, the same sheet,
the calibrate response's `forwards` pasted); the calibrate response keeps a
pass/fail per payload (`arbitrage_free`, `arbitrage_free_svi`,
`arbitrage_free_ssvi`); the note's `diagnostics.vanilla_repricing` is
`/validate-vol-surface` with `checks.simulation`.

**`POST /validate-vol-surface`.** Request: `pricing` (curves, the
underlying's spot and dividend curve, the surface in
`volatility.vol_surfaces`), `volatility_id`, `underlying_id`,
`discounting_curve`, `quotes` (an `OptionQuoteSheet`), optional `filter`
(the calibrate endpoint's rules), optional `forwards` (an implied-forwards
response: the sheet's forwards instead of the curves'), `checks` (`fit`
and `arbitrage` on by default, `simulation` off), `simulation` (a
`MonteCarloSpec`, required iff `checks.simulation`), optional `bands`
(moneyness and maturity edges of the summary table). Response:
`inputs_echo`, `expiries` (every sheet expiry with its forward, discount
factor, quotes used and dropped), `fit` (per quote the Black-76 price at
the surface's own vol, inside the bid/ask, vol and price errors; a table by
moneyness band x maturity bucket; the at-the-money error per expiry;
totals; a verdict; per quote also `bid_vol` / `ask_vol`, the spread in vol
units), `arbitrage` (the local vol scan through the structured note's own
code path, `LocalVolCheck`; the slice conditions of an SVI or SSVI payload;
`clamp_edges`, the strike clamps the note applies when reading the surface;
with `checks.arbitrage_grid` every scanned point as `local_vol_grid`; a
verdict), `simulation` (per quote the Monte Carlo price on
the surface's local vol with the note's path engine, its standard error and
implied vol, within the noise of the bid/ask; the same table; a verdict) and
`timing`. One implementation of each check
(`src/evaluators/vol_surface_validation_evaluator.cpp`), run by the
calibrate endpoint as well on the surfaces it returns. Details in
`docs/http-api.md`, "Validating a surface"; examples in
`examples/data/vol_surface_validation/`.

Verified against 0.16.6 on the synthetic Scotia sheets (all three methods):
the fit equals the former `vanilla_repricing` quote for quote; the local
vol check of the SVI-method grid, the SVI and the SSVI payloads and the
eSSVI check equal the former `local_vol_check*` / `ssvi_arbitrage` blocks
(counts exactly, the smallest local variance to the JSON print of the
surface); the simulation equals the former note diagnostic of a
one-underlying note whose observation dates are sheet expiries, option for
option (same seed, paths and grid). On an Andreasen-Huge grid the scan
regions are now read from the spec, whose strike axis is the model's own,
instead of the calibration's output range: the scan covers a slightly
different set of points (29 refusals instead of 33 on the Scotia NDX sheet;
the verdict, not arbitrage free, is the same).

**`/calibrate-vol-surface` response, field by field.** Rule: what
describes how the surface was built stays; what measures how good it is
moves to `/validate-vol-surface`.

| Field | 0.16.7 | Where it is now |
|---|---|---|
| `vol_surface_spec`, `vol_surface_spec_svi`, `vol_surface_spec_svi_note`, `vol_surface_spec_ssvi`, `forwards`, `forwards_source`, `filter`, `spot`, `method`, grid axes | kept | |
| `expiries[]`: `expiry`, `time`, `usable`, `dropped_reason`, `forward`, `implied_discount_factor`, `quotes_used`, `quotes_dropped`, `dropped`, `svi`, `ssvi` | kept | |
| `expiries[]`: `butterfly_action`, `calendar_refit`, `calendar_refit_max_vol_shift`, `calendar_fit_action`, `calendar_violations`, `calendar_max_clip_vol`, `calendar_action`, `grid_points_filled`, `ssvi_anchor_k`, `ssvi_anchor_total_variance`, `ssvi_anchor_clamped` | kept (how the fit and the grid were built) | |
| `expiries[].rmse_vol`, `max_abs_error_vol` | removed | `fit.bands`, `fit.atm_error_per_expiry`, `fit.quotes[].vol_error` (the fit of the returned surface rather than of the internal object) |
| `expiries[].butterfly_min_g`, `butterfly_violation` | removed | `arbitrage.svi_slices[].butterfly_min_g`, `wing_slope`; `butterfly_action` keeps what the fit found and did |
| `expiries[].calendar_min_margin`, `calendar_min_forward_vol` | removed | `arbitrage.svi_slices[].calendar_min_dw`, `calendar_min_forward_vol` |
| `expiries[].ssvi_margin_*` | removed | `arbitrage.ssvi_slices[].margin_*` |
| `andreasen_huge_error` | removed | `fit` (against the returned grid) |
| `vanilla_repricing` | removed | `fit` (`inside_bid_ask` → `inside_spread`, `count` / `inside_bid_ask` → `count` / `inside`) |
| `local_vol_check`, `local_vol_check_svi`, `local_vol_check_ssvi` | removed | `arbitrage.local_vol_check` (validating the payload); the field `action` is gone |
| `ssvi_arbitrage` | removed | `arbitrage.ssvi_check` |
| `arbitrage_free`, `arbitrage_free_svi`, `arbitrage_free_ssvi` | new | the `arbitrage_free` of the former blocks (for SSVI also the slices' closed-form conditions) |
| `svi_local_vol_refit_action` | new | the former `local_vol_check_svi.action` (the refit passes the check made the SVI fit do) |

**`/price-structured-note`.** `diagnostics.vanilla_repricing`,
`diagnostics.vanilla_repricing_sheets` and
`diagnostics.vanilla_repricing_filter` are removed from the request (a
request carrying them is a `400` from the JSON parser), and the
`vanilla_repricing` block (with `StructuredNoteVanillaRepricing`,
`…Underlying`, `StructuredNoteRepricingExpiry`,
`StructuredNoteRepricedOption`) from the response, and the
`vanilla_repricing` timing stage. Migration: `/validate-vol-surface` with
`checks.simulation` and the note's `monte_carlo` per underlying and sheet,
on the note's market; the former `note_npv` (the note walked on the
repricing paths) has no successor. The two Scotia synthetic-surface examples
no longer carry the sheets.

Also: `examples/structured_note_workflow/price_note_from_quotes.py` runs
the validation between the calibration and the pricing (and accepts
`--method Ssvi`; `--no-repricing` is now `--no-simulation`); the Python
client gains `validate_vol_surface`; the C++ client `ValidateVolSurface` /
`ValidateVolSurfaceJSON`.

## Version 0.16.6 (November 2026) — eSSVI surfaces (contributed)

`v0.16.6` is additive on the wire: a new vol payload, a new fit method and
new response fields; every 0.16.5 request is a valid 0.16.6 request and
returns the same result. QuantLib stays at 1.44. The eSSVI mathematics —
the slice parametrisation and its arbitrage conditions, the calibrator and
the closed-form local vol — were contributed by @AlvaroRojas (#156) and
integrated into the existing endpoints.

**`SsviVolSpec`: eSSVI slices as a Black vol.** A new `VolPayload` member
(reported shape `SsviSlices`): `base` (conventions only), `spot`, and per
slice `expiry`, `forward`, `theta` (at-the-money total variance), `rho`,
`psi = theta phi`, optional `strike_min` / `strike_max`; optional
`local_vol_time_extrapolation` and `local_vol_strike_range` as on
`SviVolSpec`. Total variance `w(k) = (theta + rho psi k + sqrt(psi^2 k^2 +
2 rho psi theta k + theta^2)) / 2` at `k = ln(K / F_ref(t))`, with theta,
psi and rho psi linear in time between slices (Corbetta, Cohort, Laachir,
Martini 2019), scaled before the first, theta continuing the last forward
variance after the last; `ln F_ref` linear in time through the spot and the
slice forwards, so the payload needs no curve. The slices are checked for
static arbitrage when the payload is read (butterfly per slice, calendar
between slices including a sufficient condition, a numeric scan; a `400`
lists every violation). Accepted wherever a Black vol is: equity options,
`/sample-vol-surfaces`, structured notes. Beyond the last slice the local
vol is the model's own continuation when `local_vol_time_extrapolation` is
absent; `FlatLocalVol` holds it as for the other payloads; `FlatImpliedVol`
is a `400` (not an eSSVI reading).

**Closed-form local vol in structured notes.** A note on an `SsviVolSpec`
simulates the analytic Dupire local variance of the slices, `(dw/dt +
dw/dk Δμ) / g(k, k − ln(F_note / F_ref))`, where `Δμ` corrects for a note
forward that differs from the slices' reference forward (a quanto drift,
another dividend curve, a bumped spot); no finite-difference stencil. The
strike edges, per-expiry bands, `FlatLocalVol` and `local_vol_floor` apply
as for the other surfaces (the floor reads the closed form and floors a
refused point). The vega bump shifts every slice's at-the-money vol and
keeps rho and psi, so the bumped run is eSSVI too (`400` if the bumped
slices admit arbitrage). The closed form agrees with QuantLib's
`LocalVolSurface` on the same surface to the stencil's accuracy (1e-4
relative, gtest). The Python reference evaluates the same formula: a note
on the example payload is reproduced bit for bit (catalog).

**`/calibrate-vol-surface` method `Ssvi`.** New `VolSurfaceFitMethod.Ssvi`
and request table `ssvi` (`weighting`: `Uniform`, `Vega` (default) or
`BidAsk`; `max_abs_log_moneyness`, default `1.0`). The same used quotes,
forwards and discount factors as the other methods go into the eSSVI
calibrator, fitted short to long: the quote nearest the money reproduced
exactly, `(rho, psi)` under the butterfly and calendar conditions, so the
slices are free of static arbitrage by construction. When the quotes
cross at the money between two expiries (a daily expiry quoted below the
day before), the anchor's total variance is raised to the smallest
feasible value (`ssvi_anchor_clamped`). New response fields:
`vol_surface_spec_ssvi` (the slices as an `SsviVolSpec` with the regions as
per-expiry strike ranges), `ssvi_arbitrage` (conditions and scan),
`local_vol_check_ssvi` (the 0.16.5 scan on the closed-form local vol); per
expiry `ssvi`, `ssvi_anchor_k`, `ssvi_anchor_total_variance`,
`ssvi_anchor_clamped` and the condition margins `ssvi_margin_*`. The grid
(`vol_surface_spec`) is sampled from the eSSVI surface and may use any
expiry up to the last fitted one. Three parameters per expiry fit a dense
index sheet less closely than SVI's five; the method trades fit for a
surface free of arbitrage everywhere and a local vol in closed form. On the
synthetic sheets (examples `vol_surface/synthetic_svi_sheet_2026-09-30_ssvi.json`
and `vol_surface/synthetic_spx_essvi_sheet_2026-10-21_ssvi.json`, generated
from eSSVI surfaces) the fit returns the generating slices to 1e-6; on the
daily-expiry sheet three slices are raised at the money and the note
`structured_note/synthetic_spx_1y_autocall_on_daily_expiry_ssvi.json`
prices on the returned payload at 983.07 ± 1.75 (983.76 ± 1.73 on the SVI
payload, same paths). Example `equity/eqopt_eur_call_on_ssvi_slices.json`:
a European call between two slices.

**Not ported from the contribution.** Its separate calibration endpoint
and the quote-calibrated vol shape (calibration lives in
`/calibrate-vol-surface`), its calibration cache, and three structured-note
behaviour changes (a `400` on discrete dividends, a pre-simulation scan of
strike x expiry surfaces, a version string in a message), to be considered
separately.

## Version 0.16.5 (November 2026) — calendar-consistent SVI surfaces, local vol checked as the note reads it

`v0.16.5` is additive on the wire: new optional request fields, new
response fields, one new enum; no field changes meaning for a request that
does not use them, and every 0.16.4 request is a valid 0.16.5 request.
`/calibrate-vol-surface` returns different surfaces where the calendar
binds (most of a sheet with daily or weekly expiries), and its SVI slices
carry a different strike range (below): say so plainly, recalibrate rather
than reuse a 0.16.4 surface for a note. Structured notes, equity options
and every other endpoint price as before on the same inputs. QuantLib stays
at 1.44.

**What was wrong.** With `SviPerExpiry` every expiry was fitted on its own.
On a Cboe SPX sheet of 47 expiries (daily, weekly, monthly) the fitted
slices were not ordered in time — the 19-day slice 12 % below the 18-day
one in total variance at the money, 30 of 46 adjacent pairs crossing
somewhere between 55 % and 180 % of the spot — although the quotes
themselves are ordered on every strike two expiries share. The grid's
calendar clip lifted the later expiry to the earlier one's total variance
+ 1e-8 at the 41 grid strikes only (566 of 1,886 adjacent node pairs ended
equal), the slices still crossed between nodes and in the SVI payload
(not clipped at all), and a structured note on either returned spec was a
400 ("decreasing variance at strike ... between time t and t+1e-4", or,
next to a flattened node, "negative local vol^2 ... not smooth enough").

**Calendar-consistent SVI fit.** The slices are fitted from the last
expiry back, each held under the next one on 401 strikes over the region a
simulation reaches around the expiry pair (5 standard deviations of the
log-forward with the market's implied vols along the way, and the strikes
both expiries quote). The condition is the one QuantLib's local vol needs
between two expiries — total variance interpolated linearly in time at
fixed strike, differentiated along the forward — at both ends of the
interval: `w_i − w_{i−1} + ln(F_i / F_{i−1}) s ≥ min_forward_vol² (T_i −
T_{i−1})` with `s = dw/d ln K` of either slice. A slice that breaks it is
refitted with a calendar penalty together with the butterfly penalty, then
lowered by any excess left, so the condition holds at every point. New
request field `svi.min_forward_vol` (default `0.01`, `0` = no margin,
finite and `≥ 0`). New report fields per expiry: `calendar_refit`,
`calendar_refit_max_vol_shift`, `calendar_min_margin`,
`calendar_min_forward_vol`, `calendar_fit_action`. The butterfly check now
covers the regions too and also flags a wing slope `b (1 + |ρ|)` above
Roger Lee's bound 2 (QuantLib's fit admits up to 4; such slices are
refitted within the bound). The grid's calendar clip compares at the same
forward moneyness, inside the region only, and stays a last resort: after
the fit it does not fire on the SPX sheet. `enforce_calendar: false` keeps
the independent fits.

**Local vol checked on the continuous object.** New response blocks
`local_vol_check` (the grid) and `local_vol_check_svi` (the SVI payload):
each spec is rebuilt as the note pricer builds it and QuantLib's
`LocalVolSurface` is evaluated at every expiry ± 2e-4, the midpoints and
the last expiry − 1e-3, on 201 strikes per time over the region; reported:
points, refusals (and the first message), the smallest local variance and
where, `arbitrage_free` (no refusal). If the SVI payload fails, the slices
involved are refitted with a larger margin or butterfly weight (at most 3
passes, in `action`). On the SPX sheet both specs pass without a pass; the
SVI fit reprices 2,573 of 10,176 quotes inside their bid/ask (independent
fits: 3,955; 0.16.4's clipped grid: 3,669) with the largest price error
19.4 points (0.16.4: 181); the loss is on the short-dated deep calls quoted
at the minimum tick.

**Per-expiry local vol strike ranges.** So that a note never reads the
surface where nothing was checked, the returned specs carry the regions:
`BlackVolSpec.local_vol_strike_min` / `local_vol_strike_max` (new, optional,
one entry per expiry, `SmileCube3D` only) and, on `SviVolSpec`, the new enum
field `local_vol_strike_range` (`LocalVolStrikeRange { Global, PerExpiry
}`, absent = `Global`, the 0.16.4 reading) set to `PerExpiry` with each
slice's `strike_min` / `strike_max` = its region (in 0.16.4: the strikes
of its quotes, read as one range). At time `t` a note's local vol is read
at the simulated level clamped to the range of the first expiry at or
after `t`. Absent fields: the reading is unchanged.

**Andreasen-Huge.** The returned grid's strikes are now the model's own
finite-difference points within the output range and the regions (344 on
the SPX sheet), so the cubic reading no longer re-interpolates 41 samples;
`strikes_as_moneyness` echoes them. Its continuous check fails on the SPX
sheet (867 of 28,542 points refused: the model itself misses the sheet by
5.7 vol points on average with 47 expiries) and is reported as such: use
`SviPerExpiry` for structured notes.

**Local vol floor (opt-in).** New `MonteCarloSpec.local_vol_floor`
(absent = today: a refused read is a 400). Present and `> 0` (only with a
local-vol underlying, `400` otherwise): the local vol simulated is
`max(Dupire, floor)` and a read QuantLib would refuse takes the floor. The
Dupire formula is QuantLib's `localVolImpl` step by step, bit-identical to
`LocalVolSurface` where no floor binds (gtest). The model echo reports per
underlying `local_vol_floor`, `local_vol_reads`,
`local_vol_floored_fraction` and `local_vol_refusals_floored`; the terms
echo the floor. A floored fraction above about 1e-3 means the surface is
not usable: recalibrate. The four refused SPX requests of the 0.16.4
surfaces price with a 5 % floor at floored fractions of 1.0e-3 to 2.7e-2;
the note on the 0.16.5 surfaces of the same sheet prices without a floor
(991.92 ± 0.47 on the grid, 991.89 ± 0.47 on the SVI payload).

**Not changed, documented.** The check uses the sheet's forwards; a note
priced on another forward drift `Δμ` adds `Δμ · s` to the local vol's time
derivative, so price on the dividend curve `/calibrate-implied-forwards`
returns (or raise `min_forward_vol`). Example:
`structured_note/synthetic_spx_1y_autocall_on_daily_expiry_svi.json` on
the sheet `vol_surface/synthetic_spx_daily_expiries_sheet_svi.json`
(new generator preset `spx_daily`: daily expiries with jittered
at-the-money vols, 1 % noise and a minimum price): a 400 on the 0.16.4
calibration, prices on the 0.16.5 one.

## Version 0.16.4 (November 2026) — local vol beyond the quoted surface, configurable time limits

`v0.16.4` is additive: new optional fields and new runtime knobs, no
existing request changes. Every request that priced on 0.16.3 returns the
same result; a client has nothing to do. The responses of
`/calibrate-vol-surface` carry new fields (below), which a structured note
reads when the specs are pasted as returned. QuantLib stays at 1.44.

**Local vol beyond the last expiry: `local_vol_time_extrapolation`.** A
strike-dependent surface (a `CubicStrikeLinearTime` grid, an `SviVolSpec`)
extends its implied vol flat in time beyond its last expiry. With a skew
that is not arbitrage-free at long horizons: with total variance
`w = v(k) t` the `−w′²/16` term of Durrleman's condition grows like `t²`,
the Dupire local variance turns negative, and a note maturing years after
the last quoted expiry was a 400 ("negative local vol^2 at strike ... and
time ..."). New enum `LocalVolTimeExtrapolation { FlatImpliedVol,
FlatLocalVol }` and an optional field `local_vol_time_extrapolation` on
`BlackVolSpec` and `SviVolSpec`:

- absent or `FlatImpliedVol`: today's behaviour;
- `FlatLocalVol`: the local vol a structured note simulates is held
  time-homogeneous beyond the last expiry, read at
  `min(t, T_last − 0.001)` years at the simulated level. The 0.001-year
  margin keeps QuantLib's `LocalVolSurface` time stencil (`t ± dt`,
  `dt ≤ 1e-4`) inside the quoted expiries. Before `T_last − 0.001` the
  local vol is unchanged, bit for bit;
- the Black vol itself (vanilla options, diagnostics) is unchanged: flat
  implied vol beyond the last expiry under both;
- `/calibrate-vol-surface` sets `FlatLocalVol` on both specs it returns
  (`vol_surface_spec` and `vol_surface_spec_svi`), so a calibrated surface
  prices a long note as returned. On a two-asset note running to five
  years on surfaces that end after 1.2 years, the request goes from a 400 to
  954.05 ± 7.56 (example
  `structured_note/synthetic_ndx_rty_5y_autocall_on_svi_flat_local_vol.json`).

**SVI wings beyond the quotes: `strike_min` / `strike_max`.** A reported
note on an SVI surface (both arbitrage options on) failed the same check
at 132 % of the spot, inside the expiry range. The cause, reproduced on a
synthetic SPX-like sheet quoted from 60 % to 115 % of the spot: the
calibrator checked and enforced Durrleman's `g ≥ 0` on its output strike
range only (50 % to 150 % by default), while an SVI slice is global in
strike and a structured note read its raw wing wherever a path went. The
fitted slices were clean on the output range and not beyond it (the
2028-06-16 slice: `min g −0.042` at `k = −1.18`), and a two-year note was a
400 at 30 % of the spot at `t = 1.63` years. Changes:

- `SviSlice` gets optional `strike_min` / `strike_max` (absolute strikes,
  `> 0`, `strike_min < strike_max`, 400 otherwise);
  `/calibrate-vol-surface` fills them with the strike range of each
  slice's used quotes;
- a structured note clamps the local vol of an SVI payload to
  `[min strike_min, max strike_max]` over the slices exactly as it does
  for a grid (the edge local vol beyond); a slice without one leaves that
  side open; absent on every slice = no clamp, today's behaviour. No Black
  vol read changes. The reproduction now prices (example
  `structured_note/synthetic_spx_2y_autocall_on_svi_quoted_strike_edges.json`,
  sheet `vol_surface/synthetic_spx_steep_wings_sheet_svi.json`);
- the calibrator also scans each slice beyond its output range out to
  `k = ±5 σ_ATM √T` and REPORTS a negative `g` there in `butterfly_action`
  (appended text); `butterfly_min_g` and `butterfly_violation` keep
  covering the output range only, and nothing beyond it is enforced.

Not changed, documented: the slices are not calendar-clipped (only the
grid is), so two fitted slices whose unquoted wings cross are a calendar
arbitrage the slices keep. On a sheet with a strong call-wing upturn the
2026-12-18 and 2027-03-19 slices cross inside the quoted range and a note
is a 400 "decreasing variance at strike 5126.23 between time 0.444444 and
time 0.444544"; the fix is on the quotes or the fit, not in the
simulation.

**Request time limits are configurable.** A long request (a calibration on
thousands of quotes, a 65,536-path local-vol note at 252 steps per year)
hit the gateway's fixed 10 s deadline. New knobs, documented in
`configuration.md` ("Which limit binds"):

- `QUANTRA_GATEWAY_DEADLINE_MS` (gateway, default 10000): the deadline the
  JSON gateway sets on every call to the engine; read once at startup and
  logged; an invalid value logs a line and keeps the default;
- `QUANTRA_ROUTE_TIMEOUT_MS` (process manager and
  `scripts/envoy_config.py`, default 30000): Envoy's route timeout. For gRPC
  traffic Envoy applies the caller's deadline capped at
  `grpc_timeout_header_max` instead of the route timeout, so that cap is
  now `max(60 s, route timeout)`; the generated config is unchanged at the
  defaults.

The smallest of the Envoy cap, the gateway deadline and the engine budget
(`QUANTRA_REQUEST_BUDGET_MS`) wins; for a long batch raise the gateway
deadline and the engine budget together (and the Envoy cap past 60 s).

## Version 0.16.3 (October 2026) — discount factors above one (negative rates)

`v0.16.3` is a PATCH release: no request or response shape changes and no
change to the binary or JSON wire. Every request that priced on 0.16.2
returns the same result; the only change is that requests that were
rejected now price. QuantLib stays at 1.44.

**Discount-factor points above 1 are accepted.** The points of an
`InterpolatedDiscount` curve (`DiscountFactorPoint.discount_factor`) were
limited to `(0, 1]`. Under negative rates discount factors are above 1, so
such curves were rejected with a 400, including the discount factors that
`/bootstrap-curves` returns for a negative-rate curve when they are sent
back as points. QuantLib's `InterpolatedDiscountCurve` requires only a
positive discount factor and a discount factor of exactly 1.0 on the first
(reference-date) point. Both checks remain, each a 400 naming the field;
the upper bound is gone, as it always was for zero-rate and forward-rate
points. Reported by a user building negative-rate curves.

## Version 0.16.2 (October 2026) — multi-curve Hull-White, CMS on smile and SABR surfaces

`v0.16.2` is a PATCH release: fixes only, no request or response shape
changes, no change to the binary or JSON wire. Every 0.16.1 request is a
valid 0.16.2 request. Outputs change only where they were wrong; each change
is listed below. QuantLib stays at 1.44.

**Hull-White honours the forwarding curve.** QuantLib's tree and Jamshidian
swaption engines are single-curve: the Hull-White model is fitted to the
discount curve and floating coupons are valued off it, so the index's
forwarding curve was ignored. A 20 bp basis left a Bermudan price unchanged,
and calibration distorted sigma to absorb it.

- When a swaption's floating or overnight leg forwards on a different curve
  id than it discounts on, pricing now uses QuantLib's
  `FdHullWhiteSwaptionEngine`, which projects the index on its own curve
  with the same a and sigma. Its time grid is `lattice_steps`; the other
  grid settings are QuantLib's defaults.
- Hull-White calibration prices its helpers with the same engine when
  `forwarding_curve_id` differs from `discount_curve_id`.
- Requests whose curve ids are the same are unchanged (byte-identical).
  Multi-curve requests change: a Bermudan under a 20 bp basis goes from
  25,238 to 29,707, equal to QuantLib-Python's FD engine.
- Where QuantLib's FD engine cannot price correctly, the multi-curve case is
  a 400 naming the field, never a single-curve price:
  - forwarding and discount curves with different day counters or
    reference dates;
  - American exercise;
  - cash settlement with `ParYieldCurve`;
  - an overnight lookback that observes rates before an exercise date.
- Multi-curve calibration is 10–20 times slower than Jamshidian, and large
  joint `(a, sigma)` fits can exceed the gateway's 10 s deadline.

Details in `docs/http-api.md`, "Hull-White engines and the forwarding
curve".

**CMS legs on spread-from-ATM cubes and SABR surfaces price.** These vol
kinds build their QuantLib structure only once the server's ATM forwards are
injected. The CMS coupon path skipped that step, so such legs failed with an
internal "swaption vol handle is empty" error (422). The CMS path now runs
the same step as swaptions, on the vol's swap index and the leg's discounting
and forwarding curves. Constant, ATM-matrix and absolute-strike vols are
unchanged.

**Hull-White calibration cache key.** QuantLib's `SwaptionHelper` starts the
underlying swap at the IBOR index's fixing days after exercise. Those fixing
days were not in the cache key, so two calibrations differing only in them
shared an entry. The key now includes the fixing days, and whether the index
is an overnight index (key version v3).

**Quanto correlation convention documented correctly (no price change).**
`QuantoSpec.fx_underlying_correlation` has always been used in QuantLib's
convention: the correlation with the FX rate quoted as note-currency units
per one unit of the underlying's currency (USD per CHF for a CHF index in a
USD note), giving the drift `r_local − q − ρ·σS·σFX`. The documentation
described the inverse quote and said the FX spot was inverted, which it is
not. A correlation measured against the inverse quote must be entered with
its sign flipped.

## Version 0.16.1 (October 2026) — fixes from a full code review

`v0.16.1` is a PATCH release: fixes only, no request or response shape
changes, no change to the binary or JSON wire. Every 0.16.0 request is a
valid 0.16.1 request. Outputs change only where they were wrong; each change
is listed below. QuantLib stays at 1.44.

**Pricing**

- Swaption smile-cube and SABR surfaces extend the edge implied vol flat
  outside the quoted expiries. The edge total variance was divided by the
  query time, so vol grew as √(T_edge/t) before the first expiry and fell
  after the last (0.69 instead of 0.20 at one month on a flat 20% cube).
  Prices of swaptions and CMS fixings outside the expiry grid change; reads
  inside the grid are unchanged.
- LinearTsr CMS coupons on absolute-strike smile cubes use the swap rate as
  the forward. The cube's smile sections reported the middle grid strike as
  the ATM level, so the price depended on how the strike grid was laid out.
- Inflation swaps and YoY caps/floors price on the curve the trade
  references. With several curves on one index, every trade priced on the
  first curve by id. Index specs that shared a QuantLib name (the same
  family name in one region, for example CAD and JPY `CPI`) no longer share
  one fixing series. QuantLib error texts name inflation indices by spec id,
  and `family_name` is descriptive only.
- Equity options with discrete dividends: `implied_volatility` accounts for
  the dividends (the discrete-dividend example goes from 0.1297 to 0.2000).
- Business/252 year fractions no longer depend on whether the worker earlier
  served a request with Brazil holiday overrides.

**Requests that failed and now price**

- A seasoned CDS (upfront payment date passed) prices. `fair_upfront` is
  omitted when QuantLib has none (binary readers see 0.0).
- A floating-rate bond with `bond_pricing_flows` and a past fixing of 1.0 or
  more prices; it returned 500.

**Requests now rejected with a 400 that names the problem**

- `FixedRateBond` without `issue_date`.
- A YoY index spec whose `underlying_zero_index_id` is not a zero index,
  including self-references and cycles.
- A request body that is not a single JSON object: leading schema
  statements, and comments anywhere (comments are not JSON). The message
  names the line and column.

**Results no longer depend on earlier requests on the same worker**

- `/bootstrap-curves`, `/bootstrap-inflation-curves` and
  `/sample-vol-surfaces` no longer change whether later requests extrapolate
  on cached curves or SABR cubes.
- The calibrated SABR cube cache key includes the surface's calendar,
  business-day convention and day counter.
- The curve cache holds only helper-free copies of bootstrapped curves,
  exact at every node, so cached curves are never re-dated by other
  requests. A request whose as-of differs from the previous one no longer
  pays for re-dating every cached curve. With the cache on, a curve that
  fails to bootstrap on `/bootstrap-curves` returns a per-query error, as
  with the cache off.
- A failure while restoring the evaluation date or clearing fixings at the
  end of a request is logged instead of stopping the worker. Between
  requests the evaluation date returns to unset.

**Responses**

- Bond flows (`bond_pricing_flows`): floating coupons carry their
  `fixing_date` (ISO date), and paid fixed-rate coupons carry their `rate`.

**Operations and documentation**

- The JSON gateway sets `TCP_NODELAY`: POSTs on kept-alive connections no
  longer wait about 40 ms for a delayed ACK.
- Envoy's least-request balancing compares `4 × workers` candidates instead
  of two, so light requests rarely queue behind heavy ones.
- `FRAType.Long` is documented as paying the fixed rate (QuantLib's
  convention, which the engine always used; the comment said the opposite),
  and `FRAResponse.spot_value` as equal to `npv`.
- The Python client takes its version from `VERSION`, and its bond example
  runs again. Dead code (`EngineFactory` and helpers without callers) and a
  per-request debug print are removed.

## Version 0.16.0 (October 2026) — correlation as an input you can reproduce

`v0.16.0` is a backward-compatible MINOR release. No existing request
changes shape or value unless it opts in. It closes the market-data chain
for basket products: the correlation matrix a structured note runs on can
now be estimated by the engine from price histories, reproducibly, and
shifted on the note by the desk.

- `POST /calibrate-correlation` (additive): a `CorrelationMatrixSpec` from
  the closes of two or more underlyings (`PriceSeries` of `{date, value}`;
  the engine fetches nothing). Dates aligned by inner join inside a window
  (`window_days` back from `as_of_date`, an explicit `start_date` /
  `end_date`, or every common date), `Daily` or `Weekly` sampling, `Log` or
  `Simple` returns, the sample correlation, shrinkage toward the
  constant-correlation target (`LedoitWolf` default with the Ledoit-Wolf
  2004 intensity clipped to `[0, 1]`, `Fixed`, or `None`), and an
  eigenvalue floor (`SymmetricSchurDecomposition`, clipped and rescaled,
  reported when applied). The response carries the spec, the unshrunk
  `sample_correlation`, the target and intensity, the smallest eigenvalue
  before and after, the observations and dates used, per-series closes
  supplied / dropped / realised vol (Act/252), and `estimator_echo`, the
  settings as resolved, which reproduce the response when posted back.
  New enums `ReturnType`, `ReturnFrequency`, `CorrelationShrinkage`. Python
  reference `calibrate_correlation_ql` (bit-level to `1e-12`), gtests,
  contract tests, two catalog rows on public index closes
  (`examples/data/correlation/`: the NDX / RTY year that reproduces the
  Scotia example's 0.747, and NDX / RTY / SPX with Ledoit-Wolf).
- `StructuredNote.correlation_markup` (additive, optional): a uniform shift
  of every off-diagonal entry of the note's correlation matrix before
  pricing, clipped to `[-1, 1]`, repaired like the Greeks' bump when the
  shift breaks positive semi-definiteness; `terms` echoes
  `correlation_markup` and `correlation_markup_repaired`, the `market`
  diagnostic gains `correlation_base` and `correlation_markup` next to the
  matrix used. The Greeks' correlation bump and the limit checks start from
  the marked-up matrix. `400` outside `[-1, 1]` or on a single-underlying
  note. A request without the field is unchanged.
- Docs and examples: `examples/structured_note_workflow/` walks through
  pricing a structured note at three levels of market data (flat vols;
  price histories → `/calibrate-correlation`, option quotes →
  `/calibrate-implied-forwards` → `/calibrate-vol-surface` →
  `/price-structured-note` with the repricing diagnostic, as a runnable
  Python script with `--closes` and its curl equivalent; pasted surfaces,
  forwards, correlation and funding). The two synthetic Scotia quote sheets
  are files under `examples/data/vol_surface/`; a contract test runs the
  script. `docs/http-api.md` gains "Correlation from price histories".

## Version 0.15.0 (October 2026) — surfaces that the simulation can trust

`v0.15.0` is a backward-compatible MINOR release. No existing request changes
shape or value unless it opts in. It closes the chain quotes → surface →
simulation with an acceptance check the engine runs on itself (the
`vanilla_repricing` diagnostic), and it changes how a calibrated surface is
carried: as a grid read with a cubic spline in strike, or as the fitted SVI
parameters themselves. Also includes the repository layout change (paths
only: `src/decode/{market,products}`, `clients/`, `flatbuffers/grpc/`,
examples in product folders).

- Strike x expiry Black surfaces (`BlackVolSpec` `SmileCube3D` and
  `SurfaceFromPrices`, and the vega-shifted rebuilds) now extrapolate
  CONSTANT in strike on both sides: a strike beyond the grid reads the edge
  strike's vol instead of QuantLib's linear continuation of the total
  variance, which turns negative far from the grid. No wire change; a
  request whose strikes and simulated levels stay inside the grid is
  unchanged. Beyond the last expiry the implied vol is flat (QuantLib's
  own rule, unchanged). Under a structured note the Dupire local vol is
  read at the simulated level clamped to the interior of the strike grid
  (margin 1e-3 in log-strike), so a path beyond the quoted strikes evolves
  with the edge strike's local vol; one local-vol object per distinct
  market is shared by every simulation of the request. The Scotia
  synthetic-surface catalog row moves by the extrapolation policy (paths
  below half the spot).
- `POST /price-structured-note`: `diagnostics.vanilla_repricing` with
  `diagnostics.vanilla_repricing_sheets` (`OptionQuoteSheet`s, one per
  underlying) and optionally `diagnostics.vanilla_repricing_filter`
  (`QuoteFilter`), additive: every usable quote priced as a European option
  on one extra simulation of the note's model (the note's grid plus the
  sheet expiries), compared with its bid/ask, with the implied vols of the
  mid and of the model price; the response gains the optional
  `vanilla_repricing` block (`StructuredNoteVanillaRepricing`) and the
  timing stage `vanilla_repricing`. The headline is unchanged by the flag.
  The Scotia synthetic-surface example now carries the two sheets its
  surfaces were calibrated from (generated on the note's own curves) and
  the flag; the generator gains the `svi_scotia_ndx` / `svi_scotia_rty`
  presets.
- `SurfaceInterpolator2D` gains `CubicStrikeLinearTime` (additive): a
  strike x expiry `BlackVolSpec` read as one natural cubic spline of total
  variance in log-strike per expiry (C2: every node reproduced, the smile's
  curvature kept between nodes), linear in total variance between expiries
  at fixed strike, constant beyond the first and last strike, flat implied
  vol beyond the last expiry. `/calibrate-vol-surface` now returns its grid
  with this interpolator (the only change to its `vol_surface_spec`; its
  `vanilla_repricing` reads the grid the same way). A grid supplied by a
  request keeps `Bilinear` unless it asks: no existing request changes.
  `Bicubic` is kept.
- New `VolPayload` member `SviVolSpec` (additive): a Black vol as raw SVI
  slices (`SviSlice {expiry, forward, a, b, sigma, rho, m}` per expiry,
  `time_interpolation: LinearTotalVariance`, `allow_extrapolation`),
  accepted wherever a Black vol is referenced (`price-equity-option`,
  structured-note underlyings — Dupire local vol with `steps_per_year` —,
  `sample-vol-surfaces`). `VolSurfaceShape` gains `SviSlices` for the
  structured-note `model.underlyings[].vol_shape` echo (a `BlackVolSpec`
  with that shape is a `400`). `/calibrate-vol-surface` with `SviPerExpiry`
  returns the fitted slices as `vol_surface_spec_svi` next to the grid;
  with `AndreasenHuge` the field is absent and `vol_surface_spec_svi_note`
  says why. New example
  `examples/data/structured_note/scotia_..._with_synthetic_svi.json` (the
  Scotia note on the SVI payloads) and catalog row.
- The acceptance check of the chain is met: the vanilla-repricing
  diagnostic on the Scotia synthetic-surface examples has every used quote
  within the Monte Carlo noise of its bid/ask on the `CubicStrikeLinearTime`
  grid and on the SVI payload every option beyond the first expiry with a mid of at least 0.25 % of the spot within 3.5 standard errors of its bid/ask at 2048, 16 384 and 65 536 paths, the mean vol error per expiry within 0.12 vol points from two months on at 65 536 paths; the 16-day expiry keeps a −1 vol point residual from the Euler steps and the deep short wings are at the sampling limit, reported separately. The documented limit of the
  previous bullet is now the `Bilinear` reading only. The Scotia
  synthetic-surface catalog row moves with the regenerated example (its
  surfaces now carry `CubicStrikeLinearTime`); the two 0.14.0 vol-surface
  rows move in their `vanilla_repricing.model_vol` / `model_price` fields
  (the grid is read cubic instead of bilinear).

- Repository layout tidy-up, paths only, no behaviour or wire change:
  `src/parsers/` → `src/decode/market/` and the per-product mappers →
  `src/decode/products/` (step 1 of the pipeline, FlatBuffers → C++, now in
  one folder); `src/common/product_catalog.h` → `src/transport/`; `grpc/` →
  `flatbuffers/grpc/`; `client/cpp/` → `clients/cpp/` and `quantra-python/` →
  `clients/python/`; the loose `examples/data/*.json` requests → their product
  folders (`bonds/`, `ir_swaps/`, `swaption/`, …); `docs/*.jpg` → `docs/img/`.
  Every example, test, catalog row and generated artefact is unchanged in
  content. Scripts that set `PYTHONPATH` to `quantra-python` or `pip install`
  it need the new `clients/python` path.

## Version 0.14.0 (October 2026) — Option quotes to implied forwards and a volatility surface

`v0.14.0` is a MINOR, additive release: two calibration endpoints, one
market-data payload, a converter tool and examples. No existing request or
response changes; every 0.13.1 request is a valid 0.14.0 request with the
same answer. QuantLib stays at 1.44.

**`POST /calibrate-implied-forwards`.** From a sheet of listed European
option quotes (`OptionQuoteSheet`: per line expiry, strike, call bid/ask,
put bid/ask, optional volume and open interest; `QuoteFilter` with
`max_relative_spread`, `min_quotes_per_expiry`, `moneyness_window`,
`min_open_interest`) the per-expiry weighted least-squares put-call-parity
fit gives the implied forward, the implied discount factor (the index's
funding), the funding spread against the request's discounting curve in bp,
the continuous dividend yield and the fit residuals, every dropped line
with its reason, and a ready-to-paste `InterpolatedZero` dividend curve
that reproduces the forwards exactly. `docs/http-api.md`, "Implied forwards
and dividends from option quotes".

**`POST /calibrate-vol-surface`.** The same sheet (plus, optionally, the
pasted forwards of the first endpoint) to a Black implied-vol surface on a
moneyness x expiry grid converted to absolute strikes with the spot,
returned as a complete `VolSurfaceSpec` (`BlackVolSpec`, `SmileCube3D`,
NullCalendar / Unadjusted / Act/365F, calendar-day expiries, bilinear,
extrapolation allowed) for `pricing.volatility.vol_surfaces`. Two methods
(`VolSurfaceFitMethod`): `AndreasenHuge` (QuantLib
`AndreasenHugeVolatilityInterpl`, arbitrage-free by construction) and
`SviPerExpiry` (QuantLib `SviInterpolation` per expiry, Durrleman butterfly
check with a penalized refit, calendar check with a clip). The response
carries the forwards used, a per-expiry report (quotes used and dropped
with reasons, RMSE and max error in vol units, SVI parameters, arbitrage
findings and actions) and the vanilla repricing of every used quote on the
returned surface (inside the bid/ask or not, largest price and vol
excursions). `docs/http-api.md`, "Volatility surface from option quotes".

**Tools and examples.** `tools/option_quotes/cboe_eod_to_quote_sheet.py`
converts a Cboe DataShop end-of-day file into a quote sheet (vendor data is
never committed); `tests/tools/make_synthetic_quote_sheet.py` generates the
committed synthetic sheets (polynomial smile, SSVI surface, flat) from
stated inputs. Examples: `examples/data/implied_forwards/`,
`examples/data/vol_surface/` (one request per method) and
`examples/data/structured_note/scotia_…_with_synthetic_surface.json` (the
Scotia note on synthetic surfaces under Dupire local vol). Functional
catalog rows: `impfwd_synthetic_ndx_like_2026_09_30`,
`volsurf_synthetic_svi_sheet_andreasen_huge`,
`volsurf_synthetic_svi_sheet_svi_per_expiry`,
`structured_note_scotia_…_synthetic_surface`.

**Schema additions (all new tables; `enums.fbs` gains
`VolSurfaceFitMethod`; the `ExerciseType` comment widens).**
`option_quotes.fbs` (`OptionQuote`, `OptionQuoteSheet`, `QuoteFilter`),
`calibrate_implied_forwards_request.fbs` / `_response.fbs`
(`DroppedQuote`, `ImpliedForwardExpiry`, `ImpliedForwardPoint`),
`calibrate_vol_surface_request.fbs` (`VolSurfaceOutputGrid`,
`SviFitOptions`) / `_response.fbs` (`VolSurfaceForward`, `SviParameters`,
`AndreasenHugeCalibrationError`, `VolSurfaceExpiryReport`,
`VanillaRepricePoint`, `VanillaRepricing`). OpenAPI regenerated.

## Version 0.13.1 (October 2026) — Non-finite responses, holiday valuation with deposit helpers, bounded Hull-White calibration

`v0.13.1` is a PATCH release: three fixes, no request shape changes. Every
0.13.0 request is a valid 0.13.1 request; the two optional request fields and
the four optional response fields below are additive and default to the
previous behaviour where the fix does not apply. QuantLib stays at 1.44.

**Non-finite values are never shipped in a JSON body (`500` naming the
field).** FlatBuffers prints an infinite or NaN double as a bare `inf` /
`-inf` / `nan`, which is not JSON; 0.13.0 returned such bodies with HTTP
200 (for example `"npv": inf` for a bond on an `InterpolatedZero` curve
whose discount factors overflow). The gateway now scans every generated
response and answers `500` with `{"error": "non-finite value in response at
<path>"}` (`bonds[0].npv`, `results[0].series[0].values[1]`) plus a server
log line. Strings containing `inf` / `nan` are never flagged. Documented in
`docs/http-api.md`, "Error body".

**Valuation on a holiday with deposit helpers (QuantLib ≥ 1.44) is a `400`
with the remedy.** With `as_of_date` on a holiday of a `DepositHelper`'s
calendar and `fixing_days > 0`, 0.11.0–0.13.0 failed deep inside the
bootstrap with `422 "Missing no-fix3M … fixing"` (the helper's internal
index needs a fixing before the valuation date that no request field can
supply; see `docs/quantlib-upgrades/1.41-to-1.44.md` § 3.1). The pillar is
now rejected when it is built: `400 rates.curves['<id>'].points[<j>]
(DepositHelper): the evaluation date 2025-05-01 is not a business day of the
helper's calendar TARGET and fixing_days is 2 … Value on a business day of
TARGET, or set fixing_days: 0 on the DepositHelper`. The date is not rolled.
`fixing_days: 0` pillars and business-day valuation dates are unchanged
(gate-pinned to the 0.13.0 numbers on the upgrade-notes probe: `fair_rate`
0.03, leg NPVs ∓57,389.296863753894).

**Joint Hull-White calibration is bounded in `a` and reports its
flatness.** The joint `(a, sigma)` Levenberg-Marquardt fit was unconstrained
on an objective that is flat in `a` below ~1e-3, so the fitted `a` was
engine noise (2.3× apart between two QuantLib builds on the same request).
Additive request fields on `SwaptionHwCalibrationSpec`: `a_lower` (default
0.001) and `a_upper` (default 1.0), applied only when `calibrate_a` is true;
`a_init` is clipped into the box; invalid bounds are `400` naming the field.
Additive response fields, present iff `calibrate_a`: `a_flatness`
(`1 - rmse / max(rmse(2a), rmse(a/2))` at the calibrated sigma, in `[0, 1]`)
and `a_poorly_identified` (`a_flatness < 0.01`) on
`CalibrateSwaptionModelResponse`, and `used_hw_a_flatness` /
`used_hw_a_poorly_identified` on an inline-calibrated swaption. A sigma-only
fit (`calibrate_a: false`) is unchanged and recommended for production use
with a market `a`; see `docs/http-api.md`, "Hull-White calibration". The
in-process Hull-White calibration cache key now includes the bounds
(`hw-calib:v2`).

Observable changes on the shipped examples where the bound binds (every
other calibration example and every functional-catalog row is unchanged;
`CATALOG.md` is identical):

| Request | Field | 0.13.0 | 0.13.1 |
| --- | --- | --- | --- |
| `calibrate_swaption_model_request.json` (and the `hull_white_calibration_a_and_sigma` upgrade probe's market) | `hw_a` | 0.000278081655 | 0.001000003603 |
| | `hw_sigma` | 0.006631481998 | 0.006646865746 |
| | `rmse` | 0.019048784649 | 0.019114130326 (+0.34 %) |
| | `a_flatness` / `a_poorly_identified` | — | 0.004168989254 / true |
| `swaption_bermudan_hw_calibrated.json` (and the `swaption_bermudan_hull_white_lattice_calibrated` probe) | `npv` | 8415.43462984673 | 8420.416773859031 |
| | `used_hw_a` / `used_hw_sigma` / `used_hw_rmse` | as above | as above |
| `vol/hwcal_eur_atm_matrix_roundtrip.json` (catalog row) | `hw_a`, `hw_sigma`, `rmse` | 0.049999969129, 0.0079999992, 1.899e-09 | unchanged (`a_flatness` 0.999999941, not flagged) |

## Version 0.13.0 (October 2026) — Final-observation marker, collars, exact-step Monte Carlo

`v0.13.0` is a backward-compatible MINOR release: two additive request
fields and one previously rejected enum value now priced. No request
shape changes: every 0.12.0 request is a valid 0.13.0 request with the
same response values. QuantLib stays at 1.44.

**`ObservationEvent.is_final` (structured notes, additive).** A maturity-
only observation: when true, only the capital protection block (and
`upside`, if present) is tested at that event; there is no coupon test and
no call test, so unpaid coupons in the memory are not paid there.
`coupon`, `coupon_trigger` and `autocall_trigger` must be absent (`400`
naming the field), `payment_date` is required and must equal
`maturity_payment_date` (`400`), and the event must be the last one
(`400`). Absent, the 0.12.0 rules stand (the last event must carry a
trigger; a final event with a trigger behaves as before). A final
observation that also tests a coupon or a call (an 80 % low hurdle, a
100 % call at maturity) keeps using triggers. The terms echo gains
`StructuredNoteTermsEvent.is_final` (false for every 0.12.0 request). The
Scotia example's last event is now `{observation_date, payment_date,
is_final: true}` instead of `coupon_trigger 0.0, coupon 0.0`; the price is
bit-identical on the same stream (the always-true zero coupon test paid
nothing), and the event no longer reports a `p_coupon` (there is no coupon
test). `event_probabilities[].p_coupon` stays "present iff a coupon test".

**`price-cap-floor`: `Collar` priced.** `cap_floor_type: Collar` was a
`400` ("Collar not yet supported"); it now builds
`QuantLib::Collar(leg, {strike}, {floor_strike})` with the same engine as
a cap or a floor. New field `CapFloor.floor_strike` (additive): required
with `Collar` and `<= strike` (the cap strike), must be absent for a `Cap`
or a `Floor` (`400` each). The response is unchanged in shape: `npv` (the
cap's minus the floor's, held to 1e-10 relative by a gate test),
`atm_rate`, and with `include_details` one `cap_floor_lets[]` row per
unpaid period (the same period for the caplet and the floorlet; the row's
`strike` / `price` fields stay unpopulated as before). One new example
(`examples/data/cap_floor/collar_eur_5y_cap_4pct_floor_2pct_euribor3m.json`)
and one functional-catalog row.

**Internal, no wire change.** Exact-step table in the Monte Carlo core:
for constant / term vols on the observation-date grid the paths are
generated from a precomputed per-step drift / standard-deviation table
with the array's own pseudo-square-root, bit-identical to QuantLib's
`MultiPathGenerator` paths (a gate test compares every level) and 28x
cheaper per step; the BNP example round trip through the JSON gateway
goes from 1.5 s to 0.21 s. Debug `std::cout` NPV prints removed from the
swap, FRA, FX, cap/floor, swaption and structured-note evaluators.
Per-request guards (roll offset, calendar overrides) are `thread_local`.
Zero-rate sampling at or before a curve's reference date returns the
curve's t -> 0 limit instead of the one-day-out rate (bootstrap-curves
`zero_rates` only; no catalog value moved).

## Version 0.12.0 (October 2026) — Structured notes on index baskets (Monte Carlo)

`v0.12.0` is a backward-compatible MINOR release: one new endpoint, one new
market-data block and four new enums. Nothing that existed in 0.11.0
changes shape, value or status code; a 0.11.0 request is a valid 0.12.0
request with the same response. QuantLib stays at 1.44 (no numerical
change to any existing product).

**New endpoint: `POST /price-structured-note`** (gRPC `PriceStructuredNote`,
request `PriceStructuredNoteRequest`, response `PriceStructuredNoteResponse`).
Notes on baskets of equity indices priced by Monte Carlo through one
declarative payoff: a strip of `ObservationEvent`s (each with an optional
`autocall_trigger`, an optional `coupon_trigger` and a `coupon`; both
triggers absent = an accrual-only period), a `CouponRule` (`memory`,
`pay_only_on_call`), a `CapitalProtection` block (barrier, `European` or
`DailyClose` monitoring, geared put via `put_strike` / `participation`,
`knocked_in` for a seasoned daily barrier), an optional
`UpsideParticipation`, a `performance_rule` (`WorstOf` / `BestOf` /
`Average`), a funding spread over the note's discounting curve and a
`MonteCarloSpec` (paths, seed, `Sobol` or `PseudoRandom`, antithetic,
Brownian bridge, `steps_per_year`). Classic / snowball autocallables, low
hurdles, step-downs, Phoenix coupons with or without memory, reverse
convertibles, bonus and twin-win shapes are configurations of these fields,
never products of their own. Underlyings in another currency than the note
are quanto'd through a `QuantoSpec` (local rate curve, FX spot, FX vol,
underlying / FX correlation) on QuantLib's `QuantoTermStructure`; past
observations are resolved from `historical_levels` before anything is
simulated, and a note the past already decided (`Called` / `Matured`) is
priced from its known payment without simulation.

**New market-data block: `pricing.correlations`** (`CorrelationMarketData`
with `CorrelationMatrixSpec {id, labels, values}`), validated at parse
(symmetric, unit diagonal, entries in `[-1, 1]`, positive semi-definite —
a `400` naming the pair or the smallest eigenvalue otherwise) and shared
by any basket product. Additive on `Pricing`; requests without it are
unchanged. New enums in `enums.fbs`: `BasketPerformanceRule`,
`CapitalBarrierMonitoring`, `RandomSequence`, `StructuredNoteStatus`.

**Response.** `npv`, `npv_pct_of_notional`, `standard_error`, `paths`,
`seed`, `status`; `legs` (`bond_redemption`, `coupons`, `capital_loss`,
each with its standard error, summing to the headline);
`event_probabilities` (`p_call` / `p_coupon` per simulated event);
`p_barrier_breach`; `expected_life_years`; and always the `terms` echo
(every booked field with defaults filled and each event flagged
`accrual_only` / `final` / `resolved_from_history`), the `seasoning`
resolution (per past event the levels, performances, outcomes, coupon paid
and memory carried) and the `model` echo (vol shape and local-vol flag per
underlying, quanto drift, grid, sequence dimension) — a mis-read factsheet
is visible before the price is trusted.

**Opt-in blocks (request flags).** `greeks` = bump-and-reprice
sensitivities on common random numbers (every bumped run on the same seed,
paths, grid and sequence): per underlying `delta` (per +1 % spot, central),
`gamma`, `vega` (per vol point, the Black vol rebuilt with every value
shifted; it also enters the quanto drift), `correlation_sensitivity` (per
+0.01 on every off-diagonal entry, the bumped matrix repaired by eigenvalue
clipping when it is no longer positive semi-definite — `correlation_bump_repaired`
and the matrix used are returned), `funding_dv01` and `rho` (per bp; the
rate bump moves both the processes' drift and the discounting), each with
the standard error of its per-path difference; cost `1 + 2N + N + 3`
simulations. `diagnostics` = `market` (the market as used: forwards,
implied yields and rates, ATM and barrier vols, quanto drift, discount
factors, the correlation matrix), `decomposition` (PV per event, survival /
breach / loss-given-breach), `checks` (running means, the zero-vol forward
walk, the correlation-one and correlation-zero limit prices, the bond-only
value), `distribution` (basket-performance and payout quantiles, a payout
histogram, `sample_paths` individual paths), `timing` (milliseconds per
stage). Every block is computed from the same paths as the price.

**Examples.** `examples/data/structured_note/`: the two IDAD factsheet
transcriptions (BNP Paribas Quad Index Classic Autocall June 2025,
XS2741509801; Santander Quad Index Low Hurdle Autocall August 2025,
XS3109492564) with PLACEHOLDER market data (the README there maps every
factsheet line to a field and lists every placeholder), the BNP request
with every diagnostic flag on, the BNP request with `greeks` on, a
single-underlying reverse convertible and a synthetic two-asset basket.

**Verification.** Three layers: (1) C++ collapse tests against closed
forms — a coupon-only event is a cash-or-nothing digital
(`AnalyticEuropeanEngine`), capital-only is a put plus a digital put, a
two-asset worst-of is the bivariate normal, the quanto forward is
`S exp((r_local - q - rho sigma_S sigma_FX) T)`, zero vol reproduces the
snowball arithmetic, the Greeks of the digital match the analytic engine's
delta / gamma / vega, the correlation sensitivity of the worst-of digital
matches the bivariate normal at `rho + 0.01`, `rho` and `funding_dv01` of a
bond-like note match `sum CF_i DF_i (exp(-b t_i) - 1) / b` exactly; (2) a
Python QuantLib reference (`tests/contract/ql_reference.py`,
`structured_note_ql`) that draws the engine's own random stream
(`SobolBrownianBridgeRsg`, Sobol JoeKuoD7 or Mersenne Twister, time-major,
antithetic pairs averaged into one sample) and reproduces the market
set-up, the grid, the walk, every diagnostic block and every Greek from the
request JSON — the price, its standard error, the buckets, the
probabilities, the diagnostic blocks and the Greeks agree with the server
to the 12 decimals the gateway prints on the two factsheet requests and
every synthetic case (flat, term and surface vol, quanto, Phoenix + daily
barrier, pseudo-random, Sobol without bridge, correlation repair); (3) the
three example files as functional-catalog cases (family "Structured
Notes") plus a convergence test (standard error ∝ 1 / sqrt(paths)).

**Known limits** (`docs/http-api.md`, "Structured notes"): composite
(non-quanto) foreign underlyings are not supported (the FX rate is not
simulated); the funding spread is flat; best-of / average baskets have no
weights; issuer-callable notes, lookbacks, coupon floors, FX-settled notes
and scripted payoffs are out of scope; paths are not shared across the
notes of one request; the correlation sensitivity is one uniform
off-diagonal bump (no per-pair Greeks); the `DailyClose` barrier is tested
on the simulated closes only (no continuity correction). A 4-underlying
note with `greeks` on is 16 simulations (about 21 s of engine time at
65 536 paths, beyond the JSON gateway's 10 s RPC deadline; the engine
checks the request budget before every extra run): size the paths of a
Greeks request to the deadline (the shipped example uses 16 384).

**Migration.** None: nothing existing changed. The OpenAPI document gains
the endpoint and its components; `/meta` reports `0.12.0`.

## Version 0.11.0 (October 2026) — QuantLib 1.41 → 1.44

`v0.11.0` is a backward-compatible MINOR release with **no API change**: every
endpoint, request and response schema, status code and error body is the same
as in 0.10.0. The only change is the pricing library underneath — the engine is
rebuilt on QuantLib 1.44 (from 1.41) — and `/meta` reports `quantlib: 1.44`.
Some results move; every numerical change, with its upstream cause and the
requests it affects, is documented in
[`docs/quantlib-upgrades/1.41-to-1.44.md`](quantlib-upgrades/1.41-to-1.44.md).
The cross-version diff tool (`tools/version_diff/`, see its README) replays
your own requests against a 0.10.0 and a 0.11.0 server and lists every field
that differs, so the effect on your book can be measured before adopting the
release.

**What moves (summary; the page has the numbers).** The comparison corpus
was 291 requests (the 208 functional-catalog cases plus 83 targeted probes),
sent to a 1.41 and a 1.44 server built from the same source: 215 unchanged,
2 noise-only, 72 changed, 2 status changes; 28 of the 208 catalog values
changed. The changes cluster as: cross-currency swaps and the collateral
curves built from `CrossCcyBasisHelper` pillars (a compounded overnight leg on
a schedule calendar other than the index calendar under-accrued across index
holidays on 1.41; 1.44 prices it at par, leg NPVs move by about 3 bp of
notional); CDS on bootstrapped credit curves with `settlement_days: 0` and a
non-`CDS` date rule (helper trade-date convention, up to 1.4% of NPV);
overnight coupons with `lookback_days > 0` and `apply_observation_shift`
(1.41 under-stated them); inflation swaps and caps/floors (helper pillar
placement; ZCIIS fixed leg on contract dates); joint Hull-White `a`/`sigma`
calibration (a different point on a flat objective); American options whose
exercise window starts after the valuation date (1.41 ignored the start);
bond yield/duration/convexity with `bond_pricing_details` (≤ 6e-9 in yield);
holiday lists of eleven calendars. Two requests that returned 200 now return
422: a swap valued on a holiday of the helpers' calendar with
`DepositHelper.fixing_days > 0` (a historical fixing the API cannot supply;
value on a business day or use `fixing_days: 0`), and an overnight coupon
whose `payment_convention` / `payment_calendar` put the payment before the
accrual end (1.41 returned `inf`). Full detail, worked numbers and remedies:
[`quantlib-upgrades/1.41-to-1.44.md`](quantlib-upgrades/1.41-to-1.44.md).

### Quantra version → QuantLib version

| Quantra | QuantLib |
|---|---|
| 0.1.0 – 0.10.0 | 1.41 |
| 0.11.0 | 1.44 |

Release notes for the upgrades this project tracked (1.42, 1.43 and 1.44) are
summarised on the upgrade page above; a QuantLib upgrade ships as its own
release containing nothing else.

## Version 0.10.0 (October 2026) — one swap leg model for every swap

`v0.10.0` is a **wire-breaking** MINOR release under the 0.x rules: the four
swap endpoints and the swaption request change shape. Numbers do not change:
every 0.9.0 swap and swaption request, converted with the shipped tool,
returns the same values (11,430 of 11,478 captured values identical; the
remaining 48 are the CMS `fair_rate` and `fair_spread`, which 0.9.0 reported
as `0.0` and 0.10.0 now computes). A converter and a field-by-field guide
are included.

### Changed — the `SwapLeg` model

Every swap endpoint (`price-vanilla-swap`, `price-ois-swap`,
`price-basis-swap`, `price-cross-currency-swap`) and the swaption's
underlying now describe each leg with one shared table, `SwapLeg`:

```
SwapLeg {
  pay_receive          Pay | Receive          (replaces swap_type; one leg Pay, the other Receive)
  currency             ISO code               (must match a floating index's IndexDef.currency)
  notional | notionals[]
  schedule, day_counter, payment_convention
  payment_calendar, payment_lag               (optional; required on both legs of an OIS)
  coupons_type + coupons                      FixedCoupons { rate }
                                              IborCoupons { index, spread, fixing_days, in_arrears }
                                              OvernightCoupons { index, spread, averaging_method, lookback_days,
                                                                 lockout_days, apply_observation_shift,
                                                                 telescopic_value_dates }
                                              CmsCoupons { swap_index_id, swap_tenor, swaption_vol_id, gear,
                                                           spread, cap, floor, pricer, fixing_days }
  discounting_curve, forwarding_curve         (curves live on the leg, not on the request wrapper)
  initial_exchange, final_exchange            (required on the cross-currency endpoint;
                                               absent = false on the single-currency endpoints)
}
```

The swap tables keep their names and their role-named fields, so a request
still reads as the instrument it is: `VanillaSwap { fixed_leg, floating_leg }`
(a CMS swap is a vanilla swap whose floating leg carries `CmsCoupons`; the
`cms_leg` field is gone), `OisSwap { fixed_leg, overnight_leg }`,
`BasisSwap { leg1, leg2 }`, `CrossCurrencySwap { leg1, leg2 }`. Each endpoint
checks that the leg in a role carries the coupon kind the role requires
(`400` naming the field), that the two legs are one `Pay` and one `Receive`,
and — on the single-currency endpoints — that both legs share one currency.
The request wrappers carry the swap only: `discounting_curve`,
`forwarding_curve`, `forwarding_curve_leg1/2` and `swap_type` are gone.

Responses report `legs:[SwapLegResult]` in field order (`fixed_leg` then
`floating_leg` / `overnight_leg`; `leg1` then `leg2`), each with `npv` in the
leg's currency signed from the reported party's view, `npv_reporting`, and
`flows` with `include_flows` (each flow tagged `Coupon` or
`NotionalExchange`). The per-leg fields `fixed_leg_npv`, `floating_leg_npv`,
`overnight_leg_npv`, `leg1_npv`, `leg2_npv`, the `*_bps` fields and the
per-leg `*_flows` lists are removed. One rule on every swap endpoint:
`fair_rate` is present iff exactly one leg carries `FixedCoupons` and
`fair_spread` iff a leg carries `IborCoupons`, `OvernightCoupons` or
`CmsCoupons` — on vanilla and OIS the floating / overnight leg's spread
(QuantLib's `fairSpread`, as in 0.9.0), on basis the leg that carries the
request's spread (both zero → `leg2`), on cross-currency the floating leg of
a fixed/float swap (new: 0.9.0 reported it only for float/float) or `leg2`
of a float/float one. Both are `= null` optionals; basis
`fair_spread_leg1/2` are removed. The vanilla `used_cms_*` diagnostics are
present only for a CMS leg.

Swaptions: `PriceSwaption.discounting_curve` / `forwarding_curve` are gone;
the underlying's legs carry the curves, and both legs must name the same
`discounting_curve`.

### Fixed

- CMS swaps report their real `fair_rate` and `fair_spread` (0.9.0 wrote
  `0.0` for both; the spread is solved on the CMS coupons' annuity with a
  `cap` / `floor` held at its strike).
- An overnight index under `IborCoupons` is rejected with a `400` naming
  `OvernightCoupons` on every swap endpoint; 0.9.0's `price-vanilla-swap` /
  `price-basis-swap` silently priced it as daily IBOR coupons (#150).
- `IborCoupons.fixing_days` is optional: absent means the index's own fixing
  days (0.9.0 carried a literal default of 2 that the vanilla path ignored).

### Migrating

- `tools/migrate_swap_requests/migrate_swap_requests.py` rewrites 0.9.0
  vanilla, OIS, basis, cross-currency and swaption request JSON into the
  0.10.0 shape (`--out DIR` or `--in-place`; idempotent; refuses anything it
  cannot map rather than guessing). `--check OLD_URL NEW_URL` prices each
  original against a 0.9.0 server and its conversion against a 0.10.0 server
  and compares `npv`, fair quantities and per-leg NPVs.
- The field-by-field tables, per endpoint, are in
  `tools/migrate_swap_requests/README.md`; the leg model is documented in
  `docs/http-api.md` § "Swap legs".
- Responses: read `legs[i].npv` where you read `<leg>_npv`; `legs[i].flows`
  where you read `<leg>_flows`; `fair_spread` where you read
  `fair_spread_leg1/2`.

Unchanged: every other endpoint, including `price-fx-forward`, the
zero-coupon and inflation swaps, bonds, caps/floors, CDS, equity, curves,
calendars. Still on QuantLib 1.41.

---

## Version 0.9.0 (October 2026) — multi-currency market, FX forwards and cross-currency swaps

`v0.9.0` is a backward-compatible MINOR release: every request that worked on
0.8.x works unchanged and returns the same numbers. It adds FX market data,
two FX-implied curve helpers and two products.

### Added

- **FX spot market data.** `pricing.fx.spots[]` (`FxSpotSpec`): a currency
  pair as two ISO codes (`base_currency`, `quote_currency`), the spot `rate`
  (or a `quote_id` naming a quote with the new `quote_type` `Fx`), the spot lag
  (`spot_days`), the settlement calendars (joined) and a business-day
  convention. The rate is the market spot for delivery on the spot date; where
  the engine needs an as-of rate it uses `S · DF_quote(spot) / DF_base(spot)`
  from the two discount curves. Spot lag and calendars are per-pair facts the
  request states; the engine keeps no convention table.
- **FX swap helper** (`FxSwapHelper`, QuantLib `FxSwapRateHelper`): bootstraps
  the discount curve of the non-collateral currency of a pair from FX forward
  points, given the spot and the collateral currency's discount curve.
- **Cross-currency basis helper** (`CrossCcyBasisHelper`, QuantLib
  `ConstNotionalCrossCurrencyBasisSwapRateHelper`): extends the same curve
  from constant-notional basis swaps between two floating legs (IBOR or
  overnight), with explicit forwarding and discount curves per leg.
  Together the two helpers build the classic collateral-adjusted curve
  (FX swaps to 1Y, basis swaps beyond); both take part in the curve
  dependency ordering and in the curve cache key.
- **FX forward** (`POST /price-fx-forward`): NPV in a chosen reporting
  currency (base or quote), fair forward rate, forward points, per-currency
  leg PVs, spot and adjusted maturity dates. Spot-date and maturity rules are
  QuantLib's FX-swap-helper rules, so a forward struck on an FX swap pillar
  reprices spot + points.
- **Cross-currency swap** (`POST /price-cross-currency-swap`): two legs in
  two currencies, each discounted on its own curve, with explicit
  `initial_exchange` / `final_exchange` flags, converted to the reporting
  currency at the as-of rate. Returns the total NPV, each leg's NPV in its
  own currency and in the reporting currency (`legs[]`, signed from the
  reported party's view), `fair_rate` (exactly one fixed leg) or
  `fair_spread` (no fixed leg), the spot used, the as-of rate and, with
  `include_flows`, each leg's flows tagged `Coupon` or `NotionalExchange`
  (new `SwapLegFlow.kind`, absent on the other swap endpoints).
- **`SwapLeg`, the shared swap-leg table** (`swap_leg.fbs`): `pay_receive`
  (new `PayReceive` enum, per leg, replacing a swap-level side), `currency`,
  `coupons` (union `SwapCoupons` of `FixedCoupons`, `IborCoupons`,
  `OvernightCoupons`, `CmsCoupons`; written in JSON as `coupons_type` +
  `coupons`), `discounting_curve`, `forwarding_curve`, `initial_exchange`,
  `final_exchange`; `SwapLegResult` (`npv`, `npv_reporting`, `flows`). The
  cross-currency swap is its first user; from 0.10.0 the vanilla, OIS and
  basis swaps keep their endpoints and take `SwapLeg` legs too. An
  `OvernightCoupons` leg is built by the same code as `price-ois-swap`'s
  overnight leg; `IborCoupons` on an overnight index (and the reverse) is a
  `400` naming the table to use; `CmsCoupons` is a `400` on the
  cross-currency swap in this version.
- `FxDirection`, `SwapFlowKind` and `PayReceive` enums; `fx_forward.fbs`,
  `swap_leg.fbs`, `cross_currency_swap.fbs` and their request/response
  schemas; Python client `price_fx_forwards` / `price_cross_currency_swaps`
  (exports `SwapLegT`, `SwapCoupons`, `PayReceive`, `OvernightCouponsT`,
  `RateAveragingType`); OpenAPI "FX" tag.

### Changed

- `FxSwapHelper` and `CrossCcyBasisHelper` tables are redefined. Both were
  schema-only and returned `501` with any payload, so no working request is
  affected. `HelperDependencies.projection_curve_2` and
  `HelperDependencies.fx_spot_quote_id`, which no helper ever read, are
  removed (last two slots; earlier fields keep their positions).
- **Coupon tables renamed** (generated class names only; every field name,
  the JSON and the binary wire of the existing endpoints are unchanged):
  `SwapFixedLeg` -> `FixedCoupons`, `SwapFloatingLeg` -> `IborCoupons`,
  `OisFloatingLeg` -> `OvernightCoupons`, `SwapCmsLeg` -> `CmsCoupons`. C++
  and Python object-API users rename the types (`FixedCouponsT`,
  `IborCouponsBuilder`, ...); JSON users change nothing. Error messages of
  the existing endpoints keep their wording.
- The vanilla, OIS, basis and cross-currency swap evaluators share one
  swap-leg builder (the OIS legs are now built through it and priced as a
  generic `Swap`, exactly QuantLib's `OvernightIndexedSwap` chain). Results
  of the existing endpoints are unchanged (pinned by the parity suites and
  a byte-for-byte comparison of every catalog case); a basis-swap IBOR leg
  marked `in_arrears` now carries the explicit zero-volatility coupon pricer,
  which is a no-op for in-advance coupons.

### Rejections added (all `400` naming the field, `404` for unknown ids)

See `docs/http-api.md` sections "FX spot market data", "FX swap helper",
"Cross-currency basis helper", "FX forward" and "Cross-currency swap" for the
complete lists: malformed pairs and currencies, `rate`/`quote_id` or
`spread`/`quote_id` both or neither, non-`Fx` quote behind a spot, missing
spot lag / calendars / convention, collateral currency outside the pair,
both or neither discount curve on a basis helper, mark-to-market reset
requested, a forward or swap whose currencies do not match the spot (no
implicit pair inversion), a reporting currency outside the pair, two legs in
one currency, both legs on the same side, a coupon table that does not match
its index kind, `CmsCoupons`, per-period notionals.

### Still not supported (planned)

Mark-to-market resetting cross-currency swaps, FX sensitivities (spot delta,
curve rebump) for the two products, FX options, reporting in a third
currency, `TenorBasisSwapHelper` (still `501`).

---

## Version 0.8.2 (October 2026) — rebump greeks bump curves once

`v0.8.2` is a patch release: no schema change, no change to any plain pricing
result.

### Fixes

- **Rebump greeks shift each curve quote exactly once.** With
  `swaption_pricing_rebump`, the engine rebuilds the curves with every quote
  moved by ±1bp and reprices. A curve pillar written with `quote_id` received
  that shift twice: once when the request's `quotes[]` were loaded and again
  when the pillar resolved its quote. Curves written entirely with `quote_id`
  pillars therefore reported a rebump `dv01` about twice and a `gamma` about
  four times the true value, and a curve mixing inline and `quote_id` pillars
  got a lopsided shock (only the `quote_id` pillars moved 2bp). `vega` and
  `theta`, plain pricing, `swaption_pricing_details` and curves written with
  inline rates were never affected. Every pillar is now shifted once, so the
  two spellings of the same market produce identical greeks.

---

## Version 0.8.1 (October 2026) — request isolation and input guards

`v0.8.1` is a patch release: no schema change, no change to any correctly
formed request's results.

### Fixes

- **Index fixings no longer leak between requests.** QuantLib keeps past
  fixings in a process-global store, and the server only cleared an index's
  fixings when the request supplied `fixings`. A request that supplied none
  inherited the previous request's fixings on that worker and could price with
  them. The store is now cleared at the start and end of every request.
  Requests that relied on this by accident (omitting fixings a seasoned trade
  needs) will now get the missing-fixing error they should always have had.
- **Negative day counts are rejected.** `fixing_days`, `settlement_days`,
  `cash_settlement_days` and `spot_days` across indices, bonds, deposit / FRA /
  OIS / bond helpers, swap floating legs, CDS and constant optionlet volatility
  used to be passed to QuantLib's unsigned parameters, so a negative value
  became a day count of about four billion (a 500, or a silently wrong date).
  They now return a `400` naming the field.

### Internal

- Dead code removed (unused flow builders, unreachable swaption helpers, a
  duplicate guard); global `using namespace` directives removed from headers.
- A `.clang-format` now encodes the existing code style and was applied once
  to all hand-written C++ sources; `scripts/format.sh` reproduces it.

## Version 0.8.0 (October 2026) — response fixes: zero values kept, swaption greeks corrected

`v0.8.0` fixes three response defects. Requests are unchanged; what changes is
what comes back, so this is a MINOR bump under the 0.x rules. Every change is
listed here so a client can tell what to expect.

### Zero-valued fields are no longer dropped

A response number that was exactly zero (`npv`, `fair_spread`, a holiday
`count`, `theta`, …) used to disappear from the JSON because the FlatBuffer did
not store scalars equal to their schema default. Responses now carry every
scalar, so a computed zero is returned as `0` / `0.0`. Fields declared optional
(`= null`) are unaffected: they are still present only when set.

Clients that treated a missing field as zero are unaffected. Clients that
treated a missing field as "not applicable" should rely on the optional fields,
which keep that meaning.

### Swaption `swaption_pricing_details` greeks: units corrected

The analytic greeks were written in the raw units of QuantLib's Black and
Bachelier calculators, and `theta` was computed with the swap annuity passed
where the calculator expects a discount factor, which made the number
meaningless. All details greeks are now in the same units as the rebump
greeks:

| Field | Unit (both modes) | Change from 0.7.0 (details mode) |
| --- | --- | --- |
| `delta` | currency per 1 bp move of the forward | divided by 10,000 |
| `gamma` | currency per bp² | divided by 1e8 |
| `vega` | currency per 1 bp of volatility | divided by 10,000 |
| `theta` | currency per calendar day | recomputed: time decay of the option premium with forward, volatility and annuity held fixed |
| `dv01` | currency per 1 bp | unchanged (equals `delta`) |

The values match native QuantLib computed with the same definitions.

### Swaption `swaption_pricing_rebump` theta: the roll now moves the whole market

Rebump `theta` is `NPV(tomorrow) − NPV(today)` with the market rolled one
calendar day. In 0.7.0 the roll moved the evaluation date but left explicit
curve and volatility `reference_date`s where they were, so a request whose
curves carry a `reference_date` (the common shape) reported `theta` as exactly
zero, which was then dropped, and helper-curve requests produced a partial
roll without option time decay. The roll now shifts every explicit reference
date and re-references the volatility entry, so `theta` includes both the
curve roll and the option's time decay. Rebump theta values therefore change
for every request. Known limit: SABR-calibrated surfaces roll curves only.

### Swaption greek fields are optional

`delta`, `annuity`, `atm_forward`, `vega`, `gamma`, `theta`, `dv01`,
`implied_volatility` and the `used_*` diagnostics are now `= null` optionals,
present only when the request flag that produces them is set. When both flags
are set, the rebump values of `dv01`, `gamma`, `vega` and `theta` take
precedence. Units, definitions and precedence are documented in
[`docs/http-api.md`](http-api.md).

### Migration (0.7.0 → 0.8.0)

No request changes. If you consume swaption greeks, re-read the units above;
if you parse responses strictly, expect scalars that were previously absent at
zero to be present.

## Version 0.7.0 (October 2026) — per-request calendar holiday overrides, backward-compatible

`v0.7.0` adds one feature, requested during an external validation POC: a
request can carry its own holiday corrections, so a short-notice or one-off
holiday no longer needs a new server image. Every 0.6.0 request prices
identically on 0.7.0: the change is one new optional field, and a request
without it behaves — and is cached — exactly as before. No migration is needed.

### Calendar holiday overrides

- New optional `calendar_overrides` list: one entry per calendar, with
  `added_holidays` (dates that must be holidays) and `removed_holidays` (dates
  that must be business days).
- It goes inside `pricing`, beside `as_of_date`, on every endpoint that has a
  `pricing` block (pricing, curve bootstrapping, calibration, sampling), and at
  the top level of `/calendar-holidays`, `/calendar-advance` and
  `/calendar-business-days`.
- Overrides apply for the duration of that one request and to every use of the
  calendar in it (indices, curve helpers, schedules, query grids). Nothing is
  stored on the server.
- An override that is already true is accepted with no effect, so a client's
  override list keeps working if a later server version already includes that
  holiday.
- Rejected with `400`, naming the field path: an entry without a calendar, an
  unparseable date, a date in both lists, a duplicate date, the same calendar
  in two entries (`UnitedStates` and `UnitedStatesSettlement` are the same
  calendar), `BespokeCalendar` / `NullCalendar`, and a weekend date in
  `removed_holidays`.
- Cached curves and calibrations are kept per override set: requests with
  different overrides never share a cached result.

The intended workflow is to fetch the server's holidays from
`/calendar-holidays`, reconcile them against your own calendar client-side, and
send only the differences. Full reference:
[`docs/http-api.md`](http-api.md#calendar-holiday-overrides).

## Version 0.6.0 (July 2026) — BREAKING: explicit OIS conventions

`v0.6.0` closes a gap reported by an external validation POC: the OIS curve
helpers could not be given the market-standard payment lag (or the other
overnight-coupon conventions), so a curve bootstrapped from OIS quotes
silently assumed a zero-day lag. The per-coupon effect is tiny but compounds
along the bootstrap into a measurable long-end discount-factor shift.

### OIS helpers and the OIS swap leg: conventions are now explicit and required

`OISHelper` and `DatedOISHelper` gain five required fields, mirroring the
priced `OisFloatingLeg` field-for-field:

- `payment_lag` — business days between accrual end and coupon payment
  (US SOFR market standard: 2). Must be >= 0.
- `averaging_method` — `Compound` or `Simple` averaging of the overnight
  fixings.
- `lookback_days` — observation lookback; `0` = no lookback. Must be >= 0.
- `lockout_days` — end-of-period fixing lockout; `0` = no lockout. Must
  be >= 0.
- `apply_observation_shift` — whether lookback uses the shifted date's
  day-count weight.

On the priced `OisFloatingLeg`, the same fields (plus `payment_convention`
and `payment_calendar`) are now required rather than silently defaulted, and
the old `lookback_days = -1` "none" sentinel is gone — `0` means no lookback.
Internally a wire `0` maps to QuantLib's null lookback (measurably different
from a literal zero-day lookback on indices with fixing days).

Two behavior corrections ride along:

- The helper fields `fixed_leg_convention`, `fixed_leg_frequency` and
  `calendar` were previously accepted but **ignored** (QuantLib used
  Following / Annual / the index calendar regardless). They are now honored —
  they feed QuantLib's payment convention/frequency/calendar — and the
  migrated examples state the values the engine previously applied, so
  results are unchanged for migrated requests. If your payloads stated
  different values, your curves will now (correctly) reflect them.
- `fixed_leg_day_counter` on both helpers and `day_counter` on the overnight
  leg are deprecated: no QuantLib overload consumes them (the day count comes
  from the overnight index). They are accepted-but-ignored and documented as
  such.

### Migration (0.5.0 → 0.6.0)

Add the five fields to every OIS helper point and the required conventions to
every OIS swap leg. A standard US SOFR curve is:

```json
"payment_lag": 2,
"averaging_method": "Compound",
"lookback_days": 0,
"lockout_days": 0,
"apply_observation_shift": false
```

To reproduce pre-0.6.0 behavior exactly, state `payment_lag: 0` and, on
helper points, `fixed_leg_convention: Following`, `fixed_leg_frequency:
Annual` (the values the engine silently used). The functional catalog
contains worked SOFR-style examples, including a payment-lag-2 curve, its
zero-lag twin (documenting the long-end divergence between them), and a
lookback/lockout curve, each verified against QuantLib.

## Version 0.5.0 (July 2026) — BREAKING: no implicit values, richer curves

`v0.5.0` completes the "an omitted field is an error, never a silent default"
theme across the whole request surface, redesigns curve construction around an
explicit trait, and cleans up response-side sentinels. It is **wire-breaking**:
requests that omitted a field and relied on its default, and gRPC/FlatBuffers
consumers reading response fields whose accessor type changed, must migrate.
Numbers are unchanged — a request made fully explicit prices exactly as before.

### Curve construction is now trait-driven (six explicit families)

`TermStructure.bootstrap_trait` is **required** and selects the curve family;
the server no longer guesses from the point types, and each trait validates the
points it receives (mismatch → 400):

- `Discount` / `ZeroRate` / `FwdRate` — bootstrap a `PiecewiseYieldCurve` from
  rate helpers (deposits, swaps, …).
- `InterpolatedZero` — interpolate zero rates directly from `ZeroRatePoint`s.
- `InterpolatedDiscount` — interpolate discount factors from the new
  `DiscountFactorPoint`s.
- `InterpolatedFwd` — interpolate instantaneous forwards from the new
  `ForwardRatePoint`s (Linear / BackwardFlat / ForwardFlat only — QuantLib
  cannot integrate a log-interpolated forward).

The three interpolated families are genuinely distinct curves (interpolating
zeros, discount factors or forwards off the same points yields different values
between nodes). **Explicit zero-rate curves that previously sent
`bootstrap_trait: Discount` must send `InterpolatedZero`.** Zero-rate points in
one curve must share one `compounding`/`frequency` (a mixed set was silently
mis-built before).

### Every request value and convention must be explicit

- **Values** (were silent zeros): notionals, `base_nominal`, `face_amount`,
  `redemption`, strikes and digital `cash`, `QuoteSpec.value`, `coupon_rate`,
  optionlet `volatility`, inflation helper quotes, and `CdsQuote.quote_type`.
  Amounts must be positive; rates may be negative but must be present and finite.
- **Conventions** (were hard defaults): calendars, day counters, frequencies,
  business-day conventions, settlement/fixing days, `recovery_rate`,
  `end_of_month`, `Period` unit/count, curve interpolator, and the CDS ISDA
  accrual flags, across the index, swap-index, curve-helper, inflation and
  credit-curve specs. Omission → 400 naming the field.
- **Duplicate ids** in a curves / indices / vol-surfaces / models /
  credit-curves list are rejected (were silently first-wins).

### Response changes

- Swaption Hull-White diagnostics, swaption implied volatility and the
  vanilla-swap CMS diagnostics are now **omitted when inapplicable** instead of
  reporting a `-1.0` / `-1` sentinel (which was ambiguous for a legitimate
  negative value). The `has_cms_swap_rate` flag is removed — the `cms_swap_rate`
  field alone carries presence. JSON consumers: those keys are simply absent;
  the only visible removal is `has_cms_swap_rate`. gRPC/FlatBuffers consumers:
  the accessors are now optional.

### Behavior fixes

- `in_arrears` / `fixing_days` on a floating swap leg are now honored on the
  vanilla-swap path (an in-arrears swap was silently priced in advance); the
  swaption underlying rejects `in_arrears` (QuantLib's `VanillaSwap` cannot
  express it) rather than mispricing it.
- The unused `FRA` day-counter/calendar/convention fields are no longer
  required (accepted-but-ignored, deprecated); discrete equity-barrier
  monitoring is rejected with a clear message (no native QuantLib engine).
- The shipped container now enables the Hull-White calibration cache
  (`QUANTRA_HW_CACHE_ENABLED=1`). In the 0.3.0 and 0.4.0 images it was
  available but off unless set explicitly.

### Migration

Make every request fully explicit — the ~130 payloads under `examples/data/`
are all valid, fully-explicit 0.5.0 requests and serve as templates. The
functional catalog (`tests/functional/CATALOG.md`) maps each to its expected
QuantLib result. `bootstrap_trait` and the presence rules are runtime contracts
the OpenAPI schema cannot express, so this section is authoritative.

## Version 0.4.0 (July 2026) — product expansion, backward-compatible

`v0.4.0` widens the product catalog. Every 0.3.0 request prices identically on
0.4.0: all schema changes are additive (new optional fields, new tables, new
endpoints), with one exception noted below that only affects previously
rejected inputs.

### New products (four new endpoints)

- **Zero-coupon bond** (`/price-zero-coupon-bond`) and **zero-coupon swap**
  (`/price-zero-coupon-swap`). The swap's fixed side is either an explicit
  `fixed_payment` or a `fixed_rate` + day-counter pair (exactly one form;
  QuantLib's rate form compounds annually by definition, so there are no
  compounding/frequency fields).
- **Year-on-year inflation cap/floor/collar**
  (`/price-year-on-year-inflation-cap-floor`), priced under Black,
  unit-displaced Black or Bachelier engines over a constant year-on-year
  optionlet volatility spec (a strike/tenor surface form is a planned
  additive extension).
- **Callable/puttable fixed-rate bond**
  (`/price-callable-fixed-rate-bond`): call/put schedule of dated clean
  prices, priced on the tree-based Hull-White engine; the model is referenced
  by id exactly like swaptions (explicit or calibrated, calibration cache
  applies). Response is npv + clean/dirty price; option-adjusted spread and
  risk measures are a planned optional extension.

### Widened existing products (all additive)

- **Amortizing/step-up notionals**: optional `notionals` vector (one entry
  per coupon period) on vanilla-swap legs and fixed/floating bonds. Paths
  that do not support it yet (OIS fixed leg, basis legs, swaption
  underlyings) reject it explicitly rather than ignoring it.
- **CMS capped/floored coupons**: the CMS leg's `cap`/`floor` now price
  (previously rejected as unsupported). These two fields changed from a
  negative-sentinel convention to optional-with-presence — the one
  non-additive change, affecting only requests that were rejected before.
- **Stub periods**: optional `first_date` / `next_to_last_date` on the shared
  `Schedule`, enabling short/long first and last coupons on every
  schedule-carrying product.

### Verification

- CMS legs gained full independent parity coverage against native QuantLib
  (matched to ~1e-7); every new product/feature above ships with functional
  parity cases and error-contract tests. The suite grew 537 → 624 cases.

## Version 0.3.0 (July 2026) — features + minor behavior changes

`v0.3.0` adds engine introspection, broader equity coverage, and
robustness/performance hardening. Requests that were valid and priced
correctly on 0.2.0 price identically on 0.3.0; the behavior changes below
affect only degenerate cases (no-op flags, requests slower than the new edge
timeout).

### New

- **gRPC `Meta` RPC** on the engine: `api_version` (the `VERSION` file
  verbatim), `backend_version`, `git_sha`, `build_time_utc`, `products[]`,
  `rpc_methods[]`, and `dependencies{quantlib, grpc, flatbuffers}`. gRPC-only;
  the JSON gateway keeps its richer `GET /meta`.
- **Standard `grpc.health.v1.Health`** (Check/Watch) served by the engine;
  Envoy now health-checks workers over gRPC instead of TCP, distinguishing a
  dead worker from a busy one.
- **Equity options: American and Bermudan exercise, digital payoffs.**
  American/Bermudan vanilla via finite-difference, European digitals
  analytically, American digitals via the analytic at-hit engine.
  Combinations QuantLib does not natively support (e.g. discrete cash
  dividends with American/Bermudan or digital payoffs) return a 400 naming
  the combination. Greeks an engine cannot compute are omitted from the
  response instead of serializing as invalid JSON.
- **Hull-White calibration cache** (`QUANTRA_HW_CACHE_ENABLED`, off by
  default; on in the shipped container from 0.5.0): repeat calibrations
  ~150ms → ~47ms, bit-for-bit transparent (gated by the cache-correctness
  suite).

### Behavior changes (degenerate cases only)

- **Requests are bounded end-to-end.** Envoy route timeout is 30s (was
  unlimited; configurable), client `grpc-timeout` is capped at 60s, and the
  engine abandons work when the caller's deadline has already expired —
  mid-request, at per-curve/per-trade checkpoints (`DEADLINE_EXCEEDED`,
  HTTP 504). Calibration iteration knobs from the wire are clamped
  (`max_iterations` ≤ 1000, `function_evaluations` ≤ 5000).
- **ZCIIS `adjust_observation_dates=true` is rejected** (400). The flag is
  numerically inert in the pinned QuantLib — it never changed any NPV — so it
  now errors instead of implying an adjustment that does not happen.
- Load balancing is least-request (was round-robin); worker count defaults to
  `min(cores, 8)` (was fixed 4). `QUANTRA_WORKERS` still overrides.

No schema fields were removed or renamed; no request that priced on 0.2.0
prices differently on 0.3.0.

## Version 0.2.0 (July 2026) — BREAKING

> **Why 0.2.0 and not a major bump:** the only published releases are the
> `v0.1.x` tags. The `2.0.0` previously recorded in `VERSION` was an internal
> development number that never shipped as a tag or container. While the wire
> contract is still stabilizing the project stays in `0.x`, where semver allows
> breaking changes on a minor bump.

`v0.2.0` bundles two coordinated wire-hardening batches with one theme: **an
omitted field no longer silently selects a default — it is an error.**
Previously, on the FlatBuffers wire a missing scalar/enum was indistinguishable
from its zero value, so forgetting a field could silently mis-price (an omitted
calendar meant Argentina; an omitted CDS side meant protection Buyer). Now the
server returns `400 INVALID_ARGUMENT` naming the missing field.

### Migration guide (0.1.x → 0.2.0)

To migrate a client, make every request fully explicit:

1. **State every convention.** Schedules (`calendar`, `frequency`,
   `convention`, `termination_date_convention`, `date_generation_rule`), curve
   helpers (calendars, business-day conventions, day counters, and the term
   structure's `day_counter`/`interpolator`), volatility specs, swap legs
   (`day_counter`, `payment_convention`), bonds (`accrual_day_counter`,
   `payment_convention`), the `Yield` spec (`day_counter`, `compounding`,
   `frequency`), FRA/cap-floor/CDS conventions, and the coupon-pricer's
   optionlet volatility conventions are all mandatory. Omission → 400 naming
   the field, e.g. `Schedule.calendar is required`.
2. **State the product discriminators.** `FRA.fra_type`,
   `CapFloor.cap_floor_type` and `CDS.side` are mandatory (previously an
   omitted `side` silently priced as protection Buyer — a sign flip).
3. **Give every curve helper an explicit quote.** `rate` / `spread` /
   `fx_points` / `price` are presence-based; supply the value or a `quote_id`.
   Omitting all of them → 400. The old sentinel-zero conventions are gone,
   which also makes a genuine 0 value representable.
4. **CDS quote fields are presence-based.** `flat_hazard_rate`,
   `running_coupon` and `upfront` must be supplied where the trade needs them;
   an empty credit curve is an error (the server no longer invents a hazard
   rate). In responses, `fair_spread` is omitted when QuantLib cannot express
   it (e.g. zero-running-coupon trades) instead of reporting 0.
5. **Use ISO-8601 dates.** All date strings are exactly `YYYY-MM-DD`. Slash
   formats and impossible dates (`2024-02-30`) → 400
   `Invalid date '<value>': expected YYYY-MM-DD`. Response dates are ISO too.
6. **Bond/future helper prices are explicit fields** (from the v2 batch below,
   also first published in 0.2.0): use `price` / `futures_price` instead of
   the old price-in-`rate` sentinels.

Every JSON under `examples/data/` is a valid, fully-explicit 0.2.0 request and
doubles as a migration reference; the browsable catalog
(`tests/functional/CATALOG.md`) maps each to its expected QuantLib result. The
OpenAPI spec cannot express runtime presence rules for scalars, so this prose
is the authoritative list of newly-required fields.

Interoperability notes: the served API version is visible at `/meta`, in the
OpenAPI `info.version`, and echoed on every response in the
`X-Quantra-Api-Version` header (all sourced from the `VERSION` file). The
internal curve-cache key namespace moved to `yc:v3:` so entries from older
builds cannot collide.

### Schema hardening batch v2 (June 2026) — BREAKING (first published in 0.2.0)

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
