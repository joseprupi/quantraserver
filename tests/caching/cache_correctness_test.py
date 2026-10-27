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

The comparison core is table-driven over (product, payload), so a new
scenario drops in as a one-line CASES addition — no other change needed.
"""

import copy
import datetime
import json
import re
import time
from collections import namedtuple

import pytest

# load_json is re-exported from conftest (which puts tests/contract on the path);
# import defensively so the module also collects when run on its own.
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "contract"))
from ql_reference import ApiClient, load_json  # noqa: E402
from request_state_test import (  # noqa: E402
    run_curve_extrapolation_sequence,
    run_swaption_vol_extrapolation_sequence,
)


# (id, product key, example request file, response list key for the priced
# instrument — or None for the bootstrap endpoint, whose response is a curve set
# rather than a single NPV). Every row must bootstrap >=1 curve so the cache
# engages (curveBump == 0, which the example payloads use by default).
Case = namedtuple("Case", "id product filename npv_key")

CASES = [
    Case("fixed_rate_bond", "fixed_rate_bond",
         "bonds/fixed_rate_bond_request.json", "bonds"),
    # ForwardFlat discount curve: the frozen L1 reconstruction must use the
    # same inter-pillar interpolation as the live curve. The bond's semiannual
    # May/Nov-15 coupons fall between the curve's pillars, where a wrong
    # interpolator drifts even though the pillar DFs match exactly.
    Case("fixed_rate_bond_forwardflat", "fixed_rate_bond",
         "bonds/fixed_rate_bond_forwardflat_request.json", "bonds"),
    Case("vanilla_swap_multicurve", "vanilla_swap",
         "ir_swaps/vanilla_swap_multicurve_request.json", "swaps"),
    Case("swaption", "swaption",
         "swaption/swaption_request.json", "swaptions"),
    Case("swaption_smile_cube", "swaption",
         "swaption/swaption_smile_cube_request.json", "swaptions"),
    Case("swaption_sabr_calibrate", "swaption",
         "vol/swaption_sabr_calibrate_request.json", "swaptions"),
    # Hull-White swaption-model calibration cache. The calibrate endpoint runs
    # a Levenberg-Marquardt fit and returns (a, sigma, rmse); the cache must
    # reproduce those bit-for-bit on a hit. npv_key is None: the response is a
    # calibration result, not a priced instrument.
    Case("calibrate_swaption_model", "calibrate_swaption_model",
         "vol/calibrate_swaption_model_request.json", None),
    # A swaption priced with an HW-calibrated model exercises the same
    # cross-request calibration cache through the pricing path (the evaluator
    # calls the shared calibration helper).
    Case("swaption_bermudan_hw_calibrated", "swaption",
         "swaption/swaption_bermudan_hw_calibrated.json", "swaptions"),
    Case("cds", "cds",
         "cds/cds_request.json", "cds_list"),
    Case("bootstrap_curves_forward", "bootstrap_curves",
         "curves/bootstrap_curves_forward.json", None),
    # ZeroRatePoint discount curve: built as an InterpolatedZeroCurve,
    # NOT a PiecewiseYieldCurve, so the freeze path cannot extract exact pillars.
    # A cache hit must serve the LIVE curve — never a weekly-resampled
    # reconstruction — or the bond's semiannual coupons (which fall between the
    # user's zero nodes and off the weekly resample grid) drift on a hit.
    Case("fixed_rate_bond_zerorate", "fixed_rate_bond",
         "bonds/fixed_rate_bond_zerorate_request.json", "bonds"),
    # FX-implied curve: FxSwapHelper pillars on an FX spot + a collateral
    # curve. The curve's key must carry the spot's full content (see the
    # test_fx_spot_* cases below) and the collateral curve's own key.
    Case("bootstrap_curves_fxswap", "bootstrap_curves",
         "curves/curves_eur_usd_collateral_fxswap_short_end.json", None),
    Case("bootstrap_curves_fxswap_quote_id", "bootstrap_curves",
         "curves/curves_eur_usd_collateral_fxswap_short_end_quote_id.json", None),
    # Full collateral-adjusted curve: FX swap short end + cross-currency
    # basis long end, depending on two other curves (both forwarding curves
    # and the collateral discount curve enter the key through DEP).
    Case("bootstrap_curves_xccy", "bootstrap_curves",
         "curves/curves_eur_usd_collateral_full.json", None),
    Case("bootstrap_curves_xccy_quote_id", "bootstrap_curves",
         "curves/curves_eur_usd_collateral_full_quote_id.json", None),
    # FX forward priced off the collateral-adjusted EUR curve + USD SOFR OIS:
    # three cached curves (two of them pure rates) and an FX spot that is
    # per-request data, never cached (see test_fx_forward_spot_change_*).
    Case("fx_forward_eurusd_6m", "fx_forward",
         "fx_forward/fx_forward_eurusd_6m_buy_eur.json", "fx_forwards"),
    Case("fx_forward_eurusd_6m_spot_quote_id", "fx_forward",
         "fx_forward/fx_forward_eurusd_6m_buy_eur_spot_quote_id.json", "fx_forwards"),
    # Cross-currency swaps on the same three curves: a fixed/float one (fair
    # rate) and a float/float one (fair spread); the spot is request data.
    Case("xccy_eur_fixed_vs_usd_sofr_5y", "cross_currency_swap",
         "cross_currency_swap/xccy_eur_fixed_vs_usd_sofr_5y_payer_usd.json", "swaps"),
    Case("xccy_usd_sofr_vs_eur_estr_5y", "cross_currency_swap",
         "cross_currency_swap/xccy_usd_sofr_vs_eur_estr_minus15bp_5y_both_exchanges.json", "swaps"),
]

# The product whose log we probe to prove the cache engaged. Any cacheable row
# works; the fixed-rate bond bootstraps a single deposit/bond curve.
ENGAGED_CASE = CASES[0]

# The calibrate-swaption-model row, probed separately in
# test_hw_calib_cache_engaged so the HW-calibration transparency rows above
# cannot pass trivially with the calibration cache never engaging.
HW_CALIB_CASE = next(c for c in CASES if c.id == "calibrate_swaption_model")

# The zero-curve row + the curve id its payload bootstraps, probed separately
# in test_cache_engaged_zero_curve so the transparency assertion above cannot
# pass trivially with the zero curve unkeyable/uncached.
ZERO_CURVE_CASE = next(c for c in CASES if c.id == "fixed_rate_bond_zerorate")
ZERO_CURVE_ID = "usd_zero_curve"


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


REBUMP_FIXTURE = "swaption/swaption_ois_bbg_zerorate_request.json"


def _rebump_request(data_dir):
    request = load_json(data_dir / REBUMP_FIXTURE)
    request["pricing"].setdefault("options", {})["swaption_pricing_rebump"] = True
    return request


def test_cache_transparency_swaption_rebump(nocache_client, cache_client, data_dir):
    """The rebump greeks build four extra market snapshots (curve +/-1bp and a
    one-day roll with the curve reference dates shifted). Each must key
    separately from the base curve and from each other: cache-ON == cache-OFF
    cold and warm, including theta, which collapses to 0.0 if the rolled
    curve is served from the unrolled entry."""
    request = _rebump_request(data_dir)

    off = nocache_client.price("swaption", request)
    warm = cache_client.price("swaption", request)
    hit = cache_client.price("swaption", request)

    assert _canonical(off) == _canonical(warm), (
        "swaption_rebump: cache-ON response differs from cache-OFF")
    assert _canonical(warm) == _canonical(hit), (
        "swaption_rebump: cache-hit response differs from the warm response")
    theta = off["swaptions"][0]["theta"]
    assert theta != 0.0, f"rebump theta is exactly 0.0: the roll did not move: {off}"
    print(f"[cache] swaption_rebump: theta off==warm==hit = {theta}")


def test_rebump_after_warm_plain_request_is_isolated(nocache_client, cache_client,
                                                     data_dir):
    """Price the PLAIN request first on the cache-ON server (base curve cached
    at as-of), then the rebump request: the rolled snapshot must not be
    satisfied by the base entry, so theta equals the cache-OFF theta."""
    request = _rebump_request(data_dir)
    plain = copy.deepcopy(request)
    plain["pricing"]["options"].pop("swaption_pricing_rebump")

    cache_client.price("swaption", plain)   # warms the base curve entry
    on = cache_client.price("swaption", request)["swaptions"][0]
    off = nocache_client.price("swaption", request)["swaptions"][0]

    for key in ("npv", "dv01", "gamma", "vega", "theta"):
        assert on[key] == off[key], (
            f"swaption_rebump after warm plain: {key} cache-ON {on[key]} != "
            f"cache-OFF {off[key]}")


def _post_raw(client: ApiClient, product: str, request: dict):
    """POST like ApiClient.price but without raising on non-200: returns
    (status_code, body_text) so error responses can be compared bit-for-bit."""
    endpoint = ApiClient.ENDPOINTS[product]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    return r.status_code, r.text


def test_cache_transparency_beyond_pillar_error(nocache_client, cache_client,
                                                data_dir):
    """Error transparency: a request that fails without the cache must fail
    identically on a cache hit.

    The payload prices a bond whose cashflows run past the discount curve's
    last pillar, so the live curve throws a QuantLib range error. The frozen
    L1 reconstruction must preserve that: if it (re-)enabled extrapolation,
    the cache hit would silently turn the error into an extrapolated number.
    """
    request = load_json(data_dir / "bonds/fixed_rate_bond_beyond_pillar_request.json")

    off = _post_raw(nocache_client, "fixed_rate_bond", request)   # reference
    warm = _post_raw(cache_client, "fixed_rate_bond", request)    # populates L1
    hit = _post_raw(cache_client, "fixed_rate_bond", request)     # serves hit

    # The case only proves error transparency if the reference actually errors;
    # guard against the payload drifting into a priceable request.
    assert off[0] != 200, (
        f"beyond-pillar payload unexpectedly priced on the cache-OFF server "
        f"(HTTP {off[0]}): the request no longer reaches past the curve's "
        f"last pillar, so it cannot test error transparency"
    )
    assert off == warm, (
        f"beyond_pillar_error: cache-ON (warm) response differs from cache-OFF "
        f"— off={off}, warm={warm}"
    )
    assert warm == hit, (
        f"beyond_pillar_error: cache-hit response differs from the warm (miss) "
        f"response — the frozen curve does not fail like the live one "
        f"(warm={warm}, hit={hit})"
    )
    print(f"[cache] beyond_pillar_error: off==warm==hit = "
          f"HTTP {off[0]}, body {off[1]!r}")


def test_bogus_quote_id_not_masked_by_warm_cache(nocache_client, cache_client,
                                                 data_dir):
    """Reference-error transparency: a helper `quote_id` that resolves to
    nothing must return the parser's NOT_FOUND error even when the cache is
    warm with the equivalent inline-value request.

    The cache key stores resolved quote VALUES, never quote ids, and it is
    built before the request is parsed. A key builder that silently falls
    back to the helper's inline value when a quote_id does not resolve hashes
    the bogus request to the same key as the valid inline-value request — so
    a warm cache would serve a cached price where the no-cache path serves
    404. The key builder must instead refuse to key the curve (leaving it
    uncached), letting the parser report the unknown id.
    """
    valid = load_json(data_dir / "bonds/fixed_rate_bond_request.json")

    # Warm the cache-ON server with the valid inline-value request so that a
    # key collision with the bogus request below would actually serve a hit.
    cache_client.price("fixed_rate_bond", valid)

    bogus = copy.deepcopy(valid)
    point = bogus["pricing"]["rates"]["curves"][0]["points"][0]["point"]
    point["quote_id"] = "quote-id-that-does-not-exist"

    off = _post_raw(nocache_client, "fixed_rate_bond", bogus)   # reference
    first = _post_raw(cache_client, "fixed_rate_bond", bogus)   # warm cache
    second = _post_raw(cache_client, "fixed_rate_bond", bogus)  # again: no
    # entry may have been created by the first attempt either.

    assert off[0] == 404 and "Unknown quote id" in off[1], (
        f"cache-OFF reference did not reject the bogus quote_id as NOT_FOUND "
        f"(got HTTP {off[0]}, body {off[1]!r})"
    )
    for label, resp in (("first", first), ("second", second)):
        assert resp[0] == 404 and "Unknown quote id" in resp[1], (
            f"bogus_quote_id: {label} POST to the warm cache-ON server did "
            f"not return the parser's NOT_FOUND — a cached price masked the "
            f"validation error (got HTTP {resp[0]}, body {resp[1]!r})"
        )
    print(f"[cache] bogus_quote_id: off/warm-first/warm-second all "
          f"HTTP 404, body {off[1]!r}")


def test_curve_extrapolation_option_not_left_on_cached_curve(cache_client, data_dir):
    """A /bootstrap-curves query that allows extrapolation reads the cached
    curve; a later bond past that curve's last pillar must still fail."""
    run_curve_extrapolation_sequence(cache_client, data_dir)


