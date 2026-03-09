# Parser Conventions

The `parser/` directory follows a role-based naming convention so responsibilities stay separated and request handlers remain thin.

## Naming Rules

### `*_parser`

Responsibilities:

- decode FlatBuffers payloads
- validate shape and required fields
- return domain structs or QuantLib-ready primitives

Constraints:

- should not own endpoint orchestration
- should not assemble final endpoint responses

### `*_service`

Responsibilities:

- run pricing or other business computations on already decoded inputs
- encapsulate reusable model selection, calibration, or engine logic

Constraints:

- should be reusable from multiple request handlers

### `*_builder`

Responsibilities:

- assemble deterministic response structures
- construct output rows or flow structures from already computed results

Constraints:

- should not own endpoint policy
- should not own model calibration decisions

## Request Layer Contract

Files under `request/` should own endpoint lifecycle only. That includes:

- registry creation
- evaluation-date guards
- per-trade orchestration
- response vector assembly
- error wrapping

Request handlers should avoid deep FlatBuffers union decoding except for trivial utility endpoints. Parsing, heavy computation, and response construction should live in `parser/`.

## Examples

- `equity_option_parser`: converts an equity option payload into parsed components
- `vanilla_swap_pricing_service`: reusable swap pricing logic
- `bond_flow_builder`: deterministic response-flow assembly
- `swaption_model_parser`: swaption model payload extraction

## Rule Of Thumb

If code is deciding how an endpoint behaves, it belongs closer to `request/`. If it is decoding data, pricing instruments, or building response payloads, it likely belongs in `parser/`.
