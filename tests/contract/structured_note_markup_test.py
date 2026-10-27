"""StructuredNote.correlation_markup: a uniform shift of every off-diagonal
correlation applied before pricing.

Locked here, on the synthetic two-asset note (1,024 paths) and the JPM
three-index note:

  * a note with `correlation_markup: 0.05` prices exactly as the same note
    with the matrix shifted by 0.05 by hand (same stream, same pseudo
    square root): npv, legs, probabilities to the bit;
  * the terms echo carries the mark-up and the repaired flag, the market
    diagnostic shows the base matrix, the mark-up and the matrix used;
  * the shift is clipped to [-1, 1] (a +0.8 mark-up on 0.5 gives 1);
  * the Python reference (structured_note_ql) applies the same shift and
    agrees with the engine;
  * the Greeks' correlation bump starts from the marked-up matrix;
  * a mark-up that breaks positive semi-definiteness (three indices, +0.2)
    is repaired and flagged, like the Greeks' bump;
  * 400s: |mark-up| > 1, a mark-up on a single-underlying note (the rows
    are in error_contract_test as well).
"""

import copy

from ql_reference import load_json, structured_note_ql

TWO_ASSET = "structured_note/synthetic_two_asset_worst_of_flat_vol_autocall.json"
JPM = "structured_note/jpm_review_notes_ndx_rty_spx_46661pt64.json"
SINGLE = "structured_note/single_underlying_reverse_convertible.json"


def _post(client, request):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS['structured_note']}", json=request)


def _note(client, request):
    r = _post(client, request)
    assert r.status_code == 200, r.text[:400]
    return r.json()["notes"][0]


def _matrix(request):
    return request["pricing"]["correlations"]["matrices"][0]


def _shifted(values, n, delta):
    return [min(1.0, max(-1.0, v + delta)) if i % n != i // n else v
            for i, v in enumerate(values)]


def test_markup_equals_a_hand_shifted_matrix_on_the_same_stream(client, data_dir):
    base = load_json(data_dir / TWO_ASSET)
    base["diagnostics"] = {"market": True}
    n = len(base["notes"][0]["underlyings"])
    values = _matrix(base)["values"]["values"]

    marked = copy.deepcopy(base)
    marked["notes"][0]["correlation_markup"] = 0.05
    by_hand = copy.deepcopy(base)
    _matrix(by_hand)["values"]["values"] = _shifted(values, n, 0.05)

    a = _note(client, marked)
    b = _note(client, by_hand)
    for key in ("npv", "standard_error", "p_barrier_breach", "expected_life_years",
                "event_probabilities", "legs"):
        assert a[key] == b[key], key
    assert a["terms"]["correlation_markup"] == 0.05
    assert a["terms"]["correlation_markup_repaired"] is False
    assert "correlation_markup" not in b["terms"]
    assert b["terms"]["correlation_markup_repaired"] is False
    assert a["market"]["correlation"] == _shifted(values, n, 0.05)
    assert a["market"]["correlation"] == b["market"]["correlation"]
    assert a["market"]["correlation_base"] == values
    assert a["market"]["correlation_markup"] == 0.05
    assert "correlation_base" not in b["market"]
    assert "correlation_markup" not in b["market"]

    # The reference applies the same shift.
    ref = structured_note_ql(marked)
    assert abs(ref["npv"] - a["npv"]) < 1e-9
    assert ref["market"]["correlation"] == a["market"]["correlation"]
    assert ref["market"]["correlation_base"] == values
    assert ref["market"]["correlation_markup"] == 0.05

    # A negative mark-up moves the worst-of the other way: it is a different
    # price, and the price is the hand-shifted one again.
    down = copy.deepcopy(base)
    down["notes"][0]["correlation_markup"] = -0.1
    down_hand = copy.deepcopy(base)
    _matrix(down_hand)["values"]["values"] = _shifted(values, n, -0.1)
    c = _note(client, down)
    assert c["npv"] == _note(client, down_hand)["npv"]
    assert c["npv"] != a["npv"]


