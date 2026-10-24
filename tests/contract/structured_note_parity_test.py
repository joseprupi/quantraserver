"""Structured notes: Python QuantLib parity on the SAME random stream
same-stream parity, convergence and the RNG conventions.

`POST /price-structured-note` prices a note by Monte Carlo; the reference
`ql_reference.structured_note_ql` rebuilds the market (curves, equity
underlyings, Black vols, FX spots and vols, quanto drift, correlation
sub-matrix, funding spread), the time grid and the payoff walk from the
request JSON and drives it with the engine's own random sequence
(SobolBrownianBridgeRsg / Sobol JoeKuoD7 / Mersenne Twister, time-major,
antithetic = the negated draws). The two sides therefore agree bit for bit
up to summation order; the only noise is the 12 decimal places the JSON
gateway prints doubles with (absolute 5e-13), which is why the comparison
is `rel 1e-9 + abs 1e-11`. A quantity that needed more than that would be a
real difference (stream, ordering, antithetic rule, discounting, grid),
never something to loosen.

Cases:
  * the two factsheet examples (seasoned, 4 assets, 3 quanto, OIS discount
    curve + funding spread, 65 536 Sobol + bridge antithetic paths);
  * synthetic two assets rho = 0.7 worst-of with flat vols (also an example
    file and a catalog case), a quanto EUR asset next to a USD one, a
    term-vol asset (BlackVarianceCurve, exact steps) next to a flat-vol one,
    all 1024 paths Sobol + bridge;
  * a strike x expiry surface next to a flat vol on a fine grid (Dupire
    local vol, log-Euler, steps_per_year 252, 256 paths): the same QuantLib
    `evolve` on both sides, so it is bit-level too;
  * Phoenix coupons (pay_only_on_call = false) with a DailyClose barrier on
    a 52-steps-per-year grid;
  * pseudo-random (Mersenne Twister) without the bridge, and Sobol without
    bridge and without antithetic.
Plus: the Sobol seed does not change the stream at these dimensions (the
reference relies on it), and the convergence test (standard error scales
as 1 / sqrt(paths) within 25 %, the price moves by less than 4 standard
errors).

Diagnostics: with the request's `diagnostics` flags
the reference also returns the `market`, `decomposition`, `checks` and
`distribution` blocks, computed from their definitions in the response
schema doc comments on the same paths: forwards, implied yields and rates,
ATM and barrier vols, the quanto drift in bp, discount factors, per-event
PVs, survival and breach probabilities, the expected loss given breach, the
running means with their standard errors, the deterministic forward walk,
the bond-only price, the correlation-one and correlation-zero limit runs
(the same stream again on the all-ones and the identity matrix), the
quantiles by linear interpolation between order statistics over the
individual paths, the 20-bin payout histogram and the first sample paths.
Every numeric field of the four blocks is compared to the print floor,
histogram counts and sample paths exactly; `timing` is only checked for
its presence and stage names (wall-clock values are not reproducible).

Every synthetic basket but the single-underlying example has two
underlyings; a single-underlying note carries no `correlation_id` and is
simulated on the 1 x 1 unit matrix.
"""

import copy
import json
import math

import pytest

from ql_reference import load_json, structured_note_ql

BNP = "structured_note/bnp_quad_index_classic_autocall_xs2741509801.json"
SANTANDER = "structured_note/santander_quad_index_low_hurdle_autocall_xs3109492564.json"
TWO_ASSET_FILE = "structured_note/synthetic_two_asset_worst_of_flat_vol_autocall.json"

AS_OF = "2026-10-21"
REL_TOL = 1e-9
ABS_TOL = 1e-11  # the gateway prints 12 decimal places


# ---------------------------------------------------------------------------
# Synthetic requests
# ---------------------------------------------------------------------------

def _flat_curve(curve_id, rate, calendar="TARGET"):
    return {
        "id": curve_id,
        "day_counter": "Actual365Fixed",
        "interpolator": "Linear",
        "reference_date": AS_OF,
        "bootstrap_trait": "InterpolatedZero",
        "points": [
            {"point_type": "ZeroRatePoint",
             "point": {"zero_rate": rate, "calendar": calendar,
                       "business_day_convention": "ModifiedFollowing",
                       "compounding": "Continuous", "frequency": "Annual",
                       "date": d}}
            for d in (AS_OF, "2036-10-21")
        ],
    }


