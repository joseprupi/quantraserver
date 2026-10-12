# Migrate swap requests to the 0.10.0 `SwapLeg` shape

`migrate_swap_requests.py` rewrites request JSON for `price-vanilla-swap`,
`price-ois-swap`, `price-basis-swap`, `price-cross-currency-swap` and
`price-swaption` from the 0.9.0 shape to the 0.10.0 shape, in which every
swap leg is one flat `SwapLeg`: the leg says whether it is paid or received,
its currency, its notional, schedule, day count and payment conventions,
which kind of coupons it carries (and only what is specific to that kind),
and which curves it is discounted and projected on. The endpoints, the swap
tables and their role-named fields (`fixed_leg`, `floating_leg`,
`overnight_leg`, `leg1`, `leg2`) keep their names; what changes is what a leg
looks like, and that the single-currency request wrappers no longer carry
curve ids or a `swap_type`.

Python 3 standard library only. No server, QuantLib or build is needed to
convert; `--check` needs a running 0.9.0 server and a running 0.10.0 server.

The 0.10.0 engine prices a converted request to the same numbers the 0.9.0
engine gave the original (same leg builder); `--check` proves it request by
request.

## Usage

```bash
# Dry run: convert in memory, report per file, write nothing
python3 tools/migrate_swap_requests/migrate_swap_requests.py my_requests/

# Write the converted files under another directory (relative layout kept)
python3 tools/migrate_swap_requests/migrate_swap_requests.py my_requests/ --out migrated/

# Overwrite in place
python3 tools/migrate_swap_requests/migrate_swap_requests.py my_requests/ a_single_file.json --in-place

# Price each original on a 0.9.0 server and its conversion on a 0.10.0 server,
# compare, exit non-zero on any difference
python3 tools/migrate_swap_requests/migrate_swap_requests.py my_requests/ \
    --check http://localhost:8091 http://localhost:8093
```

Inputs are files or directories (searched recursively for `*.json`). Each file
is reported as:

- `OK` converted (written only with `--in-place` / `--out`);
- `SAME` already in the 0.10.0 shape (left untouched);
- `SKIP` not one of the five request kinds (bonds, FRAs, FX forwards,
  inflation swaps, curves... are never modified);
- `ERROR` the file names a field the converter cannot place, or is not valid
  JSON. The message names the file and the JSON path of the field.

Exit status: `0` when no file errored, `1` on any error (or any `--check`
mismatch), `2` on a usage error. Converted files are written with a 2-space
indent and a trailing newline, like the repository's examples.

Importable API: `convert_request(dict) -> dict` (raises `ConversionError`
with `.field`, or `NotASwapRequest`), `detect_endpoint(dict) -> str`
(`"vanilla_swap" | "ois_swap" | "basis_swap" | "cross_currency_swap" |
"swaption"`), `validate_converted(dict) -> list[str]` (structural problems of
a 0.10.0 request; empty when it conforms).

## Rules

- **Nothing is dropped silently.** Every 0.9.0 field has a place in the new
  shape (tables below). A field the converter does not know (a typo, a field
  from another version, a mix of the two shapes in one request) is an error
  naming it; the file is left untouched.
- **Nothing is invented.** A shared field or a kind-specific field is written
  only when the 0.9.0 body carried it. The one addition is the OIS fixed leg's
  `payment_calendar` / `payment_lag`, copied from the overnight leg (below).
- **Idempotent.** A request already in the 0.10.0 shape converts to itself, so
  the tool can be re-run over a directory that is partly migrated.
- **Everything that is not a swap leg is copied verbatim**: `pricing`,
  `include_flows`, `include_diagnostics`, the swaption's exercise and
  settlement fields, `volatility`, `model`, the cross-currency wrapper's
  `fx_spot` and `reporting_currency`, and the field values themselves
  (`schedule`, `notional` / `notionals`, `rate`, `spread`, `in_arrears`,
  `fixing_days`, the CMS pricer block, caps and floors...). Top-level and
  wrapper key order is kept; the leg is written in schema order.

## What changes, field by field

### The leg

A 0.9.0 leg of a vanilla, OIS or basis swap (or a swaption underlying) is the
coupon body itself, with the shared fields and the kind-specific ones side by
side:

