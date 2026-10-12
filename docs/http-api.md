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