def _vol_base(calendar, shape="Constant"):
    return {"reference_date": AS_OF, "calendar": calendar,
            "business_day_convention": "ModifiedFollowing",
            "day_counter": "Actual365Fixed", "shape": shape}


def _constant_vol(vol_id, vol, calendar="TARGET"):
    base = _vol_base(calendar)
    base["constant_vol"] = vol
    return {"id": vol_id, "payload_type": "BlackVolSpec", "payload": {"base": base}}


def _term_vol(vol_id, expiries, vols, calendar="TARGET"):
    return {"id": vol_id, "payload_type": "BlackVolSpec", "payload": {
        "base": _vol_base(calendar, "AtmMatrix2D"),
        "expiries": [{"n": n, "unit": unit} for n, unit in expiries],
        "term_vols": {"n_rows": len(vols), "n_cols": 1, "values": list(vols)},
    }}


def _surface_vol(vol_id, expiries, strikes, vols, calendar="TARGET"):
    rows = len(expiries)
    cols = len(strikes)
    assert len(vols) == rows * cols
    return {"id": vol_id, "payload_type": "BlackVolSpec", "payload": {
        "base": _vol_base(calendar, "SmileCube3D"),
        "expiries": [{"n": n, "unit": unit} for n, unit in expiries],
        "strikes": list(strikes),
        "surface_vols": {"n_rows": rows, "n_cols": cols, "values": list(vols)},
        "surface_interpolator": "Bilinear",
        "allow_extrapolation": True,
    }}


# (observation, payment) pairs: semi-annual from 6M, rolled by hand.
SEMI_ANNUAL = [
    ("2027-04-21", "2027-04-28"), ("2027-10-21", "2027-10-28"),
    ("2028-04-24", "2028-05-01"), ("2028-10-23", "2028-10-30"),
]
QUARTERLY_1Y = [
    ("2027-01-21", "2027-01-28"), ("2027-04-21", "2027-04-28"),
    ("2027-07-21", "2027-07-28"), ("2027-10-21", "2027-10-28"),
]


def _events(dates, coupon, trigger=1.0, coupon_trigger=None, final_coupon_trigger=None):
    out = []
    for i, (obs, pay) in enumerate(dates):
        ev = {"observation_date": obs, "payment_date": pay, "coupon": coupon}
        is_final = i + 1 == len(dates)
        if is_final and final_coupon_trigger is not None:
            ev["coupon_trigger"] = final_coupon_trigger
        else:
            ev["autocall_trigger"] = trigger
            if coupon_trigger is not None:
                ev["coupon_trigger"] = coupon_trigger
        out.append(ev)
    return out


