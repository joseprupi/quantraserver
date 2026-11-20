# Commodity swaps — examples (preview)

`POST /price-commodity-swap` requests. The endpoint is a preview: the request
and response formats may still change. The fields, the rules and the
response are described in `docs/http-api.md`, section "Commodity swaps".

Every number in these files is synthetic: the contract `SYN_CL`, the spot
prices `SYN_SPOT*`, their prices, fixings, last trading dates and holidays,
and the `USD_OIS` discount factors are round made-up values, not market
data. The valuation date is 13 November 2026 throughout.

Each file is a row of the functional catalog (`tests/functional/`): the
engine's response is compared with an independent reference implementation
(`commodity_swap_ql` in `tests/contract/ql_reference.py`) on every swap's
NPV, fair value and leg NPVs, and by `tests/contract/commodity_swap_test.py`
on every flow field, observation and delta.

| File | Content |
|---|---|
| `synthetic_two_month_fixed_vs_front_month.json` | The smallest example, the one in the documentation: December 2026 and January 2027, 10,000 per month, 60.50 fixed paid against the front futures month. Flows without observations. |
| `synthetic_benchmark_fixed_vs_futures_and_spot.json` | Twelve months from November 2026, 10,000 per month, 61.00 fixed paid against the front month (roll 0), the front month rolled two pricing days before its last trading date, and a spot price read linearly between monthly points. Nine past fixings in November, nine own pricing holidays. |
| `synthetic_fixed_vs_front_month_monthly_partly_elapsed.json` | Fixed received against the front month, November 2026 to March 2027, November partly fixed. |
| `synthetic_fixed_vs_front_month_roll2_mid_month_start.json` | Periods from 16 November (balance-of-month first period), roll two pricing days before the last trading date. |
| `synthetic_fixed_vs_second_nearby_monthly.json` | The second nearby month (`nearby: 2`). |
| `synthetic_fixed_vs_spot_backward_forward_flat_and_linear.json` | Three swaps on spot prices: `BackwardFlat` monthly prices on month ends (`extrapolate_flat: false`), `ForwardFlat` prices on the 15th (held flat before the first point), `Linear`. |
| `synthetic_futures_vs_spot_plus_spread.json` | Floating against floating: the front month received, the spot price plus 0.25 paid; `fair_spread`. |
| `synthetic_explicit_periods_per_pricing_day.json` | Explicit periods with a gap, one quantity per pricing day and one fixed price per period. |
| `synthetic_fixed_vs_front_month_quarterly.json` | Quarterly periods, the first cut to start on 15 January, payment two business days after each quarter. |
| `synthetic_own_pricing_holidays.json` | The same swap on two contracts that differ by two own pricing holidays (22 against 20 pricing days in December). |
| `synthetic_pricing_calendar_override.json` | A holiday added to the pricing calendar through `pricing.calendar_overrides`. |
| `synthetic_flows_paid_before_valuation_dropped.json` | Periods paid before and on the valuation date are left out. |
| `synthetic_futures_vs_spot_common_pricing.json` | Futures against spot, the same swap without and with `common_pricing` (the spot has three extra days off). |
| `synthetic_fixed_vs_averaging_contract_one_price_per_period.json` | A calendar-month averaging contract read one price per period (`observation: OnePricePerPeriod`): monthly periods, the first read on its expired month's final settlement, and mid-month periods read on their payment dates. |
| `synthetic_fixed_vs_contract_built_from_quotes_in_request.json` | The contract built inside the swap request from a pinned month and eleven monthly average quotes (`pricing.commodity.curve_builds`); the response adds `quote_deltas` and `curve_builds`. |

To price one against a running gateway:

```bash
curl -s -X POST http://localhost:8080/price-commodity-swap \
     -H 'Content-Type: application/json' \
     --data @examples/data/commodity_swap/synthetic_two_month_fixed_vs_front_month.json
```