```json
{
  "notional": 1000000.0,
  "schedule": { "...": "..." },
  "index": { "id": "EUR_6M" },
  "spread": 0.0005,
  "day_counter": "Actual360",
  "payment_convention": "ModifiedFollowing"
}
```

A 0.10.0 leg is a flat `SwapLeg`. The fields every coupon kind shares
(`notional` / `notionals`, `schedule`, `day_counter`, `payment_convention`,
`payment_calendar`, `payment_lag`) move up onto the leg, as in the FpML / ORE
leg model, next to the side, the currency and the curves; `coupons` keeps
only what is specific to the coupon kind:

```json
{
  "pay_receive": "Receive",
  "currency": "EUR",
  "notional": 1000000.0,
  "schedule": { "...": "..." },
  "day_counter": "Actual360",
  "payment_convention": "ModifiedFollowing",
  "coupons_type": "IborCoupons",
  "coupons": { "index": { "id": "EUR_6M" }, "spread": 0.0005 },
  "discounting_curve": "EUR_OIS",
  "forwarding_curve": "EUR_6M_FWD"
}
```

The converter writes the leg in schema order: `pay_receive`, `currency`,
`notional`, `notionals`, `schedule`, `day_counter`, `payment_convention`,
`payment_calendar`, `payment_lag`, `coupons_type`, `coupons`,
`discounting_curve`, `forwarding_curve`, `initial_exchange`,
`final_exchange` (each only when it has a value).

| 0.9.0 | 0.10.0 | Rule |
|---|---|---|
| `<swap>.swap_type` (`Payer` / `Receiver`) | `<leg>.pay_receive` (`Pay` / `Receive`) on every leg | `Payer` means the party pays the leg the old field referred to (vanilla / OIS: `fixed_leg`; basis: `leg1`); that leg gets `Pay`, the other `Receive`. `Receiver` is the inverse. |
| (implicit) | `<leg>.currency` | A floating leg takes `IndexDef.currency` of its index (`pricing.rates.indices`); a CMS leg takes it through `pricing.rates.swap_indices[swap_index_id].float_index_id`; the fixed leg takes the other leg's currency. A missing `currency` on the index is an error naming `pricing.rates.indices[<id>].currency`. |
| body `notional`, `notionals`, `schedule`, `day_counter`, `payment_convention`, `payment_calendar`, `payment_lag` | the same fields on `<leg>` | Shared fields: copied verbatim onto the leg, only those present in the body. |
| body `rate` | `<leg>.coupons.rate` with `coupons_type: FixedCoupons` | |
| body `index`, `spread`, `fixing_days`, `in_arrears` | `<leg>.coupons.{...}` with `coupons_type: IborCoupons` | |
| body `index`, `spread`, `averaging_method`, `lookback_days`, `lockout_days`, `apply_observation_shift`, `telescopic_value_dates` | `<leg>.coupons.{...}` with `coupons_type: OvernightCoupons` | The overnight body's `payment_calendar` / `payment_lag` are shared fields: they go to the leg level (previous row). |
| body `swap_index_id`, `swap_tenor`, `swaption_vol_id`, `fixing_days`, `gear`, `spread`, `pricer`, `cap`, `floor` | `<leg>.coupons.{...}` with `coupons_type: CmsCoupons` | |
| any other body field | error naming it | The body is split field by field; a field that is neither shared nor of the leg's coupon kind cannot be placed. |
| wrapper `discounting_curve` | `<leg>.discounting_curve` on both legs | |
| wrapper `forwarding_curve` / `forwarding_curve_leg1` / `forwarding_curve_leg2` | `<leg>.forwarding_curve` on the floating leg(s) | Never written on a fixed leg (0.10.0 rejects it there). |
| — | `<leg>.initial_exchange`, `<leg>.final_exchange` | Not written on the single-currency endpoints: an absent flag means `false` (no principal exchange), which is what 0.9.0 priced. |

A leg of a vanilla, OIS or basis swap (or a swaption underlying) that already
has `coupons_type` / `coupons` but with shared fields inside `coupons` (an
intermediate nested form that never shipped) is not a supported input: it is
an error naming the first such field (`...coupons.notional`). Only the
cross-currency endpoint, whose 0.9.0 shape nested the shared fields, is
flattened by hoisting (below).

### `price-vanilla-swap`