def test_vol_extrapolation_option_not_left_on_cached_sabr_cube(cache_client, data_dir):
    """A /sample-vol-surfaces query that forbids extrapolation reads the
    cached SABR cube; a later swaption past the cube's last tenor must still
    price, at the same NPV."""
    run_swaption_vol_extrapolation_sequence(cache_client, data_dir)


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


def _count_zero_curve_hits(log_path: Path) -> int:
    if not log_path.exists():
        return 0
    return sum(
        1
        for line in log_path.read_text(errors="replace").splitlines()
        if "event=L1_HIT" in line and f"curve={ZERO_CURVE_ID} " in line
    )


def test_cache_engaged_zero_curve(cache_client, data_dir, cache_log_path):
    """Companion to the fixed_rate_bond_zerorate transparency row: prove the
    ZeroRatePoint (InterpolatedZeroCurve) curve itself actually hits the cache —
    an L1_HIT line naming its curve id must be logged. Guards against the
    transparency assertions passing trivially because the zero curve became
    unkeyable/uncached (e.g. KEY_BUILD_FAILED_UNCACHED)."""
    request = load_json(data_dir / ZERO_CURVE_CASE.filename)

    before = _count_zero_curve_hits(cache_log_path)
    cache_client.price(ZERO_CURVE_CASE.product, request)   # warm (may miss)
    cache_client.price(ZERO_CURVE_CASE.product, request)   # hit

    after = before
    for _ in range(20):
        after = _count_zero_curve_hits(cache_log_path)
        if after > before:
            break
        time.sleep(0.1)

    assert after > before, (
        f"No CurveCache L1 hit for curve={ZERO_CURVE_ID} logged in "
        f"{cache_log_path} (hits before={before}, after={after}); the "
        f"ZeroRatePoint curve did not engage the cache, so the "
        f"transparency case proves nothing."
    )
    print(f"[cache] zero curve engaged — L1 hits for {ZERO_CURVE_ID}: "
          f"{before} -> {after}")