def _synthetic(assets, events, monte_carlo, *, correlation=None, coupon_rule=None,
               capital=None, trade_id="SYNTHETIC", funding_bp=50.0):
    """A complete price-structured-note request on flat USD 4 % discounting.

    `assets` = [{id, currency, spot, initial, dividend, vol: <VolSurfaceSpec>,
    [fx_rate, fx_vol, rho, local_rate]}]: a non-USD asset gets a quanto block
    with its own flat local curve, FX spot and FX vol.
    """
    quotes, curves, vols, fx_spots, underlyings, basket = [], [_flat_curve("USD_FLAT", 0.04)], [], [], [], []
    for a in assets:
        quotes.append({"id": f"{a['id']}_SPOT", "kind": "Price", "value": a["spot"],
                       "quote_type": "Curve"})
        curves.append(_flat_curve(f"{a['id']}_DIV", a["dividend"]))
        vols.append(a["vol"])
        underlyings.append({"id": a["id"], "currency": a["currency"],
                            "spot_quote_id": f"{a['id']}_SPOT",
                            "dividend_yield_curve_id": f"{a['id']}_DIV"})
        u = {"underlying_id": a["id"], "volatility_id": a["vol"]["id"],
             "initial_level": a["initial"]}
        if a["currency"] != "USD":
            ccy = a["currency"]
            curves.append(_flat_curve(f"{ccy}_FLAT", a["local_rate"]))
            fx_spots.append({"id": f"{ccy}USD_SPOT", "base_currency": ccy,
                             "quote_currency": "USD", "rate": a["fx_rate"],
                             "spot_days": 2,
                             "settlement_calendars": ["TARGET", "UnitedStatesSettlement"],
                             "business_day_convention": "ModifiedFollowing"})
            vols.append(_constant_vol(f"{ccy}USD_VOL", a["fx_vol"]))
            u["quanto"] = {"discounting_curve": f"{ccy}_FLAT",
                           "fx_spot_id": f"{ccy}USD_SPOT",
                           "fx_volatility_id": f"{ccy}USD_VOL",
                           "fx_underlying_correlation": a["rho"]}
        basket.append(u)
    n = len(assets)
    if correlation is None:
        correlation = [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]
    pricing = {
        "as_of_date": AS_OF,
        "quotes": quotes,
        "rates": {"curves": curves},
        "volatility": {"vol_surfaces": vols},
        "equity": {"equity_underlyings": underlyings},
        "correlations": {"matrices": [{
            "id": "BASKET_CORR", "labels": [a["id"] for a in assets],
            "values": {"n_rows": n, "n_cols": n,
                       "values": [v for row in correlation for v in row]}}]},
    }
    if fx_spots:
        pricing["fx"] = {"spots": fx_spots}
    note = {
        "trade_id": trade_id,
        "currency": "USD",
        "notional": 1000.0,
        "underlyings": basket,
        "performance_rule": "WorstOf",
        "events": events,
        "coupon_rule": coupon_rule or {"memory": True, "pay_only_on_call": True},
        "capital": capital or {"barrier": 0.7, "monitoring": "European",
                               "put_strike": 1.0, "participation": 1.0},
        "maturity_payment_date": events[-1]["payment_date"],
        "discounting_curve": "USD_FLAT",
        "funding_spread_bp": funding_bp,
        "correlation_id": "BASKET_CORR",
        "monte_carlo": monte_carlo,
    }
    return {"pricing": pricing, "notes": [note]}


SOBOL_1024 = {"paths": 1024, "seed": 42, "sequence": "Sobol", "antithetic": True,
              "brownian_bridge": True}


def _asset(asset_id, vol_spec, *, currency="USD", spot=100.0, initial=100.0,
           dividend=0.02, **quanto):
    a = {"id": asset_id, "currency": currency, "spot": spot, "initial": initial,
         "dividend": dividend, "vol": vol_spec}
    a.update(quanto)
    return a


def _usd_partner(asset_id="IDX_B", vol=0.30):
    """The flat-vol USD index every synthetic basket pairs its asset under
    test with (a note needs a correlation matrix of at least two labels)."""
    return _asset(asset_id, _constant_vol(f"{asset_id}_VOL", vol), spot=50.0, initial=50.0,
                  dividend=0.01)


def two_asset_request():
    """Two USD indices (flat 25 % and 30 % vols, rho = 0.7), worst-of, four
    semi-annual 100 % autocalls paying 5 % snowball coupons, 70 % European
    barrier: the catalog's synthetic case (examples/data/structured_note/
    synthetic_two_asset_worst_of_flat_vol_autocall.json)."""
    return _synthetic(
        [_asset("IDX_A", _constant_vol("IDX_A_VOL", 0.25)), _usd_partner()],
        _events(SEMI_ANNUAL, 0.05), dict(SOBOL_1024),
        correlation=[[1.0, 0.7], [0.7, 1.0]], trade_id="SYNTHETIC_TWO_ASSETS_RHO_0_7")


def quanto_request():
    """A EUR index quanto'd into the USD note (rho(S, FX) = 0.3) next to a
    USD index, rho = 0.5."""
    return _synthetic(
        [_asset("IDX_EUR", _constant_vol("IDX_EUR_VOL", 0.22), currency="EUR",
                spot=4000.0, initial=4000.0, dividend=0.03, local_rate=0.025,
                fx_rate=1.1, fx_vol=0.10, rho=0.3),
         _usd_partner()],
        _events(SEMI_ANNUAL, 0.045), dict(SOBOL_1024),
        correlation=[[1.0, 0.5], [0.5, 1.0]], trade_id="SYNTHETIC_QUANTO_EUR")