| 0.9.0 | 0.10.0 |
|---|---|
| `swaps[i].vanilla_swap.swap_type` | `fixed_leg.pay_receive` / `floating_leg.pay_receive` |
| `swaps[i].vanilla_swap.fixed_leg` (body) | `fixed_leg`: shared fields on the leg, `coupons_type: FixedCoupons`, `coupons: { rate }` |
| `swaps[i].vanilla_swap.floating_leg` (body) | `floating_leg`: shared fields on the leg, `coupons_type: IborCoupons`, `coupons: { index, spread, fixing_days, in_arrears }` |
| `swaps[i].vanilla_swap.cms_leg` (body) | `floating_leg`: shared fields on the leg, `coupons_type: CmsCoupons`, `coupons: { swap_index_id, swap_tenor, swaption_vol_id, fixing_days, gear, spread, pricer, cap, floor }` (the `cms_leg` field is gone: a CMS swap is a vanilla swap whose floating leg carries CMS coupons) |
| `swaps[i].discounting_curve` | `fixed_leg.discounting_curve` and `floating_leg.discounting_curve` |
| `swaps[i].forwarding_curve` | `floating_leg.forwarding_curve` |
| `swaps[i]` = `{vanilla_swap, discounting_curve, forwarding_curve}` | `swaps[i]` = `{vanilla_swap}` |

Response: `fixed_leg_npv` / `floating_leg_npv` / `*_bps` / `*_flows` become
`legs[0]` (fixed) and `legs[1]` (floating) of `legs:[SwapLegResult]`; `npv`,
`fair_rate`, `fair_spread` and the `used_cms_*` fields stay.

### `price-ois-swap`