# ---------------------------------------------------------------------------
# FX spot market data and the FX-implied curve
# ---------------------------------------------------------------------------

FX_CASE = next(c for c in CASES if c.id == "bootstrap_curves_fxswap")
FX_CURVE_ID = "EUR_USD_COLLATERAL"
FX_COLLATERAL_CURVE_ID = "USD_SOFR_OIS"


def _count_curve_hits(log_path: Path, curve_id: str) -> int:
    if not log_path.exists():
        return 0
    return sum(
        1
        for line in log_path.read_text(errors="replace").splitlines()
        if "event=L1_HIT" in line and f"curve={curve_id} " in line
    )


def _wait_for_hits(log_path: Path, curve_id: str, before: int) -> int:
    after = before
    for _ in range(20):
        after = _count_curve_hits(log_path, curve_id)
        if after > before:
            break
        time.sleep(0.1)
    return after


def _fx_request(data_dir, spot_rate=None):
    request = load_json(data_dir / FX_CASE.filename)
    if spot_rate is not None:
        request["pricing"]["fx"]["spots"][0]["rate"] = spot_rate
    return request


def test_fx_curve_cache_engaged(cache_client, data_dir, cache_log_path):
    """The FX-implied curve itself must hit the cache on a repeated request
    (an L1_HIT line naming its id), so the transparency row above cannot pass
    trivially with the curve unkeyable (e.g. the spot failing the key build)."""
    request = _fx_request(data_dir)
    before = _count_curve_hits(cache_log_path, FX_CURVE_ID)
    cache_client.price(FX_CASE.product, request)   # warm (may miss)
    cache_client.price(FX_CASE.product, request)   # hit
    after = _wait_for_hits(cache_log_path, FX_CURVE_ID, before)
    assert after > before, (
        f"No CurveCache L1 hit for curve={FX_CURVE_ID} logged in {cache_log_path} "
        f"(hits before={before}, after={after}); the FX-implied curve did not "
        f"engage the cache"
    )


def test_fx_spot_change_is_a_cache_miss(nocache_client, cache_client, data_dir):
    """Same curves, same points, a different spot: the cached FX curve must
    not be served. The spot is part of the curve's key, so the cache-ON
    answer must equal the cache-OFF answer for the new spot."""
    plain = _fx_request(data_dir)
    moved = _fx_request(data_dir, spot_rate=1.0950)

    off_plain = nocache_client.price(FX_CASE.product, plain)
    off_moved = nocache_client.price(FX_CASE.product, moved)
    assert _canonical(off_plain) != _canonical(off_moved), (
        "moving the spot does not change the cache-OFF curve, so this case "
        "cannot detect a stale entry"
    )

    cache_client.price(FX_CASE.product, plain)            # populate with the old spot
    warm_plain = cache_client.price(FX_CASE.product, plain)
    assert _canonical(warm_plain) == _canonical(off_plain)

    got = cache_client.price(FX_CASE.product, moved)
    assert _canonical(got) != _canonical(off_plain), (
        "a request with a different FX spot was served the curve cached for "
        "the old spot"
    )
    assert _canonical(got) == _canonical(off_moved), (
        "cache-ON response for the moved spot differs from cache-OFF"
    )

    # And the moved-spot entry must not shadow the original one.
    again = cache_client.price(FX_CASE.product, plain)
    assert _canonical(again) == _canonical(off_plain)


def test_fx_spot_does_not_invalidate_pure_rates_curves(cache_client, data_dir,
                                                       cache_log_path):
    """The USD collateral curve has no FX pillar, so moving the spot must
    leave its cache entry alone: the request with the moved spot still hits
    the collateral curve's entry while the FX curve misses."""
    plain = _fx_request(data_dir)
    moved = _fx_request(data_dir, spot_rate=1.0750)

    cache_client.price(FX_CASE.product, plain)            # populate both curves
    cache_client.price(FX_CASE.product, plain)            # both hit
    before_coll = _count_curve_hits(cache_log_path, FX_COLLATERAL_CURVE_ID)
    before_fx = _count_curve_hits(cache_log_path, FX_CURVE_ID)

    cache_client.price(FX_CASE.product, moved)            # collateral hits, FX misses

    after_coll = _wait_for_hits(cache_log_path, FX_COLLATERAL_CURVE_ID, before_coll)
    assert after_coll > before_coll, (
        f"the pure-rates collateral curve {FX_COLLATERAL_CURVE_ID} did not hit the "
        f"cache after an FX spot change (hits {before_coll} -> {after_coll}); the "
        f"spot must not take part in its key"
    )
    time.sleep(0.2)
    after_fx = _count_curve_hits(cache_log_path, FX_CURVE_ID)
    assert after_fx == before_fx, (
        f"the FX-implied curve {FX_CURVE_ID} was served from the cache after the "
        f"spot moved (hits {before_fx} -> {after_fx})"
    )


def _count_hw_calib_hits(log_path: Path) -> int:
    if not log_path.exists():
        return 0
    return sum(
        1
        for line in log_path.read_text(errors="replace").splitlines()
        if "[HwCalibCache]" in line and "event=hit" in line
    )


