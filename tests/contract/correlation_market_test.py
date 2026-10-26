"""Correlation market data (`pricing.correlations`): request contract.

Correlation matrices are market data shared through `Pricing`, like FX spots:
the registry parses and validates every matrix on every request that carries
the block, whether or not a product references it. So an endpoint that has
no use for correlations must

  * answer a request carrying a valid block exactly as it answers the same
    request without it (same status, identical numbers);
  * reject a malformed block with a 400 naming the matrix and the rule, before
    any pricing happens: not positive semi-definite (with the smallest
    eigenvalue), asymmetric (naming the offending pair), a diagonal entry that
    is not 1, an entry outside [-1, 1], a label count that does not match the
    matrix, a duplicate label, a duplicate id;
  * accept a rank-deficient matrix (rho = 1 on every pair: eigenvalue 0).

The vanilla swap example is the carrier; the 400 rows are also in
error_contract_test.py's table.
"""

import copy
import json

import pytest

CARRIER = "ir_swaps/vanilla_swap_request.json"
PRODUCT = "vanilla_swap"

QUAD_LABELS = ["SMI", "SPX", "SX5E", "NKY"]
QUAD_VALUES = [
    1.00, 0.55, 0.80, 0.45,
    0.55, 1.00, 0.60, 0.50,
    0.80, 0.60, 1.00, 0.50,
    0.45, 0.50, 0.50, 1.00,
]


def _matrix(matrix_id, labels, values, n_rows=None, n_cols=None):
    n = len(labels)
    return {
        "id": matrix_id,
        "labels": list(labels),
        "values": {
            "n_rows": n if n_rows is None else n_rows,
            "n_cols": n if n_cols is None else n_cols,
            "values": list(values),
        },
    }


def _with_correlations(request, matrices):
    req = copy.deepcopy(request)
    req["pricing"]["correlations"] = {"matrices": matrices}
    return req


def _post(client, request):
    r = client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[PRODUCT]}", json=request)
    return r.status_code, r.text


@pytest.fixture(scope="module")
def carrier(data_dir):
    with open(data_dir / CARRIER) as fh:
        return json.load(fh)


def test_valid_block_leaves_an_unrelated_endpoint_untouched(client, carrier):
    baseline = client.price(PRODUCT, carrier)
    with_block = client.price(
        PRODUCT, _with_correlations(carrier, [_matrix("QUAD", QUAD_LABELS, QUAD_VALUES)]))
    assert with_block == baseline


def test_perfect_correlation_is_accepted(client, carrier):
    ones = [1.0] * 16
    baseline = client.price(PRODUCT, carrier)
    with_block = client.price(
        PRODUCT, _with_correlations(carrier, [_matrix("ONES", QUAD_LABELS, ones)]))
    assert with_block == baseline


def test_two_matrices_with_distinct_ids_are_accepted(client, carrier):
    pair = _matrix("PAIR", ["SMI", "SPX"], [1.0, 0.55, 0.55, 1.0])
    quad = _matrix("QUAD", QUAD_LABELS, QUAD_VALUES)
    status, _ = _post(client, _with_correlations(carrier, [pair, quad]))
    assert status == 200


def test_quote_ids_resolve_through_pricing_quotes(client, carrier):
    req = _with_correlations(carrier, [_matrix("QUAD", QUAD_LABELS, QUAD_VALUES)])
    m = req["pricing"]["correlations"]["matrices"][0]["values"]
    m["values"][1] = 0.0
    m["values"][4] = 0.0
    m["quote_ids"] = [""] * 16
    m["quote_ids"][1] = "RHO_SMI_SPX"
    m["quote_ids"][4] = "RHO_SMI_SPX"
    req["pricing"].setdefault("quotes", []).append(
        {"id": "RHO_SMI_SPX", "value": 0.55})
    assert client.price(PRODUCT, req) == client.price(PRODUCT, carrier)

    # An unknown quote id is a 404, as for every other quote reference.
    req["pricing"]["quotes"] = [q for q in req["pricing"]["quotes"]
                                if q["id"] != "RHO_SMI_SPX"]
    status, body = _post(client, req)
    assert status == 404, body[:200]
    assert "RHO_SMI_SPX" in body


def _asymmetric():
    values = list(QUAD_VALUES)
    values[1 * 4 + 2] = 0.61   # (SPX, SX5E) != (SX5E, SPX)
    return values


def _non_psd():
    return [1.0, 0.9, 0.9,
            0.9, 1.0, -0.9,
            0.9, -0.9, 1.0]


REJECTED = [
    ("not positive semi-definite",
     [_matrix("IMPOSSIBLE", ["A", "B", "C"], _non_psd())],
     ["correlations[IMPOSSIBLE]", "not positive semi-definite",
      "smallest eigenvalue -0.8"]),
    ("asymmetric names the pair",
     [_matrix("QUAD", QUAD_LABELS, _asymmetric())],
     ["correlations[0].values: not symmetric", "(SPX, SX5E) = 0.61",
      "(SX5E, SPX) = 0.6", "matrix 'QUAD'"]),
    ("diagonal not one",
     [_matrix("QUAD", QUAD_LABELS,
              [v if i != 10 else 0.99 for i, v in enumerate(QUAD_VALUES)])],
     ["correlations[0].values: diagonal entry (SX5E, SX5E) must be 1, got 0.99"]),
    ("entry outside [-1, 1]",
     [_matrix("QUAD", QUAD_LABELS,
              [v if i not in (3, 12) else 1.2 for i, v in enumerate(QUAD_VALUES)])],
     ["correlations[0].values: entry (SMI, NKY) must be in [-1, 1], got 1.2"]),
    ("label count mismatch",
     [_matrix("QUAD", ["SMI", "SPX", "SX5E"], QUAD_VALUES, n_rows=4, n_cols=4)],
     ["correlations[0].values: n_rows and n_cols must both equal len(labels) = 3, got 4x4"]),
    ("values length mismatch",
     [_matrix("QUAD", QUAD_LABELS, QUAD_VALUES[:12])],
     ["correlations[0].values: values must hold n_rows*n_cols = 16 entries, got 12"]),
    ("duplicate label",
     [_matrix("QUAD", ["SMI", "SPX", "SMI", "NKY"], QUAD_VALUES)],
     ["correlations[0].labels: duplicate label 'SMI'"]),
    ("one label",
     [_matrix("ONE", ["SMI"], [1.0])],
     ["correlations[0].labels must list at least two labels"]),
    ("duplicate id",
     [_matrix("QUAD", QUAD_LABELS, QUAD_VALUES), _matrix("QUAD", QUAD_LABELS, QUAD_VALUES)],
     ["duplicate correlation matrix id in pricing.correlations.matrices: QUAD"]),
    ("second matrix is reported at its own index",
     [_matrix("QUAD", QUAD_LABELS, QUAD_VALUES), _matrix("BAD", QUAD_LABELS, _asymmetric())],
     ["correlations[1].values: not symmetric", "matrix 'BAD'"]),
]


@pytest.mark.parametrize("label,matrices,expected", REJECTED, ids=[r[0] for r in REJECTED])
def test_malformed_block_is_a_400_naming_the_rule(client, carrier, label, matrices, expected):
    status, body = _post(client, _with_correlations(carrier, matrices))
    assert status == 400, f"{label}: expected 400, got {status} :: {body[:200]}"
    for substr in expected:
        assert substr in body, f"{label}: body lacks {substr!r}: {body[:300]}"