| 0.9.0 | 0.10.0 |
|---|---|
| `swaps[i].ois_swap.swap_type` | `fixed_leg.pay_receive` / `overnight_leg.pay_receive` |
| `swaps[i].ois_swap.fixed_leg` (body) | `fixed_leg`: shared fields on the leg **plus** `payment_calendar` and `payment_lag` copied from the overnight leg, `coupons_type: FixedCoupons`, `coupons: { rate }` |
| `swaps[i].ois_swap.overnight_leg` (body) | `overnight_leg`: shared fields on the leg (including the body's `payment_calendar`, `payment_lag` and its accepted-but-unused `day_counter`), `coupons_type: OvernightCoupons`, `coupons: { index, spread, averaging_method, lookback_days, lockout_days, apply_observation_shift, telescopic_value_dates }` |
| `swaps[i].discounting_curve` | both legs' `discounting_curve` |
| `swaps[i].forwarding_curve` | `overnight_leg.forwarding_curve` |

Both legs of a 0.10.0 OIS must state `payment_calendar` / `payment_lag`.
0.9.0 applied the overnight leg's payment lag to both legs (QuantLib's
`OvernightIndexedSwap` does); copying the two values onto the fixed leg
states that explicitly, so the converted swap pays on the same dates. A
0.9.0 OIS fixed-leg body that already carries one of the two is an error
naming it (it cannot be told apart from the overnight leg's value).

Response: `fixed_leg_npv` / `overnight_leg_npv` / `*_bps` / `*_flows` become
`legs[0]` / `legs[1]`; `npv`, `fair_rate` and `fair_spread` stay.

### `price-basis-swap`

| 0.9.0 | 0.10.0 |
|---|---|
| `swaps[i].basis_swap.swap_type` | `leg1.pay_receive` / `leg2.pay_receive` (`Payer` = pay `leg1`) |
| `swaps[i].basis_swap.leg1` / `leg2` (bodies) | `leg1` / `leg2`: shared fields on the leg, `coupons_type: IborCoupons`, `coupons: { index, spread, fixing_days, in_arrears }` |
| `swaps[i].discounting_curve` | both legs' `discounting_curve` |
| `swaps[i].forwarding_curve_leg1` / `forwarding_curve_leg2` | `leg1.forwarding_curve` / `leg2.forwarding_curve` |

Response: `leg1_npv` / `leg2_npv` become `legs[0]` / `legs[1]`;
`fair_spread_leg1` / `fair_spread_leg2` become one `fair_spread`, on the leg
that carries the request's spread (both spreads zero: leg2).

### `price-cross-currency-swap`

The 0.9.0 cross-currency endpoint already used `SwapLeg`s (side, currency,
curves and exchange flags on the leg) but nested the whole coupon body under
`coupons`. 0.10.0 flattens it: the shared fields are hoisted out of
`coupons` onto the leg, everything else stays where it was.

| 0.9.0 | 0.10.0 |
|---|---|
| `leg.coupons.notional`, `.notionals`, `.schedule`, `.day_counter`, `.payment_convention`, `.payment_calendar`, `.payment_lag` | the same fields on `leg` (only those present) |
| `leg.coupons.<kind-specific>` (`rate`; `index`, `spread`...) | `leg.coupons.<kind-specific>`, unchanged |
| `leg.pay_receive`, `leg.currency`, `leg.coupons_type`, `leg.discounting_curve`, `leg.forwarding_curve`, `leg.initial_exchange`, `leg.final_exchange` | unchanged (re-ordered to schema order) |
| `swaps[i].fx_spot`, `swaps[i].reporting_currency` | unchanged |

A shared field that is both on the leg and inside `coupons`, or a leg with
shared fields on both levels, is an error naming the field inside `coupons`.
Response: unchanged (`npv`, `legs[]`, `fair_rate`, `fair_spread`, the FX
fields); a fixed/float swap now also reports `fair_spread` (the floating
leg's), which 0.9.0 gave only for float/float.

### `price-swaption`

The swaption itself does not change; its underlying is a `VanillaSwap` or an
`OisSwap`, and those tables changed as above. The curve ids the swaption
wrapper carried for the underlying move onto the underlying's legs.

| 0.9.0 | 0.10.0 |
|---|---|
| `swaptions[i].swaption.underlying` (0.9.0 `VanillaSwap` / `OisSwap`) | the same table in the 0.10.0 shape (flat `SwapLeg`s with `pay_receive`, `currency`, the shared fields, `coupons_type` / `coupons`, curves) |
| `swaptions[i].discounting_curve` | `underlying.<leg>.discounting_curve` on both legs |
| `swaptions[i].forwarding_curve` | `underlying.floating_leg.forwarding_curve` / `underlying.overnight_leg.forwarding_curve` |
| `swaptions[i].volatility`, `swaptions[i].model`, every `swaption.*` field other than `underlying` | unchanged |

## `--check OLD_URL NEW_URL`

For every input file the original is POSTed to `OLD_URL/price-<endpoint>` and
the converted request to `NEW_URL/price-<endpoint>`. Compared per trade:

- `npv`;
- `fair_rate` and `fair_spread` (vanilla, OIS); for a basis swap the old
  `fair_spread_leg1` or `fair_spread_leg2` (whichever leg carries the request's
  spread; both zero: leg2) against the new `fair_spread`;
- each old per-leg NPV (`fixed_leg_npv`, `floating_leg_npv`,
  `overnight_leg_npv`, `leg1_npv`, `leg2_npv`) against `legs[i].npv` in field
  order; for a cross-currency swap `legs[i].npv` on both sides;
- swaptions: `npv`.

Tolerance: money fields (`npv`, leg NPVs) agree when the absolute difference is
within `1e-9 x notional` (the largest notional of the two legs) or `1e-12`
relative; rates (`fair_rate`, `fair_spread`) within `1e-12` absolute or
`1e-12` relative. A field missing on either side is a mismatch. The tool
prints one table per file (`ok` / `BAD` per field) and exits `1` if any file
has a mismatch, an HTTP error, or a conversion error. Files already in the
0.10.0 shape are skipped in this mode (a 0.9.0 server cannot price them).

Run the two versions side by side (one container per version, each on its own
host port, the HTTP/JSON API listens on 8080 inside the image).

## Tests

`tests/contract/migrate_swap_requests_test.py` (pure Python, collected by the
repository's Suite 3; no server needed): every swap, swaption and
cross-currency request under `examples/data` is already in the 0.10.0 shape
and is reported `SAME` (it converts to itself and passes the structural
check); its 0.9.0 form, rebuilt in the test by the inverse of the tables
above, converts back to the fixture field for field; literal 0.9.0 requests
of every kind pin the old shapes in plain sight; the mapping rules above;
every error case; the CLI modes; and `--check` against a fake HTTP server.

```bash
python3 -m pytest tests/contract/migrate_swap_requests_test.py -q
```
