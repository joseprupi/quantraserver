"""Structured-note contract suite: the example requests through the JSON
gateway (POST /price-structured-note).

Every market number in the examples is a placeholder (see
examples/data/structured_note/README.md), so nothing here is a valuation of
the two notes: the suite locks the shape and the internal consistency of the
response -- HTTP 200, a finite price between 0 and 1.5 x notional with a
positive standard error, buckets that add up to the headline, call
probabilities that sum to at most one, the note Alive after its one past
observation, the future events echoed with their flags and the accrual-only
events echoed as such; the opt-in diagnostic blocks (each present iff its
flag is set, the per-event PVs adding up to the buckets, the histogram
counting every path, the sample paths shaped [path][observation date]); the
single-underlying note without `correlation_id`; the opt-in Greeks (the
BNP example with `greeks` on: one block per underlying with delta / gamma /
vega and their standard errors, the bumped correlation matrix echoed, the
funding DV01 negative, the bump sizes echoed, the headline unchanged; the
timing stages of the bumped runs on the single-underlying note).
"""

import json
import math

import pytest

EXAMPLES = [
    ("structured_note/bnp_quad_index_classic_autocall_xs2741509801.json",
     "XS2741509801", 12, 1, 10, 1),
    ("structured_note/santander_quad_index_low_hurdle_autocall_xs3109492564.json",
     "XS3109492564", 24, 3, 20, 1),
]


def _post(client, request):
    endpoint = client.ENDPOINTS["structured_note"]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    return r.status_code, r.text


@pytest.mark.parametrize(
    "filename, isin, n_events, n_accrual, n_future, n_past",
    EXAMPLES,
    ids=[e[1] for e in EXAMPLES],
)
def test_example_prices(client, data_dir, filename, isin, n_events, n_accrual, n_future, n_past):
    with open(data_dir / filename) as fh:
        req = json.load(fh)
    status, body = _post(client, req)
    assert status == 200, f"{isin}: HTTP {status} :: {body[:300]}"
    resp = json.loads(body)
    notes = resp["notes"]
    assert len(notes) == 1
    n = notes[0]
    notional = req["notes"][0]["notional"]

    assert n["trade_id"] == isin
    assert n["status"] == "Alive"
    assert n["paths"] == req["notes"][0]["monte_carlo"]["paths"]
    assert n["seed"] == 42
    assert math.isfinite(n["npv"]) and 0.0 < n["npv"] < 1.5 * notional
    assert abs(n["npv_pct_of_notional"] - 100.0 * n["npv"] / notional) < 1e-9
    assert n["standard_error"] > 0.0

    legs = n["legs"]
    total = (legs["bond_redemption"]["pv"] + legs["coupons"]["pv"]
             + legs["capital_loss"]["pv"])
    assert abs(total - n["npv"]) < 1e-9 * notional
    assert legs["capital_loss"]["pv"] <= 0.0
    assert legs["coupons"]["pv"] > 0.0
    for b in legs.values():
        assert b["standard_error"] >= 0.0

    probs = n["event_probabilities"]
    assert len(probs) == n_future
    p_call = sum(p["p_call"] for p in probs if "p_call" in p)
    assert 0.0 < p_call <= 1.0 + 1e-12
    for p in probs:
        assert 0.0 <= p["p_coupon"] <= 1.0
    assert 0.0 <= n["p_barrier_breach"] <= 1.0
    assert 0.0 < n["expected_life_years"] < 5.0

    terms = n["terms"]
    events = terms["events"]
    assert len(events) == n_events
    assert sum(1 for e in events if e["accrual_only"]) == n_accrual
    for e in events:
        if e["accrual_only"]:
            assert "payment_date" not in e
            assert "autocall_trigger" not in e and "coupon_trigger" not in e
        assert e["observation_date"] == req["notes"][0]["events"][e["index"]]["observation_date"]
    assert events[-1]["final"] and not events[0]["final"]
    assert sum(1 for e in events if e["resolved_from_history"]) == n_accrual + n_past
    assert len(terms["underlyings"]) == 4
    assert sum(1 for u in terms["underlyings"] if u["quanto"]) == 3
    assert terms["funding_spread_bp"] == 100.0
    assert terms["put_strike"] == 1.0 and terms["participation"] == 1.0

    seas = n["seasoning"]
    assert seas["as_of_date"] == req["pricing"]["as_of_date"]
    assert seas["status"] == "Alive"
    assert len(seas["events"]) == n_accrual + n_past
    assert seas["next_event_index"] == n_accrual + n_past
    assert "known_payment" not in seas
    past = seas["events"][-1]
    assert not past["called"]
    assert len(past["levels"]) == 4
    # Snowball: the accrual-only coupons plus the missed one are carried.
    coupon = req["notes"][0]["events"][-1]["coupon"]
    assert abs(seas["memory_carried"] - coupon * (n_accrual + n_past)) < 1e-12

    model = n["model"]
    assert model["observation_dates_only"] is True
    assert model["time_steps"] == n_future
    assert model["sequence_dimension"] == 4 * n_future
    assert len(model["time_grid"]) == n_future + 1 and model["time_grid"][0] == 0.0
    for u in model["underlyings"]:
        assert u["vol_shape"] == "Constant" and u["local_vol"] is False
        assert ("quanto_drift" in u) == (u["underlying_id"] != "SPX")
    assert "greeks" not in n and "market" not in n and "checks" not in n


