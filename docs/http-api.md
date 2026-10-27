# HTTP API Contract

What the JSON gateway guarantees, independent of any single product. The
per-endpoint request and response shapes live in the generated OpenAPI spec
(`jsonserver/openapi/`).

## Request rules

- **`Content-Type: application/json` is required** on every POST. Anything else
  is `415`.
- **The body must be non-empty** and at most **10 MiB**. An empty or
  whitespace-only body is `400`; an oversized one is `413`.
- **Dates are ISO-8601 `YYYY-MM-DD`, exactly.** Slash formats and impossible
  dates (`2024-02-30`) are rejected with
  `Invalid date '<value>': expected YYYY-MM-DD`. Response dates are ISO too.
- **Omitted is not defaulted.** A field the product needs but the request does
  not carry is an error naming the field (`Schedule.calendar is required`) —
  never a silent zero, and never a silently chosen convention. This covers
  schedule and leg conventions, day counters, curve-helper quotes, product
  discriminators (`fra_type`, `cap_floor_type`, CDS `side`), and volatility
  specs. See `versioning.md` for the full list introduced in 0.2.0.
- **Presence, not sentinels, selects a variant.** Where several quote forms are
  accepted (`rate` / `price` / `spread` / `quote_id`), supply exactly the one
  you mean; supplying none is an error, and a genuine `0` is representable.

## Status codes

The gateway maps the engine's gRPC status onto HTTP:

| HTTP | When |
| --- | --- |
| `200` | Priced. List endpoints may still carry per-item errors in the body. |
| `400` | Invalid argument: missing required field, malformed date, or an unsupported combination of otherwise-valid fields. |
| `401` / `403` | Reserved; the shipped server does not authenticate. |
| `404` | Unknown route, or a referenced id (curve, model, volatility, credit curve, index, quote) that is not present in the request's `pricing` block. |
| `409` | Reserved. |
| `413` | Body over the 10 MiB cap, or a gRPC message over the transport cap. |
| `415` | `Content-Type` is not `application/json`. |
| `422` | Well-formed request the pricing engine could not evaluate — a QuantLib-level failure such as a degenerate schedule or an unbootstrappable curve. |
| `429` | Resource exhausted for a reason other than payload size. |
| `500` | Unexpected server fault. |
| `501` | A field or combination that is on the wire but not implemented. |
| `503` | No worker available. |
| `504` | The request's deadline or server-side budget expired (see `QUANTRA_REQUEST_BUDGET_MS` in `configuration.md`). |

The distinction that matters most: **`400` means the request is wrong, `422`
means the request is well-formed but unpriceable.** Retrying either without
changing the payload will not help.

## Error body

Every non-2xx response is a JSON object:

```json
{
  "error": "Schedule.calendar is required",
  "code": 3,
  "code_name": "INVALID_ARGUMENT",
  "message": "Schedule.calendar is required"
}
```

`error` carries the real cause and is the field to read. `code` /
`code_name` are the underlying gRPC status, kept for compatibility;
`message` repeats `error`.

A response is never shipped with a non-finite number in it: if a priced value
comes out as infinity or NaN (for example a discount factor that overflows on
a degenerate curve), the gateway answers `500` with
`{"error": "non-finite value in response at <path>"}` where `<path>` names the
field (`bonds[0].npv`, `results[0].series[0].values[1]`), and logs the same
line, instead of a `200` whose body carries a bare `inf` / `nan` token that
strict JSON parsers reject.

## Headers

| Header | Direction | Meaning |
| --- | --- | --- |
| `X-Quantra-Api-Version` | response | The served API version, from the `VERSION` file. Matches OpenAPI `info.version` and `GET /meta`. Present on every POST response, success or failure. |
| `X-Request-Id` | request → response | If you send one, it is sanitized (printable non-space ASCII, capped at 128 characters), forwarded to the engine as gRPC metadata, tagged onto every engine log line for that request, and echoed back. If you do not send one, none is echoed. |

Send an `X-Request-Id` on anything you may need to trace: it is the only way to
correlate a client-side failure with the engine log lines that produced it.

## Calendar holiday overrides

A request may carry its own holiday corrections in an optional
`calendar_overrides` field. The overrides apply while that request is processed
and are discarded afterwards: nothing is stored on the server, and every request
that needs an override must carry it. A request without the field behaves
exactly as before.

### Where the field goes

- **Inside `pricing`**, beside `as_of_date`, on every endpoint whose request has
  a `pricing` block (pricing, curve bootstrapping, calibration, sampling).
- **At the top level** of `/calendar-holidays`, `/calendar-advance` and
  `/calendar-business-days`, which have no `pricing` block.

The shape is the same in both places: a list with one entry per calendar.

| Field | Meaning |
| --- | --- |
| `calendar` | The calendar to override. Required. |
| `added_holidays` | Dates (`YYYY-MM-DD`) that must be holidays. Optional. |
| `removed_holidays` | Dates (`YYYY-MM-DD`) that must be business days. Optional. |

In a pricing request (only the relevant part of `pricing` is shown; the rest of
the block is unchanged). This one overrides two calendars:

```json
{
  "pricing": {
    "as_of_date": "2024-01-15",
    "calendar_overrides": [
      {
        "calendar": "TARGET",
        "added_holidays": ["2024-06-14"],
        "removed_holidays": ["2024-05-01"]
      },
      {
        "calendar": "UnitedKingdom",
        "added_holidays": ["2024-07-05"]
      }
    ]
  }
}
```

In a calendar request:

```json
{
  "calendar": "TARGET",
  "start_date": "2024-04-29",
  "end_date": "2024-06-21",
  "include_weekends": false,
  "calendar_overrides": [
    {
      "calendar": "TARGET",
      "added_holidays": ["2024-06-14"],
      "removed_holidays": ["2024-05-01"]
    }
  ]
}
```

Posted to `/calendar-holidays`, the returned `dates` include `2024-06-14` and no
longer include `2024-05-01`.

### Semantics

- `added_holidays` asserts "this date is a holiday"; `removed_holidays` asserts
  "this date is a business day".
- An override applies to **every use of that calendar in the request**: indices,
  curve helpers, schedules, and query grids. It cannot be scoped to one
  instrument or one curve.
- **An override that is already true is accepted and has no effect** — adding a
  date that is already a holiday (or a weekend), or removing a date that is
  already a business day. A client's override list therefore keeps working if a
  later server version already includes that holiday.
- An override for a calendar the request does not use is accepted. So are an
  entry with no dates and an empty list.

### Rejected requests

Each of these is a `400` whose message names the field path, including indexes
— for example
`pricing.calendar_overrides[0].removed_holidays[0]: 2024-06-15 is a weekend day; a weekend cannot be turned into a business day`.
On the calendar endpoints the path has no `pricing.` prefix.

- An entry without `calendar`.
- A date that does not parse as `YYYY-MM-DD`, or is outside the supported date
  range.
- The same date in both `added_holidays` and `removed_holidays` of one entry.
- A duplicate date inside one list.
- The same calendar in two entries.
- `BespokeCalendar` or `NullCalendar` as the `calendar`: these cannot be
  overridden.
- A weekend date in `removed_holidays`: a weekend cannot be turned into a
  business day.

The whole field is validated before anything is applied, and an invalid field
fails the whole request with `400` — including on endpoints that otherwise
report per-item errors inside a `200` response.

### Notes

- **`UnitedStates` and `UnitedStatesSettlement` are the same calendar.** An
  override on one applies to both, and listing both in one request is rejected
  as a duplicate. `UnitedStatesNYSE` and the other United States calendars are
  separate.
- **Fixings.** An added holiday can move an index fixing date. If it moves one
  before `as_of_date`, the request fails with the usual missing-fixing error
  (`422`) unless that fixing is supplied.
- **Query grids on `NullCalendar`.** A grid with `business_days_only` set and
  `NullCalendar` as its calendar uses a weekends-only calendar internally. To
  affect it, override `WeekendsOnly`.
- **Caching.** Cached curves and calibrations are kept per override set.
  Requests with different overrides never share a cached result, the order of
  entries and dates does not matter, and requests without overrides are
  unaffected.

### Suggested workflow

1. Fetch the server's holidays for the calendars and date range you care about
   from `/calendar-holidays`.
2. Compare them with your own calendar, client-side.
3. Send only the differences as `calendar_overrides` on each request.

To check the effect, call `/calendar-holidays`, `/calendar-advance` or
`/calendar-business-days` with the same overrides.

## Curve helpers and the valuation date

`DepositHelper`, `SwapHelper` and the other bootstrap pillars count their
settlement days from the request's `as_of_date` itself. Since QuantLib 1.44
that holds even when the as-of date is a holiday of the helper's calendar
(it used to be rolled to the next business day first, which gave the wrong
spot date). A deposit pillar with `fixing_days > 0` valued on such a holiday
then needs a fixing dated before the valuation date, and it needs it for the
helper's own internal index, which no request field can supply. The engine
rejects that pillar up front with a `400` that names it, the date, the
calendar and the two remedies, for example:

```
rates.curves['discount'].points[0] (DepositHelper): the evaluation date
2025-05-01 is not a business day of the helper's calendar TARGET and
fixing_days is 2, so the deposit's fixing date precedes the evaluation date
and the bootstrap would need a historical fixing the request cannot supply.
Value on a business day of TARGET, or set fixing_days: 0 on the DepositHelper
```

The date is not rolled silently. Either value on a business day of the
helper's calendar, or set `fixing_days: 0` on the deposit pillars (the fixing
then coincides with the spot date and nothing historical is needed). Deposit
pillars with `fixing_days: 0`, and every request valued on a business day,
are unaffected.

## FX spot market data

FX spot rates live in `pricing.fx.spots[]` and are referenced by id, the same
way curves and indices are: by the FX-implied curve helpers (`FxSwapHelper`,
`CrossCcyBasisHelper`) and by the FX forward product (`/price-fx-forward`).

| Field | Meaning |
| --- | --- |
| `id` | Identifier referenced by helpers and products. Required, unique within `pricing.fx.spots`. |
| `base_currency` | ISO 4217 code (three uppercase letters). One unit of it costs `rate` units of `quote_currency`. Required. |
| `quote_currency` | ISO 4217 code. Required; must differ from `base_currency`. |
| `rate` | Spot rate, quote units per base unit, for delivery on the spot date. Exactly one of `rate` / `quote_id`; must be positive. |
| `quote_id` | Entry of `pricing.quotes` carrying the rate instead of `rate`. That quote must have `quote_type: "Fx"` (see below). |
| `spot_days` | Spot lag in business days on the joint settlement calendar. Required; `>= 0`. |
| `settlement_calendars` | One or more calendars; a holiday in any of them is a holiday of the pair (QuantLib `JointCalendar`). Required, non-empty. |
| `business_day_convention` | Adjustment for a settlement date that falls on a holiday of the joint calendar. Required. |

```json
{
  "pricing": {
    "as_of_date": "2025-01-15",
    "fx": {
      "spots": [
        {
          "id": "EURUSD_SPOT",
          "base_currency": "EUR",
          "quote_currency": "USD",
          "rate": 1.0850,
          "spot_days": 2,
          "settlement_calendars": ["TARGET", "UnitedStatesSettlement"],
          "business_day_convention": "ModifiedFollowing"
        }
      ]
    }
  }
}
```

### Spot-date semantics

`rate` is the market spot: the rate for delivery on the spot date, which is
`spot_days` business days after `as_of_date` on the joint settlement calendar.
The engine never turns it into a valuation-date rate implicitly. Where a
conversion between the two currencies at the valuation date is needed, it is
computed from the spot and the two discount curves,

```
X = S * DF_quote(spot_date) / DF_base(spot_date)
```

which is the same algebra QuantLib's `FxSwapRateHelper` uses to reprice a
forward from the spot. Both discount curves are curves of the request, chosen
explicitly by the helper or product that uses the spot; the engine never picks
a curve or converts through a third currency.

### The pair's conventions are yours

The spot lag and the settlement calendars are per-pair market conventions
(T+2 for most pairs, T+1 for USD/CAD, USD/TRY and a few others; both
currencies' calendars, plus the USD calendar for crosses when the market
settles through USD). The engine keeps no convention table and infers nothing
from the currency codes: every spot states its own `spot_days` and
`settlement_calendars`, and a request that leaves them out is rejected
(`400` naming the field).

### `quote_id` and the rate bump

A spot given through `pricing.quotes` must use `"quote_type": "Fx"`. Quotes of
type `Curve` are shifted by the bump-and-reprice machinery
(`swaption_pricing_rebump`), and an FX spot must never move with a parallel
rate bump; a spot that references a `Curve` quote is rejected (`400`). FX
swap points, on the other hand, do move with rates and are ordinary `Curve`
quotes.

### Rejected requests

Each is a `400` naming the field: a missing `id`; `base_currency` equal to
`quote_currency`; a currency that is not three uppercase letters (a pair
string such as `"EURUSD"` is not a currency); both or neither of `rate` /
`quote_id`; a rate that is zero or negative; a `quote_id` whose quote is not
of type `Fx`; a missing or negative `spot_days`; an empty
`settlement_calendars`; a missing `business_day_convention`; the same `id`
twice. An unknown `quote_id` is a `404`, as for every other quote reference.

## Correlation matrices

Correlation matrices live in `pricing.correlations.matrices[]` and are
referenced by id from basket products (`StructuredNote.correlation_id`).
There is no correlation market: the matrix is an input (a historical estimate,
a dealer mark) and the engine only validates it. Like FX spots, the block is
parsed and validated on every request that carries it, whether or not a
product references a matrix, so a malformed block is a `400` on any endpoint.
`POST /calibrate-correlation` returns a matrix of this shape from price
histories (see "Correlation from price histories"); a structured note can
shift it uniformly with `correlation_markup`.

| Field | Meaning |
| --- | --- |
| `id` | Identifier referenced by products. Required, unique within `pricing.correlations.matrices`. |
| `labels` | Ids of the underlyings the rows/columns refer to, in matrix order (`EquityUnderlyingSpec.id`). Required; at least two, none empty, no duplicates. A product names its underlyings in its own order and receives the matrix permuted to match. |
| `values` | The matrix as a `QuoteMatrix2D`: `n_rows` = `n_cols` = number of labels, `values` row-major in label order. Entries may come from `pricing.quotes` through `quote_ids` (same length as `values`; a non-empty id replaces the inline entry, any quote type). Required. |

```json
{
  "pricing": {
    "as_of_date": "2025-01-15",
    "correlations": {
      "matrices": [
        {
          "id": "QUAD_INDEX_HIST_2Y",
          "labels": ["SMI", "SPX", "SX5E", "NKY"],
          "values": {
            "n_rows": 4,
            "n_cols": 4,
            "values": [
              1.00, 0.55, 0.80, 0.45,
              0.55, 1.00, 0.60, 0.50,
              0.80, 0.60, 1.00, 0.50,
              0.45, 0.50, 0.50, 1.00
            ]
          }
        }
      ]
    }
  }
}
```

### Validation

A matrix must be square with the label count as its dimension, symmetric,
with a unit diagonal, every entry in `[-1, 1]`, and positive semi-definite.
Symmetry and the unit diagonal are checked to `1e-12`; positive
semi-definiteness through the eigenvalues (QuantLib
`SymmetricSchurDecomposition`), which must all be `>= -1e-10`, so a
rank-deficient matrix such as `rho = 1` on every pair (eigenvalue 0) is
accepted. No repair is applied to the matrix as supplied.

### Rejected requests

Each is a `400` naming the matrix (by position, `correlations[i]`, with the
id appended) and the rule: a missing `id`; fewer than two labels, an empty or
duplicate label; `n_rows`/`n_cols` different from the label count; a `values`
(or `quote_ids`) length other than `n_rows * n_cols`; a non-finite entry or
one outside `[-1, 1]` (the entry is named by its pair of labels); a diagonal
entry other than 1; an asymmetric pair, e.g. `correlations[0].values: not
symmetric, (SPX, SX5E) = 0.61 but (SX5E, SPX) = 0.6 (matrix 'QUAD')`; a
matrix that is not positive semi-definite,
`correlations[QUAD]: not positive semi-definite (smallest eigenvalue -0.8)`;
the same `id` twice. An unknown `quote_id` is a `404`. A product that names a
label the matrix does not have is a `400` naming the label.

## FX swap helper

`FxSwapHelper` is a curve pillar (`pricing.rates.curves[].points[]`) that
bootstraps the discount curve of the **non-collateral** currency of a pair
from an FX swap point, given the spot and the discount curve of the
collateral currency (QuantLib `FxSwapRateHelper`). It is the standard way to
build the short end of a collateral-adjusted discount curve, for example a
EUR discount curve for USD-collateralised trades.

| Field | Meaning |
| --- | --- |
| `fx_points` | Forward points in price units of the pair (forward = spot + points, e.g. `0.0025` for 25 pips on EUR/USD). Exactly one of `fx_points` / `quote_id`. May be negative. |
| `quote_id` | Entry of `pricing.quotes` (`quote_type: "Curve"`) carrying the points instead of `fx_points`. |
| `tenor` | Forward tenor from the spot date (`{"n": 3, "unit": "Months"}`). Required. |
| `fx_spot` | Id of the `FxSpotSpec` in `pricing.fx.spots`. Required. The spot supplies the pair, the rate, the spot lag, the joint settlement calendar and the business-day convention of the pillar. |
| `end_of_month` | End-of-month rule for the forward date. Required. |
| `collateral_currency` | The currency whose discount curve is given. Required; must be `base_currency` or `quote_currency` of the spot. The curve being bootstrapped is the discount curve of the other currency. |
| `collateral_curve` | `{"id": ...}` of the collateral currency's discount curve, another curve of `pricing.rates.curves`. Required. |

