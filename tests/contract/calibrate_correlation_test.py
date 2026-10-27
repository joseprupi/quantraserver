"""Correlation from price histories (/calibrate-correlation).

The two shipped request files under examples/data/correlation/ carry public
index closes (Nasdaq-100, Russell 2000, S&P 500; see the README there).
These tests pin, end to end through the JSON gateway:

  * the NDX / RTY file reproduces the 0.747 the Scotia example was booked
    with (one year of daily log returns, no shrinkage), to the digits of the
    script that produced it;
  * with Ledoit-Wolf on two series the intensity is 0 and the matrix is the
    sample (the constant-correlation target is the sample itself); on the
    three-series file the intensity is in (0, 1] and every entry lies
    between the sample and the target;
  * the response equals the independent Python reference
    (calibrate_correlation_ql) field for field to 1e-12 on both files and
    on synthetic sets (fixed seed, a Cholesky-generated 3 x 3 population
    matrix, a singular set for the eigenvalue floor) under every estimator
    setting: Log / Simple, Daily / Weekly, None / LedoitWolf / Fixed,
    window_days and an explicit window;
  * the string fields: labels in the requested order, first and last date,
    the estimator echo, which posted back reproduces the response;
  * the returned correlation_spec is accepted by /price-structured-note and
    echoed by the market diagnostic;
  * the 400s naming their field (the full list is in error_contract_test).
"""

import copy
import math
import random

import pytest

from ql_reference import calibrate_correlation_ql, load_json

NDX_RTY = "correlation/ndx_rut_closes_2025-09-29_to_2026-09-30.json"
NDX_RTY_SPX = "correlation/ndx_rty_spx_closes_2025-10-17_to_2026-10-19.json"
SCOTIA = "structured_note/scotia_autocallable_trigger_ndx_rty_us06419jme72.json"

# The value the earlier script computed from the same closes (one year of
# daily log returns, Pearson correlation), the Scotia example's 0.747.
SCRIPTED_NDX_RTY = 0.7472070756883675


def _post(client, request, product="calibrate_correlation"):
    return client.session.post(
        f"{client.base_url}/{client.ENDPOINTS[product]}", json=request)


def _ok(client, request, product="calibrate_correlation"):
    r = _post(client, request, product)
    assert r.status_code == 200, r.text[:400]
    return r.json()


def _dig(response, path):
    value = response
    for part in path.split("."):
        value = value[int(part)] if part.isdigit() else value[part]
    return value


def _assert_fields(api, expected, tol=1e-12):
    worst = (0.0, None)
    for path, ref in expected.items():
        got = _dig(api, path)
        if isinstance(ref, list):
            assert len(got) == len(ref), path
            pairs = list(zip(got, ref))
        else:
            pairs = [(got, ref)]
        for a, b in pairs:
            if isinstance(b, bool) or isinstance(b, int):
                assert a == b, (path, a, b)
            else:
                gap = abs(a - b)
                if gap > worst[0]:
                    worst = (gap, path)
    assert worst[0] <= tol, worst


def _cholesky(c):
    n = len(c)
    l = [[0.0] * n for _ in range(n)]
    for i in range(n):
        for j in range(i + 1):
            s = c[i][j] - sum(l[i][k] * l[j][k] for k in range(j))
            l[i][j] = math.sqrt(s) if i == j else s / l[j][j]
    return l


def _synthetic(ids, big_t, seed, population=None, vols=None, start_day=1, singular=False):
    """Closes on consecutive calendar days from 2025-01-<start_day>, log
    returns from fixed-seed Gaussians through the Cholesky factor of
    `population` (identity when None); with `singular` the last series'
    returns are the sum of the others'."""
    rng = random.Random(seed)
    n = len(ids)
    pop = population or [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]
    vols = vols or [0.01] * n
    chol = _cholesky(pop)
    import datetime
    d0 = datetime.date(2025, 1, start_day)
    prices = [[100.0 * (i + 1)] for i in range(n)]
    for _t in range(big_t):
        z = [rng.gauss(0.0, 1.0) for _ in range(n)]
        r = [vols[i] * sum(chol[i][k] * z[k] for k in range(n)) for i in range(n)]
        if singular:
            r[-1] = sum(r[:-1])
        for i in range(n):
            prices[i].append(prices[i][-1] * math.exp(r[i]))
    series = []
    for i, uid in enumerate(ids):
        closes = [{"date": (d0 + datetime.timedelta(days=k)).isoformat(), "value": p}
                  for k, p in enumerate(prices[i])]
        series.append({"underlying_id": uid, "closes": closes})
    as_of = (d0 + datetime.timedelta(days=big_t)).isoformat()
    return {"as_of_date": as_of, "series": series, "correlation_id": "SYN"}