SCOTIA_EXAMPLE = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72.json"


def test_is_final_marker_is_echoed_and_has_no_coupon_test(client, data_dir):
    """The Scotia note's last event is a maturity-only observation written as
    `is_final: true` (no coupon, no trigger). The echo marks it `final` and
    `is_final` with no coupon fields; the probabilities carry no `p_coupon`
    (there is no coupon test) and no `p_call` at that event; and the price is
    the same as the zero-coupon workaround (`coupon_trigger 0.0, coupon 0.0`)
    on the same stream, to the print precision."""
    with open(data_dir / SCOTIA_EXAMPLE) as fh:
        req = json.load(fh)
    last = req["notes"][0]["events"][-1]
    assert last == {"observation_date": "2028-10-02", "payment_date": "2028-10-05",
                    "is_final": True}
    status, body = _post(client, req)
    assert status == 200, body[:300]
    n = json.loads(body)["notes"][0]
    events = n["terms"]["events"]
    assert events[-1]["final"] and events[-1]["is_final"]
    assert not events[-1]["accrual_only"]
    assert not events[0]["is_final"] and events[0]["final"] is False
    for k in ("coupon", "coupon_trigger", "autocall_trigger"):
        assert k not in events[-1]
    assert events[-1]["payment_date"] == n["terms"]["maturity_payment_date"] == "2028-10-05"
    probs = n["event_probabilities"]
    assert [p["index"] for p in probs] == [0, 1]
    assert "p_call" in probs[0] and "p_coupon" in probs[0]
    assert "p_coupon" not in probs[1] and "p_call" not in probs[1]
    assert 0.0 < n["p_barrier_breach"] < 1.0

    workaround = json.loads(json.dumps(req))
    workaround["notes"][0]["events"][-1] = {"observation_date": "2028-10-02",
                                            "payment_date": "2028-10-05",
                                            "coupon_trigger": 0.0, "coupon": 0.0}
    status, body = _post(client, workaround)
    assert status == 200, body[:300]
    w = json.loads(body)["notes"][0]
    assert w["npv"] == n["npv"]
    assert w["standard_error"] == n["standard_error"]
    for bucket in ("bond_redemption", "coupons", "capital_loss"):
        assert w["legs"][bucket]["pv"] == n["legs"][bucket]["pv"]
    assert w["p_barrier_breach"] == n["p_barrier_breach"]
    # The zero coupon test holds on every path alive at the final observation.
    survival = 1.0 - w["event_probabilities"][0]["p_call"]
    assert abs(w["event_probabilities"][1]["p_coupon"] - survival) < 1e-9
    assert not w["terms"]["events"][-1]["is_final"]


GREEKS_EXAMPLE = "structured_note/bnp_quad_index_classic_autocall_xs2741509801_greeks.json"
SINGLE_UNDERLYING_EXAMPLE = "structured_note/single_underlying_reverse_convertible.json"
GREEKS_DEFAULTS = {"spot_bump": 0.01, "vol_bump": 0.01, "correlation_bump": 0.01,
                   "funding_bump_bp": 1.0, "rate_bump_bp": 1.0}
GREEK_FIELDS = ("delta", "gamma", "vega")


