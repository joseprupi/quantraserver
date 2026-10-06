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
