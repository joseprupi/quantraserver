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
`CrossCcyBasisHelper`) and by the FX products as they ship.

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
the Hull-White `used_hw_*` diagnostics, `cms_swap_rate`, `fair_spread`) is
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