def test_greeks_example(client, data_dir):
    """The BNP request with `greeks` on (16 simulations on the same
    stream): the block's shape, the standard errors of the differences, the
    bumped matrix (the request's matrix + 0.01 off the diagonal, no repair
    needed), funding_dv01 < 0, a positive correlation sensitivity (the
    worst-of note is long correlation: its correlation-one limit is far
    above its correlation-zero limit), the bump sizes echoed, and the
    headline identical to the plain run."""
    with open(data_dir / GREEKS_EXAMPLE) as fh:
        req = json.load(fh)
    assert req["greeks"] == GREEKS_DEFAULTS
    n = _price(client, req)
    plain = json.loads(json.dumps(req))
    del plain["greeks"]
    p = _price(client, plain)
    assert n["npv"] == p["npv"] and n["standard_error"] == p["standard_error"]
    assert "timing" not in n
    g = n["greeks"]
    ids = [u["underlying_id"] for u in req["notes"][0]["underlyings"]]
    assert [u["underlying_id"] for u in g["underlyings"]] == ids
    for u in g["underlyings"]:
        for f in GREEK_FIELDS:
            assert math.isfinite(u[f]) and math.isfinite(u[f + "_standard_error"])
            assert u[f + "_standard_error"] > 0.0
    for k, v in GREEKS_DEFAULTS.items():
        assert g[k] == v
    assert g["correlation_bump_repaired"] is False
    corr = req["pricing"]["correlations"]["matrices"][0]
    assert corr["labels"] == ids
    values = corr["values"]["values"]
    assert len(g["correlation_bumped"]) == 16
    for i in range(4):
        for j in range(4):
            expected = 1.0 if i == j else min(1.0, values[i * 4 + j] + 0.01)
            assert abs(g["correlation_bumped"][i * 4 + j] - expected) < 1e-15
    assert g["correlation_sensitivity"] > 0.0
    assert g["correlation_sensitivity_standard_error"] > 0.0
    assert g["funding_dv01"] < 0.0 and g["funding_dv01_standard_error"] >= 0.0
    assert math.isfinite(g["rho"]) and g["rho_standard_error"] >= 0.0


def test_greeks_timing_lists_one_stage_per_bumped_run(client, data_dir):
    """Single underlying: no correlation stage and no correlation
    sensitivity; the five bumped runs each get a timing stage after the
    simulation."""
    with open(data_dir / SINGLE_UNDERLYING_EXAMPLE) as fh:
        req = json.load(fh)
    req["notes"][0]["monte_carlo"]["paths"] = 4096
    req["greeks"] = {}
    req["diagnostics"] = {"timing": True}
    n = _price(client, req)
    g = n["greeks"]
    assert [u["underlying_id"] for u in g["underlyings"]] == ["SPX"]
    assert "correlation_sensitivity" not in g
    assert "correlation_sensitivity_standard_error" not in g
    assert "correlation_bumped" not in g
    assert g["correlation_bump_repaired"] is False
    stages = [st["stage"] for st in n["timing"]["stages"]]
    assert stages == ["market_build", "simulation", "greek_spot_up_SPX", "greek_spot_down_SPX",
                      "greek_vol_SPX", "greek_funding", "greek_rate"]
    assert all(st["milliseconds"] >= 0.0 for st in n["timing"]["stages"])
    # Unconditional coupons: no capital risk below the barrier only at the
    # final observation, so the funding DV01 is a pure discounting number.
    assert g["funding_dv01"] < 0.0


BLOCKS = ("market", "decomposition", "checks", "distribution", "timing")
DIAGNOSTICS_EXAMPLE = "structured_note/bnp_quad_index_classic_autocall_xs2741509801_diagnostics.json"


def _price(client, req):
    status, body = _post(client, req)
    assert status == 200, f"HTTP {status} :: {body[:300]}"
    return json.loads(body)["notes"][0]


def test_each_diagnostic_block_is_present_iff_its_flag_is_set(client, data_dir):
    with open(data_dir / EXAMPLES[0][0]) as fh:
        base = json.load(fh)
    plain = _price(client, base)
    assert "vanilla_repricing" not in plain
    for block in BLOCKS:
        req = json.loads(json.dumps(base))
        req["diagnostics"] = {block: True}
        n = _price(client, req)
        for other in BLOCKS:
            assert (other in n) == (other == block), (block, other)
        assert "vanilla_repricing" not in n  # needs its sheets (own suite)
        # the headline is the same run (same stream)
        assert n["npv"] == plain["npv"] and n["standard_error"] == plain["standard_error"]
    req = json.loads(json.dumps(base))
    req["diagnostics"] = {}
    n = _price(client, req)
    assert not any(b in n for b in BLOCKS)


