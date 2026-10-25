# Equity SSVI volatility surface examples

> **The quotes in this directory are synthetic placeholders, not market data.**
> Every option premium, bid and ask was generated from an eSSVI surface chosen
> for the example (see "How the quotes were made"). The spot, the curves, the
> forwards and the dates are invented too. Do not use these numbers as an SPX
> (or any other) market view.

Two requests for the equity SSVI surfaces described in `docs/http-api.md`,
"Equity SSVI volatility surfaces". Valuation date `2026-10-21`, spot `6000`,
risk-free `4%` (continuous, flat), `Actual365Fixed`.

| File | Endpoint | What it shows |
| --- | --- | --- |
| `calibrate_equity_vol_spx_like.json` | `POST /calibrate-equity-vol` | A `BlackVolSpec` with `shape: SsviCalibrate` and 80 option quotes (five expiries from 1M to 2Y, 16 quotes each: puts from 80% to 105% of the forward, calls from 95% to 115%, so calls and puts overlap at five strikes; each with `price`, `bid` and `ask`). `ssvi_fit.weighting` is `BidAsk`. |
| `price_equity_option_ssvi_params.json` | `POST /price-equity-option` | A European call (strike 6100, expiry 2027-07-21, between the 6M and 1Y slices) on a `BlackVolSpec` with `shape: SsviParams`: the five slices that the first request calibrates to, each with its `forward`. |

```bash
curl -s -X POST http://localhost:8080/calibrate-equity-vol \
  -H 'Content-Type: application/json' \
  -d @examples/data/equity_vol/calibrate_equity_vol_spx_like.json

curl -s -X POST http://localhost:8080/price-equity-option \
  -H 'Content-Type: application/json' \
  -d @examples/data/equity_vol/price_equity_option_ssvi_params.json
```

## What the calibration returns

`slices[]` carries `theta`, `rho`, `psi` and `forward` for each expiry. The
second file is the first file's response with `rho`, `psi` and `forward`
rounded to the values the quotes were generated from:

| Expiry | ATM vol | `theta` | `rho` | `psi` | `forward` |
| --- | --- | --- | --- | --- | --- |
| 2026-11-20 | 16.5% | 0.002237671233 | -0.75 | 0.045 | 6013.58 |
| 2027-01-21 | 17.5% | 0.007719178082 | -0.72 | 0.095 | 6040.21 |
| 2027-04-21 | 18.2% | 0.016516624658 | -0.70 | 0.14 | 6078.29 |
| 2027-10-21 | 18.8% | 0.035344 | -0.68 | 0.20 | 6154.97 |
| 2028-10-20 | 19.2% | 0.073728 | -0.65 | 0.28 | 6307.63 |

The forward of each slice is found by put-call parity, not taken from the
request's dividend curve: the curve forward of the first expiry is
6013.33 and the parity forward is 6013.58 (`diagnostics.slices[0].pcp_forward`
and `curve_forward`; `implied_dividend_yield` is the yield that parity
implies). `diagnostics.violations` is empty, `scan_min_g` and `scan_min_w_t`
are positive, and the fit reproduces the quotes to a few 1e-9 in vol
(`overall_max_abs_error`). Calls in the money are not used where a put exists
at the same strike (`exclusion_reason: in_the_money_side`).

The option in the second file prices at `npv` 393.4860, `implied_volatility`
18.71%, `delta` 0.5360 (rounded; the response has twelve decimals).

## How the quotes were made

For each expiry `T` the forward is `F` (6013.58, 6040.21, 6078.29, 6154.97, 6307.63), the
surface is `w(k) = 1/2 (theta + rho psi k + sqrt(psi^2 k^2 + 2 rho psi theta k
+ theta^2))` with the parameters in the table above, and the quote at strike
`K` is the discounted Black premium with total standard deviation
`sqrt(w(ln(K / F)))`, rounded to eight decimals. The bid and ask are the
premium minus and plus the larger of 0.05 and 0.4% of the premium. Strikes are
multiples of 25 near 80% to 115% of the forward, as on a listed chain, so no
strike sits exactly at the forward. The calibrator solves its anchor exactly
(`theta` reproduces the variance of the quote nearest the forward), so the fit
returns the generating parameters to about 1e-10.

Real quotes are noisy, carry wider spreads and never come from one eSSVI
surface; expect `rmse` and `max_abs_error` in the diagnostics to be the size of
the quote noise, and read `anchor_clamped` and the margins before trusting a
fit.

Both requests are in the functional catalog (`tests/functional/`): the
calibration as a round trip against the generating parameters (the Python
reference first checks that the premiums are that surface's), the option
against an independent Python rebuild of the surface.