def test_hw_calib_cache_engaged(cache_client, data_dir, cache_log_path):
    """Prove the Hull-White calibration cache actually engaged: a second
    identical calibrate-swaption-model POST to the cache-ON server must log at
    least one HwCalibCache hit. Guards against the HW-calibration transparency
    rows silently passing with a calibration cache that never triggers."""
    request = load_json(data_dir / HW_CALIB_CASE.filename)

    before = _count_hw_calib_hits(cache_log_path)
    cache_client.price(HW_CALIB_CASE.product, request)   # warm (may miss)
    cache_client.price(HW_CALIB_CASE.product, request)   # hit

    after = before
    for _ in range(20):
        after = _count_hw_calib_hits(cache_log_path)
        if after > before:
            break
        time.sleep(0.1)

    assert after > before, (
        f"No HwCalibCache hit logged in {cache_log_path} "
        f"(hits before={before}, after={after}); the calibration cache did not "
        f"engage. Is QUANTRA_HW_CACHE_ENABLED=1 / QUANTRA_HW_CACHE_LOG=1 set on "
        f"the cache-ON gRPC server (and QUANTRA_CURVE_CACHE_ENABLED=1 so the "
        f"curve keys the HW key delegates to are populated)?"
    )
    print(f"[cache] HW calibration cache engaged — hits: {before} -> {after}")


# ---------------------------------------------------------------------------
# Per-request calendar holiday overrides
#
# A cached entry holds a result computed under the calendars in force when it
# was built, so the active overrides are part of every process-lifetime cache
# key (curve, SABR cube, Hull-White calibration). These tests prove, per cache:
#   - transparency: a request WITH overrides is identical cache-ON vs cache-OFF;
#   - isolation: an entry built without overrides is never served to a request
#     with them, nor the reverse;
#   - engagement: overrides do not simply switch caching off.
# ---------------------------------------------------------------------------

# (id, product, example request file, TARGET business day declared a holiday,
# cache the row targets). Each date is a roll or expiry date of the request's
# instruments, so the override moves a schedule date and changes the response —
# asserted in the isolation tests, which would prove nothing with an inert
# override.
OverrideCase = namedtuple("OverrideCase", "id product filename added_holiday cache")

OVERRIDE_CASES = [
    OverrideCase("vanilla_swap_multicurve", "vanilla_swap",
                 "ir_swaps/vanilla_swap_multicurve_request.json", "2024-07-17", "curve"),
    OverrideCase("bootstrap_curves_forward", "bootstrap_curves",
                 "curves/bootstrap_curves_forward.json", "2025-06-17", "curve"),
    OverrideCase("swaption_sabr_calibrate", "swaption",
                 "vol/swaption_sabr_calibrate_request.json", "2026-01-19", "sabr"),
    OverrideCase("calibrate_swaption_model", "calibrate_swaption_model",
                 "vol/calibrate_swaption_model_request.json", "2026-01-15", "hw"),
    OverrideCase("swaption_bermudan_hw_calibrated", "swaption",
                 "swaption/swaption_bermudan_hw_calibrated.json", "2026-01-15", "hw"),
]
OVERRIDE_IDS = [c.id for c in OVERRIDE_CASES]

# Log markers of a hit in each cache (the three loggers differ in casing).
_HIT_MARKERS = {
    "curve": ("[CurveCache]", "event=L1_HIT"),
    "sabr": ("[SabrCalibrateCache]", "event=HIT"),
    "hw": ("[HwCalibCache]", "event=hit"),
}


def _with_overrides(request: dict, case) -> dict:
    out = copy.deepcopy(request)
    out["pricing"]["calendar_overrides"] = [
        {"calendar": "TARGET", "added_holidays": [case.added_holiday]}
    ]
    return out


def _bump_first_rate(node, eps: float) -> bool:
    """Shift the first numeric `rate` found under `node` by eps (in place)."""
    if isinstance(node, dict):
        rate = node.get("rate")
        if isinstance(rate, float):
            node["rate"] = rate + eps
            return True
        return any(_bump_first_rate(v, eps) for v in node.values())
    if isinstance(node, list):
        return any(_bump_first_rate(v, eps) for v in node)
    return False


def _count_hits(log_path: Path, cache: str) -> int:
    if not log_path.exists():
        return 0
    tag, event = _HIT_MARKERS[cache]
    return sum(
        1
        for line in log_path.read_text(errors="replace").splitlines()
        if tag in line and event in line
    )


@pytest.mark.parametrize("case", OVERRIDE_CASES, ids=OVERRIDE_IDS)
def test_calendar_overrides_cache_transparency(case, nocache_client,
                                               cache_client, data_dir):
    request = _with_overrides(load_json(data_dir / case.filename), case)

    off = nocache_client.price(case.product, request)   # reference (cache OFF)
    cold = cache_client.price(case.product, request)    # cache ON, populates
    warm = cache_client.price(case.product, request)    # cache ON, serves hit

    assert _canonical(off) == _canonical(cold), (
        f"{case.id}: with calendar overrides, the cold cache-ON response "
        f"differs from cache-OFF"
    )
    assert _canonical(off) == _canonical(warm), (
        f"{case.id}: with calendar overrides, the warm cache-ON response "
        f"differs from cache-OFF"
    )
    print(f"[cache] {case.id}: +{case.added_holiday} TARGET holiday "
          f"off==cold==warm identical")


@pytest.mark.parametrize("case", OVERRIDE_CASES, ids=OVERRIDE_IDS)
def test_calendar_overrides_not_served_from_plain_entry(
        case, nocache_client, cache_client, data_dir):
    """Warm the cache WITHOUT overrides, then send the same request WITH them:
    the answer must be the with-overrides one, not the warm entry."""
    plain = load_json(data_dir / case.filename)
    overridden = _with_overrides(plain, case)

    off_plain = nocache_client.price(case.product, plain)
    off_overridden = nocache_client.price(case.product, overridden)
    assert _canonical(off_plain) != _canonical(off_overridden), (
        f"{case.id}: the {case.added_holiday} override does not change the "
        f"cache-OFF response, so this case cannot detect a stale cache entry"
    )

    cache_client.price(case.product, plain)             # populate
    warm_plain = cache_client.price(case.product, plain)
    assert _canonical(warm_plain) == _canonical(off_plain)

    got = cache_client.price(case.product, overridden)
    assert _canonical(got) != _canonical(off_plain), (
        f"{case.id}: a request with calendar overrides was served the entry "
        f"cached without them"
    )
    assert _canonical(got) == _canonical(off_overridden), (
        f"{case.id}: with-overrides response after a no-override warm-up "
        f"differs from cache-OFF"
    )

    # And the with-overrides entry just stored must not shadow the plain one.
    again_plain = cache_client.price(case.product, plain)
    assert _canonical(again_plain) == _canonical(off_plain)


