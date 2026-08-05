"""Smoke test for the historical-simulation VaR example.

examples/risk_var/historical_var.py is a public-facing showcase script; this
test keeps it honest against the live wire contract. It imports the script's
core functions and runs a miniature end-to-end VaR (5 swaps x 4 scenarios)
against the test server: the payloads must be accepted by /price-ois-swap,
the base book value must be finite, the P&L vector must have one entry per
scenario, and the VaR/ES statistics must be finite.
"""

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "examples" / "risk_var"))

import historical_var as hv  # noqa: E402  (path set up above)

N_SWAPS = 5
N_SCENARIOS = 4
SEED = 7


def test_portfolio_is_seeded_and_deterministic():
    a = hv.build_portfolio(N_SWAPS, seed=SEED)
    b = hv.build_portfolio(N_SWAPS, seed=SEED)
    assert a == b, "same seed must reproduce the same book"
    assert len(a) == N_SWAPS
    types = {t["ois_swap"]["swap_type"] for t in a}
    assert types <= {"Payer", "Receiver"}


def test_request_payload_passes_wire_contract(client):
    """The script's request body must be accepted verbatim by /price-ois-swap."""
    portfolio = hv.build_portfolio(N_SWAPS, seed=SEED)
    request = hv.build_request(portfolio, hv.BASE_RATES)
    resp = client.session.post(f"{client.base_url}/price-ois-swap", json=request)
    assert resp.status_code == 200, (
        f"wire contract rejected the example's payload "
        f"({resp.status_code}): {resp.text[:300]}"
    )
    swaps = resp.json()["swaps"]
    assert len(swaps) == N_SWAPS, "one NPV per swap in the book"
    assert all(math.isfinite(s.get("npv", 0.0)) for s in swaps)


def test_mini_var_run_completes(client):
    result = hv.run_var(
        url=client.base_url,
        n_swaps=N_SWAPS,
        n_scenarios=N_SCENARIOS,
        seed=SEED,
        concurrency=2,
    )
    assert math.isfinite(result["base_value"])
    assert result["synthetic_history"] is True
    assert len(result["pnl"]) == N_SCENARIOS
    assert all(math.isfinite(p) for p in result["pnl"])
    assert math.isfinite(result["var_99"])
    assert math.isfinite(result["es_99"])
    # ES is the tail mean beyond VaR, so it can never be a smaller loss.
    assert result["es_99"] >= result["var_99"]
    assert result["timing"]["requests"] == N_SCENARIOS + 1

    # A shocked curve must actually change the book value (the shocks are
    # applied to the par quotes and re-bootstrapped, not silently dropped).
    assert any(abs(p) > 1e-6 for p in result["pnl"])


def test_tail_stats_pure_math():
    """The quantile/ES math is stdlib-only; pin its conventions."""
    pnl = [-100.0, -50.0, -10.0, 0.0, 5.0, 20.0]
    stats = hv.tail_stats(pnl)
    assert stats["worst_pnl"] == -100.0
    assert stats["second_worst_pnl"] == -50.0
    assert stats["var"] >= 0.0
    assert stats["es"] >= stats["var"]