# ---------------------------------------------------------------------------
# The shipped files
# ---------------------------------------------------------------------------

def test_ndx_rty_file_reproduces_the_scripted_correlation(client, data_dir):
    request = load_json(data_dir / NDX_RTY)
    out = _ok(client, request)
    spec = out["correlation_spec"]
    assert spec["id"] == "NDX_RTY"
    assert spec["labels"] == ["NDX", "RTY"]
    assert spec["values"]["n_rows"] == 2 and spec["values"]["n_cols"] == 2
    rho = spec["values"]["values"][1]
    assert abs(rho - SCRIPTED_NDX_RTY) < 1e-9
    assert round(rho, 3) == 0.747
    assert spec["values"]["values"] == [1.0, rho, rho, 1.0]
    assert out["sample_correlation"] == spec["values"]["values"]
    assert out["shrinkage_intensity_used"] == 0.0
    assert out["shrinkage_target"] == rho
    assert out["eigenvalue_floor_applied"] is False
    assert abs(out["smallest_eigenvalue_before"] - (1.0 - rho)) < 1e-12
    assert out["smallest_eigenvalue_after"] == out["smallest_eigenvalue_before"]
    assert out["observations_used"] == 253 and out["returns_used"] == 252
    assert out["first_date"] == "2025-09-29" and out["last_date"] == "2026-09-30"
    for s in out["series"]:
        assert s["observations"] == 253 and s["dropped"] == 0
        assert 0.15 < s["annualised_volatility"] < 0.25
    echo = out["estimator_echo"]
    assert echo == {"start_date": "2025-09-29", "end_date": "2026-09-30", "returns": "Log",
                    "frequency": "Daily", "shrinkage": "None", "min_overlap": 60,
                    "eigenvalue_floor": 1e-8}
    _assert_fields(out, calibrate_correlation_ql(request))

    # Ledoit-Wolf next to it: two series, the target is the sample itself.
    lw = copy.deepcopy(request)
    lw["estimator"]["shrinkage"] = "LedoitWolf"
    out_lw = _ok(client, lw)
    assert out_lw["shrinkage_intensity_used"] == 0.0
    assert out_lw["correlation_spec"]["values"]["values"] == spec["values"]["values"]
    assert out_lw["estimator_echo"]["shrinkage"] == "LedoitWolf"
    assert "shrinkage_intensity" not in out_lw["estimator_echo"]
    _assert_fields(out_lw, calibrate_correlation_ql(lw))


