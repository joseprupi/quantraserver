# Parser Layer Conventions

This directory follows a role-based naming contract to keep separation of concerns explicit.

## Naming Rules

- `*_parser`
  - Responsibility: decode FlatBuffers payloads and validate shape/required fields.
  - Output: domain structs or QuantLib-ready primitives.
  - Must not perform endpoint orchestration or response assembly.

- `*_service`
  - Responsibility: business computation/orchestration on already decoded inputs.
  - Typical examples: pricing a product, selecting/calibrating model params, choosing engines.
  - Should be callable from multiple request handlers.

- `*_builder`
  - Responsibility: deterministic assembly/serialization helpers for response structures (I know, this is not parsing, but...).
  - Typical examples: flow row construction for bond/swap/cap-floor outputs.
  - Must not own model calibration or endpoint policy.

## Request Layer Contract (`request/*.cpp`)

- Owns endpoint policy and lifecycle only:
  - registry creation,
  - evaluation date guard,
  - per-trade loop orchestration,
  - response vector assembly and error wrapping.
- Avoid direct FlatBuffers union decoding in request handlers (`*_as_*`) except trivial utility endpoints.
- Delegate parsing, model decoding, heavy computation, and response-flow construction to `parser/*`.

## Quick Examples

- `equity_option_parser`: FBS trade payload → parsed equity option components.
- `vanilla_swap_pricing_service`: swap pricing logic (including CMS path) reused by request layer.
- `bond_flow_builder`: builds response flow rows without endpoint policy logic.
- `swaption_model_parser`: model payload extraction for swaption handlers.