```json
{
  "point_type": "FxSwapHelper",
  "point": {
    "fx_points": 0.00641,
    "tenor": {"n": 3, "unit": "Months"},
    "fx_spot": "EURUSD_SPOT",
    "end_of_month": false,
    "collateral_currency": "USD",
    "collateral_curve": {"id": "USD_SOFR_OIS"}
  }
}
```

The pillar reprices `S + points = S * DF_collateral(T) / DF_other(T)` with
both discount factors taken from the spot date. The collateral curve is a
dependency of the curve carrying the pillar: it is bootstrapped first
whatever the order of `pricing.rates.curves`, a reference to a curve that is
not in the request is a `404`, a reference to the curve being bootstrapped
itself is a `400`, and a cycle between curves is a `400`. The curve's cache
entry is keyed on the spot's full content (rate, pair, lag, calendars,
convention) as well as on the points and the collateral curve, so a change to
the spot is never served from a cached curve, while curves without FX pillars
are unaffected by the spot.

Rejected pillars (`400` naming the field): both or neither of `fx_points` /
`quote_id`; a missing `tenor`, `fx_spot`, `end_of_month`,
`collateral_currency` or `collateral_curve`; a `collateral_currency` that is
neither currency of the spot. An unknown `fx_spot`, `quote_id` or
`collateral_curve` is a `404`.

A complete example, with a QuantLib reference it is checked against, is
`examples/data/curves/curves_eur_usd_collateral_fxswap_short_end.json` (and
its `_quote_id` twin, which carries the points and the spot through
`pricing.quotes`).

## Cross-currency basis helper

`CrossCcyBasisHelper` is a curve pillar that bootstraps the discount curve of
**one** currency of a pair from a constant-notional cross-currency basis swap
(QuantLib `ConstNotionalCrossCurrencyBasisSwapRateHelper`): a base-currency
floating leg against a quote-currency floating leg, notional 1 in each
currency exchanged at the start and at the end, the basis spread added to one
leg. It is the long end of a collateral-adjusted discount curve; `FxSwapHelper`
pillars are its short end, and the two combine on one curve (see
`examples/data/curves/curves_eur_usd_collateral_full.json`).

