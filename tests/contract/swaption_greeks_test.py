"""Swaption greeks: units, definitions and precedence (API vs native QuantLib).

`swaption_pricing_details` reports analytic Black/Bachelier greeks in the same
units as the bump-and-reprice `swaption_pricing_rebump` path: rate
sensitivities per 1bp (gamma per bp^2), vega per 1bp of the quoted vol, theta
in currency per calendar day. The analytic numbers are checked against
QuantLib's BlackCalculator / BachelierCalculator built with discount = 1 and
multiplied by the swap annuity (theta = pure time decay, forward/vol/annuity
held fixed). The analytic and rebump values are then compared loosely to prove
the units agree, and the precedence rule (rebump overwrites when both flags are
set) is pinned.
"""

import copy
import math

import QuantLib as ql
import pytest

from ql_reference import build_swaption_ql, load_json, parse_date

# (fixture, expected vol type)
FIXTURES = [
    ("swaption/swpt_eur_1y5y_payer_near_atm_physical.json", "Lognormal"),
    ("swaption_ois_bbg_zerorate_request.json", "Normal"),
]

GREEKS = ("delta", "gamma", "vega", "theta", "dv01")
REL_TOL = 1e-6
BP = 1.0e-4


def _with_options(request, **flags):
    out = copy.deepcopy(request)
    out["pricing"].setdefault("options", {}).update(flags)
    return out


def _row(client, request):
    response = client.price("swaption", request)
    rows = response.get("swaptions")
    assert isinstance(rows, list) and len(rows) == 1, response
    return rows[0]


def _vol_spec(request, sw_data):
    pricing = request["pricing"]
    surfaces = pricing.get("volatility", pricing).get("vol_surfaces", [])
    surface = next(v for v in surfaces if v["id"] == sw_data["volatility"])
    inner = surface["payload"]
    assert inner["payload_type"] == "SwaptionVolConstantSpec", (
        "this test derives stdDev from a constant vol spec"
    )
    return inner["payload"]["base"]


def _native_greeks(request):
    """Analytic greeks with the documented definitions, from native QuantLib."""
    built = build_swaption_ql(request)
    swaption = built["swaption"]
    sw_data = request["swaptions"][0]
    base = _vol_spec(request, sw_data)

    annuity = swaption.annuity()
    forward = built["swap"].fairRate()
    strike = sw_data["swaption"]["underlying"]["fixed_leg"]["rate"]
    exercise_date = swaption.exercise().dates()[0]
    assert base["day_counter"] == "Actual365Fixed"
    expiry_time = ql.Actual365Fixed().yearFraction(
        parse_date(base["reference_date"]), exercise_date
    )
    vol = base["constant_vol"]
    std_dev = vol * math.sqrt(expiry_time)
    option_type = (
        ql.Option.Call
        if sw_data["swaption"]["underlying"]["swap_type"] == "Payer"
        else ql.Option.Put
    )
    displacement = base.get("displacement", 0.0)

    if base["volatility_type"] == "Normal":
        payoff = ql.PlainVanillaPayoff(option_type, strike)
        calc = ql.BachelierCalculator(payoff, forward, std_dev, 1.0)
        spot = forward
    else:
        payoff = ql.PlainVanillaPayoff(option_type, strike + displacement)
        calc = ql.BlackCalculator(payoff, forward + displacement, std_dev, 1.0)
        spot = forward + displacement

    delta = annuity * calc.deltaForward() * BP
    greeks = {
        "npv": swaption.NPV(),
        "atm_forward": forward,
        "annuity": annuity,
        "delta": delta,
        "gamma": annuity * calc.gammaForward() * BP * BP,
        "vega": annuity * calc.vega(expiry_time) * BP,
        "theta": annuity * calc.theta(spot, expiry_time) / 365.0,
        "dv01": delta,
    }
    # The calculator inputs must reproduce the engine's premium, otherwise the
    # greek comparison below would be checking against the wrong (F, sigma, T).
    assert abs(annuity * calc.value() - greeks["npv"]) <= 1e-6 * abs(greeks["npv"]), (
        f"calculator premium {annuity * calc.value()} vs engine NPV {greeks['npv']}"
    )
    return greeks


def _assert_close(name, got, want, rel):
    assert got == pytest.approx(want, rel=rel, abs=1e-12), (
        f"{name}: API {got!r} vs QuantLib {want!r}"
    )