@pytest.mark.parametrize("case", OVERRIDE_CASES, ids=OVERRIDE_IDS)
def test_plain_request_not_served_from_overridden_entry(
        case, nocache_client, cache_client, data_dir):
    """The reverse order: warm the cache WITH overrides, then send the request
    WITHOUT them. The market is shifted by a negligible epsilon first so that
    neither variant is cached yet on the freshly started cache-ON server (every
    earlier test used the unshifted request) — the with-overrides request really
    is the one that populates the cache."""
    plain = load_json(data_dir / case.filename)
    assert _bump_first_rate(plain["pricing"], 1e-9), (
        f"{case.id}: no numeric rate found to shift"
    )
    overridden = _with_overrides(plain, case)

    off_plain = nocache_client.price(case.product, plain)
    off_overridden = nocache_client.price(case.product, overridden)
    assert _canonical(off_plain) != _canonical(off_overridden), (
        f"{case.id}: the {case.added_holiday} override does not change the "
        f"cache-OFF response, so this case cannot detect a stale cache entry"
    )

    cache_client.price(case.product, overridden)        # populate
    warm_overridden = cache_client.price(case.product, overridden)
    assert _canonical(warm_overridden) == _canonical(off_overridden)

    got = cache_client.price(case.product, plain)
    assert _canonical(got) != _canonical(off_overridden), (
        f"{case.id}: a request without calendar overrides was served the entry "
        f"cached with them"
    )
    assert _canonical(got) == _canonical(off_plain), (
        f"{case.id}: no-override response after a with-overrides warm-up "
        f"differs from cache-OFF"
    )


@pytest.mark.parametrize("case", OVERRIDE_CASES, ids=OVERRIDE_IDS)
def test_calendar_overrides_cache_engaged(case, cache_client, data_dir,
                                          cache_log_path):
    """Overrides must not simply disable caching: the second identical request
    WITH overrides is a hit in the cache this row targets."""
    request = _with_overrides(load_json(data_dir / case.filename), case)

    cache_client.price(case.product, request)           # warm (may miss)
    before = _count_hits(cache_log_path, case.cache)
    cache_client.price(case.product, request)           # must hit

    after = before
    for _ in range(20):
        after = _count_hits(cache_log_path, case.cache)
        if after > before:
            break
        time.sleep(0.1)

    assert after > before, (
        f"{case.id}: no {case.cache} cache hit logged in {cache_log_path} for a "
        f"repeated request with calendar overrides (hits before={before}, "
        f"after={after}); overrides appear to bypass the cache"
    )
    print(f"[cache] {case.id}: {case.cache} cache engaged with overrides — "
          f"hits: {before} -> {after}")


# ---------------------------------------------------------------------------
# Hull-White calibration and the IBOR index's fixing days
#
# QuantLib's SwaptionHelper starts each calibration swap the index's fixing
# days after expiry, so two requests differing only in those days calibrate to
# different parameters. The curves are moved onto a twin of the index, so
# their keys do not see the change: only the calibration key can tell the two
# requests apart.
# ---------------------------------------------------------------------------

HW_FIXING_DAYS_CASES = [
    c for c in CASES
    if c.id in ("calibrate_swaption_model", "swaption_bermudan_hw_calibrated")
]


def _with_ibor_fixing_days(request: dict, fixing_days: int, twin_suffix: str) -> dict:
    """The request with its curve helpers on a twin of the calibration's IBOR
    index and the index itself on `fixing_days`."""
    out = copy.deepcopy(request)
    rates = out["pricing"]["rates"]
    spec = next(m["payload"]["hw_calibration"]
                for m in out["pricing"]["volatility"]["models"]
                if "hw_calibration" in m.get("payload", {}))
    ibor_id = next(s["float_index_id"] for s in rates["swap_indices"]
                   if s["id"] == spec["swap_index_id"])
    ibor = next(i for i in rates["indices"] if i["id"] == ibor_id)
    twin = dict(copy.deepcopy(ibor), id=f"{ibor_id}_CURVE_{twin_suffix}")
    rates["indices"].append(twin)
    moved = 0
    for curve in rates["curves"]:
        for point in curve.get("points", []):
            ref = point.get("point", {}).get("float_index")
            if ref is not None and ref.get("id") == ibor_id:
                ref["id"] = twin["id"]
                moved += 1
    assert moved, f"no curve helper references {ibor_id}"
    ibor["fixing_days"] = fixing_days
    return out


def _hw_sigma(response: dict):
    if "hw_sigma" in response:
        return response["hw_sigma"]
    return response.get("swaptions", [{}])[0].get("used_hw_sigma")


@pytest.mark.parametrize("case", HW_FIXING_DAYS_CASES,
                         ids=[c.id for c in HW_FIXING_DAYS_CASES])
def test_hw_calib_ibor_fixing_days_not_served_from_another_entry(
        case, nocache_client, cache_client, data_dir, cache_log_path):
    """Calibrate with the IBOR index on 2 fixing days, then on 0: the second
    answer must be the cache-OFF one for 0 days, not the entry cached for 2.
    The twin index is named per case, so each case starts with nothing
    cached."""
    base = load_json(data_dir / case.filename)
    two = _with_ibor_fixing_days(base, 2, case.id)
    zero = _with_ibor_fixing_days(base, 0, case.id)

    off_two = nocache_client.price(case.product, two)
    off_zero = nocache_client.price(case.product, zero)
    assert _canonical(off_two) != _canonical(off_zero), (
        f"{case.id}: the fixing days do not change the cache-OFF response, so "
        f"this case cannot detect a stale cache entry"
    )

    cache_client.price(case.product, two)               # populate
    before = _count_hits(cache_log_path, "hw")
    warm_two = cache_client.price(case.product, two)    # must hit
    assert _canonical(warm_two) == _canonical(off_two)
    after = before
    for _ in range(20):
        after = _count_hits(cache_log_path, "hw")
        if after > before:
            break
        time.sleep(0.1)
    assert after > before, (
        f"{case.id}: the repeated 2-day request logged no HwCalibCache hit, so "
        f"the calibration is not cached and this case proves nothing"
    )

    got = cache_client.price(case.product, zero)
    assert _canonical(got) == _canonical(off_zero), (
        f"{case.id}: the 0-day request was served the calibration cached for "
        f"2 days (sigma {_hw_sigma(got)}; cache-OFF {_hw_sigma(off_zero)}, "
        f"2 days {_hw_sigma(off_two)})"
    )
    print(f"[cache] {case.id}: sigma 2 days {_hw_sigma(off_two)}, "
          f"0 days {_hw_sigma(off_zero)}; cache-ON == cache-OFF for both")



# ---------------------------------------------------------------------------
# Cross-currency basis pillars
# ---------------------------------------------------------------------------

XCCY_CASE = next(c for c in CASES if c.id == "bootstrap_curves_xccy")
XCCY_CURVE_ID = "EUR_USD_COLLATERAL"