def test_diagnostics_example(client, data_dir):
    with open(data_dir / DIAGNOSTICS_EXAMPLE) as fh:
        req = json.load(fh)
    assert req["diagnostics"] == {"market": True, "decomposition": True, "checks": True,
                                  "distribution": True, "timing": True, "sample_paths": 3}
    n = _price(client, req)
    note = req["notes"][0]
    notional = note["notional"]
    n_future = 10
    legs = n["legs"]
    for b in BLOCKS:
        assert b in n

    # --- market: deterministic, read off the curves the processes use ----
    m = n["market"]
    assert len(m["observation_dates"]) == n_future
    assert m["observation_dates"] == [e["observation_date"] for e in note["events"][2:]]
    assert [u["underlying_id"] for u in m["underlyings"]] == ["SMI", "SPX", "SX5E", "NKY"]
    spots = {q["id"]: q["value"] for q in req["pricing"]["quotes"]}
    for u in m["underlyings"]:
        assert u["spot"] == spots[u["underlying_id"] + "_SPOT"]
        for key in ("forwards", "implied_dividend_yields", "implied_rates", "atm_vols",
                    "barrier_vols"):
            assert len(u[key]) == n_future, key
        assert u["atm_vols"] == u["barrier_vols"]  # constant vols
        assert all(f > 0 for f in u["forwards"])
        assert ("quanto_drift_bp" in u) == (u["underlying_id"] != "SPX")
    # SPX: flat 2 % dividend yield, USD rate curve, forward = S exp((r - q) t)
    spx = next(u for u in m["underlyings"] if u["underlying_id"] == "SPX")
    assert all(abs(q - 0.02) < 1e-9 for q in spx["implied_dividend_yields"])
    assert abs(spx["atm_vols"][0] - 0.20) < 1e-12
    t_grid = n["model"]["time_grid"][1:]
    for t, f, r in zip(t_grid, spx["forwards"], spx["implied_rates"]):
        assert 0.0 < r < 0.1
    # quanto drift in bp = model echo drift x 1e4 (constant vols: same at every date)
    for u, mu in zip(m["underlyings"], n["model"]["underlyings"]):
        if "quanto_drift_bp" in u:
            assert abs(u["quanto_drift_bp"] - mu["quanto_drift"] * 1e4) < 1e-6
    assert m["payment_dates"] == [e["payment_date"] for e in note["events"][2:]]  # final = maturity
    assert len(m["discount_factors"]) == len(m["payment_dates"])
    for plain, funded in zip(m["discount_factors"], m["discount_factors_with_funding"]):
        assert 0 < funded < plain < 1
    assert m["correlation_labels"] == ["SMI", "SPX", "SX5E", "NKY"]
    assert m["correlation"] == req["pricing"]["correlations"]["matrices"][0]["values"]["values"]

    # --- decomposition: event PVs add up to the buckets --------------------
    d = n["decomposition"]
    assert len(d["events"]) == n_future
    probs = {p["index"]: p for p in n["event_probabilities"]}
    for e in d["events"]:
        assert e["p_call"] == probs[e["index"]]["p_call"]
        assert e["p_coupon"] == probs[e["index"]]["p_coupon"]
        assert e["pv_coupons"] >= 0.0 and e["pv_redemption"] >= 0.0
    assert abs(sum(e["pv_coupons"] for e in d["events"]) - legs["coupons"]["pv"]) < 1e-9 * notional
    assert abs(sum(e["pv_redemption"] for e in d["events"]) - legs["bond_redemption"]["pv"]) \
        < 1e-9 * notional
    assert abs(sum(e["pv_coupons"] + e["pv_redemption"] for e in d["events"])
               + legs["capital_loss"]["pv"] - n["npv"]) < 1e-9 * notional
    assert abs(d["p_survive_to_maturity"] + sum(p["p_call"] for p in probs.values()) - 1.0) < 1e-9
    assert d["p_breach"] == n["p_barrier_breach"]
    assert d["expected_life_years"] == n["expected_life_years"]
    assert 0.4 < d["expected_loss_given_breach"] < 1.0  # 60 % barrier, 1-for-1

    # --- checks ------------------------------------------------------------
    c = n["checks"]
    rms = c["running_means"]
    assert [r["fraction"] for r in rms] == [0.25, 0.5, 0.75, 1.0]
    assert [r["paths"] for r in rms] == [16384, 32768, 49152, 65536]
    assert rms[-1]["npv"] == n["npv"] and rms[-1]["standard_error"] == n["standard_error"]
    for r in rms:
        assert abs(r["npv"] - n["npv"]) < 5 * r["standard_error"]
    assert 0 < c["bond_only_npv"] < notional
    assert abs(c["bond_only_npv"] - notional * m["discount_factors_with_funding"][-1]) < 1e-9
    # The worst-of digital strip is short correlation.
    assert c["correlation_one_npv"] > n["npv"] > c["correlation_zero_npv"]
    assert c["correlation_one_standard_error"] > 0 and c["correlation_zero_standard_error"] > 0
    assert 0 < c["deterministic_forward_npv"] < 1.5 * notional

    # --- distribution --------------------------------------------------------
    dist = n["distribution"]
    assert [q["observation_date"] for q in dist["basket_performance_quantiles"]] \
        == m["observation_dates"]
    for q in dist["basket_performance_quantiles"]:
        assert q["q05"] <= q["q25"] <= q["q50"] <= q["q75"] <= q["q95"]
    assert dist["payout_q05"] <= dist["payout_q25"] <= dist["payout_q50"] \
        <= dist["payout_q75"] <= dist["payout_q95"]
    hist = dist["payout_histogram"]
    assert len(hist) == 20
    assert sum(b["paths"] for b in hist) == n["paths"]
    for prev, nxt in zip(hist, hist[1:]):
        assert abs(prev["upper"] - nxt["lower"]) < 1e-12
    assert abs(hist[0]["lower"] - dist["payout_q05"]) < 1e-9 or hist[0]["lower"] < dist["payout_q05"]
    assert hist[-1]["upper"] >= dist["payout_q95"]
    sps = dist["sample_paths"]
    assert len(sps) == 3
    for sp in sps:
        assert len(sp["basket_performances"]) == n_future
        assert sp["payout"] >= 0.0 and sp["payout_pv"] >= 0.0
        assert hist[0]["lower"] <= sp["payout"] <= hist[-1]["upper"]
        if "called_on" in sp:
            assert sp["called_on"] in m["observation_dates"]
            assert sp["breached"] is False

    # --- timing --------------------------------------------------------------
    tm = n["timing"]
    assert [s["stage"] for s in tm["stages"]] == [
        "market_build", "simulation", "deterministic_forward", "correlation_one",
        "correlation_zero", "diagnostics"]
    assert all(s["milliseconds"] >= 0.0 for s in tm["stages"])
    assert tm["total_milliseconds"] >= sum(s["milliseconds"] for s in tm["stages"]) - 1e-6


