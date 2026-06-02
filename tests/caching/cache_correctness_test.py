"""Curve-cache correctness / transparency — Suite 6.

The curve cache (parser/curve_cache.h) must be invisible: turning it on may not
change any result. For each representative product this suite POSTs the example
payload to a cache-OFF server and a cache-ON server and asserts the responses
are bit-for-bit identical, then POSTs to the cache-ON server a second time
(warm -> hit) and asserts the cache-hit response is identical too. So the gate
covers both "cache-ON == cache-OFF" and "a cache hit does not perturb the
result".

A separate test reads the cache-ON gRPC server log and asserts at least one
CurveCache L1 hit was logged across these requests, so the suite can never
silently pass without the cache actually engaging.

The comparison core is table-driven over (product, payload). When D23 makes
bumped curves (curveBump != 0) cacheable, a bumped variant drops in as a
one-line CASES addition — no other change needed.
"""

import json
import time
from collections import namedtuple

import pytest

# load_json is re-exported from conftest (which puts tests/contract on the path);
# import defensively so the module also collects when run on its own.
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "contract"))
from ql_reference import load_json  # noqa: E402


# (id, product key, example request file, response list key for the priced
# instrument — or None for the bootstrap endpoint, whose response is a curve set
# rather than a single NPV). Every row must bootstrap >=1 curve so the cache
# engages (curveBump == 0, which the example payloads use by default).
Case = namedtuple("Case", "id product filename npv_key")

CASES = [
    Case("fixed_rate_bond", "fixed_rate_bond",
         "fixed_rate_bond_request.json", "bonds"),
    Case("vanilla_swap_multicurve", "vanilla_swap",
         "vanilla_swap_multicurve_request.json", "swaps"),
    Case("swaption", "swaption",
         "swaption_request.json", "swaptions"),
    Case("swaption_smile_cube", "swaption",
         "swaption_smile_cube_request.json", "swaptions"),
    Case("swaption_sabr_calibrate", "swaption",
         "swaption_sabr_calibrate_request.json", "swaptions"),
    Case("cds", "cds",
         "cds_request.json", "cds_list"),
    Case("bootstrap_curves_forward", "bootstrap_curves",
         "bootstrap_curves_forward.json", None),
]

# The product whose log we probe to prove the cache engaged. Any cacheable row
# works; the fixed-rate bond bootstraps a single deposit/bond curve.
ENGAGED_CASE = CASES[0]


def _canonical(response: dict) -> str:
    """Bit-for-bit comparable form of a response: key order-independent, every
    float compared at full precision via its serialized JSON text."""
    return json.dumps(response, sort_keys=True)


def _npvs(response: dict, npv_key):
    """NPVs of a priced-instrument response (for the human-readable report);
    None for the bootstrap endpoint, which returns curves instead."""
    if npv_key is None:
        return None
    return [item.get("npv") for item in response.get(npv_key, [])]


@pytest.mark.parametrize("case", CASES, ids=[c.id for c in CASES])
def test_cache_transparency(case, nocache_client, cache_client, data_dir):
    request = load_json(data_dir / case.filename)

    off = nocache_client.price(case.product, request)   # reference (cache OFF)
    warm = cache_client.price(case.product, request)    # cache ON, populates
    hit = cache_client.price(case.product, request)     # cache ON, serves hit

    assert _canonical(off) == _canonical(warm), (
        f"{case.id}: cache-ON response differs from cache-OFF — "
        f"cache is not transparent"
    )
    assert _canonical(warm) == _canonical(hit), (
        f"{case.id}: cache-hit response differs from the warm (miss) response — "
        f"a cache hit perturbed the result"
    )

    npvs = _npvs(off, case.npv_key)
    detail = f"NPVs off==warm==hit = {npvs}" if npvs is not None \
        else "response (curve set) off==warm==hit identical"
    print(f"[cache] {case.id}: {detail}")


def _count_cache_hits(log_path: Path) -> int:
    if not log_path.exists():
        return 0
    return log_path.read_text(errors="replace").count("event=L1_HIT")


def _last_hit_line(log_path: Path) -> str:
    if not log_path.exists():
        return ""
    for line in reversed(log_path.read_text(errors="replace").splitlines()):
        if "event=L1_HIT" in line:
            return line
    return ""


def test_cache_engaged(cache_client, data_dir, cache_log_path):
    """Prove the cache actually engaged: a second identical POST to the cache-ON
    server must log at least one CurveCache L1 hit. Guards against the
    transparency assertions silently passing with caching that never triggers.
    """
    request = load_json(data_dir / ENGAGED_CASE.filename)

    before = _count_cache_hits(cache_log_path)
    cache_client.price(ENGAGED_CASE.product, request)   # warm (may miss)
    cache_client.price(ENGAGED_CASE.product, request)   # hit

    # The engine flushes each CurveCache line (std::endl), and the HTTP response
    # only returns after bootstrapping completes, so the hit is already on disk;
    # retry briefly purely as belt-and-suspenders against filesystem lag.
    after = before
    for _ in range(20):
        after = _count_cache_hits(cache_log_path)
        if after > before:
            break
        time.sleep(0.1)

    assert after > before, (
        f"No CurveCache L1 hit logged in {cache_log_path} "
        f"(hits before={before}, after={after}); the cache did not engage. "
        f"Is QUANTRA_CURVE_CACHE_ENABLED=1 / QUANTRA_CURVE_CACHE_LOG=1 set on "
        f"the cache-ON gRPC server?"
    )
    print(f"[cache] cache engaged — CurveCache hit logged: {_last_hit_line(cache_log_path)}")