@pytest.mark.parametrize("filename,vol_type", FIXTURES)
def test_details_greeks_match_quantlib_definitions(client, data_dir, filename, vol_type):
    request = load_json(data_dir / filename)
    assert _vol_spec(request, request["swaptions"][0])["volatility_type"] == vol_type
    row = _row(client, _with_options(request, swaption_pricing_details=True))
    native = _native_greeks(request)

    for key in GREEKS:
        assert key in row, f"{filename}: details response lacks {key!r}: {row}"
    _assert_close("npv", row["npv"], native["npv"], 1e-9)
    _assert_close("atm_forward", row["atm_forward"], native["atm_forward"], REL_TOL)
    _assert_close("annuity", row["annuity"], native["annuity"], REL_TOL)
    for key in GREEKS:
        _assert_close(key, row[key], native[key], REL_TOL)
    # Sanity on signs/units: a payer swaption has positive delta and vega and
    # decays with time.
    assert row["delta"] > 0 and row["vega"] > 0 and row["gamma"] > 0
    assert row["theta"] < 0
    assert row["dv01"] == row["delta"]


@pytest.mark.parametrize("filename,vol_type", FIXTURES)
def test_details_and_rebump_greeks_share_units(client, data_dir, filename, vol_type):
    request = load_json(data_dir / filename)
    details = _row(client, _with_options(request, swaption_pricing_details=True))
    rebump = _row(client, _with_options(request, swaption_pricing_rebump=True))

    # Analytic vega vs central-difference +/-1bp vol bump: the bump is tiny
    # relative to the vol, so the two agree to well under 1%.
    assert rebump["vega"] == pytest.approx(details["vega"], rel=1e-2), (
        f"{filename}: vega details {details['vega']} vs rebump {rebump['vega']}"
    )
    # Analytic DV01 moves only the forward; the rebump also moves the annuity,
    # so agreement is to a few percent -- enough to show both are "per 1bp".
    assert rebump["dv01"] == pytest.approx(details["dv01"], rel=5e-2), (
        f"{filename}: dv01 details {details['dv01']} vs rebump {rebump['dv01']}"
    )
    # Gamma per bp^2: the parallel-bump second difference is a noisier
    # estimate, assert same sign and order of magnitude.
    assert details["gamma"] > 0 and rebump["gamma"] > 0
    assert 0.5 < rebump["gamma"] / details["gamma"] < 2.0, (
        f"{filename}: gamma details {details['gamma']} vs rebump {rebump['gamma']}"
    )
    # Theta: both are currency per calendar day and negative for a long option.
    assert details["theta"] < 0
    assert "theta" in rebump
    # The analytic-only fields are optionals written only when computed, so
    # the rebump-only response does not carry them at all (a 0.0 would read as
    # a computed value).
    for key in ("delta", "atm_forward", "annuity"):
        assert key not in rebump, f"{key} must be absent in rebump-only mode: {rebump}"


def test_no_greek_flag_omits_every_greek(client, data_dir):
    """Without either pricing flag no greek is computed, so none may appear --
    not even as 0.0. Always-computed scalars (npv, used_volatility) stay."""
    filename, _ = FIXTURES[0]
    request = load_json(data_dir / filename)
    request["pricing"].pop("options", None)
    row = _row(client, request)
    for key in GREEKS + ("atm_forward", "annuity"):
        assert key not in row, f"{key} present without a greek flag: {row}"
    assert "npv" in row and "used_volatility" in row


def test_rebump_overwrites_analytic_when_both_flags_set(client, data_dir):
    filename, _ = FIXTURES[0]
    request = load_json(data_dir / filename)
    details = _row(client, _with_options(request, swaption_pricing_details=True))
    rebump = _row(client, _with_options(request, swaption_pricing_rebump=True))
    both = _row(
        client,
        _with_options(request, swaption_pricing_details=True, swaption_pricing_rebump=True),
    )

    for key in ("dv01", "gamma", "vega", "theta"):
        assert both[key] == rebump[key], f"{key}: both-flags {both[key]} vs rebump {rebump[key]}"
        assert both[key] != details[key], f"{key}: rebump did not overwrite the analytic value"
    for key in ("delta", "annuity", "atm_forward"):
        assert key in both, f"both-flags response lacks analytic field {key!r}"
        assert both[key] == details[key]