def _xccy_request(data_dir, basis_only=False, spot_rate=None, spread_2y=None):
    request = load_json(data_dir / XCCY_CASE.filename)
    curve = next(c for c in request["pricing"]["rates"]["curves"] if c["id"] == XCCY_CURVE_ID)
    if basis_only:
        curve["points"] = [p for p in curve["points"]
                           if p["point_type"] == "CrossCcyBasisHelper"]
        request["queries"][0]["grid"]["grid"]["tenors"] = [
            t for t in request["queries"][0]["grid"]["grid"]["tenors"]
            if t["unit"] == "Years" and t["n"] >= 2]
    if spot_rate is not None:
        request["pricing"]["fx"]["spots"][0]["rate"] = spot_rate
    if spread_2y is not None:
        next(p for p in curve["points"]
             if p["point_type"] == "CrossCcyBasisHelper")["point"]["spread"] = spread_2y
    return request


def test_xccy_curve_cache_engaged(cache_client, data_dir, cache_log_path):
    """The collateral-adjusted curve itself hits the cache on a repeated
    request, so its transparency row cannot pass with the curve unkeyable."""
    request = _xccy_request(data_dir)
    before = _count_curve_hits(cache_log_path, XCCY_CURVE_ID)
    cache_client.price(XCCY_CASE.product, request)
    cache_client.price(XCCY_CASE.product, request)
    after = _wait_for_hits(cache_log_path, XCCY_CURVE_ID, before)
    assert after > before, (
        f"No CurveCache L1 hit for curve={XCCY_CURVE_ID} logged in {cache_log_path} "
        f"(hits before={before}, after={after})"
    )


def test_xccy_basis_spread_change_is_a_cache_miss(nocache_client, cache_client, data_dir):
    """One basis spread moved: the cached curve must not be served, and the
    cache-ON answer equals the cache-OFF answer for the new spread."""
    plain = _xccy_request(data_dir)
    moved = _xccy_request(data_dir, spread_2y=-0.0020)
    off_plain = nocache_client.price(XCCY_CASE.product, plain)
    off_moved = nocache_client.price(XCCY_CASE.product, moved)
    assert _canonical(off_plain) != _canonical(off_moved)

    cache_client.price(XCCY_CASE.product, plain)
    assert _canonical(cache_client.price(XCCY_CASE.product, plain)) == _canonical(off_plain)
    got = cache_client.price(XCCY_CASE.product, moved)
    assert _canonical(got) == _canonical(off_moved), (
        "cache-ON response for the moved basis differs from cache-OFF"
    )
    assert _canonical(cache_client.price(XCCY_CASE.product, plain)) == _canonical(off_plain)


def test_xccy_spot_rate_change_misses_but_the_curve_is_unchanged(
        nocache_client, cache_client, data_dir, cache_log_path):
    """Constant notional: the spot rate does not enter a basis-only
    bootstrap, so the cache-OFF curves for two spots are identical; the
    spot's full content is nevertheless part of the key (the pair and lag it
    carries are), so the second spot is a MISS (no L1 hit logged) that
    rebuilds the same curve. Both facts are pinned here on purpose."""
    plain = _xccy_request(data_dir, basis_only=True)
    moved = _xccy_request(data_dir, basis_only=True, spot_rate=1.25)
    off_plain = nocache_client.price(XCCY_CASE.product, plain)
    off_moved = nocache_client.price(XCCY_CASE.product, moved)
    assert _canonical(off_plain) == _canonical(off_moved), (
        "a basis-only curve must not depend on the spot rate"
    )

    cache_client.price(XCCY_CASE.product, plain)            # populate
    cache_client.price(XCCY_CASE.product, plain)            # hit
    before = _count_curve_hits(cache_log_path, XCCY_CURVE_ID)
    got = cache_client.price(XCCY_CASE.product, moved)      # different key: miss
    assert _canonical(got) == _canonical(off_moved)
    time.sleep(0.3)
    after = _count_curve_hits(cache_log_path, XCCY_CURVE_ID)
    assert after == before, (
        f"the moved-spot request was served from the cache ({after - before} L1 hit(s)); "
        f"the spot's content must be part of the curve's key"
    )
    # The old entry is still there and still correct.
    assert _canonical(cache_client.price(XCCY_CASE.product, plain)) == _canonical(off_plain)


# ---------------------------------------------------------------------------
# FX forward: the spot is request data, the curves are cached
# ---------------------------------------------------------------------------
FX_FORWARD_CASE = next(c for c in CASES if c.id == "fx_forward_eurusd_6m")
FX_FORWARD_PURE_RATES_CURVES = ("USD_SOFR_OIS", "EUR_ESTR_OIS")


def _fx_forward_request(data_dir, spot_rate=None):
    request = load_json(data_dir / FX_FORWARD_CASE.filename)
    if spot_rate is not None:
        request["pricing"]["fx"]["spots"][0]["rate"] = spot_rate
    return request


def test_fx_forward_spot_change_changes_npv_but_not_the_pure_rates_curves(
        nocache_client, cache_client, data_dir, cache_log_path):
    """Moving the FX spot in a priced request must change the forward's NPV
    (the spot enters the pricing directly and through the FX-implied EUR
    curve, which misses) while the pure-rates curves of the same request
    (USD SOFR OIS, EUR ESTR OIS) keep hitting the cache: the spot is no
    part of their keys. Cache-ON equals cache-OFF for the moved spot."""
    plain = _fx_forward_request(data_dir)
    moved = _fx_forward_request(data_dir, spot_rate=1.0950)
    off_plain = nocache_client.price(FX_FORWARD_CASE.product, plain)
    off_moved = nocache_client.price(FX_FORWARD_CASE.product, moved)
    npv_plain = off_plain["fx_forwards"][0]["npv"]
    npv_moved = off_moved["fx_forwards"][0]["npv"]
    assert npv_moved != npv_plain, "a spot change must move the forward's NPV"
    assert off_moved["fx_forwards"][0]["fx_spot_used"] == 1.0950

    cache_client.price(FX_FORWARD_CASE.product, plain)       # populate
    cache_client.price(FX_FORWARD_CASE.product, plain)       # all three curves hit
    before = {c: _count_curve_hits(cache_log_path, c) for c in FX_FORWARD_PURE_RATES_CURVES}
    before_fx = _count_curve_hits(cache_log_path, FX_CURVE_ID)
    got = cache_client.price(FX_FORWARD_CASE.product, moved)  # rates curves hit, FX curve misses
    assert _canonical(got) == _canonical(off_moved), (
        "cache-ON response for the moved spot differs from cache-OFF")
    for curve in FX_FORWARD_PURE_RATES_CURVES:
        after = _wait_for_hits(cache_log_path, curve, before[curve])
        assert after > before[curve], (
            f"the pure-rates curve {curve} did not hit the cache after an FX spot "
            f"change (hits {before[curve]} -> {after}); the spot must not take part in "
            f"its key")
    time.sleep(0.3)
    assert _count_curve_hits(cache_log_path, FX_CURVE_ID) == before_fx, (
        "the FX-implied EUR curve was served from the cache although the spot moved")
    # The original entries still answer the original request.
    assert _canonical(cache_client.price(FX_FORWARD_CASE.product, plain)) == _canonical(off_plain)