def test_single_underlying_reverse_convertible(client, data_dir):
    with open(data_dir / "structured_note/single_underlying_reverse_convertible.json") as fh:
        req = json.load(fh)
    note = req["notes"][0]
    assert "correlation_id" not in note and len(note["underlyings"]) == 1
    req["diagnostics"] = {"market": True, "checks": True, "decomposition": True}
    n = _price(client, req)
    notional = note["notional"]
    assert n["status"] == "Alive" and n["paths"] == 65536
    assert 0.0 < n["npv"] < 1.2 * notional and n["standard_error"] > 0.0
    assert "correlation_id" not in n["terms"]
    assert len(n["terms"]["underlyings"]) == 1
    # Unconditional coupons: every coupon test holds, nothing can call.
    probs = n["event_probabilities"]
    assert len(probs) == 8
    for p in probs:
        assert p["p_coupon"] == 1.0 and "p_call" not in p
    d = n["decomposition"]
    assert d["p_survive_to_maturity"] == 1.0
    coupon_pv = sum(e["pv_coupons"] for e in d["events"])
    assert abs(coupon_pv - n["legs"]["coupons"]["pv"]) < 1e-9 * notional
    m = n["market"]
    assert m["correlation_labels"] == ["SPX"] and m["correlation"] == [1.0]
    assert m["payment_dates"][-1] == note["maturity_payment_date"]
    # Coupons are paid whatever happens: their PV is the sum of DF x coupon.
    dfs = dict(zip(m["payment_dates"], m["discount_factors_with_funding"]))
    expected = sum(0.02 * dfs[e["payment_date"]] for e in note["events"]) * notional
    assert abs(coupon_pv - expected) < 1e-9 * notional
    c = n["checks"]
    assert c["correlation_one_npv"] == n["npv"] == c["correlation_zero_npv"]
    assert c["correlation_one_standard_error"] == n["standard_error"]
    # Forward of SPX stays above the 70 % barrier: the deterministic walk
    # pays every coupon and the full notional.
    assert abs(c["deterministic_forward_npv"] - (expected + c["bond_only_npv"])) < 1e-9 * notional
    assert c["deterministic_forward_npv"] > n["npv"]


def test_single_underlying_with_correlation_id_is_400(client, data_dir):
    with open(data_dir / "structured_note/single_underlying_reverse_convertible.json") as fh:
        req = json.load(fh)
    req["notes"][0]["correlation_id"] = "ANY"
    status, body = _post(client, req)
    assert status == 400 and "correlation_id: must be absent with a single underlying" in body