def test_three_series_file_ledoit_wolf_shrinks_toward_the_target(client, data_dir):
    request = load_json(data_dir / NDX_RTY_SPX)
    out = _ok(client, request)
    assert out["correlation_spec"]["labels"] == ["NDX", "RTY", "SPX"]
    assert out["first_date"] == "2025-10-20"  # 365 days back from 2026-10-19 excludes 10-17
    assert out["last_date"] == "2026-10-19"
    assert out["observations_used"] == 251
    assert out["series"][0]["observations"] == 266 and out["series"][0]["dropped"] == 15
    assert out["series"][2]["observations"] == 252 and out["series"][2]["dropped"] == 1
    sample = out["sample_correlation"]
    shrunk = out["correlation_spec"]["values"]["values"]
    delta = out["shrinkage_intensity_used"]
    target = out["shrinkage_target"]
    assert 0.0 < delta <= 1.0
    assert abs(target - (sample[1] + sample[2] + sample[5]) / 3.0) < 1e-11
    for k in (1, 2, 5):
        assert abs(shrunk[k] - (delta * target + (1.0 - delta) * sample[k])) < 1e-11
        assert min(sample[k], target) - 1e-11 <= shrunk[k] <= max(sample[k], target) + 1e-11
    for k in (0, 4, 8):
        assert shrunk[k] == 1.0
    assert shrunk[1] == shrunk[3] and shrunk[2] == shrunk[6] and shrunk[5] == shrunk[7]
    # The earlier script's values on the same closes (0.746, 0.925, 0.831).
    assert abs(sample[1] - 0.746) < 1.5e-3
    assert abs(sample[2] - 0.925) < 1.5e-3
    assert abs(sample[5] - 0.831) < 1.5e-3
    assert out["eigenvalue_floor_applied"] is False
    echo = out["estimator_echo"]
    assert echo["start_date"] == "2025-10-19" and echo["end_date"] == "2026-10-19"
    assert "window_days" not in echo
    _assert_fields(out, calibrate_correlation_ql(request))

    # The echo posted back reproduces the response.
    again = copy.deepcopy(request)
    again["estimator"] = echo
    assert _ok(client, again) == out

    # No shrinkage and a fixed intensity of 1 bracket the Ledoit-Wolf result.
    none = copy.deepcopy(request)
    none["estimator"]["shrinkage"] = "None"
    out_none = _ok(client, none)
    assert out_none["correlation_spec"]["values"]["values"] == sample
    assert out_none["shrinkage_intensity_used"] == 0.0
    fixed = copy.deepcopy(request)
    fixed["estimator"].update({"shrinkage": "Fixed", "shrinkage_intensity": 1.0})
    out_fixed = _ok(client, fixed)
    for k in (1, 2, 3, 5, 6, 7):
        assert abs(out_fixed["correlation_spec"]["values"]["values"][k] - target) < 1e-11
    assert out_fixed["estimator_echo"]["shrinkage_intensity"] == 1.0
    _assert_fields(out_fixed, calibrate_correlation_ql(fixed))


def test_labels_permute_the_output(client, data_dir):
    request = load_json(data_dir / NDX_RTY_SPX)
    request["labels"] = ["SPX", "NDX", "RTY"]
    out = _ok(client, request)
    base = _ok(client, load_json(data_dir / NDX_RTY_SPX))
    assert out["correlation_spec"]["labels"] == ["SPX", "NDX", "RTY"]
    assert [s["underlying_id"] for s in out["series"]] == ["SPX", "NDX", "RTY"]
    b = base["correlation_spec"]["values"]["values"]
    assert out["correlation_spec"]["values"]["values"] == [
        b[8], b[6], b[7], b[2], b[0], b[1], b[5], b[3], b[4]]
    assert out["shrinkage_intensity_used"] == base["shrinkage_intensity_used"]
    _assert_fields(out, calibrate_correlation_ql(request))


def test_spec_is_accepted_by_the_structured_note_pricer(client, data_dir):
    corr = _ok(client, load_json(data_dir / NDX_RTY))
    note_req = load_json(data_dir / SCOTIA)
    note_req["pricing"]["correlations"]["matrices"] = [corr["correlation_spec"]]
    note_req["notes"][0]["monte_carlo"]["paths"] = 1024
    note_req["diagnostics"] = {"market": True}
    note = _ok(client, note_req, "structured_note")["notes"][0]
    assert note["terms"]["correlation_id"] == "NDX_RTY"
    assert note["market"]["correlation_labels"] == ["NDX", "RTY"]
    assert note["market"]["correlation"] == corr["correlation_spec"]["values"]["values"]


# ---------------------------------------------------------------------------
# Synthetic sets against the reference, every estimator setting
# ---------------------------------------------------------------------------

POPULATION = [[1.0, 0.6, 0.3], [0.6, 1.0, 0.5], [0.3, 0.5, 1.0]]