def test_markup_is_clipped_to_one(client, data_dir):
    base = load_json(data_dir / TWO_ASSET)
    base["diagnostics"] = {"market": True}
    n = len(base["notes"][0]["underlyings"])
    _matrix(base)["values"]["values"] = [1.0 if i % n == i // n else 0.5 for i in range(n * n)]
    marked = copy.deepcopy(base)
    marked["notes"][0]["correlation_markup"] = 0.8
    one = copy.deepcopy(base)
    _matrix(one)["values"]["values"] = [1.0] * (n * n)
    a = _note(client, marked)
    assert a["market"]["correlation"] == [1.0] * (n * n)
    assert a["terms"]["correlation_markup_repaired"] is False
    assert a["npv"] == _note(client, one)["npv"]


def test_greeks_bump_starts_from_the_marked_up_matrix(client, data_dir):
    base = load_json(data_dir / TWO_ASSET)
    n = len(base["notes"][0]["underlyings"])
    values = _matrix(base)["values"]["values"]
    base["notes"][0]["correlation_markup"] = 0.05
    base["greeks"] = {"correlation_bump": 0.01}
    a = _note(client, base)
    g = a["greeks"]
    assert g["correlation_bump"] == 0.01
    expected = _shifted(_shifted(values, n, 0.05), n, 0.01)
    assert len(g["correlation_bumped"]) == n * n
    for got, want in zip(g["correlation_bumped"], expected):
        assert abs(got - want) < 1e-15
    assert g["correlation_bump_repaired"] is False


def test_markup_that_breaks_psd_is_repaired_and_flagged(client, data_dir):
    base = load_json(data_dir / JPM)
    base["notes"][0]["monte_carlo"]["paths"] = 1024
    base["diagnostics"] = {"market": True}
    values = _matrix(base)["values"]["values"]
    base["notes"][0]["correlation_markup"] = 0.2
    a = _note(client, base)
    assert a["terms"]["correlation_markup"] == 0.2
    assert a["terms"]["correlation_markup_repaired"] is True
    used = a["market"]["correlation"]
    assert a["market"]["correlation_base"] == values
    assert a["market"]["correlation_markup"] == 0.2
    assert used[0] == used[4] == used[8] == 1.0
    assert used[1] == used[3] and used[2] == used[6] and used[5] == used[7]
    assert all(-1.0 <= v <= 1.0 for v in used)
    # Repaired: not the plain clipped shift (which has two entries at 1 and
    # is not positive semi-definite).
    assert used != _shifted(values, 3, 0.2)
    # The repaired matrix prices as the same matrix pasted by hand.
    by_hand = copy.deepcopy(base)
    del by_hand["notes"][0]["correlation_markup"]
    _matrix(by_hand)["values"]["values"] = used
    b = _note(client, by_hand)
    # The pasted matrix is the printed one (12 significant digits), so the
    # two prices agree to the print precision, not to the bit.
    assert abs(b["npv"] - a["npv"]) < 1e-6 * abs(a["npv"])
    assert b["terms"]["correlation_markup_repaired"] is False
    ref = structured_note_ql(base)
    for got, want in zip(ref["market"]["correlation"], used):
        assert abs(got - want) < 1e-10
    assert abs(ref["npv"] - a["npv"]) < 1e-6 * abs(a["npv"])


def test_rejected_markups(client, data_dir):
    base = load_json(data_dir / TWO_ASSET)
    for bad in (1.5, -1.0000001):
        req = copy.deepcopy(base)
        req["notes"][0]["correlation_markup"] = bad
        r = _post(client, req)
        assert r.status_code == 400, r.text[:300]
        assert "notes[0].correlation_markup: must be finite and in [-1, 1]" in r.text
    single = load_json(data_dir / SINGLE)
    single["notes"][0]["correlation_markup"] = 0.05
    r = _post(client, single)
    assert r.status_code == 400, r.text[:300]
    assert "notes[0].correlation_markup: must be absent with a single underlying" in r.text
    # Zero is a valid mark-up and changes nothing.
    req = copy.deepcopy(base)
    req["notes"][0]["correlation_markup"] = 0.0
    assert _note(client, req)["npv"] == _note(client, base)["npv"]