# ---------------------------------------------------------------------------
# Cross-currency swap: the spot is request data, the curves are cached
# ---------------------------------------------------------------------------
XCCY_SWAP_CASE = next(c for c in CASES if c.id == "xccy_eur_fixed_vs_usd_sofr_5y")

_CURVE_KEY_LINE = re.compile(r"\[CurveCache\] curve=(?P<curve>\S+) key=(?P<key>\S+) ")


def _curve_keys(log_path: Path, curve_id: str, start_line: int = 0) -> set:
    """Set of cache keys the server logged for `curve_id` from `start_line`
    on (every event: hit, miss, store)."""
    if not log_path.exists():
        return set()
    keys = set()
    for line in log_path.read_text(errors="replace").splitlines()[start_line:]:
        m = _CURVE_KEY_LINE.search(line)
        if m and m.group("curve") == curve_id:
            keys.add(m.group("key"))
    return keys


def _log_lines(log_path: Path) -> int:
    return len(log_path.read_text(errors="replace").splitlines()) if log_path.exists() else 0


def test_xccy_spot_change_changes_npv_but_not_the_pure_rates_curves(
        nocache_client, cache_client, data_dir, cache_log_path):
    """Moving the FX spot re-values the swap's notional exchanges and the
    FX-implied EUR curve, whose key carries the spot: the moved request must
    look the curve up under a key the plain request never used, while the
    pure-rates curves (USD SOFR OIS, EUR ESTR OIS) keep their keys and hit.
    Keys, not hit counts, are pinned: the moved spot's entry may already be
    in the cache from an earlier request of the same market (the FX forward
    test moves the same spot), in which case the moved request legitimately
    hits it. Cache-ON equals cache-OFF for the moved spot and the original
    entries still answer the original request."""
    plain = load_json(data_dir / XCCY_SWAP_CASE.filename)
    moved = copy.deepcopy(plain)
    moved["pricing"]["fx"]["spots"][0]["rate"] = 1.0950
    off_plain = nocache_client.price(XCCY_SWAP_CASE.product, plain)
    off_moved = nocache_client.price(XCCY_SWAP_CASE.product, moved)
    assert off_moved["swaps"][0]["npv"] != off_plain["swaps"][0]["npv"], (
        "a spot change must move the swap's NPV")
    assert off_moved["swaps"][0]["fx_spot_used"] == 1.0950

    first = _log_lines(cache_log_path)
    cache_client.price(XCCY_SWAP_CASE.product, plain)       # populate
    cache_client.price(XCCY_SWAP_CASE.product, plain)       # all three curves hit
    time.sleep(0.5)
    plain_keys = {c: _curve_keys(cache_log_path, c, first)
                  for c in (FX_CURVE_ID,) + FX_FORWARD_PURE_RATES_CURVES}
    for curve, keys in plain_keys.items():
        assert len(keys) == 1, f"{curve}: the plain request used keys {keys}"
    before = {c: _count_curve_hits(cache_log_path, c) for c in FX_FORWARD_PURE_RATES_CURVES}

    mark = _log_lines(cache_log_path)
    got = cache_client.price(XCCY_SWAP_CASE.product, moved)  # rates curves hit, FX curve re-keyed
    assert _canonical(got) == _canonical(off_moved), (
        "cache-ON response for the moved spot differs from cache-OFF")
    for curve in FX_FORWARD_PURE_RATES_CURVES:
        after = _wait_for_hits(cache_log_path, curve, before[curve])
        assert after > before[curve], (
            f"the pure-rates curve {curve} did not hit the cache after an FX spot "
            f"change (hits {before[curve]} -> {after})")
    time.sleep(0.5)
    moved_fx_keys = _curve_keys(cache_log_path, FX_CURVE_ID, mark)
    assert moved_fx_keys and not (moved_fx_keys & plain_keys[FX_CURVE_ID]), (
        f"the FX-implied EUR curve kept its key after the spot moved: "
        f"{moved_fx_keys} vs {plain_keys[FX_CURVE_ID]}")
    for curve in FX_FORWARD_PURE_RATES_CURVES:
        assert _curve_keys(cache_log_path, curve, mark) == plain_keys[curve], (
            f"the pure-rates curve {curve} changed key on an FX spot move")
    # The original entries still answer the original request.
    assert _canonical(cache_client.price(XCCY_SWAP_CASE.product, plain)) == _canonical(off_plain)


# ---------------------------------------------------------------------------
# Curves bootstrapped from rate helpers
#
# QuantLib's tenor rate helpers re-date themselves on every evaluation-date
# change, with the calendars in force at that moment. The cache therefore
# holds a copy of each bootstrapped curve's nodes on the same interpolated
# curve, without the helpers: the copy gives the bootstrapped curve's numbers,
# and no later request can re-date or re-bootstrap it.
# ---------------------------------------------------------------------------

EUR_CURVE_FIXTURE = "curves/curves_eur_depo_swap_all_measures_tenor_grid.json"
TRAITS = ("Discount", "ZeroRate", "FwdRate")
INTERPOLATORS = ("Linear", "LogLinear", "LogCubic", "BackwardFlat", "ForwardFlat")
# QuantLib cannot bootstrap these on that curve: a log interpolator cannot
# take the -1 end of the bracket for a zero or forward rate, a backward-flat
# discount curve gives the spot-starting first deposit no rate, and a
# forward-flat forward does not reach its own pillar.
NOT_BOOTSTRAPPABLE = {
    ("ZeroRate", "LogLinear"), ("ZeroRate", "LogCubic"),
    ("FwdRate", "LogLinear"), ("FwdRate", "LogCubic"),
    ("Discount", "BackwardFlat"), ("FwdRate", "ForwardFlat"),
}
# A request at another as-of moves the evaluation date twice: to its own
# as-of, then back to the worker's resting date.
UNRELATED_CASE = CASES[0]  # fixed-rate bond, as-of 2008-09-15


def _eur_curve_request(data_dir, trait, interpolator, as_of=None):
    """The EUR deposit/swap curve example with the given bootstrap trait and
    interpolator, optionally moved to another as-of (reference date too)."""
    request = load_json(data_dir / EUR_CURVE_FIXTURE)
    curve = request["pricing"]["rates"]["curves"][0]
    curve["bootstrap_trait"] = trait
    curve["interpolator"] = interpolator
    if as_of is not None:
        request["pricing"]["as_of_date"] = as_of
        curve["reference_date"] = as_of
    return request