VARIANTS = {
    "default": {},
    "simple_returns": {"returns": "Simple"},
    "weekly": {"frequency": "Weekly", "min_overlap": 30},
    "none": {"shrinkage": "None"},
    "fixed_half": {"shrinkage": "Fixed", "shrinkage_intensity": 0.5},
    "window_days": {"window_days": 200},
    "explicit_window": {"start_date": "2025-03-01", "end_date": "2025-09-30"},
    "weekly_simple_fixed": {"frequency": "Weekly", "returns": "Simple", "shrinkage": "Fixed",
                            "shrinkage_intensity": 0.2, "min_overlap": 30},
}


@pytest.mark.parametrize("variant", sorted(VARIANTS))
def test_three_series_population_matrix_equals_the_reference(client, variant):
    request = _synthetic(["A", "B", "C"], 1200, seed=11, population=POPULATION,
                         vols=[0.010, 0.015, 0.012])
    request["estimator"] = dict(VARIANTS[variant])
    out = _ok(client, request)
    _assert_fields(out, calibrate_correlation_ql(request))
    sample = out["sample_correlation"]
    # Recovered within sampling error: (1 - rho^2) / sqrt(T) <= 0.03 on the
    # full daily set; the windows and the weekly sampling use fewer returns.
    tol = 4.0 / math.sqrt(out["returns_used"])
    assert abs(sample[1] - 0.6) < tol
    assert abs(sample[2] - 0.3) < tol
    assert abs(sample[5] - 0.5) < tol
    assert 0.0 <= out["shrinkage_intensity_used"] <= 1.0
    if variant == "window_days":
        import datetime
        as_of = datetime.date.fromisoformat(request["as_of_date"])
        assert out["estimator_echo"]["start_date"] == (as_of - datetime.timedelta(days=200)).isoformat()
        assert out["estimator_echo"]["end_date"] == request["as_of_date"]
        assert out["observations_used"] == 201
    if variant == "explicit_window":
        assert out["first_date"] == "2025-03-01" and out["last_date"] == "2025-09-30"
    if variant == "weekly":
        assert out["observations_used"] == 1200 // 5 + 1
        assert out["last_date"] == request["as_of_date"]


def test_perfectly_correlated_and_independent_series(client):
    # B is A at twice the level: identical log returns, correlation exactly 1.
    request = _synthetic(["A", "B"], 300, seed=3)
    a = request["series"][0]["closes"]
    request["series"][1]["closes"] = [{"date": c["date"], "value": 2.0 * c["value"]} for c in a]
    out = _ok(client, request)
    assert out["correlation_spec"]["values"]["values"] == [1.0, 1.0, 1.0, 1.0]
    assert out["series"][0]["annualised_volatility"] == out["series"][1]["annualised_volatility"]
    _assert_fields(out, calibrate_correlation_ql(request))
    # Independent: about zero, within 3.5 standard errors (1 / sqrt(T)).
    request = _synthetic(["A", "B"], 2000, seed=5)
    out = _ok(client, request)
    assert abs(out["sample_correlation"][1]) < 3.5 / math.sqrt(2000)
    _assert_fields(out, calibrate_correlation_ql(request))


def test_eigenvalue_floor_on_a_singular_set(client, data_dir):
    request = _synthetic(["A", "B", "C"], 400, seed=8, singular=True)
    request["estimator"] = {"shrinkage": "None", "eigenvalue_floor": 1e-6}
    out = _ok(client, request)
    # An eigenvalue at 0 up to rounding: repaired iff rounding put it below
    # the registry's -1e-10; either way the spec is a valid pricing input.
    assert abs(out["smallest_eigenvalue_before"]) < 1e-10
    assert out["eigenvalue_floor_applied"] is (out["smallest_eigenvalue_before"] < -1e-10)
    assert out["smallest_eigenvalue_after"] >= -1e-10
    v = out["correlation_spec"]["values"]["values"]
    assert v[0] == v[4] == v[8] == 1.0
    assert v[1] == v[3] and v[2] == v[6] and v[5] == v[7]
    for a, b in zip(v, out["sample_correlation"]):
        assert abs(a - b) < 1e-4
    _assert_fields(out, calibrate_correlation_ql(request))
    # The registry parses and PSD-checks the block on any request.
    swap = load_json(data_dir / "ir_swaps/vanilla_swap_request.json")
    swap["pricing"]["correlations"] = {"matrices": [out["correlation_spec"]]}
    assert _post(client, swap, "vanilla_swap").status_code == 200
    # The same closes with a perfectly correlated pair: eigenvalue 0, kept.
    pair = _synthetic(["A", "B"], 200, seed=9)
    pair["series"][1]["closes"] = [{"date": c["date"], "value": 3.0 * c["value"]}
                                   for c in pair["series"][0]["closes"]]
    out = _ok(client, pair)
    assert out["correlation_spec"]["values"]["values"] == [1.0, 1.0, 1.0, 1.0]
    assert out["eigenvalue_floor_applied"] is False