def term_vol_request():
    """A term-vol index (BlackVarianceCurve 20/22/25/27 % at 6M/1Y/2Y/3Y,
    exact lognormal steps) next to a flat-vol one, rho = 0.6."""
    return _synthetic(
        [_asset("IDX", _term_vol("IDX_TERM_VOL",
                                 [(6, "Months"), (1, "Years"), (2, "Years"), (3, "Years")],
                                 [0.20, 0.22, 0.25, 0.27])),
         _usd_partner()],
        _events(SEMI_ANNUAL, 0.04), dict(SOBOL_1024),
        correlation=[[1.0, 0.6], [0.6, 1.0]], trade_id="SYNTHETIC_TERM_VOL")


def surface_vol_request():
    """A strike x expiry surface (mild skew, wide strike grid so the paths
    stay inside it) next to a flat vol, rho = 0.4, two observations over one
    year on a 252-steps-per-year grid."""
    strikes = [20.0, 60.0, 100.0, 140.0, 180.0, 300.0]
    expiries = [(6, "Months"), (1, "Years"), (2, "Years")]
    rows = [[0.30, 0.27, 0.25, 0.235, 0.225, 0.21],
            [0.29, 0.265, 0.25, 0.24, 0.23, 0.215],
            [0.28, 0.26, 0.25, 0.245, 0.24, 0.22]]
    mc = {"paths": 256, "seed": 42, "sequence": "Sobol", "antithetic": True,
          "brownian_bridge": True, "steps_per_year": 252}
    return _synthetic(
        [_asset("IDX", _surface_vol("IDX_SURF_VOL", expiries, strikes,
                                    [v for row in rows for v in row])),
         _usd_partner()],
        _events(QUARTERLY_1Y[1::2], 0.03), mc,
        correlation=[[1.0, 0.4], [0.4, 1.0]], trade_id="SYNTHETIC_SURFACE_VOL")


def phoenix_daily_request():
    """Phoenix: 2 % quarterly coupon at 80 % paid when earned, 100 % call,
    memory on, 65 % barrier observed on the daily closes (52 steps/year)."""
    mc = {"paths": 512, "seed": 42, "sequence": "Sobol", "antithetic": True,
          "brownian_bridge": True, "steps_per_year": 52}
    return _synthetic(
        [_asset("IDX_A", _constant_vol("IDX_A_VOL", 0.28)),
         _asset("IDX_B", _constant_vol("IDX_B_VOL", 0.24), spot=200.0, initial=200.0)],
        _events(QUARTERLY_1Y, 0.02, coupon_trigger=0.8), mc,
        correlation=[[1.0, 0.5], [0.5, 1.0]],
        coupon_rule={"memory": True, "pay_only_on_call": False},
        capital={"barrier": 0.65, "monitoring": "DailyClose", "put_strike": 1.0,
                 "participation": 1.0},
        trade_id="SYNTHETIC_PHOENIX_DAILY")


def pseudo_random_request():
    req = two_asset_request()
    req["notes"][0]["monte_carlo"] = {"paths": 1024, "seed": 42, "sequence": "PseudoRandom",
                                      "antithetic": True, "brownian_bridge": False}
    req["notes"][0]["trade_id"] = "SYNTHETIC_PSEUDO_RANDOM"
    return req


def sobol_no_bridge_request():
    req = two_asset_request()
    req["notes"][0]["monte_carlo"] = {"paths": 1024, "seed": 42, "sequence": "Sobol",
                                      "antithetic": False, "brownian_bridge": False}
    req["notes"][0]["trade_id"] = "SYNTHETIC_SOBOL_NO_BRIDGE"
    return req


def three_asset_repair_request():
    """Three USD indices on a singular correlation matrix (det = 1 + 2 x 0.9
    x 0.9 x 0.62 - 0.81 - 0.81 - 0.3844 = 0, accepted as positive
    semi-definite): the +0.01 correlation bump of the Greeks makes it
    indefinite, so the engine repairs it (eigenvalue clipping, unit
    diagonal) and reports the matrix used."""
    req = _synthetic(
        [_asset("IDX_A", _constant_vol("IDX_A_VOL", 0.25)),
         _usd_partner(),
         _asset("IDX_C", _constant_vol("IDX_C_VOL", 0.20), spot=200.0, initial=200.0,
                dividend=0.015)],
        _events(SEMI_ANNUAL, 0.05), dict(SOBOL_1024),
        correlation=[[1.0, 0.9, 0.9], [0.9, 1.0, 0.62], [0.9, 0.62, 1.0]],
        trade_id="SYNTHETIC_THREE_ASSETS_SINGULAR")
    req["greeks"] = {"spot_bump": 0.01, "vol_bump": 0.01, "correlation_bump": 0.01,
                     "funding_bump_bp": 1.0, "rate_bump_bp": 1.0}
    return req