def _price_unrelated_request(cache_client, data_dir):
    cache_client.price(UNRELATED_CASE.product, load_json(data_dir / UNRELATED_CASE.filename))


@pytest.mark.parametrize(
    "trait,interpolator",
    [(t, i) for t in TRAITS for i in INTERPOLATORS],
    ids=[f"{t}-{i}" for t in TRAITS for i in INTERPOLATORS],
)
def test_bootstrapped_curve_cache_transparency(trait, interpolator, nocache_client,
                                               cache_client, data_dir):
    """Every bootstrap trait and interpolator: discount factors, zero and
    forward rates sampled weekly to a year past the last pillar are identical
    cache-OFF, on the miss and on the hit; a curve QuantLib cannot bootstrap
    fails identically."""
    request = _eur_curve_request(data_dir, trait, interpolator)
    request["queries"][0]["grid"] = {
        "grid_type": "RangeGrid",
        "grid": {"start_date": "2025-01-15", "end_date": "2036-01-15",
                 "step_number": 7, "step_time_unit": "Days"},
    }

    off = _post_raw(nocache_client, "bootstrap_curves", request)
    warm = _post_raw(cache_client, "bootstrap_curves", request)
    hit = _post_raw(cache_client, "bootstrap_curves", request)

    assert off[0] == 200, f"{trait}/{interpolator}: HTTP {off[0]} :: {off[1][:300]}"
    error = json.loads(off[1])["results"][0].get("error")
    expect_error = (trait, interpolator) in NOT_BOOTSTRAPPABLE
    assert bool(error) == expect_error, f"{trait}/{interpolator}: error {error!r}"
    assert warm == off, f"{trait}/{interpolator}: the miss differs from cache-OFF"
    assert hit == off, f"{trait}/{interpolator}: the hit differs from cache-OFF"


def test_bootstrapped_curve_unchanged_by_a_request_at_another_as_of(
        nocache_client, cache_client, data_dir):
    """A ZeroRate curve, then a request at another as-of, then the curve
    again: the same numbers, equal to cache-OFF."""
    request = _eur_curve_request(data_dir, "ZeroRate", "Linear", as_of="2025-02-03")

    off = nocache_client.price("bootstrap_curves", request)
    first = cache_client.price("bootstrap_curves", request)
    _price_unrelated_request(cache_client, data_dir)
    again = cache_client.price("bootstrap_curves", request)

    assert _canonical(first) == _canonical(off), "the miss differs from cache-OFF"
    assert _canonical(again) == _canonical(off), (
        "a request at another as-of changed the cached ZeroRate curve")


def test_bootstrapped_curve_with_calendar_overrides_unchanged_at_the_resting_date(
        nocache_client, cache_client, data_dir):
    """The same with holiday overrides on the curve's calendar, at the
    worker's resting date (today: the evaluation date is unset between
    requests). A request there does not move the evaluation date, so helpers
    that another request re-dated without the overrides would stay so."""
    today = datetime.date.today()
    plain = _eur_curve_request(data_dir, "ZeroRate", "Linear", as_of=today.isoformat())
    request = copy.deepcopy(plain)
    request["pricing"]["calendar_overrides"] = [{
        "calendar": "TARGET",
        "added_holidays": [(today + datetime.timedelta(days=k)).isoformat()
                           for k in range(1, 15)],
    }]

    off = nocache_client.price("bootstrap_curves", request)
    assert _canonical(off) != _canonical(nocache_client.price("bootstrap_curves", plain)), (
        "the overrides do not move the curve, so this case cannot detect a re-dated one")

    first = cache_client.price("bootstrap_curves", request)
    _price_unrelated_request(cache_client, data_dir)
    again = cache_client.price("bootstrap_curves", request)

    assert _canonical(first) == _canonical(off), "the miss differs from cache-OFF"
    assert _canonical(again) == _canonical(off), (
        "a request at another as-of changed the cached curve built with overrides")


LOCKOUT_FIXTURE = "curves/curves_usd_ois_sofr_lookback2_lockout2_df_zero.json"
LOCKOUT_CURVE_ID = "USD_SOFR_OIS"


def _lockout_curve_request(data_dir, as_of):
    """The SOFR OIS example as a linear ZeroRate curve whose first pillar is
    a one-week OIS with a four-day lockout, which needs five fixings in the
    week."""
    request = load_json(data_dir / LOCKOUT_FIXTURE)
    request["pricing"]["as_of_date"] = as_of
    curve = request["pricing"]["rates"]["curves"][0]
    curve["reference_date"] = as_of
    curve["bootstrap_trait"] = "ZeroRate"
    curve["interpolator"] = "Linear"
    first = curve["points"][0]["point"]
    first["tenor"] = {"n": 1, "unit": "Weeks"}
    first["lockout_days"] = 4
    return request


def test_cached_curve_does_not_fail_a_request_at_another_as_of(
        nocache_client, cache_client, data_dir, cache_log_path):
    """A request at as-of 2025-01-15 answers the same before and after the
    cache holds a curve that cannot be rebuilt at that date: from 2025-01-15
    the curve's first week holds Martin Luther King Day, so its one-week OIS
    has four fixings, too few for the lockout; from 2025-01-22 it has five."""
    lockout = _lockout_curve_request(data_dir, "2025-01-22")
    status, body = _post_raw(nocache_client, "bootstrap_curves", lockout)
    assert status == 200 and not json.loads(body)["results"][0].get("error"), body[:300]
    status, body = _post_raw(nocache_client, "bootstrap_curves",
                             _lockout_curve_request(data_dir, "2025-01-15"))
    assert "Lockout" in body, (
        f"the lockout curve should not build at 2025-01-15 :: HTTP {status} {body[:300]}")

    unrelated = load_json(data_dir / EUR_CURVE_FIXTURE)  # as-of 2025-01-15
    off = _post_raw(nocache_client, "bootstrap_curves", unrelated)
    before = _post_raw(cache_client, "bootstrap_curves", unrelated)

    hits = _count_curve_hits(cache_log_path, LOCKOUT_CURVE_ID)
    cache_client.price("bootstrap_curves", lockout)
    cache_client.price("bootstrap_curves", lockout)
    assert _wait_for_hits(cache_log_path, LOCKOUT_CURVE_ID, hits) > hits, (
        "the lockout curve did not engage the cache, so this case proves nothing")

    after = _post_raw(cache_client, "bootstrap_curves", unrelated)
    assert before == off, f"cache-ON differs from cache-OFF :: {before[1][:300]}"
    assert after == off, (
        f"a cached curve changed a request at another as-of: HTTP {after[0]} :: "
        f"{after[1][:300]}")
