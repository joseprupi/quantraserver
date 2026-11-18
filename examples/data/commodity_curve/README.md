# Commodity curve builds — examples (preview)

`POST /build-commodity-curves` requests. The endpoint is a preview: the
request and response formats may still change. The fields, the rule and the
response are described in `docs/http-api.md`, section "Commodity curves from
futures quotes".

Each request gives a futures contract in the shape
`pricing.commodity.contracts[]` of `/price-commodity-swap` takes, with some
live months (last trading date on or after the valuation date) left without
a price. The response returns the same contract with every live month
priced, ready to paste into a swap request, and a report of where each
month's price comes from. Some requests add average-price quotes, solved
together with the month prices as one linear system. The six fill methods (`Linear`, `LogLinear`, `BackwardFlat`, `ForwardFlat`,
`KrugerCubic`, `NaturalCubic`) each have an example.

Every number in these files is synthetic: the contract `SYN_CL` (and its
copies `SYN_CL_BF`, `SYN_CL_FF`), its prices, fixings, last trading dates and
holidays are round made-up values, not market data.

Each file is a row of the functional catalog (`tests/functional/`): the
engine's prices are compared with an independent implementation of the fill
rule (`commodity_curve_build_ql` in `tests/contract/ql_reference.py`), and
`tests/contract/commodity_curve_build_test.py` compares every returned field
and the report, and prices a swap equal to each average quote on the built
contract (worth zero).

| File | Content |
|---|---|
| `synthetic_linear_gaps.json` | Five quotes among thirteen live months, the other eight filled `Linear`; an expired month without a price, fixings and pricing holidays echoed. The example in the documentation. |
| `synthetic_backward_and_forward_flat.json` | Two builds of the same quotes: `BackwardFlat` (the next quote) and `ForwardFlat` (the previous quote). |
| `synthetic_extrapolate_flat_both_ends.json` | Three quotes; months before the first and after the last take the nearest quote (`extrapolate_flat`); the front month is last traded on the valuation date and is still filled. |
| `synthetic_log_linear.json` | The quotes of `synthetic_linear_gaps.json` filled `LogLinear` (straight in log price). |
| `synthetic_kruger_cubic.json` | The same quotes filled `KrugerCubic` (a cubic that limits overshoot between quotes). |
| `synthetic_natural_cubic_flat_after_last_quote.json` | Five quotes to September 2027 filled `NaturalCubic` (smooth, can overshoot); October to December take the September quote (`extrapolate_flat`): the cubic is never continued beyond the quotes. |
| `synthetic_average_quotes_months_and_quarter_unique.json` | Average quotes: January 50, February 52 and a first quarter at 53 paid monthly and discounted determine March (57.0246). Unique. Weights and sensitivities included. |
| `synthetic_average_quotes_quarter_flat_and_smooth.json` | January 50 and the quarter 53 only: two builds, `Flat` (February = March = 54.5076) and `Smooth` (53.0069, 56.0139). |
| `synthetic_average_quotes_monthly_strip.json` | The swap examples' contract with only January 2027 priced and eleven one-month average quotes: the strip they were computed from comes back. |
| `synthetic_average_quotes_monthly_strip_rounded_fragile.json` | The same quotes rounded to the cent: an exact but wildly oscillating strip, `max_sensitivity` about 8,171. |
| `synthetic_fully_priced_identity.json` | Every month priced and no `interpolation`: the contract comes back unchanged. |

To build one against a running gateway:

```bash
curl -s -X POST http://localhost:8080/build-commodity-curves \
     -H 'Content-Type: application/json' \
     --data @examples/data/commodity_curve/synthetic_linear_gaps.json
```