SYNTHETIC_CASES = {
    "quanto_eur": quanto_request,
    "term_vol": term_vol_request,
    "surface_vol_fine_grid": surface_vol_request,
    "phoenix_daily_barrier": phoenix_daily_request,
    "pseudo_random_no_bridge": pseudo_random_request,
    "sobol_no_bridge_no_antithetic": sobol_no_bridge_request,
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _post(client, request):
    endpoint = client.ENDPOINTS["structured_note"]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    assert r.status_code == 200, f"HTTP {r.status_code} :: {r.text[:400]}"
    return r.json()["notes"][0]


def _close(a, b):
    return abs(a - b) <= REL_TOL * max(abs(a), abs(b)) + ABS_TOL


def _compared_fields(note):
    """Every scalar the parity compares, as {name: value}."""
    out = {}
    for k in ("npv", "npv_pct_of_notional", "standard_error", "p_barrier_breach",
              "expected_life_years"):
        if k in note:
            out[k] = note[k]
    for bucket in ("bond_redemption", "coupons", "capital_loss"):
        if "legs" in note:
            out[f"legs.{bucket}.pv"] = note["legs"][bucket]["pv"]
            out[f"legs.{bucket}.standard_error"] = note["legs"][bucket]["standard_error"]
    for p in note["event_probabilities"]:
        for k in ("p_call", "p_coupon"):
            if k in p:
                out[f"event_probabilities[{p['index']}].{k}"] = p[k]
    out["seasoning.memory_carried"] = note["seasoning"]["memory_carried"]
    return out


def _rounding_level_standard_error(key, a, b, x, y):
    """A bucket whose per-path value is the same on every path (every coupon
    unconditional, say) has a true standard error of 0; what both sides
    report is the rounding residue of the sum-of-squares statistic, which
    depends on the summation order and is not a stream property. Treat two
    such residues (below 1e-8 of the bucket's PV on both sides) as equal.
    The PV sits next to the standard error as `pv` (buckets), `npv`
    (running means) or `<prefix>_npv` (correlation limits)."""
    if not key.endswith("standard_error"):
        return False
    stem = key[:-len("standard_error")]
    pv = abs(a.get(stem + "pv", a.get(stem + "npv", 0.0)))
    return abs(x) < 1e-8 * max(pv, 1.0) and abs(y) < 1e-8 * max(pv, 1.0)


def _assert_parity(api, ref):
    a, b = _compared_fields(api), _compared_fields(ref)
    assert set(a) == set(b), f"field sets differ: {sorted(set(a) ^ set(b))}"
    assert api["status"] == ref["status"] and api["paths"] == ref["paths"]
    worst = max(a, key=lambda k: abs(a[k] - b[k]) / max(abs(a[k]), abs(b[k]), 1e-300))
    gaps = {k: (a[k], b[k]) for k in a
            if not _close(a[k], b[k]) and not _rounding_level_standard_error(k, a, b, a[k], b[k])}
    assert not gaps, (f"not on the same stream: {gaps}; worst field {worst}: "
                      f"api={a[worst]!r} ql={b[worst]!r}")


# ---------------------------------------------------------------------------
# Parity on the same stream
# ---------------------------------------------------------------------------

SINGLE_UNDERLYING_FILE = "structured_note/single_underlying_reverse_convertible.json"
SCOTIA_FILE = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72.json"


@pytest.mark.parametrize("filename", [BNP, SANTANDER, TWO_ASSET_FILE, SINGLE_UNDERLYING_FILE,
                                      SCOTIA_FILE],
                         ids=["bnp_xs2741509801", "santander_xs3109492564",
                              "two_assets_rho_0_7", "single_underlying_reverse_convertible",
                              "scotia_us06419jme72_is_final"])
def test_example_parity(client, data_dir, filename):
    request = load_json(data_dir / filename)
    _assert_parity(_post(client, request), structured_note_ql(request))


@pytest.mark.parametrize("case", sorted(SYNTHETIC_CASES), ids=sorted(SYNTHETIC_CASES))
def test_synthetic_parity(client, case):
    request = SYNTHETIC_CASES[case]()
    api = _post(client, request)
    assert api["standard_error"] > 0.0 and 0.0 < api["npv"] < 1500.0
    _assert_parity(api, structured_note_ql(request))


def test_two_asset_example_file_is_the_builder_output(data_dir):
    """The committed synthetic example is exactly `two_asset_request()`."""
    assert load_json(data_dir / TWO_ASSET_FILE) == two_asset_request()


def test_model_echo_matches_the_reference_grid(client):
    """Fine grid: the echoed grid is the reference's rule (ceil(len * steps)
    per interval, Actual/365F) and the dimension is steps x assets."""
    request = surface_vol_request()
    api = _post(client, request)
    model = api["model"]
    assert model["observation_dates_only"] is False
    assert model["underlyings"][0]["local_vol"] is True
    assert model["underlyings"][0]["vol_shape"] == "SmileCube3D"
    t_obs = [(182.0 / 365.0), (365.0 / 365.0)]
    n1 = math.ceil(t_obs[0] * 252 - 1e-9)
    n2 = math.ceil((t_obs[1] - t_obs[0]) * 252 - 1e-9)
    assert (n1, n2) == (126, 127)  # ceil, so 253 steps over the year, not 252
    assert model["time_steps"] == n1 + n2
    assert model["sequence_dimension"] == 2 * (n1 + n2)
    assert model["underlyings"][1]["local_vol"] is False
    assert abs(model["time_grid"][n1] - t_obs[0]) < 1e-12
    assert abs(model["time_grid"][-1] - t_obs[1]) < 1e-12


def test_sobol_seed_does_not_change_the_stream(client):
    """QuantLib uses the Sobol seed only beyond the 1898 tabulated
    dimensions; at the dimensions of these notes every seed is the same
    stream, which is what lets the reference use the default seed."""
    base = two_asset_request()
    other = copy.deepcopy(base)
    other["notes"][0]["monte_carlo"]["seed"] = 7
    a, b = _post(client, base), _post(client, other)
    assert a["npv"] == b["npv"] and a["standard_error"] == b["standard_error"]
    assert a["seed"] == 42 and b["seed"] == 7


def test_pseudo_random_seed_changes_the_price(client):
    base = pseudo_random_request()
    other = copy.deepcopy(base)
    other["notes"][0]["monte_carlo"]["seed"] = 7
    a, b = _post(client, base), _post(client, other)
    assert a["npv"] != b["npv"]
    assert abs(a["npv"] - b["npv"]) < 4.0 * (a["standard_error"] + b["standard_error"])


# ---------------------------------------------------------------------------
# Diagnostics parity on the same stream
# ---------------------------------------------------------------------------

BNP_DIAGNOSTICS_FILE = \
    "structured_note/bnp_quad_index_classic_autocall_xs2741509801_diagnostics.json"
DIAGNOSTIC_BLOCKS = ("market", "decomposition", "checks", "distribution")
ALL_DIAGNOSTICS = {"market": True, "decomposition": True, "checks": True,
                   "distribution": True, "timing": True}
# Fields that are counts or indices: equal exactly (histogram bin counts,
# running-mean path counts, event indices).
EXACT_SUFFIXES = (".paths", ".index")


def _with_diagnostics(request, sample_paths):
    req = copy.deepcopy(request)
    req["diagnostics"] = dict(ALL_DIAGNOSTICS, sample_paths=sample_paths)
    return req


DIAGNOSTIC_CASES = {
    "bnp_xs2741509801_diagnostics_example":
        lambda data_dir: load_json(data_dir / BNP_DIAGNOSTICS_FILE),
    "single_underlying_all_blocks":
        lambda data_dir: _with_diagnostics(load_json(data_dir / SINGLE_UNDERLYING_FILE), 5),
    "two_assets_rho_0_7_all_blocks":
        lambda data_dir: _with_diagnostics(two_asset_request(), 8),
    "phoenix_daily_barrier_all_blocks":
        lambda data_dir: _with_diagnostics(phoenix_daily_request(), 8),
}


def _flatten(prefix, value, out):
    if isinstance(value, dict):
        for k, v in value.items():
            _flatten(f"{prefix}.{k}" if prefix else k, v, out)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            _flatten(f"{prefix}[{i}]", v, out)
    else:
        out[prefix] = value


def _diagnostic_fields(note):
    """Every leaf of the four diagnostic blocks as {dotted path: value}."""
    out = {}
    for block in DIAGNOSTIC_BLOCKS:
        assert block in note, f"{block} block missing"
        _flatten(block, note[block], out)
    return out


def _assert_diagnostics_parity(api, ref):
    a, b = _diagnostic_fields(api), _diagnostic_fields(ref)
    assert set(a) == set(b), f"field sets differ: {sorted(set(a) ^ set(b))[:40]}"
    gaps = {}
    worst = (0.0, None)
    for k in a:
        x, y = a[k], b[k]
        if isinstance(x, bool) or isinstance(y, bool) or isinstance(x, str) or isinstance(y, str):
            if x != y:
                gaps[k] = (x, y)
            continue
        if k.endswith(EXACT_SUFFIXES):
            if x != y:
                gaps[k] = (x, y)
            continue
        rel = abs(x - y) / max(abs(x), abs(y), 1e-300)
        if rel > worst[0]:
            worst = (rel, k)
        if not _close(x, y) and not _rounding_level_standard_error(k, a, b, x, y):
            gaps[k] = (x, y)
    assert not gaps, (f"diagnostics not on the same stream ({len(gaps)} of {len(a)} fields): "
                      f"{dict(list(gaps.items())[:12])}; worst relative gap {worst[0]:.3e} "
                      f"at {worst[1]}")


@pytest.mark.parametrize("case", list(DIAGNOSTIC_CASES), ids=list(DIAGNOSTIC_CASES))
def test_diagnostics_parity(client, data_dir, case):
    """market / decomposition / checks / distribution: every numeric field
    to the print floor, counts and sample paths exactly; timing present with
    the engine's stage names (two correlation stages only with two or more
    underlyings)."""
    request = DIAGNOSTIC_CASES[case](data_dir)
    api = _post(client, request)
    ref = structured_note_ql(request)
    _assert_parity(api, ref)
    _assert_diagnostics_parity(api, ref)
    n_u = len(request["notes"][0]["underlyings"])
    stages = [s["stage"] for s in api["timing"]["stages"]]
    expected = ["market_build", "simulation", "deterministic_forward"]
    if n_u > 1:
        expected += ["correlation_one", "correlation_zero"]
    expected.append("diagnostics")
    assert stages == expected
    assert api["timing"]["total_milliseconds"] > 0.0
    assert len(api["distribution"]["sample_paths"]) == request["diagnostics"]["sample_paths"]
    assert sum(h["paths"] for h in api["distribution"]["payout_histogram"]) == api["paths"]


# ---------------------------------------------------------------------------
# Greeks parity on the same stream (common random numbers)
# ---------------------------------------------------------------------------

BNP_GREEKS_FILE = "structured_note/bnp_quad_index_classic_autocall_xs2741509801_greeks.json"


def _with_paths(request, paths):
    req = copy.deepcopy(request)
    req["notes"][0]["monte_carlo"]["paths"] = paths
    return req


def _with_greeks(request, greeks):
    req = copy.deepcopy(request)
    req["greeks"] = greeks
    return req


GREEKS_CASES = {
    # The BNP Greeks example at 8192 paths (16 simulations on each side; the
    # Sobol stream is the same, only shorter): 1 + 2 x 4 + 4 + 3 runs.
    "bnp_xs2741509801_greeks_example_8192_paths":
        lambda data_dir: _with_paths(load_json(data_dir / BNP_GREEKS_FILE), 8192),
    # Non-default bump sizes on the single-underlying note (no correlation
    # sensitivity, 1 + 2 + 1 + 2 runs).
    "single_underlying_non_default_bumps":
        lambda data_dir: _with_greeks(
            _with_paths(load_json(data_dir / SINGLE_UNDERLYING_FILE), 16384),
            {"spot_bump": 0.02, "vol_bump": 0.005, "funding_bump_bp": 5.0, "rate_bump_bp": 2.0}),
    # Three assets whose bumped matrix must be repaired.
    "three_assets_correlation_repair":
        lambda data_dir: three_asset_repair_request(),
}
GREEKS_REPAIRED = {"bnp_xs2741509801_greeks_example_8192_paths": False,
                   "single_underlying_non_default_bumps": None,
                   "three_assets_correlation_repair": True}


def _rounding_level_greek_standard_error(key, a, b, x, y):
    """A Greek whose per-sample difference is the same on every sample (a
    pure discounting sensitivity of unconditional flows) has a true standard
    error of 0; both sides report the rounding residue of the statistic.
    Treat two residues below 1e-8 of the Greek's magnitude (or of 1) as
    equal."""
    if not key.endswith("_standard_error"):
        return False
    value = abs(a.get(key[:-len("_standard_error")], 0.0))
    return abs(x) < 1e-8 * max(value, 1.0) and abs(y) < 1e-8 * max(value, 1.0)


def _assert_greeks_parity(api, ref):
    a, b = {}, {}
    _flatten("greeks", api["greeks"], a)
    _flatten("greeks", ref["greeks"], b)
    assert set(a) == set(b), f"field sets differ: {sorted(set(a) ^ set(b))}"
    gaps = {}
    worst = (0.0, None)
    for k in a:
        x, y = a[k], b[k]
        if isinstance(x, (bool, str)) or isinstance(y, (bool, str)):
            if x != y:
                gaps[k] = (x, y)
            continue
        rel = abs(x - y) / max(abs(x), abs(y), 1e-300)
        if rel > worst[0]:
            worst = (rel, k)
        if not _close(x, y) and not _rounding_level_greek_standard_error(k, a, b, x, y):
            gaps[k] = (x, y)
    assert not gaps, (f"Greeks not on the same stream ({len(gaps)} of {len(a)} fields): "
                      f"{dict(list(gaps.items())[:12])}; worst relative gap {worst[0]:.3e} "
                      f"at {worst[1]}")
    return len(a), worst


@pytest.mark.parametrize("case", list(GREEKS_CASES), ids=list(GREEKS_CASES))
def test_greeks_parity(client, data_dir, case):
    """Every Greek and its standard error, the echoed bump sizes, the
    repaired flag and the bumped matrix: the reference rebuilds each bumped
    market from the request JSON as the engine does and forms the
    per-sample differences on the same stream, so the agreement is at the
    print floor like the price."""
    request = GREEKS_CASES[case](data_dir)
    api = _post(client, request)
    ref = structured_note_ql(request)
    _assert_parity(api, ref)
    n_fields, worst = _assert_greeks_parity(api, ref)
    g = api["greeks"]
    n_u = len(request["notes"][0]["underlyings"])
    assert [u["underlying_id"] for u in g["underlyings"]] == \
        [u["underlying_id"] for u in request["notes"][0]["underlyings"]]
    if n_u == 1:
        assert "correlation_sensitivity" not in g and "correlation_bumped" not in g
        assert g["correlation_bump_repaired"] is False
    else:
        assert g["correlation_bump_repaired"] is GREEKS_REPAIRED[case]
        assert len(g["correlation_bumped"]) == n_u * n_u
    print(f"{case}: {n_fields} Greeks fields, worst relative gap {worst[0]:.3e} at {worst[1]}")


# ---------------------------------------------------------------------------
# Convergence
# ---------------------------------------------------------------------------

def test_standard_error_scales_as_one_over_sqrt_paths(client):
    """Pseudo-random two-asset note at 1024 / 4096 / 16384 paths: each
    quadrupling halves the reported standard error within 25 %, and the
    price moves by less than 4 standard errors of the smaller run."""
    runs = []
    for paths in (1024, 4096, 16384):
        req = pseudo_random_request()
        req["notes"][0]["monte_carlo"]["paths"] = paths
        runs.append(_post(client, req))
    for small, large in zip(runs, runs[1:]):
        ratio = small["standard_error"] / large["standard_error"]
        assert abs(ratio - 2.0) < 0.5, f"SE ratio {ratio} (expected 2 within 25 %)"
        assert abs(small["npv"] - large["npv"]) < 4.0 * small["standard_error"]
        for bucket in ("bond_redemption", "coupons", "capital_loss"):
            s, l = small["legs"][bucket], large["legs"][bucket]
            assert abs(s["pv"] - l["pv"]) < 4.0 * s["standard_error"] + 1e-9