| Field | Meaning |
| --- | --- |
| `spread` | Basis spread, decimal (`-0.0015` = -15 bp), added to the leg chosen by `spread_on_base_leg`. Exactly one of `spread` / `quote_id`. May be negative. |
| `quote_id` | Entry of `pricing.quotes` (`quote_type: "Curve"`, it moves with rates) carrying the spread instead of `spread`. |
| `tenor` | Swap tenor from the settlement date. Required. |
| `fx_spot` | Id of the `FxSpotSpec`. Required. It names the pair (`base_index` must be in its `base_currency`, `quote_index` in its `quote_currency`) and supplies the settlement lag (`spot_days`). Its rate does not enter the bootstrap (constant notional, see below) and its calendars are not used: the schedule follows the helper's own `calendar`. |
| `base_index`, `quote_index` | Forwarding indices of the two legs (`IndexDef` ids), IBOR or overnight. Required; each index's `currency` must equal the corresponding currency of the spot. |
| `base_forwarding_curve`, `quote_forwarding_curve` | `{"id": ...}` of each index's forwarding curve. Required; other curves of `pricing.rates.curves`, built first, never the curve being bootstrapped. |
| `base_discount_curve`, `quote_discount_curve` | `{"id": ...}` of a leg's discount curve. **Exactly one** is given: it is the collateral currency's discount curve (built first); the leg whose reference is absent is discounted with the curve being bootstrapped. |
| `spread_on_base_leg` | `true`: the basis is on the base-currency leg; `false`: on the quote-currency leg. Required. |
| `payment_frequency` | Coupon frequency of a compounded overnight leg. Required when either index is overnight (and must then equal the IBOR index's tenor if the other leg is IBOR); must be absent when both indices are IBOR, whose legs pay at their index tenor. |
| `payment_lag` | Business days (helper calendar, Following) between each accrual end and its payment, also applied to the final notional exchange. Required; `>= 0`. |
| `calendar`, `business_day_convention`, `end_of_month` | Swap schedule conventions. Required. |
| `mtm_reset` | Required; must be `false` (resetting notionals are not supported in this version, `400`). |

```json
{
  "point_type": "CrossCcyBasisHelper",
  "point": {
    "spread": -0.0017,
    "tenor": {"n": 5, "unit": "Years"},
    "fx_spot": "EURUSD_SPOT",
    "base_index": {"id": "EUR_ESTR"},
    "base_forwarding_curve": {"id": "EUR_ESTR_OIS"},
    "quote_index": {"id": "USD_SOFR"},
    "quote_forwarding_curve": {"id": "USD_SOFR_OIS"},
    "quote_discount_curve": {"id": "USD_SOFR_OIS"},
    "spread_on_base_leg": true,
    "payment_frequency": "Quarterly",
    "payment_lag": 0,
    "calendar": "TARGET",
    "business_day_convention": "ModifiedFollowing",
    "end_of_month": false,
    "mtm_reset": false
  }
}
```

### Which curve is built

The pillar above sits on the EUR curve for USD-collateralised trades: the USD
leg (SOFR, projected and discounted off `USD_SOFR_OIS`) is the collateral
side, the EUR leg (ESTR projected off `EUR_ESTR_OIS`) is discounted with the
curve under construction, and the -17 bp basis is on the EUR leg. Giving
`base_discount_curve` instead and leaving `quote_discount_curve` absent flips
the pair: the same pillars then bootstrap a EUR-collateralised USD discount
curve. In QuantLib's terms, `isFxBaseCurrencyCollateralCurrency` is true when
`base_discount_curve` is the given one, and `isBasisOnFxBaseCurrencyLeg` is
`spread_on_base_leg`.

### Constant notional

Both notionals stay fixed for the life of the swap, so the FX spot rate
cancels from the par condition: changing `rate` on the spot leaves a curve
built from basis pillars alone unchanged (a curve that also carries
`FxSwapHelper` pillars does move through them). The spot is still required:
it fixes which currency is which, and its `spot_days` is the swap's
settlement lag. Resetting-notional swaps (`mtm_reset: true`), where the spot
does matter, are not available yet.

### Legs and indices

An IBOR index pays one fixing per period at its own tenor. An overnight index
(ESTR, SOFR, ...) pays a compounded coupon every `payment_frequency`; QuantLib
needs that frequency explicitly, so it is required as soon as either index is
overnight, and it must equal the IBOR tenor when the legs are mixed (a 3M IBOR
leg against SOFR pays quarterly). With two IBOR indices there is nothing to
choose and the field must be absent. The forwarding curves are always other
curves of the request: QuantLib builds both legs from the indices as given, so
a leg projected off the curve under construction has no forwards to read.
`IndexDef.currency` is checked against the spot's currencies; an index in the
wrong currency is a `400` naming `base_index` / `quote_index`.

### Dependencies, pillar dates and the cache

The three referenced curves are dependencies of the curve carrying the pillar
and are bootstrapped first whatever the order of `pricing.rates.curves`; a
reference to a curve that is not in the request is a `404`, a reference to the
curve being bootstrapped itself is a `400`, a cycle is a `400`. The reported
`pillar_dates` are the helpers' own: settlement `spot_days` business days
after the valuation date on the helper calendar, the schedule end at
settlement + `tenor` adjusted with the helper convention, shifted by the
payment lag (the last payment date is the pillar). The curve's cache entry is
keyed on the spread, every convention, the three curve references (through
their own keys), both index definitions and the spot's full content; a spot
change therefore misses even when the rebuilt curve is identical, while
curves without FX pillars are unaffected by the spot.

Rejected pillars (`400` naming the field): both or neither of `spread` /
`quote_id`; both or neither of `base_discount_curve` / `quote_discount_curve`;
an index whose currency is not the spot's corresponding currency; a missing
`spread_on_base_leg`, `payment_lag` (or a negative one), `calendar`,
`business_day_convention`, `end_of_month` or `mtm_reset`; `mtm_reset: true`;
`payment_frequency` absent with an overnight index, present with two IBOR
indices, not matching the IBOR tenor on mixed legs, or not periodic; a
forwarding or discount curve equal to the curve being bootstrapped. Missing
required strings and tables (`tenor`, `fx_spot`, the indices, the forwarding
curves) are rejected by the JSON parser. An unknown `fx_spot`, `quote_id`,
index or curve is a `404`. `TenorBasisSwapHelper` is the only remaining
schema-ready pillar that returns `501`.

## FX forward

`POST /price-fx-forward` prices outright FX forwards: at `maturity_date` one
party delivers `base_notional` units of the base currency against
`base_notional x forward_rate` units of the quote currency. Each forward names
the FX spot it is priced off (`pricing.fx.spots`, see "FX spot market data")
and one discount curve per currency; nothing is inferred from curve or index
ids. QuantLib has no FX forward instrument: the price is discount-factor
algebra on the two curves, the same algebra `FxSwapRateHelper` uses, so a
forward maturing on an FX swap pillar of a collateral-adjusted curve reprices
that pillar's spot + points as its fair rate.

### Request

```json
{
  "pricing": { "as_of_date": "2025-01-15", "rates": { "...": "curves EUR_USD_COLLATERAL, USD_SOFR_OIS" },
               "fx": { "spots": [ { "id": "EURUSD_SPOT", "base_currency": "EUR", "quote_currency": "USD",
                                    "rate": 1.0850, "spot_days": 2,
                                    "settlement_calendars": ["TARGET", "UnitedStatesSettlement"],
                                    "business_day_convention": "ModifiedFollowing" } ] } },
  "fx_forwards": [
    {
      "fx_forward": {
        "base_currency": "EUR",
        "quote_currency": "USD",
        "base_notional": 10000000.0,
        "forward_rate": 1.0950,
        "maturity_date": "2025-07-17",
        "direction": "BuyBase"
      },
      "fx_spot": "EURUSD_SPOT",
      "base_discounting_curve": "EUR_USD_COLLATERAL",
      "quote_discounting_curve": "USD_SOFR_OIS",
      "reporting_currency": "USD"
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `fx_forward.base_currency` | ISO 4217 code (three uppercase letters). One unit of it is exchanged for `forward_rate` units of `quote_currency`. Must equal the spot's `base_currency`. Required. |
| `fx_forward.quote_currency` | ISO 4217 code; must equal the spot's `quote_currency`. A forward quoted the other way round than its spot is rejected, never inverted. Required. |
| `fx_forward.base_notional` | Notional in base-currency units. Presence-required, finite, `> 0`. |
| `fx_forward.forward_rate` | Contract rate, quote units per base unit. Presence-required, finite, `> 0`. |
| `fx_forward.maturity_date` | Delivery date, `YYYY-MM-DD`. Adjusted with the spot's joint settlement calendar and business-day convention (as an FX swap's far date is); the adjusted date must fall after the spot date. Required. |
| `fx_forward.direction` | `BuyBase` or `SellBase` for the party whose NPV is reported. Presence-required. |
| `fx_spot` | Id in `pricing.fx.spots`: the pair, the spot rate, the spot lag and the settlement calendars. Required. |
| `base_discounting_curve` | Curve in `pricing.rates.curves` that discounts the base-currency cash flow (for a collateralised trade, that currency's collateral-adjusted curve). Required. |
| `quote_discounting_curve` | Curve that discounts the quote-currency cash flow. Required. |
| `reporting_currency` | Currency of `npv`: the forward's base or quote currency. Any other value is a `400` (a third reporting currency is not supported). Required. |

### Response

One row per forward, in request order. No field is optional: a reported
`0.0` is a computed zero.

| Field | Currency / unit | Meaning |
| --- | --- | --- |
| `npv` | `reporting_currency` | Present value, positive in favour of the party whose `direction` was given. |
| `fair_forward_rate` | quote per base | The zero-NPV forward rate for the adjusted maturity. |
| `forward_points` | price units of the pair | `fair_forward_rate - spot`, as an FX swap screen quotes them. |
| `base_leg_pv` | base currency | PV of the base-currency cash flow (positive when received). |
| `quote_leg_pv` | quote currency | PV of the quote-currency cash flow (negative when paid). |
| `spot_date` | ISO date | Spot date used: the evaluation date, adjusted on the joint calendar, plus `spot_days` business days. |
| `maturity_date` | ISO date | The maturity after adjustment with the spot's calendar and convention. |
| `fx_spot_used` | quote per base | The spot rate the forward was priced with, inline or through the referenced `Fx` quote. |

### Pricing

With `S` the spot rate (delivery on the spot date `t_s`), `T` the adjusted
maturity, `N` the base notional, `K` the contract rate, `sign = +1` for
`BuyBase` and `-1` for `SellBase`, and `DF_b`, `DF_q` the two discount
curves:

```
base_leg_pv  = sign * N * DF_b(T)                     [base currency]
quote_leg_pv = -sign * N * K * DF_q(T)                [quote currency]
X            = S * DF_q(t_s) / DF_b(t_s)              [as-of exchange rate, quote per base]
npv          = base_leg_pv * X + quote_leg_pv         [reporting_currency = quote]
             = base_leg_pv + quote_leg_pv / X         [reporting_currency = base]
fair_forward_rate = X * DF_b(T) / DF_q(T)
                  = S * [DF_b(T) / DF_b(t_s)] / [DF_q(T) / DF_q(t_s)]
forward_points    = fair_forward_rate - S
```

`X` converts between the two currencies at the valuation date (see "FX spot
market data"): one base unit delivered at `t_s` is worth `DF_b(t_s)` base
units today and `S` quote units at `t_s`, i.e. `S * DF_q(t_s)` quote units
today. Substituting `K = fair_forward_rate` into `npv` gives zero identically,
so repricing a forward at its reported fair rate returns an NPV of zero to
rounding, and `npv_base * X = npv_quote`. Every value is as of the evaluation
date (the curves' reference date), like the NPV of every other product. The
spot is per-request data and never cached; the curves are cached as usual, and
a spot change leaves the pure-rates curves' cache entries untouched.

### Rejected requests

Each is a `400` naming the field: an empty `fx_forwards`; a currency that is
not three uppercase letters; `base_currency` equal to `quote_currency`; a
missing, non-positive or non-finite `base_notional` or `forward_rate`; a
missing `direction`; a `maturity_date` that is not `YYYY-MM-DD` or whose
adjusted date is not after the spot date; a pair that is not the spot's pair
in the same order (`PriceFxForward.fx_spot` is named); a `reporting_currency`
outside the pair. Missing required strings and tables (`fx_forward`,
`maturity_date`, `fx_spot`, the two curve ids, `reporting_currency`) are
rejected by the JSON parser. An unknown `fx_spot` or curve id is a `404`, as
is an unknown spot `quote_id`; a spot `quote_id` that is not an `Fx` quote is
a `400`.

Worked examples, each checked against a QuantLib reference in the gate, are
in `examples/data/fx_forward/`: the EUR/USD 6M buy, its sold twin, the same
reported in EUR, a 1Y forward struck on-market (spot + 1Y points, NPV zero
to bootstrap accuracy), the spot through an `Fx` quote, and a USD/JPY 3M sale
on SOFR and TONA OIS curves.

## Swap legs

Every swap endpoint (`/price-vanilla-swap`, `/price-ois-swap`,
`/price-basis-swap`, `/price-cross-currency-swap`) and the swaption's
underlying use one leg table, `SwapLeg`. Each swap is two legs under
role-named fields (`fixed_leg` / `floating_leg`, `fixed_leg` /
`overnight_leg`, `leg1` / `leg2`); the role fixes which coupon kinds the leg
may carry. The leg is flat, as in the FpML / ORE leg model: the fields every
coupon kind shares (side, currency, notional, schedule, day count, payment
conventions, curves, exchanges) are on the leg, and `coupons` holds only
what is specific to the kind (a fixed rate; an index and a spread; ...).
There is no `swap_type` and no curve id on the request wrapper.

| Field | Meaning |
| --- | --- |
| `pay_receive` | `Pay` or `Receive`: the side of the reported party on this leg's coupons. Presence-required; the two legs of a swap must take opposite values (`400` otherwise). A payer swap is `fixed_leg.pay_receive = "Pay"`. |
| `currency` | ISO 4217 code of the leg (three uppercase letters). A floating leg's index must carry the same `IndexDef.currency`. The two legs of a vanilla, OIS or basis swap must be in one currency, the two legs of a cross-currency swap in two. Required. |
| `notional` | Constant notional of the coupons and of the principal exchanges, `> 0`. Presence-required unless `notionals` is given. |
| `notionals` | Optional per-period notionals, one per coupon period, each `> 0` (amortizing / step-up; the first entry is the exchanged notional). Accepted on the two legs of `/price-vanilla-swap` (with `FixedCoupons` or `IborCoupons`); a `400` naming the endpoint elsewhere. |
| `schedule` | Accrual schedule of the coupons. Required. |
| `day_counter` | Accrual day count. Presence-required with `FixedCoupons`, `IborCoupons`, `CmsCoupons`; accepted but not read with `OvernightCoupons` (the index's day count applies). |
| `payment_convention` | Business-day convention of the payment dates. Presence-required. |
| `payment_calendar` | Calendar the payment dates are adjusted (and lagged) on. Absent = the schedule's calendar. Presence-required on both legs of `/price-ois-swap`; a `400` with `CmsCoupons`. |
| `payment_lag` | Business days between each accrual end and its payment (`>= 0`). Absent = 0. Presence-required on both legs of `/price-ois-swap` (QuantLib's `OvernightIndexedSwap` applies one lag to both legs; here it is a per-leg convention); a `400` with `CmsCoupons`. |
| `coupons_type`, `coupons` | The kind-specific coupon fields, a union written as a tag and a value (the tag first): `FixedCoupons`, `IborCoupons`, `OvernightCoupons` or `CmsCoupons`; which are accepted depends on the leg's role (table below). |
| `discounting_curve` | Curve in `pricing.rates.curves` that discounts every flow of the leg (coupons and exchanges). Required. |
| `forwarding_curve` | Curve the floating index projects off (for `CmsCoupons`, the curve the swap index's IBOR leg projects off). Required with `IborCoupons` / `OvernightCoupons` / `CmsCoupons`, rejected with `FixedCoupons`. |
| `initial_exchange` | Whether the leg's notional is exchanged on its schedule's start date. Presence-required on `/price-cross-currency-swap`; on the single-currency endpoints absent = `false`, and an explicit `true` is priced (the exchange becomes a `NotionalExchange` flow of the leg). Rejected when `true` on a swaption underlying. |
| `final_exchange` | Whether the leg's notional is exchanged on the last coupon's payment date. Same presence rule as `initial_exchange`. |

The coupon tables (kind-specific fields only):

| `coupons_type` | Accepted as | Fields | Index |
| --- | --- | --- | --- |
| `FixedCoupons` | `VanillaSwap.fixed_leg`, `OisSwap.fixed_leg`, either cross-currency leg | `rate` (decimal, may be negative) | none |
| `IborCoupons` | `VanillaSwap.floating_leg`, either `BasisSwap` leg, either cross-currency leg | `index`, `spread`, `fixing_days` (absent = the index's own `fixing_days`; `>= 0`), `in_arrears` (fixes at the accrual end, no convexity adjustment) | an IBOR index; an overnight index is a `400` that names `OvernightCoupons` |
| `OvernightCoupons` | `OisSwap.overnight_leg`, either `BasisSwap` leg, either cross-currency leg | `index`, `spread`, `averaging_method`, `lookback_days`, `lockout_days`, `apply_observation_shift` (presence-required), `telescopic_value_dates` | an overnight index; an IBOR index is a `400` that names `IborCoupons` |
| `CmsCoupons` | `VanillaSwap.floating_leg` only | `swap_index_id`, `swap_tenor`, `swaption_vol_id`, `gear`, `spread`, `fixing_days` (negative = the swap index's spot days), `cap`, `floor`, `pricer` | the swap index's IBOR leg (`pricing.rates.swap_indices`) |

A leg is built once, by one piece of code, whatever endpoint it comes
through: `FixedCoupons` is QuantLib's `FixedRateLeg`, `IborCoupons` its
`IborLeg` with a zero-volatility pricer (the chains `QuantLib::VanillaSwap`
assembles), `OvernightCoupons` its `OvernightLeg` with the OIS payment and
observation conventions (the chain `QuantLib::OvernightIndexedSwap`
assembles), `CmsCoupons` its `CmsLeg` with the requested CMS pricer. Each leg
pays on its own `payment_convention` / `payment_calendar` / `payment_lag`,
prices its own `notional` and is discounted on its own curve.

A leg in JSON:

```json
{
  "pay_receive": "Receive",
  "currency": "EUR",
  "notional": 1000000.0,
  "schedule": { "effective_date": "2025-01-17", "termination_date": "2030-01-17",
                "frequency": "Semiannual", "calendar": "TARGET",
                "convention": "ModifiedFollowing", "termination_date_convention": "ModifiedFollowing",
                "date_generation_rule": "Forward", "end_of_month": false },
  "day_counter": "Actual360",
  "payment_convention": "ModifiedFollowing",
  "coupons_type": "IborCoupons",
  "coupons": { "index": { "id": "EUR_6M" }, "spread": 0.0 },
  "discounting_curve": "EUR_OIS",
  "forwarding_curve": "EUR_6M_FWD"
}
```

### Legs in the response

Every swap row reports `npv` and `legs`, one `SwapLegResult` per leg in
request order, plus the endpoint's fair quantity:

| Field | Meaning |
| --- | --- |
| `npv` | Total present value in the reporting currency (the legs' currency on the single-currency endpoints), positive in favour of the reported party (the one whose `pay_receive` sides were given). |
| `legs[i].npv` | Present value of leg `i` (coupons and exchanges) in its own currency, signed from the reported party's view: the leg's own-frame value (as the receiver of its coupons sees it) times `-1` for a leg the party pays and `+1` for a leg it receives. A paid leg is negative only when it is worth paying for (paying 2.55% fixed on a 3% curve is in the payer's favour and positive). `npv` is the sum of the legs. |
| `legs[i].npv_reporting` | `legs[i].npv` in the reporting currency; equal to `legs[i].npv` on the single-currency endpoints. |
| `legs[i].flows` | With `include_flows`: the leg's future flows in leg order (coupons first, then the exchanges), in the leg's own frame: coupon amounts positive, the initial exchange `-notional`, the final `+notional`. Every flow carries `kind` (`Coupon` or `NotionalExchange`); a `NotionalExchange` has `payment_date`, `amount`, `discount`, `present_value` and `kind` only. The sum of a leg's `present_value`s is its own-frame PV (`legs[i].npv` without the side's sign). An overnight coupon's `rate` is the compounded rate (plus spread) and `index_fixing` the fixing on its first fixing date; a CMS coupon also carries `cms_swap_rate`. Flows on the evaluation date are excluded. |

With `L_i` the flows of leg `i` in its own frame, `D_i` its discounting
curve, `s_i = -1` for `Pay` / `+1` for `Receive` and `B_i = s_i *
CashFlows::bps(coupons of leg i, D_i)` its signed basis-point sensitivity
(exchanges excluded):

```
legs[i].npv = s_i * CashFlows::npv(L_i, D_i)
npv         = legs[0].npv + legs[1].npv                      (one currency)
fair_rate   = rate_fixed   - npv / (B_fixed / 1e-4)          (vanilla, OIS: QuantLib's VanillaSwap / OvernightIndexedSwap fairRate)
fair_spread = spread_float - npv / (B_float / 1e-4)          (vanilla, OIS: the floating / overnight leg; QuantLib's fairSpread)
fair_spread = spread_k     - npv * 1e-4 / B_k                (basis: leg k = the leg carrying the request's spread)
```

Every swap endpoint reports `fair_rate` iff exactly one leg carries
`FixedCoupons` and `fair_spread` iff a leg carries `IborCoupons`,
`OvernightCoupons` or `CmsCoupons` (a fixed-vs-floating swap reports both,
a float/float swap only `fair_spread`, a fixed/fixed cross-currency swap
neither). Each holds the other leg and every exchange fixed; repricing at
the fair value gives an NPV of zero to rounding (a CMS leg with a `cap` /
`floor` keeps its strikes, so there `fair_spread` zeroes the NPV to first
order). A fair quantity is absent when its leg's annuity is zero.

### Rejected requests (every swap endpoint)

Each is a `400` naming the field: a missing `pay_receive`; both legs `Pay`
or both `Receive`; a missing `coupons` / `coupons_type`, or a coupon kind
the leg's role does not accept (the message lists the accepted kinds);
`IborCoupons` on an overnight index (names `OvernightCoupons`) or
`OvernightCoupons` on an IBOR index (names `IborCoupons`); a missing
`forwarding_curve` on a floating leg or a present one on a fixed leg; a
missing, non-positive or non-finite `notional`; a per-period `notionals`
vector outside the vanilla swap; a missing `schedule`, `day_counter` or
`payment_convention`; a missing `payment_calendar` / `payment_lag` on an OIS
leg or a present one on a CMS leg; a non-finite `rate` / `spread`; a
missing overnight convention; negative `fixing_days` / `payment_lag` /
`lookback_days` / `lockout_days`; a currency that is not three uppercase
letters; a leg currency that is not its floating index's `IndexDef.currency`;
two currencies on a single-currency endpoint (use
`/price-cross-currency-swap`) or one currency on the cross-currency
endpoint. Missing required strings and tables (the swap, its legs,
`currency`, `schedule`, `coupons`, `discounting_curve`) are rejected by the
JSON parser. An unknown curve, index, swap index or vol id is a `404`.

## Vanilla swap

`POST /price-vanilla-swap` prices a fixed leg against an IBOR or CMS
floating leg in one currency. `VanillaSwap.fixed_leg` carries
`FixedCoupons`, `VanillaSwap.floating_leg` carries `IborCoupons` or
`CmsCoupons` (a CMS swap is a vanilla swap whose floating leg carries CMS
coupons; the response then adds the `used_cms_*` block with the effective
pricer settings). Both legs accept a per-period `notionals` vector
(amortizing / step-up) in place of the constant `notional`.

```json
{
  "pricing": { "as_of_date": "2025-01-15", "rates": { "...": "indices EUR_6M; curves EUR_OIS, EUR_6M_FWD" } },
  "swaps": [
    {
      "vanilla_swap": {
        "fixed_leg": {
          "pay_receive": "Pay",
          "currency": "EUR",
          "notional": 1000000.0,
          "schedule": { "effective_date": "2025-01-17", "termination_date": "2030-01-17",
                        "frequency": "Annual", "calendar": "TARGET", "...": "..." },
          "day_counter": "Thirty360",
          "payment_convention": "ModifiedFollowing",
          "coupons_type": "FixedCoupons",
          "coupons": { "rate": 0.035 },
          "discounting_curve": "EUR_OIS"
        },
        "floating_leg": {
          "pay_receive": "Receive",
          "currency": "EUR",
          "notional": 1000000.0,
          "schedule": { "frequency": "Semiannual", "...": "..." },
          "day_counter": "Actual360",
          "payment_convention": "ModifiedFollowing",
          "coupons_type": "IborCoupons",
          "coupons": { "index": { "id": "EUR_6M" }, "spread": 0.0 },
          "discounting_curve": "EUR_OIS",
          "forwarding_curve": "EUR_6M_FWD"
        }
      }
    }
  ],
  "include_flows": false
}
```

Response row: `npv`, `fair_rate` (the fixed rate that zeroes `npv`, the
floating leg held fixed; also for a CMS floating leg), `fair_spread` (the
floating leg's spread that zeroes `npv`, the fixed leg held fixed; also for
a CMS floating leg), `legs[0]` (the fixed leg), `legs[1]` (the floating leg)
and, for a CMS floating leg,
`used_cms_pricer_type`, `used_cms_yield_curve_model`,
`used_cms_mean_reversion` and the `used_cms_hagan_*` limits. Worked
examples: `examples/data/ir_swaps/irs_*.json` and `cms_*.json`.

## OIS swap

`POST /price-ois-swap` prices a fixed leg against a compounded (or
averaged) overnight leg in one currency. `OisSwap.fixed_leg` carries
`FixedCoupons`, `OisSwap.overnight_leg` carries `OvernightCoupons`. Both legs
state `payment_calendar` and `payment_lag` (presence-required on this
endpoint: an OIS pays with the overnight calendar and lag): an OIS that pays
two business days after each period end states
`"payment_calendar": "TARGET", "payment_lag": 2` on both legs, a plain one
`"payment_lag": 0`.

Response row: `npv`, `fair_rate` (the fixed rate that zeroes `npv`),
`fair_spread` (the overnight leg's spread that zeroes `npv`), `legs[0]` (the
fixed leg), `legs[1]` (the overnight leg). Worked examples:
`examples/data/ir_swaps/ois_*.json`.

## Basis swap

`POST /price-basis-swap` prices two floating legs in one currency, each
`IborCoupons` or `OvernightCoupons` (3M vs 6M IBOR, IBOR vs overnight),
each projected on its own `forwarding_curve` and discounted on its own
`discounting_curve`.

Response row: `npv`, `fair_spread` and `legs[0]` / `legs[1]` (`leg1` /
`leg2`). `fair_spread` is the par spread of the leg that carries the
request's spread: `leg1` when only `leg1.coupons.spread` is non-zero,
`leg2` otherwise (both spreads zero or both non-zero: `leg2`, the quoted
leg by market convention), the other leg held fixed. Worked examples:
`examples/data/ir_swaps/basis_*.json`.

## Swaption underlying

`POST /price-swaption` takes its underlying as a `VanillaSwap` or an
`OisSwap` in the same leg shape (`"underlying_type": "VanillaSwap"` before
`"underlying"`). The swaption's curves are the underlying legs': both legs
must name the same `discounting_curve` (the swaption is priced on one
discount curve, `400` otherwise) and the floating / overnight leg's
`forwarding_curve` projects the index; the request wrapper keeps only the
swaption-specific references (`volatility`, `model`). The underlying is
priced from QuantLib's own `VanillaSwap` / `OvernightIndexedSwap` built from
the two legs, which takes the fixed leg's notional for both legs, adjusts
both legs' payments with the floating schedule's convention and fixes the
IBOR leg on the index's own fixing days; `fixed_leg.pay_receive = "Pay"` is
a payer swaption. On an underlying leg `CmsCoupons`, `in_arrears`, a
per-period `notionals` vector and a `true` exchange flag are each a `400`
naming the field.

## Cross-currency swap

`POST /price-cross-currency-swap` prices swaps with two legs in two
currencies. Each leg is a `SwapLeg` (see "Swap legs"): a coupon stream
(fixed, IBOR or compounded overnight; `CmsCoupons` is a `400` on this
endpoint) in one currency, paid or received by the reported party,
discounted on its own curve, with its own principal exchanges, both
presence-required here. The swap names the FX spot that converts between the two currencies
(`pricing.fx.spots`, see "FX spot market data") and the currency its total
NPV is reported in. QuantLib has no cross-currency swap instrument: each leg
is a QuantLib cash-flow leg (coupons plus `SimpleCashFlow` exchanges) valued
with `CashFlows::npv` / `CashFlows::bps`; the conversion uses the same as-of
exchange rate as the FX forward, so a basis swap with the conventions of a
`CrossCcyBasisHelper` pillar reprices that pillar's spread as its fair
spread.

### Request

```json
{
  "pricing": { "as_of_date": "2025-01-15",
               "rates": { "...": "indices EUR_ESTR, USD_SOFR; curves EUR_USD_COLLATERAL, EUR_ESTR_OIS, USD_SOFR_OIS" },
               "fx": { "spots": [ { "id": "EURUSD_SPOT", "base_currency": "EUR", "quote_currency": "USD",
                                    "rate": 1.0850, "spot_days": 2,
                                    "settlement_calendars": ["TARGET", "UnitedStatesSettlement"],
                                    "business_day_convention": "ModifiedFollowing" } ] } },
  "swaps": [
    {
      "cross_currency_swap": {
        "leg1": {
          "pay_receive": "Pay",
          "currency": "EUR",
          "notional": 10000000.0,
          "schedule": { "effective_date": "2025-01-17", "termination_date": "2030-01-17",
                        "frequency": "Annual", "calendar": "TARGET", "...": "..." },
          "day_counter": "Thirty360",
          "payment_convention": "ModifiedFollowing",
          "coupons_type": "FixedCoupons",
          "coupons": { "rate": 0.0255 },
          "discounting_curve": "EUR_USD_COLLATERAL",
          "initial_exchange": true,
          "final_exchange": true
        },
        "leg2": {
          "pay_receive": "Receive",
          "currency": "USD",
          "notional": 10850000.0,
          "schedule": { "frequency": "Quarterly", "...": "..." },
          "payment_convention": "ModifiedFollowing",
          "payment_calendar": "TARGET",
          "payment_lag": 0,
          "coupons_type": "OvernightCoupons",
          "coupons": { "index": { "id": "USD_SOFR" }, "spread": 0.0, "averaging_method": "Compound",
                       "lookback_days": 0, "lockout_days": 0, "apply_observation_shift": false },
          "discounting_curve": "USD_SOFR_OIS",
          "forwarding_curve": "USD_SOFR_OIS",
          "initial_exchange": true,
          "final_exchange": true
        }
      },
      "fx_spot": "EURUSD_SPOT",
      "reporting_currency": "USD"
    }
  ],
  "include_flows": false
}
```

| Field | Meaning |
| --- | --- |
| `cross_currency_swap.leg1`, `leg2` | The two `SwapLeg`s, in different currencies (the same currency on both is a `400`: that is a vanilla, OIS or basis swap). Required. Reported as `legs[0]` and `legs[1]`. |
| `fx_spot` | Id in `pricing.fx.spots`. Its base and quote currencies must be the two leg currencies, in either order (either leg may be the base-currency leg; the spot is never inverted). Required. |
| `reporting_currency` | Currency of `npv`: `leg1.currency` or `leg2.currency`. Any other value is a `400` (a third reporting currency is not supported). Required. |
| `include_flows` | When true each row lists both legs' future flows (coupons and notional exchanges). Default false. |

### Principal exchanges and signs

The exchanges follow the leg: the party that **receives the leg's coupons
pays the leg's notional on the schedule's start date** (initial exchange)
**and receives it with the last coupon** (final exchange). In the leg's own
frame the flows are therefore coupons `+`, initial exchange `-notional`,
final exchange `+notional`. `pay_receive` then applies the side: the payer
of a leg's coupons receives its initial exchange and pays the final one. A
market-standard EUR/USD swap with `notional_USD = notional_EUR x spot` and
both exchanges on both legs therefore exchanges equal values on the start
date and re-exchanges the same amounts at maturity.

### Response

One row per swap, in request order.

| Field | Currency / unit | Meaning |
| --- | --- | --- |
| `npv` | `reporting_currency` | Total present value, positive in favour of the reported party (the one whose `pay_receive` sides were given). |
| `legs[i].npv` | the leg's currency | Present value of the leg (coupons and exchanges), signed from the reported party's view: the leg's own-frame value (as the receiver of its coupons sees it) times `-1` for a leg the party pays and `+1` for a leg it receives. A paid leg is negative only when it is worth paying for (paying 2.55% fixed with both exchanges on a 3% curve is in the payer's favour and positive). `legs[0]` is `leg1`, `legs[1]` is `leg2`. |
| `legs[i].npv_reporting` | `reporting_currency` | The leg NPV converted at the as-of exchange rate (unchanged for the leg already in the reporting currency); `npv` is their sum. |
| `fair_rate` | rate (decimal) | Present iff exactly one leg has `FixedCoupons`: the fixed rate that zeroes `npv`, holding the other leg and every exchange fixed. Absent for float/float and fixed/fixed swaps. |
| `fair_spread` | rate (decimal) | Present iff a leg has `IborCoupons` or `OvernightCoupons`: the spread on the floating leg of a fixed/float swap, or on **`legs[1]`** (`leg2`) of a float/float one, that zeroes `npv`, holding the other leg and every exchange fixed. Absent for fixed/fixed swaps. |
| `fx_spot_used` | quote per base of the spot | The spot rate used, inline or through the referenced `Fx` quote. |
| `fx_as_of_rate` | quote per base of the spot | `X = S x DF_quote(t_s) / DF_base(t_s)` on the two legs' discounting curves (see "FX forward"). |
| `spot_date` | ISO date | Spot date of the referenced spot. |
| `legs[i].flows` | — | With `include_flows`: the leg's future flows in leg order (coupons first, then the exchanges), in the leg's own frame: coupon amounts positive, the initial exchange `-notional`, the final `+notional`. Every flow carries `kind` (`Coupon` or `NotionalExchange`); a `NotionalExchange` has `payment_date`, `amount`, `discount`, `present_value` and `kind` only. The sum of a leg's `present_value`s is its own-frame PV (`legs[i].npv` without the side's sign). For an overnight coupon `rate` is the compounded rate (plus spread) and `index_fixing` the fixing on the coupon's first fixing date, as on the OIS endpoint. |

### Pricing

For leg `i` with coupon leg `C_i`, notional `N_i` and discounting curve
`D_i`, `s_i = -1` for `Pay` / `+1` for `Receive`, `S` the spot (delivery on
the spot date `t_s`) and `DF_base` / `DF_quote` the discounting curves of the
legs in the spot's base / quote currency:

```
L_i                   = C_i + [initial_exchange] (-N_i at the schedule start)
                            + [final_exchange]   (+N_i on the last coupon's payment date)
legs[i].npv           = s_i * CashFlows::npv(L_i, D_i)              [currency i]
X                     = S * DF_quote(t_s) / DF_base(t_s)            [quote per base, as of today]
legs[i].npv_reporting = legs[i].npv  (x X when converting base -> quote, / X the other way)
npv                   = legs[0].npv_reporting + legs[1].npv_reporting
A_i                   = CashFlows::bps(C_i, D_i) / 1e-4             [annuity of the coupons; exchanges excluded]
fair_rate             = rate_f   - NPV[in the fixed leg's currency] / (s_f * A_f)
fair_spread           = spread_k - NPV[in leg k's currency]         / (s_k * A_k)   (k = the floating leg, legs[1] when both float)
```

The NPV is linear in the fixed rate and in the spread (a fixed coupon is
`N x rate x accrual`; an IBOR or compounded overnight coupon adds the spread
to its rate), so repricing at the reported fair value gives an NPV of zero
to rounding; these are QuantLib's own `VanillaSwap` / `OvernightIndexedSwap`
fair-value formulas. Flows on the evaluation date are excluded, as for every
other swap. The spot is per-request data and never cached; the curves are
cached as usual.

### Rejected requests

Each is a `400` naming the field: an empty `swaps`; a missing `pay_receive`;
both legs `Pay` or both `Receive`; a missing `coupons` / `coupons_type`;
`CmsCoupons`; `IborCoupons` on an overnight index (names `OvernightCoupons`)
or `OvernightCoupons` on an IBOR index (names `IborCoupons`); a missing
`initial_exchange` or `final_exchange`; a missing `forwarding_curve` on a
floating leg or a present one on a fixed leg; a missing, non-positive or
non-finite `notional`; a per-period `notionals` vector; a non-finite `rate`
/ `spread`; a missing `schedule`, `day_counter` (fixed / IBOR legs) or
`payment_convention`; a missing overnight convention (`averaging_method`,
`lookback_days`, `lockout_days`, `apply_observation_shift`); negative
`fixing_days` / `payment_lag` /
`lookback_days` / `lockout_days`; a currency that is not three uppercase
letters; the two legs in the same currency (a vanilla, OIS or basis swap:
use those endpoints); a leg currency that is not its floating index's
`IndexDef.currency`; a spot that does not quote the two leg currencies
(`PriceCrossCurrencySwap.fx_spot` is named); a `reporting_currency` outside
the two legs. Missing required strings and tables (`cross_currency_swap`,
`leg1`, `leg2`, `currency`, `coupons`, `discounting_curve`, `fx_spot`,
`reporting_currency`) are rejected by the JSON parser. An unknown `fx_spot`,
curve or index id is a `404`.

Worked examples, each checked against a QuantLib reference in the gate, are
in `examples/data/cross_currency_swap/`: EUR 2.55% fixed vs USD SOFR with
both exchanges reported in USD and in EUR, the same struck at its fair rate
(NPV zero), USD SOFR vs EUR ESTR - 15 bp with both exchanges (fair spread =
the collateral curve's 5Y basis pillar) and with the final exchange only,
and USD 4.10% fixed vs EUR Euribor 3M received, reported in EUR.

## Caps, floors and collars

`POST /price-cap-floor` prices caps, floors and collars on an IBOR leg
(`PriceCapFloorRequest`: `pricing` plus `cap_floors[]`, each a `CapFloor`
with the ids of its `discounting_curve`, `forwarding_curve`, `volatility`
(an `OptionletVolSpec`) and `model` (a `CapFloorModelSpec`: `Black`,
`ShiftedBlack` or `Bachelier`, matched to the vol's type and displacement)
and `include_details`).

| Field | Meaning |
| --- | --- |
| `cap_floor_type` | `Cap`, `Floor` or `Collar`. Presence-required. |
| `notional` | Presence-required, `> 0`. |
| `strike` | The strike of a cap or a floor; for a collar, the CAP strike. Presence-required (may be negative). |
| `floor_strike` | The floor strike of a collar: required with `Collar` and `<= strike`; must be absent for a `Cap` or a `Floor`. |
| `schedule`, `index`, `day_counter`, `business_day_convention` | The IBOR leg: `QuantLib::IborLeg(schedule, index)` with the trade-level payment day counter and payment adjustment; the index projects off `forwarding_curve`. |

A collar is `QuantLib::Collar(leg, {strike}, {floor_strike})`: long the cap,
short the floor, on the same leg, curves, vol and engine, so its `npv` is the
cap's `npv` minus the floor's from two separate requests (a gate test holds
the identity to 1e-10 relative). The response carries `npv`, `atm_rate`
(the leg's ATM rate on the discounting curve) and, with `include_details`,
one `cap_floor_lets[]` row per unpaid period (`payment_date`,
`accrual_start_date`, `accrual_end_date`, `fixing_date`, `forward_rate`,
`discount`). For a collar the row is the same period for the caplet and the
floorlet; the strikes are the request's `strike` and `floor_strike`,
identical on every row, so they are not repeated per row. The row's `strike`
and `price` fields are not populated in this version (`0.0` for every type).

Rejected (`400`): a missing `cap_floor_type`, `notional`, `strike`,
`day_counter`, `business_day_convention` or `schedule`; `Collar` without
`floor_strike` or with `floor_strike > strike`; `floor_strike` on a `Cap`
or a `Floor`; a model / vol-type mismatch (`Bachelier` needs a `Normal`
vol, `Black` a zero displacement, `ShiftedBlack` a positive one). A missing
curve, vol, model or index id is a `404`.

## Structured notes

`POST /price-structured-note` prices notes on baskets of equity indices by
Monte Carlo: autocallables (classic, snowball, step-down), Phoenix coupons
with or without memory, low-hurdle finals, reverse convertibles, European or
daily-close capital barriers with geared puts, and upside participation.
There is one declarative payoff: a strip of observation events, each with
an optional autocall trigger, an optional coupon trigger and a coupon, a
coupon carry rule and a capital-protection block; every product shape is a
configuration of those fields, never a product of its own. Market data is
the shared `pricing` block: `EquityUnderlyingSpec` (spot, dividend curve,
currency), `VolSurfaceSpec` with a `BlackVolSpec` (constant, term or
strike x expiry surface) or an `SviVolSpec` (SVI slices), yield curves, `FxSpotSpec` and the correlation
matrices of "Correlation matrices". Nothing product-specific is in
`pricing`. The two factsheet transcriptions and the synthetic cases are in
`examples/data/structured_note/` (its README maps every factsheet line to a
field; every market number there is a placeholder).

### Request

```json
{
  "pricing": { "as_of_date": "2026-10-21",
               "quotes": [ { "id": "SPX_SPOT", "kind": "Price", "value": 6000.0, "quote_type": "Curve" }, "..." ],
               "rates": { "curves": [ { "id": "USD_SOFR_OIS", "...": "OIS helpers" },
                                      { "id": "SPX_DIV", "...": "flat 2 % zero curve" },
                                      { "id": "CHF_FLAT", "...": "CHF rate curve (quanto drift)" } ] },
               "volatility": { "vol_surfaces": [ { "id": "SPX_VOL", "payload_type": "BlackVolSpec",
                                                   "payload": { "base": { "shape": "Constant", "constant_vol": 0.2, "...": "..." } } },
                                                 { "id": "CHFUSD_VOL", "...": "FX vol, Constant or term" } ] },
               "equity": { "equity_underlyings": [ { "id": "SPX", "currency": "USD", "spot_quote_id": "SPX_SPOT",
                                                     "dividend_yield_curve_id": "SPX_DIV" },
                                                   { "id": "SMI", "currency": "CHF", "...": "..." } ] },
               "fx": { "spots": [ { "id": "CHFUSD_SPOT", "base_currency": "CHF", "quote_currency": "USD", "rate": 1.25, "...": "..." } ] },
               "correlations": { "matrices": [ { "id": "QUAD_INDEX_CORR", "labels": ["SMI", "SPX", "SX5E", "NKY"],
                                                 "values": { "n_rows": 4, "n_cols": 4, "values": [ 1.0, 0.6, "..." ] } } ] } },
  "notes": [
    {
      "trade_id": "XS2741509801",
      "currency": "USD",
      "notional": 1000.0,
      "underlyings": [
        { "underlying_id": "SMI", "volatility_id": "SMI_VOL", "initial_level": 12000.0,
          "quanto": { "discounting_curve": "CHF_FLAT", "fx_spot_id": "CHFUSD_SPOT",
                      "fx_volatility_id": "CHFUSD_VOL", "fx_underlying_correlation": -0.1 },
          "historical_levels": [ { "date": "2026-06-29", "value": 11400.0 } ] },
        { "underlying_id": "SPX", "volatility_id": "SPX_VOL", "initial_level": 6000.0,
          "historical_levels": [ { "date": "2026-06-29", "value": 6300.0 } ] },
        "..."
      ],
      "performance_rule": "WorstOf",
      "events": [
        { "observation_date": "2025-12-29", "coupon": 0.07 },
        { "observation_date": "2026-06-29", "payment_date": "2026-07-06", "autocall_trigger": 1.0, "coupon": 0.07 },
        "...",
        { "observation_date": "2031-06-27", "payment_date": "2031-07-07", "autocall_trigger": 1.0, "coupon": 0.07 }
      ],
      "coupon_rule": { "memory": true, "pay_only_on_call": true },
      "capital": { "barrier": 0.6, "monitoring": "European", "put_strike": 1.0, "participation": 1.0 },
      "maturity_payment_date": "2031-07-07",
      "discounting_curve": "USD_SOFR_OIS",
      "funding_spread_bp": 100.0,
      "correlation_id": "QUAD_INDEX_CORR",
      "monte_carlo": { "paths": 65536, "seed": 42, "sequence": "Sobol", "antithetic": true, "brownian_bridge": true }
    }
  ],
  "diagnostics": { "market": true, "decomposition": true, "checks": true, "distribution": true, "timing": true, "sample_paths": 3 }
}
```

`StructuredNote` (one per entry of `notes`, priced on its own; paths are not
shared between notes):

| Field | Meaning |
| --- | --- |
| `trade_id` | Echoed in the response. Required. |
| `currency` | ISO code of the note: coupons, redemption and NPV. Required. |
| `notional` | Coupons and the redemption are fractions of it. Presence-required, `> 0`. |
| `underlyings[]` | The basket, at least one entry, ids distinct. Each: `underlying_id` (an `EquityUnderlyingSpec`, whose `currency` is required here), `volatility_id` (a `VolSurfaceSpec` with a `BlackVolSpec` or an `SviVolSpec` payload: constant or term vols step exactly between observation dates; a strike x expiry grid or SVI slices make the simulation Dupire local-vol and require `monte_carlo.steps_per_year`; `POST /calibrate-vol-surface` returns both forms from listed option quotes, ready to paste — see "Volatility surface from option quotes"), `initial_level` (the strike fixing every performance `S(t) / initial_level` is measured against; presence-required, `> 0`), `quanto` (present iff the underlying's currency differs from the note's: `discounting_curve` = the rate curve of the underlying's currency, `fx_spot_id` = an `FxSpotSpec` quoting the two currencies in either direction, `fx_volatility_id` = a constant or term `BlackVolSpec`, `fx_underlying_correlation` in `[-1, 1]`, presence-required), `historical_levels[]` (`{date, value}` closes on past dated observation dates; a past observation without one is a `400` naming the date). |
| `performance_rule` | `WorstOf`, `BestOf` or `Average` of the underlyings' performances. Presence-required. |
| `events[]` | Strictly increasing `observation_date`s; the last is the final observation. `autocall_trigger` present: `perf >= trigger` ends the note (notional plus this coupon plus the memory on `payment_date`). `coupon_trigger` present: `perf >= trigger` earns `coupon` plus the memory, paid on `payment_date` or held (`pay_only_on_call`). Both present (Phoenix with a call): the coupon test runs first. Neither: an accrual-only event, `coupon` joins the memory, nothing is observed or paid (`coupon_rule.memory` must be true, `payment_date` absent). `payment_date` is required iff a trigger is present and must be on or after the observation date; `coupon` is required with `coupon_trigger` and on accrual-only events. The final event must carry a trigger, unless it is marked `is_final: true`: a maturity-only observation where only the capital protection block (and `upside`) is tested, with no coupon test and no call test (the memory is not paid there); `coupon`, `coupon_trigger` and `autocall_trigger` must then be absent, `payment_date` is required and must equal `maturity_payment_date`, and the event must be the last one. A final observation that also tests a coupon or a call (Santander's 80 % hurdle, BNP's 100 % call) keeps its trigger; `is_final` is for a final date that only decides the redemption (the Scotia note). |
| `coupon_rule` | `memory` (a missed coupon is added to a memory paid with the next coupon; the memory is a sum of amounts) and `pay_only_on_call` (true = Athena / snowball: earned coupons are held until a call or the final event; false = Phoenix: paid on their `payment_date`). Both presence-required. |
| `capital` | `barrier` in `(0, 1]`, `monitoring` (`European`: tested on the final observation; `DailyClose`: breached when the basket performance is below the barrier on any simulated close), `put_strike` (default 1), `participation` (default 1); when breached the redemption is `1 - participation x max(put_strike - perf_final, 0)`. `knocked_in` is presence-required with `DailyClose` when an event is on or before the valuation date, absent otherwise. |
| `upside` | Optional: `participation x max(perf_final - strike, 0)`, capped at `cap` when present, added to the redemption when the note is not called. |
| `maturity_payment_date` | Payment date of the redemption when the note is not called; on or after the final observation. Required. |
| `discounting_curve`, `funding_spread_bp` | The note-currency curve and a flat continuously compounded zero spread added on top for discounting (default 0). |
| `correlation_id` | A `CorrelationMatrixSpec` whose labels are exactly the basket's ids. Required with two or more underlyings; must be absent with one (a 1 x 1 unit matrix is used). |
| `correlation_markup` | Optional, finite, in `[-1, 1]`; must be absent with a single underlying. A uniform additive shift of every off-diagonal entry of the matrix before pricing (the desk's correlation mark-up or stress): `rho + correlation_markup` clipped to `[-1, 1]`; when the shifted matrix is not positive semi-definite it is repaired exactly as the Greeks' bump is (eigenvalue clipping, rescaled to unit diagonal) and `terms.correlation_markup_repaired` says so. The simulation, the correlation-limit checks and the Greeks' correlation bump all start from the marked-up matrix. `terms` echoes the mark-up; the `market` diagnostic shows the matrix as supplied (`correlation_base`), the mark-up and the matrix used (`correlation`). |
| `monte_carlo` | `paths` (presence-required, even with `antithetic`), `seed` (default 42; `> 0` with Sobol), `sequence` (`Sobol` default, or `PseudoRandom`), `antithetic` (default true), `brownian_bridge` (default true; must be false with `PseudoRandom`), `steps_per_year` (required with a surface vol or a `DailyClose` barrier, must be absent otherwise). |

Top-level: `greeks` (see "Greeks") and `diagnostics` (`market`,
`decomposition`, `checks`, `distribution`, `timing`, each false by default,
`sample_paths`, at most 100, only with `distribution`, and
`vanilla_repricing` with `vanilla_repricing_sheets` and optionally
`vanilla_repricing_filter`, the listed quotes to reprice through the
simulation — see "Diagnostics").

### The two factsheets in short

Both IDAD notes are worst-of autocalls on SMI / SPX / SX5E / NKY in USD, so
three underlyings carry a `quanto` block. BNP XS2741509801: 7 % per
half-year snowball coupon, first observation at 12 months, so one
accrual-only event (`{observation_date, coupon: 0.07}`) precedes eleven dated
events with `autocall_trigger: 1.0` (the coupon test is the call test), 60 %
European barrier, `coupon_rule = {memory: true, pay_only_on_call: true}`.
Santander XS3109492564: 2.5 % per quarter, three accrual-only events then
twenty quarterly 100 % autocalls and a final event with `coupon_trigger: 0.8`
and no `autocall_trigger` (the 80 % low hurdle: at the final observation a
call and a coupon-plus-redemption pay the same amounts on the same date), 60 %
European barrier. A reverse convertible is `coupon_trigger: 0.0` on every
event and no `autocall_trigger`; a step-down is a per-event trigger; a
Phoenix is `pay_only_on_call: false`; a geared put is `participation > 1`.
The Scotia note (`scotia_autocallable_trigger_ndx_rty_us06419jme72.json`)
has one 100 % call with a 14.75 % premium and a final date that only tests
the 75 % trigger and the 250 % upside: its last event is `{observation_date,
payment_date, is_final: true}`, no coupon and no trigger.

### Response

One row per note, in request order.

| Field | Meaning |
| --- | --- |
| `npv`, `npv_pct_of_notional`, `standard_error` | The price in the note currency, as a percentage of the notional, and its Monte Carlo standard error (absent when nothing was simulated). |
| `paths`, `seed`, `status` | Paths simulated (0 for a `Called` / `Matured` note), the seed, `Alive` / `Called` / `Matured`. |
| `legs` | `bond_redemption`, `coupons`, `capital_loss` (`<= 0`), each `{pv, standard_error}`; the three sum to `npv`. `upside` is in `bond_redemption`. |
| `event_probabilities[]` | Per simulated event: `p_call` (present iff `autocall_trigger`), `p_coupon` (present iff a coupon test), both "alive at this event and the test holds". |
| `p_barrier_breach`, `expected_life_years` | Breach probability; `E[t]` of the call (or the final observation), Actual/365 (Fixed) from the valuation date. |
| `terms` | Every booked field as understood, defaults filled, each event flagged `accrual_only` / `final` (the last event) / `is_final` (the request's maturity-only marker) / `resolved_from_history`. Check it before trusting the price. |
| `seasoning` | How the past was resolved: per past event the levels, performances, basket performance, call and coupon outcomes, coupon paid, memory after; `memory_carried` into the simulation; `next_event_index`; for a decided note `known_payment_date` and `known_payment`. |
| `model` | Per underlying `vol_shape`, `local_vol`, `quanto_drift` (rate units, at the first observation); `observation_dates_only`, `time_steps`, `time_grid`, `sequence_dimension`. |
| `greeks` | The bump-and-reprice sensitivities below, present iff `greeks` was in the request. |
| `market`, `decomposition`, `checks`, `distribution`, `timing`, `vanilla_repricing` | The diagnostic blocks below, each present iff its flag was set and something was simulated. |

### Diagnostics

Every block but `vanilla_repricing` is computed from the same paths as the
price, so the parts add up to the headline (`vanilla_repricing` is one
extra simulation on a grid of its own, see below). The definitions below
are the contract (they are also the doc comments of
`flatbuffers/fbs/structured_note_response.fbs`).

`market` (deterministic, no simulation): `observation_dates` (the simulated
dated events); per underlying `spot`, `forwards[]` = `spot x D_div(t) /
D_note(t)` with `D_div` the process's dividend curve (the
`QuantoTermStructure` for a quanto underlying, so the quanto drift is in the
forward) and `D_note` the discounting curve without the funding spread, at
the grid time `t`; `implied_dividend_yields[]` and `implied_rates[]`, the
continuously compounded rates `-ln(D(t_j) / D(t_{j-1})) / (t_j - t_{j-1})`
of the underlying's own dividend curve and of its own currency's rate curve
between consecutive observation dates (the first from the valuation date);
`atm_vols[]` = the Black vol to each observation date at that date's
forward; `barrier_vols[]` = the Black vol at `capital.barrier x
initial_level`, equal to `atm_vols` unless the vol is a strike x expiry
surface; `quanto_drift_bp` = `-rho x sigma_S x sigma_FX x 1e4` with the vols
to the final simulated observation date (`sigma_S` at `initial_level`,
`sigma_FX` at the FX spot). `payment_dates` = the simulated events' payment
dates followed by `maturity_payment_date` when it differs, with
`discount_factors` (plain curve) and `discount_factors_with_funding`.
`correlation_labels` / `correlation` = the matrix as used, row-major, in the
note's underlying order (`[1]` for a single underlying; the supplied matrix
is never repaired: one that is not positive semi-definite is a `400` at
parse). With a `correlation_markup`, `correlation` is the shifted (and, when
needed, repaired) matrix the simulation ran on, `correlation_base` the
matrix as supplied and `correlation_markup` the shift; both absent
otherwise.

`decomposition`: per simulated dated event `p_call` / `p_coupon` (the values
of `event_probabilities`), `pv_coupons` (PV of the coupons paid on that
event's payment date, memory included) and `pv_redemption` (PV of the
notional returned there on a call, or of the redemption before any loss,
upside included, on `maturity_payment_date` for the final event);
`sum(pv_coupons) = legs.coupons.pv` and `sum(pv_redemption) =
legs.bond_redemption.pv`, less any flow the past already fixed (those are in
`seasoning`). `p_survive_to_maturity` (`= 1 - sum(p_call)`), `p_breach`,
`expected_life_years`, and `expected_loss_given_breach` = `E[loss x
1{breach}] / p_breach` with `loss = participation x max(put_strike -
perf_final, 0)`, undiscounted, a positive fraction of the notional, absent
when no path breached. Bucket standard errors are on `legs`.

`checks`: `running_means[]` = the NPV estimate and its standard error after
1/4, 1/2, 3/4 and all of the samples in simulation order (`paths` = samples,
or 2 x samples with antithetic; a fraction is omitted when its sample count
would be 0 or repeat the previous one); `deterministic_forward_npv` = the
payoff walked once along the forward path (every underlying at its `market`
forward on every grid point, zero volatility, the same state machine, no
random numbers; at zero vol the Monte Carlo price equals it);
`correlation_one_npv` / `correlation_zero_npv` with their standard errors =
two extra runs on the same stream, paths, seed and grid with every
off-diagonal correlation forced to 1 (QuantLib's `StochasticProcessArray`
takes the spectral pseudo-square-root, so the rank-one matrix needs no
tweak) and to 0 (the identity); a worst-of structure is short correlation,
so `correlation_one_npv >= npv >= correlation_zero_npv` up to Monte Carlo
noise; with a single underlying both are the headline and no extra run is
made. `bond_only_npv` = `notional x DF_funding(maturity_payment_date)`.

`distribution` (over the individual simulated paths, each path of an
antithetic pair on its own; the pairs are averaged for the price statistics
only): `basket_performance_quantiles[]` per simulated observation date, the
5 / 25 / 50 / 75 / 95 % quantiles of the basket performance
(`performance_rule` applied, recorded whether or not the note is still alive
on that path), quantile = linear interpolation between order statistics, `h
= (n - 1) p`, `q = x[floor(h)] + (h - floor(h)) (x[floor(h) + 1] -
x[floor(h)])` on the ascending values (R / NumPy default); `payout_q05` ..
`payout_q95` of the undiscounted total payout per path as a fraction of the
notional (every coupon and redemption paid after the valuation date, loss
and upside included, plus any flow the past already fixed, the same on every
path); `payout_histogram[]` = 20 bins of equal width between the smallest
and the largest payout, `lower_k = min + k (max - min) / 20`, lower edge
inclusive, upper exclusive except the last, counts summing to `paths` (all
bins have zero width and every path is in the first one when every path pays
the same); `sample_paths[]` = the first `sample_paths` paths in simulation
order (path, its antithetic, next path, ...): `basket_performances[]` per
observation date, `payout`, `payout_pv` (discounted on the funding curve;
its average over the paths is `npv / notional`), `called_on` (absent when
the path reached the final event), `breached`. Raw underlying levels are
never returned.

`vanilla_repricing` (with `vanilla_repricing_sheets`, one `OptionQuoteSheet`
per underlying as `/calibrate-vol-surface` takes it, and optionally
`vanilla_repricing_filter`, the same `QuoteFilter`): the acceptance check of
the quotes -> surface -> simulation chain. Every usable quote of every sheet
— the per-line rules of `/calibrate-implied-forwards`, then the
out-of-the-money side with respect to the model forward (`Call` iff `strike
>= forward`) kept when its mid is at least `1e-5 x spot` and has a Black-76
implied vol — is priced as a European option on ONE extra simulation of the
note's model: same market, correlation, sequence,
seed and `paths`, on the note's time grid with the sheet expiries added as
mandatory dates (refined by `steps_per_year` as usual; an expiry after the
note's maturity extends the grid). Per option: `model_price =
D_note(T) x mean(max(+/-(S_T - K), 0))` with `D_note` the discounting curve
without the funding spread (listed options are not note liabilities),
`standard_error`, `inside_bid_ask`, `price_error = model_price - mid`,
`market_vol` and `model_vol` (Black-76 implied vols of the mid and of the
model price on the model forward `spot x D_div(T) / D_note(T)` and
`D_note(T)`, absent when they do not solve), `vol_error`. Per expiry the
forward and discount factor used, `quotes_used` and the `dropped` lines
with reasons; per sheet and overall `count`, `inside_bid_ask`,
`max_abs_price_error`, `max_abs_vol_error`, `max_standard_error`. The
headline and every other block are computed on the note's own grid and are
unchanged by the flag; `note_npv` / `note_standard_error` is the note walked
on the repricing paths (a different random stream, the same model: it
differs from the headline by Monte Carlo noise only, and the gate asserts
so). A sheet on a quanto underlying is a `400` (its listed options live in
the underlying's own measure, the note simulates the quanto-adjusted one);
a sheet whose underlying is in no note, a note with no sheet for any of its
underlyings, a non-European sheet, a sheet with no usable quote and a bad
filter value are `400`s naming the entry. Read the result against
`standard_error`: at a few thousand paths the deep wings of a short expiry
are at the sampling limit (`max_standard_error` says so) and an option is
"inside" only when the noise lets it be; the criterion is that no option
sits further than the Monte Carlo noise from its bid/ask band and that the
mean vol error per expiry is zero within that noise. The surfaces
`/calibrate-vol-surface` returns (`CubicStrikeLinearTime` grid, SVI
slices) meet it on the Scotia synthetic-surface examples every option of the core set — expiries from the second on, out-of-the-money mids of at least 0.25 % of the spot — within 3.5 standard errors of its bid/ask band at 2048, 16 384 and 65 536 paths, the mean vol error per expiry within 0.12 vol points from two months on at 65 536 paths; the 16-day expiry carries a residual −1 vol point from the 11 Euler steps of the local vol (halved at 1008 steps per year) and the deep short wings are at the sampling limit — reported, not hidden;
the same grid read `Bilinear` does not (the smile's curvature is lost
between grid strikes; see "Pricing conventions" below and `docs/versioning.md`
§ 0.15.0 for the numbers).

`timing`: wall-clock milliseconds per stage: `market_build` (curves, quanto
curves, grid, processes), `simulation` (path generation and payoff walk
together: the path engine hands each path to the walk as it is generated,
the two are not separable), `deterministic_forward`, `correlation_one`,
`correlation_zero` (only when run), `diagnostics` (sorting and summarising
the per-path records), `vanilla_repricing` (its one extra run);
`total_milliseconds` covers the whole note.

### Greeks

`greeks` in the request (`{}` for the defaults, or any of `spot_bump`,
`vol_bump`, `correlation_bump`, `funding_bump_bp`, `rate_bump_bp`, each
finite and `> 0`) turns on bump-and-reprice sensitivities. Every bumped run
is a full simulation on **common random numbers**: the same seed, paths,
grid and random sequence as the price, so the per-path difference between
the bumped and the base run is what is averaged, and its standard error is
small even where the price's is not. Cost: `1 + 2N + N + 3` simulations for
`N` underlyings (`1 + 2 + 1 + 2` for a single underlying, which has no
correlation sensitivity): the BNP note (4 underlyings) is 16 runs, about
21 s of engine time at 65 536 Sobol paths against 1.5 s for the plain
price. The JSON gateway's RPC deadline is 10 s and a deployment adds its
own (`QUANTRA_REQUEST_BUDGET_MS`, the proxy's route timeout; the engine
checks the budget before every extra run and returns `504` when it has
expired), so size the paths of a Greeks request to the deadline: the BNP
Greeks example uses 16 384 paths (about 6 s through the gateway). Every
sensitivity is in the note currency,
scaled to the unit move, and carries `<name>_standard_error`, the Monte
Carlo standard error of the per-sample difference (the path engine's
estimator on the differences, not a difference of two standard errors). A
`greeks` block on a note whose past already decided it (`Called` /
`Matured`) is a `400`: there is nothing to bump. The bump sizes used are
echoed in the block.

| Field | Definition |
| --- | --- |
| `underlyings[].delta` | NPV change per +1 % of the spot: central difference, the spot quote bumped to `spot x (1 +/- spot_bump)`, `(V+ - V-) / (2 spot_bump) x 0.01`. Only the spot moves: the initial level (strike fixing), the dividend curve and the volatility stay (sticky strike for a surface, whose local vol is unchanged in absolute strike). |
| `underlyings[].gamma` | Change of `delta` per +1 % of the spot: `(V+ - 2 V0 + V-) / spot_bump^2 x 0.01^2`. |
| `underlyings[].vega` | NPV change per +1 vol point: the underlying's Black volatility rebuilt with every value shifted by `+vol_bump` (the constant, every node of a term vol, every entry of a strike x expiry surface, from which the Dupire local vol is re-derived), `(V(sigma + vol_bump) - V0) / vol_bump x 0.01`. For a quanto underlying the shifted vol also enters the quanto drift `-rho sigma_S sigma_FX`. |
| `correlation_sensitivity` | NPV change per +0.01 on every off-diagonal correlation: every entry shifted by `+correlation_bump` and clipped to `[-1, 1]`, `(V(rho + bump) - V0) / correlation_bump x 0.01`. Absent for a single underlying. A worst-of structure is long correlation (its coupons and calls become likelier, its put cheaper), so it is normally positive. With a `correlation_markup` the bump is applied on top of the marked-up matrix. |
| `correlation_bump_repaired`, `correlation_bumped` | When the bumped matrix is not positive semi-definite (smallest eigenvalue below `-1e-10`) it is repaired before the run: `V diag(max(lambda, 0)) V^T` (`SymmetricSchurDecomposition`), rescaled to unit diagonal; the flag says so and `correlation_bumped` is the matrix actually used (row-major, underlying order, after clipping and any repair). A 2 x 2 matrix never needs the repair; a singular 3 x 3 one bumped uniformly does. Absent / `false` for a single underlying. |
| `funding_dv01` | NPV change per +1 bp of funding spread: `funding_spread_bp + funding_bump_bp` on the discounting only (the processes still drift on the plain curve), `(V(s + bump) - V0) / funding_bump_bp`. Negative for any note that pays. |
| `rho` | NPV change per +1 bp parallel move of `discounting_curve`: a continuously compounded zero spread of `+rate_bump_bp` (`ZeroSpreadedTermStructure`, Actual/365 (Fixed)) on the note curve, used both as the processes' risk-free curve and under the funding spread for discounting, `(V(r + bump) - V0) / rate_bump_bp`. A note-currency underlying's forward rises with it; a quanto underlying's drift `r_local - q - rho sigma_S sigma_FX` does not move (its quanto curve is rebuilt on the shifted note curve, which cancels), so for a quanto basket `rho` is mostly the discounting. |

Each bumped run adds a `timing` stage when `timing` is on:
`greek_spot_up_<id>`, `greek_spot_down_<id>`, `greek_vol_<id>` per
underlying, `greek_correlation` (two or more underlyings), `greek_funding`,
`greek_rate`, after the diagnostic stages.

The Python reference reproduces every bumped market from the request JSON
exactly as the engine rebuilds it and forms the same per-sample
differences on the same stream, so the gate compares every Greek, its
standard error and the bumped matrix to the print precision (12 decimals),
the surface-vol case included (the bumped surface is rebuilt on both sides,
there is no adapter around the original object).

### Pricing conventions

- Model: per underlying a `GeneralizedBlackScholesProcess(spot, dividend',
  r_note, vol)` where `r_note` is `discounting_curve` without the funding
  spread and `dividend'` is the underlying's dividend curve, or for a quanto
  underlying the `QuantoTermStructure(dividend, riskFree = r_note, foreign =
  quanto.discounting_curve, sigma_S at initial_level, sigma_FX at the FX
  spot, rho)`, so the drift is `r_local - q - rho sigma_S sigma_FX`. `rho` is
  QuantLib's convention: the correlation with the FX rate quoted as
  underlying-currency units per note-currency unit (CHF per USD for a CHF
  index in a USD note); the other quoting direction flips its sign. Positive
  `rho` lowers the drift. The processes are correlated by the
  `StochasticProcessArray` (spectral pseudo-square-root).
- Local vol: a strike-dependent vol — a strike x expiry `BlackVolSpec`
  (`SmileCube3D`, `SurfaceFromPrices`) or an `SviVolSpec` — is simulated
  as Dupire local vol (QuantLib `LocalVolSurface`: finite differences of
  the Black variance in log-strike and time) with log-Euler steps on the
  `steps_per_year` grid. A grid is read with its `surface_interpolator`
  (`Bilinear` by default for a supplied grid, `CubicStrikeLinearTime` on
  the grids `/calibrate-vol-surface` returns, `Bicubic` on request; see
  "Volatility surface from option quotes" > "The two payloads") with
  CONSTANT strike extrapolation on both sides (a strike beyond the grid
  reads the edge strike's vol; the same object the equity option pricer
  reads) and, beyond the last expiry, a flat implied vol (the last
  expiry's total variance scaled linearly in time); the local vol is read
  at the simulated level clamped to the interior of the strike grid
  (`[min strike x e^0.001, max strike x e^-0.001]`, wider than the
  finite-difference stencil), so a path beyond the quoted strikes evolves
  with the edge strike's local vol and the stencil never straddles the
  extrapolation kink (where constant extrapolation would make the total
  variance concave and the local variance negative). An SVI payload is
  global in strike: no edge, the clamp is a no-op. One local-vol object
  per distinct (vol, spot, dividend, discount) market is shared by every
  process array of the request. A `Bilinear` grid is the documented
  limit: between two grid strikes its variance is linear, so the smile's
  curvature (the `d2w/dk2` term of Dupire's formula) is zero inside every
  cell and sits in a spike at each node that the paths never meet; the
  simulated distribution is that of a surface without curvature and the
  `vanilla_repricing` diagnostic shows it (2 to 3 vol points high at one
  year on the synthetic SVI surfaces). `CubicStrikeLinearTime` and the SVI
  payload keep the curvature and reprice the quotes from two months on the mean vol error per expiry is within 0.12 vol points at 65 536 paths and every option with a mid of at least 0.25 % of the spot is within 3.5 standard errors of its bid/ask; the note prices 951.9 ± 6.2 instead of 939.6 ± 7.1 at 2048 paths — the lost convexity was worth 12 per 1,000.
- Grid: Actual/365 (Fixed) years from the valuation date to every future
  dated observation (accrual-only events are not observed); with
  `steps_per_year` each interval is split into `ceil(length x
  steps_per_year)` equal log-Euler steps; without it the steps are the
  exact lognormal ones. A `DailyClose` barrier is tested on the simulated
  closes only, with no continuity correction (the barrier is discrete by
  contract); set `steps_per_year` to the trading days per year.
- Random numbers: `Sobol` + `brownian_bridge` = `SobolBrownianBridgeRsg`
  (Diagonal ordering, JoeKuoD7 direction integers); `Sobol` without the
  bridge = `SobolRsg` with JoeKuoD7 through the inverse normal;
  `PseudoRandom` = Mersenne Twister. The dimension is `underlyings x steps`,
  consumed time-major; the antithetic path negates every draw and the pair
  is one sample of the standard error. The Python reference
  (`tests/contract/ql_reference.py`) reproduces the stream, so the gate
  compares the two to the print precision.
- Discounting: every cash flow at its payment date on
  `ZeroSpreadedTermStructure(discounting_curve, funding_spread_bp / 1e4,
  Continuous, Annual, Actual/365 (Fixed))`; the processes drift on the plain
  curve.
- Seasoning: events on or before the valuation date are resolved from
  `historical_levels` through the same walk; a note the past already decided
  is not simulated (`npv` = the known payment discounted when it is still
  ahead, 0 otherwise, `paths = 0`). A past Phoenix coupon paid after the
  valuation date is added to an `Alive` note's NPV with zero standard error.
- Standard errors are the sample standard error of the (antithetic)
  estimator; with Sobol it is an indication, not a confidence bound.

### Rejected requests

Each is a `400` naming the field (`notes[<i>].<path>: ...`): an empty
`notes`, `underlyings` or `events`; a missing, non-positive or non-finite
`notional` or `initial_level`; a duplicate `underlying_id`; a missing
`performance_rule`, `coupon_rule.memory` / `pay_only_on_call`,
`capital.barrier` (or outside `(0, 1]`) / `monitoring`, `monte_carlo.paths`;
events not strictly increasing; a final event without a trigger and
without `is_final`; an `is_final` event with a `coupon`, `coupon_trigger`
or `autocall_trigger`, without a `payment_date`, with a `payment_date`
other than `maturity_payment_date`, or that is not the last event; an
accrual-only event with a `payment_date`, without a `coupon` or without
`coupon_rule.memory`; a `coupon_trigger` without a `coupon`; a trigger
without a `payment_date`, or one before the observation date; a negative
trigger, coupon, `put_strike` or `participation`; `maturity_payment_date`
before the final observation; a duplicate `historical_levels` date, a level
after the valuation date, or a past observation without a level; a `quanto`
block on a note-currency underlying or none on a foreign one, its
correlation missing or outside `[-1, 1]`, an FX spot of another pair, an FX
vol that is a surface; an `EquityUnderlyingSpec` without `currency`;
`correlation_id` missing with two or more underlyings or present with one,
or whose labels are not exactly the basket; a `correlation_markup` that is
not finite or outside `[-1, 1]`, or given with a single underlying; a
`DailyClose` barrier or a
surface vol without `steps_per_year`, `steps_per_year` with constant / term
vols and a European barrier, or 0; `knocked_in` missing when required or
present otherwise; odd `paths` with `antithetic`; Sobol `seed` 0;
`brownian_bridge` with `PseudoRandom`; `upside.strike` / `participation`
missing or `cap <= 0`; a surface whose local vol cannot be evaluated along
the paths (names the vol ids and the remedy: the surface is not
arbitrage-free where the paths went); `diagnostics.sample_paths`
above 100, without `distribution`, or above `paths`;
`diagnostics.vanilla_repricing` without `vanilla_repricing_sheets` or sheets
(or a filter) without the flag, two sheets for one underlying, a sheet
missing `id` / `underlying_id` / `quotes` / an `expiry`, a non-European
sheet, a sheet on a quanto underlying, a sheet for an underlying of no
note, a note with no sheet for any of its underlyings, a sheet with no
usable quote (listing the reasons), a bad `vanilla_repricing_filter`
value. Missing required
strings and tables (`trade_id`, `currency`, `volatility_id`,
`discounting_curve`, `coupon_rule`, `capital`, `maturity_payment_date`,
`monte_carlo`, ...) are rejected by the JSON parser. A `greeks` bump that
is not finite and `> 0` (`PriceStructuredNoteRequest.greeks.<field>`), or
`greeks` on a note whose past already decided it (`notes[<i>]: no Greeks
for a note that is not alive`), is a `400`. An unknown underlying, vol,
curve, FX spot or correlation id is a `404`.

### Known limits

Composite (non-quanto) foreign underlyings are not supported (the FX rate
is not simulated); the funding spread is flat (use a funding curve as
`discounting_curve` for a term structure); best-of / average baskets have
no weights; issuer-callable notes (Longstaff-Schwartz), lookbacks, coupon
floors, FX-settled notes and scripted payoffs are out of scope; paths are
not shared across the notes of one request; the correlation sensitivity is
one uniform off-diagonal bump (no per-pair Greeks); delta and vega are per
underlying (no cross-gammas, no vol-of-vol). The correlation matrix as
supplied is validated (symmetric, unit diagonal, positive semi-definite)
and never repaired; only a matrix the engine shifts itself (the Greeks'
bump, the `correlation_markup`) is repaired, and the response says so. The
mark-up is one uniform shift (no per-pair mark-ups).

## Implied forwards and dividends from option quotes

`POST /calibrate-implied-forwards` reads a sheet of listed European option
quotes for one underlying on the as-of date and returns, per expiry, the
forward the market implies, the discount factor it implies (the index's
funding), the continuous funding spread against the request's discounting
curve, the continuous dividend yield that reproduces the forward, and a fit
report; plus a ready-to-paste dividend curve (`TermStructure`,
`InterpolatedZero`) under the request's `dividend_curve_id`. It is the first
step of the market-data chain for equity pricing (forwards → vol surface →
models): nothing is priced, and the response says what was dropped and why.

### Request

```json
{
  "pricing": {
    "as_of_date": "2026-09-30",
    "quotes": [{"id": "NDX_SPOT", "kind": "Price", "value": 24000.0, "quote_type": "Curve"}],
    "rates": {"curves": [ <discounting curve>, <flat 0 % placeholder dividend curve> ]},
    "equity": {"equity_underlyings": [{"id": "NDX", "currency": "USD",
               "spot_quote_id": "NDX_SPOT", "dividend_yield_curve_id": "<placeholder id>"}]}
  },
  "sheet": {
    "id": "NDX_2026-09-30", "underlying_id": "NDX", "option_style": "European",
    "quotes": [
      {"expiry": "2026-10-16", "strike": 23000.0,
       "call_bid": 1101.0, "call_ask": 1111.0, "put_bid": 91.0, "put_ask": 93.0,
       "call_open_interest": 500.0, "put_open_interest": 800.0}
    ]
  },
  "discounting_curve": "USD_OIS",
  "filter": {"max_relative_spread": 0.05, "min_quotes_per_expiry": 4, "moneyness_window": 0.25},
  "dividend_curve_id": "NDX_DIV_IMPLIED"
}
```

| Field | Meaning |
|---|---|
| `sheet.underlying_id` | `EquityUnderlyingSpec.id`; only its spot quote is read. Its `dividend_yield_curve_id` must resolve (the shared registry requires it) but is not used here: a flat 0 % `InterpolatedZero` curve is the placeholder. |
| `sheet.option_style` | `European` (default). `American` / `Bermudan` → `400`: put-call parity, which the fit rests on, does not hold under early exercise. Index options (NDX, RUT, SPX, SX5E) are European; single stocks are not. |
| `sheet.quotes[]` | One line per expiry × strike: `expiry`, `strike`, `call_bid`, `call_ask`, `put_bid`, `put_ask`, optional `call_volume`, `put_volume`, `call_open_interest`, `put_open_interest`. Prices per unit of underlying. Every scalar is optional on the wire so that an absent value is reported as a dropped line, never read as 0. |
| `discounting_curve` | Curve id in `pricing.rates.curves`; gives `DF_curve(T)` and `r(T)`. |
| `filter` | Optional; the values applied (request or defaults) are echoed in the response. |
| `dividend_curve_id` | Id of the returned curve spec. |

Filter rules, applied per line in this order, each one a dropped line with
that reason: `strike missing or not > 0`; `missing call or put side`;
`non-finite price`; `negative bid or non-positive ask`; `crossed quote
(bid > ask)`; `open interest below min_open_interest` (only when the
threshold is given; a line without open-interest fields is then dropped);
`relative spread above max_relative_spread` ((ask − bid) / mid on each side);
`duplicate strike` (the first line wins). `max_relative_spread` and
`moneyness_window` must be > 0 and `min_quotes_per_expiry` ≥ 2 (`400`
naming the field).

### Method

For European options at one expiry, `C(K) − P(K) = DF · (F − K)`: the mid
call-minus-put is affine in the strike with intercept `a = DF · F` and slope
`b = −DF`. The engine fits that line by weighted least squares over the
surviving strikes of the expiry, with weights
`w = 1 / ((call_ask − call_bid) + (put_ask − put_bid))` (equal weights for the
whole expiry if any pair has a zero total spread), and reads

```
DF_implied = −b
F          = a / DF_implied = −a / b          (the strike where C − P = 0)
```

Two passes: the first over every surviving pair gives a provisional `F1`;
the second over the pairs with `K` in `[F1 (1 − w), F1 (1 + w)]`
(`w = moneyness_window`) gives the reported values; strikes outside the
window are reported as dropped with the window bounds in the reason. An
expiry with fewer than `min_quotes_per_expiry` pairs after either pass, or
whose fitted slope is not negative, is reported as dropped
(`usable: false`, `dropped_reason`), not rejected; when no expiry survives
the request is a `400` listing every expiry's reason.

With `T` the Act/365F year fraction from the as-of date and `DF_curve` the
discounting curve's discount factor at the expiry date:

```
r(T)               = −ln(DF_curve) / T                      (continuous)
q(T)               = r(T) − ln(F / S) / T                   (from F = S e^{(r − q) T})
implied_funding_bp = −ln(DF_implied / DF_curve) / T · 1e4   (> 0: options imply a
                                                             higher funding rate)
fit_residual_rmse  = sqrt(Σ (C − P − a − b K)² / n)         (price units, final pass)
max_abs_residual   = max |C − P − a − b K|
```

The residuals are unweighted, so they read directly against the bid/ask.
The algebra, in the same words, is in
`src/evaluators/implied_forwards_evaluator.h`; the Python reference
`calibrate_implied_forwards_ql` in `tests/contract/ql_reference.py` repeats
it in plain Python and the catalog compares every field.

### Response

```json
{
  "sheet_id": "NDX_2026-09-30", "underlying_id": "NDX", "spot": 24000.0,
  "filter": {"max_relative_spread": 0.05, "min_quotes_per_expiry": 4, "moneyness_window": 0.25},
  "expiries": [
    {"expiry": "2026-10-16", "time": 0.0438, "quotes_used": 21, "quotes_dropped": 4,
     "dropped": [{"expiry": "2026-10-16", "strike": 16800.0,
                  "reason": "outside moneyness window [18025.2, 30042.0] around first-pass forward 24033.6"}],
     "usable": true,
     "forward": 24033.597, "implied_discount_factor": 0.998252, "curve_discount_factor": 0.998252,
     "implied_funding_bp": 0.0, "implied_dividend_yield": 0.008,
     "fit_residual_rmse": 1.2e-12, "max_abs_residual": 3.6e-12},
    {"expiry": "2026-12-18", "time": 0.216, "quotes_used": 0, "quotes_dropped": 1,
     "dropped": [{"expiry": "2026-12-18",
                  "reason": "fewer than min_quotes_per_expiry (4) usable pairs after the quote filters: 3"}],
     "usable": false,
     "dropped_reason": "fewer than min_quotes_per_expiry (4) usable pairs after the quote filters: 3"}
  ],
  "forwards": [{"expiry": "2026-10-16", "time": 0.0438, "forward": 24033.597,
                "implied_dividend_yield": 0.008}],
  "dividend_curve_spec": {
    "id": "NDX_DIV_IMPLIED", "day_counter": "Actual365Fixed", "interpolator": "Linear",
    "bootstrap_trait": "InterpolatedZero", "reference_date": "2026-09-30",
    "points": [
      {"point_type": "ZeroRatePoint", "point": {"date": "2026-09-30", "zero_rate": 0.008,
                                                "compounding": "Continuous", "frequency": "Annual"}},
      {"point_type": "ZeroRatePoint", "point": {"date": "2026-10-16", "zero_rate": 0.008,
                                                "compounding": "Continuous", "frequency": "Annual"}}
    ]
  }
}
```

`expiries` lists every expiry of the sheet in date order; the fit fields
(`forward` onwards) are present iff `usable`, so an absent `forward` always
means "not fitted", never 0. `quotes_used + quotes_dropped` is the number of
lines of that expiry (plus one for the expiry-level entry when the whole
expiry was dropped). `forwards` is the plain table of fitted expiries.

### Using the dividend curve

`dividend_curve_spec` is a complete `TermStructure`: append it to
`pricing.rates.curves` and set the underlying's `dividend_yield_curve_id` to
its id. It has a node at the as-of date carrying the first fitted expiry's
yield (the curve's first node is its reference date, so the short end is
flat) and one node per fitted expiry with `q(T)`, continuous, Act/365F,
linear between nodes. Pricing on the request's discounting curve and this
dividend curve gives `S · DF_div(T) / DF_curve(T) = F` exactly at every
fitted expiry — a call/put pair priced by `/price-equity-option` reproduces
the forward through parity (the contract test checks it). The yields are
"equity repo + dividend": whatever the market forward implies relative to
the chosen discounting curve, which is what pricing should carry; the
`implied_funding_bp` column shows how much of it is funding rather than
dividends. Between fitted expiries the curve interpolates the zero yield;
beyond the last one it is flat.

### Rejected requests

| Request | Status |
|---|---|
| `sheet.option_style` not `European` | `400` naming `option_style` |
| `sheet.quotes` empty, a quote without `expiry`, a malformed date | `400` |
| `filter.max_relative_spread` ≤ 0, `filter.moneyness_window` ≤ 0, `filter.min_quotes_per_expiry` < 2 | `400` naming the field |
| `discounting_curve` or `sheet.underlying_id` not in `pricing` | `404` |
| no expiry survives the filters and the minimum-pairs rule | `400` listing every expiry's reason |

### Data

The repository carries one quote sheet, synthetic and labelled so:
`examples/data/implied_forwards/synthetic_ndx_like_2026-09-30.json`,
generated by `tests/tools/make_synthetic_quote_sheet.py` from a stated spot,
zero curve, dividend yield per expiry and Black smile (its README has the
numbers). Vendor files (Cboe DataShop "Option EOD Summary") are converted
with `tools/option_quotes/cboe_eod_to_quote_sheet.py` and are never
committed.

## Volatility surface from option quotes

`POST /calibrate-vol-surface` turns the same `OptionQuoteSheet` into a Black
implied-volatility surface on a strike x expiry grid: the second step of the
chain quotes → forwards → surface → models. It returns a complete
`VolSurfaceSpec` (a `BlackVolSpec` of shape `SmileCube3D`, read with
`surface_interpolator: CubicStrikeLinearTime`) ready to paste into
`pricing.volatility.vol_surfaces`, with `SviPerExpiry` a second spec of
the same surface as its fitted SVI slices (`vol_surface_spec_svi`, an
`SviVolSpec` payload, also ready to paste), the forwards it used, a
per-expiry report and the repricing of every used quote on the returned
grid. Two methods: Andreasen-Huge local-vol interpolation (default;
arbitrage-free by construction) and one SVI slice per expiry with
butterfly and calendar checks.

### Request

```json
{
  "pricing": { "...": "as for /calibrate-implied-forwards: spot quote, discounting curve, EquityUnderlyingSpec" },
  "sheet": { "id": "NDX_2026-09-30", "underlying_id": "NDX", "option_style": "European", "quotes": [ "..." ] },
  "discounting_curve": "USD_OIS",
  "filter": {"max_relative_spread": 0.05, "min_quotes_per_expiry": 4, "moneyness_window": 0.25},
  "forwards": [ "<the expiries array of a /calibrate-implied-forwards response, optional>" ],
  "method": "AndreasenHuge",
  "output_grid": {"strikes_as_moneyness": [0.5, 0.525, "...", 1.5],
                  "expiries": [{"n": 16, "unit": "Days"}, {"n": 51, "unit": "Days"}]},
  "vol_surface_id": "NDX_VOL",
  "svi": {"enforce_butterfly": true, "enforce_calendar": true}
}
```

| Field | Meaning |
|---|---|
| `sheet`, `discounting_curve`, `filter` | As for `/calibrate-implied-forwards`; same per-line rules, same reasons. `filter.moneyness_window` concerns the forward fit only: the vol fit uses every line that passes the per-line rules. European exercise only (`400` otherwise). |
| `forwards` | Optional. The `expiries` array of an implied-forwards response pasted as is; only `expiry`, `usable`, `forward` and `implied_discount_factor` are read (non-usable entries skipped). Absent: the same fit is run here with `filter`, and `forwards_source` says `"fitted"` instead of `"request"`. A sheet expiry without a forward is reported unusable. |
| `method` | `AndreasenHuge` (default) or `SviPerExpiry`. |
| `output_grid.strikes_as_moneyness` | Strikes as multiples of the spot, > 0, strictly increasing, at least 2. Default `0.500, 0.525, …, 1.500` (41 points). The returned surface is keyed by absolute strike (`strike = moneyness x spot`); the spot used is echoed as `spot`. |
| `output_grid.expiries` | Calendar-day periods from `as_of_date` (`{n, unit}`; months and years are added on a NullCalendar, unadjusted). Default: the fitted sheet expiries. `SviPerExpiry` requires each one to be a fitted sheet expiry (`400` otherwise: the slices are not interpolated in time); `AndreasenHuge` accepts any date up to the last fitted expiry. |
| `vol_surface_id` | Id of the returned spec. |
| `svi.enforce_butterfly`, `svi.enforce_calendar` | `SviPerExpiry` only, default true: fix what the checks find (below) or report only. |

### Method

**Implied vols.** Per expiry with forward `F` and implied discount factor
`DF` (from `forwards` or the internal fit), every line that passes the
per-line rules contributes its out-of-the-money side: the call for
`K ≥ F`, the put below. Its mid is inverted with Black-76 on the forward
(`blackFormulaImpliedStdDev(type, K, F, mid, DF)`, `σ = stdDev / √T`, `T`
Act/365F). Dropped with a reason: a mid below the quote resolution
`1e-5 x spot` (0.24 points on a 24,000 index: such a price carries no vol
information, any vol prices inside its bid/ask), a mid outside the Black
bounds (no vol solves). An expiry needs
`max(filter.min_quotes_per_expiry, 5)` used quotes for `SviPerExpiry`
(five parameters) and `filter.min_quotes_per_expiry` for `AndreasenHuge`;
with fewer it is reported unusable. `400` when no expiry is usable.

**AndreasenHuge.** Every used quote becomes a calibration pair of QuantLib's
`AndreasenHugeVolatilityInterpl` (the out-of-the-money `VanillaOption` and a
quote carrying its implied vol; interpolation `CubicSpline`, calibration
type `CallPut`, 500 grid points, Levenberg-Marquardt), on a risk-free curve
built from the implied discount factors and a dividend curve with
`DF_q(T) = F · DF / S` (both log-linear, Act/365F), so the model's forwards
and discounting are the sheet's. The method calibrates a piecewise local
volatility expiry by expiry to the Black prices of those vols on a one-step
finite-difference scheme (Andreasen & Huge, *Volatility Interpolation*,
2010) and is free of static arbitrage by construction; no weights, the
calibration matches prices. `AndreasenHugeVolatilityAdapter` then gives the
Black vol at every grid point by inverting the model price. A point is
inverted only where the model's out-of-the-money price is at least
`1e-4 x spot` (the finite-difference price carries noise of about
`1e-7 x spot`, which below that moves the implied vol by more than a tenth
of a vol point and inverts to arbitrary vols where the true price is ~0);
the other points are filled flat from the nearest inverted strike of the
same expiry and counted in `grid_points_filled`. On the flat 20 % synthetic
sheet this keeps every grid vol within 0.1 vol point of 20 %.
`andreasen_huge_error` reports QuantLib's calibration error (min, max, mean
in vol units over the calibration set).

**SviPerExpiry.** Per usable expiry QuantLib's `SviInterpolation`
(Levenberg-Marquardt on the vega-weighted implied-vol residuals, several
deterministic starts; the parameter transforms keep `b ≥ 0`, `|ρ| < 1`,
`σ > 0`, `a + b σ √(1 − ρ²) ≥ 0`, `b (1 + |ρ|) ≤ 4`) fits the raw SVI
total variance

```
w(k) = a + b (ρ (k − m) + sqrt((k − m)² + σ²)),   k = ln(K / F)
```

Butterfly check: Durrleman's `g(k) = (1 − k w′/(2w))² − (w′²/4)(1/w + 1/4)
+ w″/2` on 201 points of the output strike range; `min g < 0` is a
violation (`butterfly_violation`, `butterfly_min_g`). With
`enforce_butterfly` the five parameters are refitted from the fitted ones
by Levenberg-Marquardt on the same weighted residuals plus a penalty
`100 · max(0, −g(k_j))` per point; `butterfly_action` carries `min g`
before and after (`"refit with penalty: min g -0.41 -> 0.0012"`). Grid vols
are `sqrt(w(k) / T)`.

**Calendar check (both methods).** At every grid strike total variance
`σ² T` must not decrease from one grid expiry to the next; a point where
`w_i < w_{i−1} + 1e-8` is counted in `calendar_violations`. `SviPerExpiry`
with `enforce_calendar` clips it to `w_{i−1} + 1e-8` (`calendar_action`,
`calendar_max_clip_vol` = largest vol change); `AndreasenHuge` reports only
(the surface is arbitrage-free by construction; a residual count is within
the implied-vol inversion tolerance).

**Fit error and repricing.** `rmse_vol` / `max_abs_error_vol` per expiry
compare the calibrated object (the SVI slice, the Andreasen-Huge surface)
with the market implied vols of the used quotes, in vol units (0.01 = one
vol point), unweighted. `vanilla_repricing` then rebuilds the returned
grid exactly as the pricers read it (`CubicStrikeLinearTime`: per expiry a
natural cubic spline of total variance in log-strike, linear in total
variance between expiries, NullCalendar, Act/365F, extrapolation allowed)
and reprices every used quote on it: `model_vol`, `model_price` (Black on
the same `F` and `DF`), `inside_bid_ask`, `price_error = model − mid`,
`vol_error = model_vol − market_vol`, with `count`, `inside_bid_ask`,
`max_abs_price_error`, `max_abs_vol_error` on top. This is the acceptance
check of the surface, grid interpolation included: a quote between grid
strikes reads an interpolated vol. On the synthetic SVI sheet (strikes every
2.5 % of spot, 1 % bid/ask) the default grid reprices every used quote
inside its bid/ask under both methods.

### Response

```json
{
  "sheet_id": "NDX_SYN_SVI_2026-09-30", "underlying_id": "NDX_SYN", "spot": 24000.0,
  "method": "SviPerExpiry", "forwards_source": "fitted",
  "filter": {"max_relative_spread": 0.05, "min_quotes_per_expiry": 4, "moneyness_window": 0.25},
  "forwards": [{"expiry": "2026-10-16", "time": 0.0438, "forward": 24033.597, "implied_discount_factor": 0.998252}],
  "expiries": [
    {"expiry": "2026-10-16", "time": 0.0438, "usable": true,
     "forward": 24033.597, "implied_discount_factor": 0.998252,
     "quotes_used": 19, "quotes_dropped": 6,
     "dropped": [{"expiry": "2026-10-16", "strike": 16800.0,
                  "reason": "out-of-the-money put mid below 1e-5 of the spot (quote resolution, no vol information)"}],
     "rmse_vol": 1.1e-16, "max_abs_error_vol": 2.2e-16,
     "svi": {"a": 0.000591, "b": 0.024078, "sigma": 0.034390, "rho": -0.7, "m": 0.033709},
     "butterfly_min_g": 0.61, "butterfly_violation": false, "butterfly_action": "none",
     "calendar_violations": 0, "calendar_action": "none", "grid_points_filled": 0}
  ],
  "strikes_as_moneyness": [0.5, 0.525, "...", 1.5],
  "grid_strikes": [12000.0, 12600.0, "...", 36000.0],
  "grid_expiries": ["2026-10-16", "2026-11-20", "..."],
  "vol_surface_spec": {
    "id": "NDX_SYN_VOL", "payload_type": "BlackVolSpec",
    "payload": {
      "base": {"reference_date": "2026-09-30", "calendar": "NullCalendar",
               "business_day_convention": "Unadjusted", "day_counter": "Actual365Fixed",
               "shape": "SmileCube3D"},
      "expiries": [{"n": 16, "unit": "Days"}, {"n": 51, "unit": "Days"}, "..."],
      "strikes": [12000.0, 12600.0, "..."],
      "surface_vols": {"n_rows": 8, "n_cols": 41, "values": ["row-major by expiry then strike"]},
      "allow_extrapolation": true, "surface_interpolator": "CubicStrikeLinearTime"
    }
  },
  "vol_surface_spec_svi": {
    "id": "NDX_SYN_VOL", "payload_type": "SviVolSpec",
    "payload": {
      "base": {"reference_date": "2026-09-30", "calendar": "NullCalendar",
               "business_day_convention": "Unadjusted", "day_counter": "Actual365Fixed"},
      "slices": [{"expiry": "2026-10-16", "forward": 24033.597,
                  "a": 0.000591, "b": 0.024078, "sigma": 0.034390, "rho": -0.7, "m": 0.033709}, "..."],
      "time_interpolation": "LinearTotalVariance", "allow_extrapolation": true
    }
  },
  "vanilla_repricing": {
    "count": 169, "inside_bid_ask": 169, "max_abs_price_error": 3.1e-9, "max_abs_vol_error": 4.4e-16,
    "quotes": [{"expiry": "2026-10-16", "strike": 24000.0, "option_type": "Put",
                "bid": 377.3, "ask": 381.1, "mid": 379.2, "market_vol": 0.2322,
                "model_vol": 0.2322, "model_price": 379.2, "inside_bid_ask": true,
                "price_error": -1.2e-10, "vol_error": -2.8e-17}]
  }
}
```

`expiries` lists every sheet expiry in date order; `forward` onwards is
present iff `usable`. `svi` and `butterfly_*` appear with `SviPerExpiry`,
`andreasen_huge_error` and `grid_points_filled` with `AndreasenHuge`.
`method` is always printed (it is an optional enum on the response). The
surface's expiries are calendar-day periods on a NullCalendar so that the
pricers' `calendar.advance(reference_date, period, Unadjusted)` lands on
the grid dates exactly. `vol_surface_spec_svi` is present with
`SviPerExpiry` only (one slice per fitted expiry: the forward the slice is
written against and its final `a, b, sigma, rho, m`, after the butterfly
refit; the grid's calendar clip is not applied to the slices); with
`AndreasenHuge` it is absent and `vol_surface_spec_svi_note` says why (a
piecewise local volatility has no closed-form slices).

### The two payloads and how they are read

| Payload | Reading | Use |
|---|---|---|
| `BlackVolSpec` `SmileCube3D` (the grid, `vol_surface_spec`) | `surface_interpolator`: `Bilinear` (default of a supplied grid — QuantLib `BlackVarianceSurface`, linear in total variance inside every cell, so the smile's curvature is lost between nodes and a Dupire local vol derived from it misprices vanillas by 2 to 3 vol points at a year), `Bicubic` (QuantLib's 2-D spline, a spline in time too through the zero-variance column at t = 0, which distorts the short end; ~15x the cost), `CubicStrikeLinearTime` (what `/calibrate-vol-surface` returns: per expiry a natural cubic spline of total variance in log-strike — C2, every node reproduced, the curvature kept between nodes — linear in total variance between expiries at fixed strike, constant beyond the first and last strike, flat implied vol beyond the last expiry; two 1-D evaluations per read). A grid you supply yourself keeps `Bilinear` unless it says otherwise: no existing request changes. | Any grid, from this endpoint or elsewhere. |
| `SviVolSpec` (the slices, `vol_surface_spec_svi`) | The raw SVI total variance of each slice, `w(k) = a + b (ρ (k − m) + sqrt((k − m)² + σ²))`, `k = ln(K / forward)`, Black vol `sqrt(w / T)`; linear in total variance between the bracketing slices at fixed strike (`time_interpolation: LinearTotalVariance`, the only value), `w(T₁, K) t / T₁` before the first slice and `w(Tₙ, K) t / Tₙ` after the last (flat implied vol on both ends; `allow_extrapolation` gates reading past the last slice). The formula is global in strike: no strike extrapolation exists or is needed. The exact surface the quotes were fitted to, with its exact curvature. `base.shape`, `constant_vol` and `quote_id` must be absent (`400`); slices strictly increasing after `reference_date`, `forward > 0`, `b ≥ 0`, `σ > 0`, `|ρ| < 1`, `a + bσ sqrt(1 − ρ²) ≥ 0` (`400` naming the slice). | Everywhere a Black vol is referenced: `volatility` on an equity option (a European option prices at the Black price of the slice's vol), `underlyings[].volatility_id` on a structured note (strike-dependent: Dupire local vol, `steps_per_year` required; vega bumps shift every vol by the bump — a shifted SVI smile is no SVI slice, so the shift is applied at every read), `/sample-vol-surfaces` `EquityBlack`. |

### Using the surface

Append `vol_surface_spec` to `pricing.volatility.vol_surfaces` and
reference its id from the trade: `volatility` on an equity option (the
contract test prices the used quotes this way, inside their bid/ask, next
to the implied dividend curve of `/calibrate-implied-forwards`), or
`underlyings[].volatility_id` on a structured note, where a strike x expiry
surface makes the simulation Dupire local-vol and requires
`monte_carlo.steps_per_year` (see "Structured notes"). Keep the forwards
consistent: price on the discounting curve the quotes were fitted against
and on the dividend curve the implied-forward step returned; otherwise the
surface's strikes sit at other forward moneyness than the market's. Vols
beyond the grid are extrapolated constant in strike (the edge strike's vol,
on both sides; `allow_extrapolation: true` lets a strike or an expiry
beyond the grid be read at all) and flat in implied vol beyond the last
expiry. Under a structured note the surface is simulated as Dupire local
vol; `diagnostics.vanilla_repricing` with the same sheet reprices the used
quotes through the paths (see "Structured notes", Diagnostics). Both
returned payloads pass that check on the Scotia synthetic-surface example
every option beyond the first expiry and the deep wings within the Monte Carlo noise of its bid/ask, the mean vol error per expiry within 0.12 vol points at 65 536 paths; the same grid read `Bilinear` does not (2 to 3 vol points
high at a year: the interpolation loses the smile's curvature), which is
why a grid from this endpoint carries `CubicStrikeLinearTime`.

### Rejected requests

| Request | Status |
|---|---|
| `sheet.option_style` not `European`; a bad `filter` value | `400` naming the field |
| `output_grid.strikes_as_moneyness` not finite, ≤ 0, not strictly increasing or fewer than 2 | `400` |
| `output_grid.expiries` not after `as_of_date`, not increasing, after the last fitted expiry, or (SVI) not a fitted sheet expiry | `400` naming the entry |
| `forwards` with a usable entry lacking `forward` / `implied_discount_factor`, a non-positive value, a duplicate expiry, or no usable entry at all | `400` |
| no usable expiry (nothing passes the rules, or every forward is missing) | `400` listing every expiry's reason |
| `vol_surface_id` missing | `400` |
| `discounting_curve` or `sheet.underlying_id` not in `pricing` | `404` |
| the Andreasen-Huge or SVI optimizer fails | `400` with QuantLib's message |

### Data

`examples/data/vol_surface/` carries one synthetic SVI sheet as two
requests (one per method), generated by
`tests/tools/make_synthetic_quote_sheet.py --preset svi --endpoint
vol-surface` from a stated SSVI surface (README there). The Scotia note
example `examples/data/structured_note/scotia_…_with_synthetic_surface.json`
shows the pasted result, clearly labelled synthetic. Vendor files are
converted with `tools/option_quotes/cboe_eod_to_quote_sheet.py` and never
committed.

## Correlation from price histories

`POST /calibrate-correlation` turns the price histories of two or more
underlyings into a `CorrelationMatrixSpec` ready for
`pricing.correlations.matrices`, with everything needed to reproduce it:
the dates common to every series inside a window, log or simple returns,
the sample correlation, optional shrinkage toward the constant-correlation
target (Ledoit-Wolf), and positive semi-definiteness enforced by an
eigenvalue floor. The engine never fetches closes: it receives them. There
is no `pricing` block and nothing is priced. A historical estimate is a
judgment input (the window, the frequency and the shrinkage are choices,
and the market's correlation is not the realised one); the structured
note's `correlation_markup` is where the desk's view goes.

### Request

```json
{
  "as_of_date": "2026-09-30",
  "series": [
    {"underlying_id": "NDX", "closes": [{"date": "2025-09-29", "value": 24611.35}, "..."]},
    {"underlying_id": "RTY", "closes": [{"date": "2025-09-29", "value": 2435.25}, "..."]}
  ],
  "estimator": {"window_days": 365, "returns": "Log", "frequency": "Daily",
                "shrinkage": "LedoitWolf", "min_overlap": 60, "eigenvalue_floor": 1e-8},
  "correlation_id": "NDX_RTY",
  "labels": ["NDX", "RTY"]
}
```

| Field | Meaning |
|---|---|
| `as_of_date` | Anchor of `window_days` and the date the estimate is labelled with. Required. Closes after it are used unless a window excludes them (documented, not an error). |
| `series[]` | At least two, distinct `underlying_id`s (the `EquityUnderlyingSpec.id`s the matrix will be used with: they become its labels). `closes[]` = `{date, value}` in strictly increasing date order, every value > 0, at least one entry. |
| `estimator.window_days` | Calendar days back from `as_of_date`: the window is `[as_of_date - window_days, as_of_date]`. `> 0`; `400` with `start_date` / `end_date`. |
| `estimator.start_date`, `end_date` | Explicit inclusive window, either bound optional. Absent and no `window_days`: every common date. `400` when `start_date` is after `end_date`. |
| `estimator.returns` | `Log` (default): `ln(P_t / P_{t-1})`; `Simple`: `P_t / P_{t-1} - 1`. |
| `estimator.frequency` | `Daily` (default): every common date. `Weekly`: every fifth common observation counted back from the last one (the most recent close is always used); the returns are then five-observation returns. |
| `estimator.shrinkage` | `None`, `LedoitWolf` (default) or `Fixed` (with `shrinkage_intensity` in `[0, 1]`, required iff `Fixed`). |
| `estimator.min_overlap` | Least number of common dates inside the window, before the frequency sampling (default 60, `>= 3`); `400` below it. |
| `estimator.eigenvalue_floor` | The eigenvalue negative ones are raised to when the matrix needs repair (default `1e-8`, in `[0, 1)`); see "Method". |
| `correlation_id` | Id of the returned spec. Required. |
| `labels` | Row/column order of the output, a permutation of the series ids (`400` otherwise). Absent = series order. |

### Method

Alignment: the observations used are the dates inside the window that every
series has (an inner join), in date order, then every fifth of them when
`Weekly`; at least three must remain. With `T` the number of returns
(`observations_used - 1`), `x_it` the return of series `i`, every sum in
date order:

```
m_i  = (1/T) Σ_t x_it,          y_it = x_it − m_i
S_ij = (1/T) Σ_t y_it y_jt                   (maximum-likelihood covariance)
r_ij = S_ij / sqrt(S_ii S_jj)                (the sample correlation; 1/T
                                              against 1/(T−1) cancels here)
annualised_volatility_i = sqrt(S_ii · T/(T−1) · 252 / step)   (unbiased, Act/252;
                                              step 1 Daily, 5 Weekly)
```

Shrinkage toward the constant-correlation target, Ledoit and Wolf (2004),
"Honey, I shrunk the sample covariance matrix": with `N` series the target
has every off-diagonal correlation equal to `rbar = mean of the r_ij, i < j`,
and the intensity is

```
pi_ij       = (1/T) Σ_t (y_it y_jt − S_ij)²,                    pi = Σ_ij pi_ij
theta_ii,ij = (1/T) Σ_t (y_it² − S_ii)(y_it y_jt − S_ij)
rho         = Σ_i pi_ii + Σ_{i≠j} (rbar/2) (sqrt(S_jj/S_ii) theta_ii,ij + sqrt(S_ii/S_jj) theta_jj,ij)
gamma       = Σ_ij (rbar sqrt(S_ii S_jj) [i≠j, else S_ii] − S_ij)²
delta       = max(0, min(1, ((pi − rho) / gamma) / T))
c_ij        = delta · rbar + (1 − delta) · r_ij     (i ≠ j),   c_ii = 1
```

With exactly two series the target is the sample correlation itself, so
`delta` is reported as 0 and the shrinkage changes nothing; the same for
`N >= 3` when every pair has the same sample correlation (`gamma = 0`).
`Fixed` uses the request's intensity in place of `delta`; `None` uses 0.

Eigenvalue floor: the smallest eigenvalue of the shrunk matrix (QuantLib
`SymmetricSchurDecomposition`) is reported; when it is below `-1e-10` (the
correlation registry's own acceptance rule, so a rank-deficient matrix such
as a perfectly correlated pair, eigenvalue 0, is returned as is) the matrix
is repaired as `V diag(max(lambda, eigenvalue_floor)) Vᵀ` rescaled to unit
diagonal (the structured note's bump repair with the floor in place of 0)
and `eigenvalue_floor_applied` is true, with the smallest eigenvalue of the
result next to it. A sample correlation is positive semi-definite in exact
arithmetic, and so are the constant-correlation target and every convex
combination of the two, so the floor meets rounding only, on a singular set
(a series that is a combination of the others, more series than returns);
it guarantees that the returned spec is accepted as a pricing input.

Every output is deterministic: the series are read in the order given and
every sum runs in the stated order, so the Python reference
`calibrate_correlation_ql` (tests/contract/ql_reference.py), which repeats
the arithmetic in plain Python, agrees with the engine to `1e-12`. The
algebra, in the same words, is in
`src/evaluators/correlation_calibration_evaluator.h`.

### Response

```json
{
  "correlation_spec": {"id": "NDX_RTY", "labels": ["NDX", "RTY"],
                       "values": {"n_rows": 2, "n_cols": 2, "values": [1.0, 0.747207, 0.747207, 1.0]}},
  "sample_correlation": [1.0, 0.747207, 0.747207, 1.0],
  "shrinkage_target": 0.747207, "shrinkage_intensity_used": 0.0,
  "eigenvalue_floor_applied": false,
  "smallest_eigenvalue_before": 0.252793, "smallest_eigenvalue_after": 0.252793,
  "observations_used": 253, "returns_used": 252,
  "first_date": "2025-09-29", "last_date": "2026-09-30",
  "series": [{"underlying_id": "NDX", "observations": 253, "dropped": 0, "annualised_volatility": 0.1991},
             {"underlying_id": "RTY", "observations": 253, "dropped": 0, "annualised_volatility": 0.1867}],
  "estimator_echo": {"start_date": "2025-09-29", "end_date": "2026-09-30", "returns": "Log",
                     "frequency": "Daily", "shrinkage": "None", "min_overlap": 60, "eigenvalue_floor": 1e-8}
}
```

`correlation_spec` is the matrix to paste (labels in the requested order,
values row-major); `sample_correlation` is the unshrunk matrix in the same
order, always returned next to it. `shrinkage_intensity_used` is 0 for
`None`, the Ledoit-Wolf value for `LedoitWolf`, the request's value for
`Fixed`. Per series, `observations` are the closes supplied and `dropped`
the ones not used (outside the window, on a date another series lacks, or
skipped by the weekly sampling); `observations - dropped` is
`observations_used` for every series. `annualised_volatility` is a
by-product (the realised vol of the returns used), not what pricing should
carry for a vol. `estimator_echo` is the estimator as resolved: the window
as explicit dates (never `window_days`), `shrinkage_intensity` present iff
`Fixed`; posting the same series with it as `estimator` reproduces the
response.

### Using the spec

Append `correlation_spec` to `pricing.correlations.matrices` and reference
its id from the note's `correlation_id`; its labels must be exactly the
note's underlying ids, so name the series after the `EquityUnderlyingSpec`
ids (`labels` reorders the output when the note lists them differently; the
engine permutes anyway). The desk's view goes on the note as
`correlation_markup` (see "Structured notes"): the estimate stays in the
request as it came out of this endpoint, the shift is echoed, and the
`market` diagnostic shows both.

### Rejected requests

| Request | Status |
|---|---|
| fewer than two `series`, a duplicate `underlying_id`, empty `closes` | `400` naming `series[i]` |
| closes not in strictly increasing date order, a close that is not finite and `> 0` | `400` naming `series[i].closes[k]` |
| `window_days` together with `start_date` / `end_date`, `window_days` 0, `start_date` after `end_date` | `400` naming the field |
| `shrinkage_intensity` missing with `Fixed`, outside `[0, 1]`, or given with another shrinkage | `400` naming `estimator.shrinkage_intensity` |
| `min_overlap` below 3, `eigenvalue_floor` outside `[0, 1)` | `400` naming the field |
| no date common to every series inside the window; fewer common dates than `min_overlap`; fewer than three observations after the weekly sampling | `400` with the window and the counts |
| `labels` not a permutation of the series ids | `400` naming the label |
| a malformed date, an unknown enum value, a missing `as_of_date` / `correlation_id` | `400` |

### Data

`examples/data/correlation/` carries two request files of public index
closes (Nasdaq-100, Russell 2000, S&P 500; index closes, Yahoo Finance
history pages): one year of daily NDX / RTY closes to the Scotia note's
as-of date, which reproduces the 0.747 that example is booked with, and a
three-series set to the JPMorgan note's date with Ledoit-Wolf shrinkage.
The workflow example (`examples/structured_note_workflow/`, `--closes`)
runs this endpoint ahead of the forwards and the surface.

## Hull-White calibration

`POST /calibrate-swaption-model`, and a swaption or callable bond whose model
has `param_mode: Calibrate`, fit Hull-White `(a, sigma)` to an ATM swaption
matrix with Levenberg-Marquardt (`SwaptionHwCalibrationSpec`).

**Recommendation for production use: `calibrate_a: false` with a market
mean reversion in `a_init`, calibrating `sigma` only.** On a market ATM
matrix the objective is almost flat in `a`: the sigma-only fit is
one-dimensional and well conditioned, while a joint fit lets the optimizer
wander along a plateau where the stopping point is decided by numerical
noise (the same request gave `a` = 1.2e-4 and 2.8e-4 on two QuantLib builds,
with a fit quality that differed by 0.08 %).

**Bounds on `a` (joint fit only).** When `calibrate_a` is true the fit is kept
inside `[a_lower, a_upper]`, default `[0.001, 1.0]`, and `a_init` is clipped
into that interval before the fit starts. Below about 1e-3 the model is
numerically flat in `a` and the fitted value is noise, so the optimizer is not
allowed there; above 1.0 mean reversion has no market meaning. Both fields are
optional on `SwaptionHwCalibrationSpec`; `a_lower` must be finite and > 0,
`a_upper` finite and > `a_lower` (`400` naming the field otherwise). They are
ignored for a sigma-only fit, where `a` stays exactly at `a_init` whatever
its value.

**Flatness diagnostic.** After a joint fit the engine re-evaluates the
objective at `2a` and `a/2` with `sigma` held at its calibrated value and
reports

- `a_flatness` = `1 - rmse / max(rmse(2a), rmse(a/2))`, in `[0, 1]`: 0 means
  the fit is no worse at a very different `a` (the surface does not pin it),
  values near 1 mean `a` is sharply identified;
- `a_poorly_identified` = `a_flatness < 0.01`.

Both are present iff `calibrate_a` is true (absent for a sigma-only fit), on
the calibrate response and as `used_hw_a_flatness` /
`used_hw_a_poorly_identified` on an inline-calibrated swaption. When
`a_poorly_identified` is true, `hw_a` is the point where the optimizer
stopped inside the box, not a market mean reversion: fix `a` and refit
`sigma`.

## Swaption greeks: units and definitions

`POST /price-swaption` can return greeks under two request flags in
`pricing.options`. Both report the same units, so the numbers are directly
comparable:

| Field | `swaption_pricing_details` (analytic) | `swaption_pricing_rebump` (bump-and-reprice) |
| --- | --- | --- |
| `delta` | dNPV for a 1bp move of the forward swap rate, annuity held fixed (currency per bp) | not produced (field absent) |
| `dv01` | equals `delta` | central difference of a +/-1bp parallel bump of the discounting and forwarding curves (currency per bp) |
| `gamma` | d2NPV/dF2 x 1e-8 (currency per bp^2) | second difference of the +/-1bp parallel curve bump |
| `vega` | dNPV for a 1bp (1e-4) move of the quoted vol (lognormal for Black, normal for Bachelier) | central difference of a +/-1bp vol bump |
| `theta` | pure time decay with forward, vol and annuity held fixed (-1/2 sigma^2 F^2 Gamma for Black, -1/2 sigma_N^2 Gamma for Bachelier), per calendar day | NPV(as_of + 1 calendar day) - NPV(as_of) after a clean one-day roll of the market (see below) |

The analytic values come from QuantLib's `BlackCalculator` /
`BachelierCalculator` built with discount `1`, multiplied by the swap annuity.
`atm_forward`, `annuity` and `delta` are filled only by
`swaption_pricing_details`; `implied_volatility` is reported whenever the
engine provides one. Every greek is an optional field (`= null` in the
schema): it is present exactly when it was computed. With
`swaption_pricing_rebump` alone, `delta`, `atm_forward` and `annuity` are
absent; with neither flag no greek is present at all.

The rebump roll rebuilds the market at `as_of_date + 1` with every explicit
`reference_date` (curves, helper curves, vol surfaces) shifted by the same
day, so the result equals `NPV(as_of_date + 1, reference dates + 1) -
NPV(as_of_date)` obtained from two plain requests. Date-anchored market data
(zero/discount/forward points pinned to a date, index fixings) is not shifted:
it describes the market, not the observer. The `used_atm_forward`,
`used_cube_node_atm` and `used_spread_from_atm` diagnostics are likewise
optional and absent when the vol surface carries no ATM level (or does not use
spread-from-ATM strikes).

Precedence: when both flags are set, the rebump `dv01`, `gamma`, `vega` and
`theta` overwrite the analytic ones; `delta`, `atm_forward` and `annuity` are
kept from the analytic pass.

## Response fields at their default value

Every scalar response field is serialized even when it equals its schema
default, so a `double` that is exactly `0.0` (for example a bond's
`accrued_amount` on a coupon date or a holiday `count`) is present in the JSON.
Fields declared optional (`= null` in the schema) are the exception: they are
absent until the engine sets them. A scalar that the engine computes only under
a request flag or condition (the swaption greeks, `atm_forward`, `annuity`,
the Hull-White `used_hw_*` diagnostics, `cms_swap_rate`, the CDS
`fair_spread`, every swap endpoint's `fair_rate` / `fair_spread`, the
`used_cms_*` block and the flow `kind`) is
declared optional for that reason, so its absence always means "not computed"
and a reported `0.0` always means "computed and equal to zero".

## Service endpoints

| Endpoint | Returns |
| --- | --- |
| `GET /health` | Liveness. Cheap; no engine round-trip. |
| `GET /meta` | Service and version metadata: API version, build info, the product list, and the endpoint list. |
| `GET /status` | Runtime status, including Envoy worker membership when `QUANTRA_ENVOY_ADMIN` is set. |

## gRPC callers

The same contract applies, minus the HTTP mapping: the engine returns the gRPC
status directly (`INVALID_ARGUMENT`, `ABORTED`, `UNIMPLEMENTED`,
`DEADLINE_EXCEEDED`, …) with the real cause in the status message. `Meta` and
`grpc.health.v1.Health` are described in `client.md`.