def test_closes_after_as_of_are_used_without_a_window(client):
    request = _synthetic(["A", "B"], 100, seed=4)
    request["as_of_date"] = "2025-02-01"  # before the last close
    out = _ok(client, request)
    assert out["observations_used"] == 101
    assert out["last_date"] == request["series"][0]["closes"][-1]["date"]
    _assert_fields(out, calibrate_correlation_ql(request))
    request["estimator"] = {"window_days": 20, "min_overlap": 10}
    out = _ok(client, request)
    assert out["observations_used"] == 21 and out["last_date"] == "2025-02-01"
    _assert_fields(out, calibrate_correlation_ql(request))


def test_rejected_requests_name_the_field(client, data_dir):
    base = load_json(data_dir / NDX_RTY)

    def bad(mutate, needle, status=400):
        req = copy.deepcopy(base)
        mutate(req)
        r = _post(client, req)
        assert r.status_code == status, (needle, r.text[:300])
        assert needle in r.text, (needle, r.text[:300])

    bad(lambda q: q["series"].pop(), "at least two price series")
    bad(lambda q: q["series"][1].__setitem__("underlying_id", "NDX"),
        "series[1].underlying_id: duplicate 'NDX'")
    bad(lambda q: q["series"][0]["closes"].__setitem__(5, dict(q["series"][0]["closes"][4])),
        "series[0].closes[5].date")
    bad(lambda q: q["series"][1]["closes"][7].__setitem__("value", -1.0),
        "series[1].closes[7].value must be finite and > 0")
    bad(lambda q: q["estimator"].__setitem__("min_overlap", 300),
        "overlap of 253 common dates inside the window [open, open] is below "
        "estimator.min_overlap (300)")
    bad(lambda q: q.__setitem__("labels", ["NDX", "SPX"]), "labels: 'SPX' is not a series id")
    bad(lambda q: q.__setitem__("labels", ["NDX"]), "labels must be a permutation")
    bad(lambda q: q["estimator"].update({"window_days": 30, "start_date": "2026-01-01"}),
        "either window_days or start_date / end_date")
    bad(lambda q: q["estimator"].update({"start_date": "2026-05-01", "end_date": "2026-04-01"}),
        "estimator.start_date 2026-05-01 is after end_date 2026-04-01")
    bad(lambda q: q["estimator"].__setitem__("shrinkage", "Fixed"),
        "shrinkage_intensity is required with shrinkage = Fixed")
    bad(lambda q: q["estimator"].update({"shrinkage": "Fixed", "shrinkage_intensity": 1.2}),
        "shrinkage_intensity must be in [0, 1]")
    bad(lambda q: q["estimator"].__setitem__("shrinkage_intensity", 0.3),
        "shrinkage_intensity is only read with shrinkage = Fixed")
    bad(lambda q: q["estimator"].__setitem__("eigenvalue_floor", 1.0),
        "eigenvalue_floor must be finite and in [0, 1)")
    bad(lambda q: q["estimator"].update({"start_date": "2027-01-01"}),
        "contains no date common to every series")
    bad(lambda q: q["estimator"].update({"window_days": 10, "min_overlap": 3,
                                         "frequency": "Weekly"}),
        "fewer than 3 observations after the Weekly sampling")
    bad(lambda q: q.pop("correlation_id"), "correlation_id")
    bad(lambda q: q["series"][0].__setitem__("closes", []), "series[0].closes")
    bad(lambda q: q["estimator"].__setitem__("returns", "Arithmetic"), "")
