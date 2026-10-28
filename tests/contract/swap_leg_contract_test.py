"""The `SwapLeg` contract of the single-currency swap endpoints (0.10.0).

price-vanilla-swap, price-ois-swap and price-basis-swap all take two
`SwapLeg`s (pay_receive, currency, the shared notional / schedule / day
counter / payment conventions on the leg, coupons_type + the kind-specific
coupons table, discounting_curve, forwarding_curve, exchange flags) and
answer with one `SwapLegResult` per leg in request order. These tests pin,
end to end against the running JSON API:

  - the response shape: `legs` of length 2 whose signed NPVs sum to `npv`,
    `npv_reporting == npv` on a one-currency swap, and the fair values the
    swap's shape defines (`fair_rate` iff exactly one leg is fixed,
    `fair_spread` iff a leg is floating: both on fixed-vs-floating, only
    `fair_spread` on basis);
  - the vanilla / OIS `fair_spread` is the floating leg's par spread:
    repricing that leg at it (the fixed leg untouched) zeroes the swap;
  - the sign convention: flipping both `pay_receive` values negates `npv`
    and both leg NPVs;
  - the basis `fair_spread` lands on the leg that carries the request's
    spread, and repricing that leg at it zeroes the swap;
  - principal exchanges on a vanilla leg (`initial_exchange` /
    `final_exchange`) are priced as `NotionalExchange` flows and match a
    QuantLib reference built from the same request;
  - a leg-level `payment_lag` / `payment_calendar` on an IBOR leg moves the
    payment dates and matches the QuantLib reference;
  - a CMS LinearTsr leg on a flat absolute-strike smile cube prices like the
    constant vol whatever the cube's strike grid (no ATM level is read off
    the strikes);
  - a CMS leg on a SpreadFromATM cube or a SABR surface (built on ATM
    forwards the engine computes) prices: a flat cube like the constant vol,
    a SABR surface with beta 1, rho 0 and a small nu (given, or fitted to
    flat quotes) close to it;
  - the 400s the leg model promises, each naming the offending field.
"""

import copy
import math

import pytest

from ql_reference import get_calendar, get_day_counter, load_json, parse_date, swap_legs_ql

# Role-named leg fields of each endpoint, in `legs` order.
LEG_ROLES = {
    "vanilla_swap": ("fixed_leg", "floating_leg"),
    "ois_swap": ("fixed_leg", "overnight_leg"),
    "basis_swap": ("leg1", "leg2"),
}

FIXTURES = {
    "vanilla_swap": "ir_swaps/vanilla_swap_request.json",
    "ois_swap": "ir_swaps/ois_swap_request.json",
    "basis_swap": "ir_swaps/basis_swap_request.json",
}

# The fair values each endpoint reports (fair_rate iff exactly one leg is
# fixed, fair_spread iff a leg is floating) and the ones it must NOT report.
FAIR_FIELDS = {
    "vanilla_swap": ({"fair_rate", "fair_spread"}, set()),
    "ois_swap": ({"fair_rate", "fair_spread"}, set()),
    "basis_swap": ({"fair_spread"}, {"fair_rate"}),
}
# The leg whose spread `fair_spread` solves for on the fixed-vs-floating
# endpoints.
FLOATING_ROLE = {"vanilla_swap": "floating_leg", "ois_swap": "overnight_leg"}


def _swap(request, product):
    """The swap body (`vanilla_swap` / `ois_swap` / `basis_swap`) of the
    first entry of `request`."""
    return request["swaps"][0][product]


def _row(client, product, request):
    """Price `request` and return the first swap's response row."""
    response = client.price(product, request)
    rows = response.get("swaps")
    assert isinstance(rows, list) and len(rows) == 1, response
    return rows[0]


def _post_status(client, product, request):
    """POST verbatim and return (status code, body text): the error-contract
    tests need the status, which `client.price` folds into an exception."""
    endpoint = client.ENDPOINTS[product]
    r = client.session.post(f"{client.base_url}/{endpoint}", json=request)
    return r.status_code, r.text


def _notional(request, product):
    """The first leg's notional (a shared field, so it sits on the leg)."""
    leg = _swap(request, product)[LEG_ROLES[product][0]]
    return float(leg["notional"])


# ---------------------------------------------------------------------------
# Response shape
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("product", list(FIXTURES))
def test_legs_sum_to_npv_and_fair_value_matches_the_product(client, data_dir, product):
    """`legs` holds one result per leg in request order; their signed NPVs
    add up to `npv`; `npv_reporting` equals `npv` in one currency; the swap
    reports the fair value it solves for and nothing else."""
    request = load_json(data_dir / FIXTURES[product])
    row = _row(client, product, request)

    legs = row["legs"]
    assert len(legs) == 2, f"{product}: expected one result per leg, got {row}"
    assert row["npv"] == pytest.approx(legs[0]["npv"] + legs[1]["npv"], abs=1e-6), (
        f"{product}: npv {row['npv']} != legs {legs[0]['npv']} + {legs[1]['npv']}"
    )
    for i, leg in enumerate(legs):
        assert leg["npv_reporting"] == leg["npv"], (
            f"{product}: legs[{i}].npv_reporting {leg['npv_reporting']} != npv {leg['npv']} "
            "on a single-currency swap"
        )

    present, absent = FAIR_FIELDS[product]
    for name in present:
        assert name in row, f"{product}: response lacks {name}: {row}"
    for name in absent:
        assert name not in row, f"{product}: response must not carry {name}: {row}"


@pytest.mark.parametrize("product", list(FIXTURES))
def test_legs_match_the_quantlib_leg_algebra(client, data_dir, product):
    """Each leg's signed NPV (and the fair value) equals the QuantLib reference
    built with the engine's leg algebra from the same request."""
    request = load_json(data_dir / FIXTURES[product])
    row = _row(client, product, request)
    ref = swap_legs_ql(request)

    assert row["npv"] == pytest.approx(ref["npv"], abs=1e-4), (row["npv"], ref["npv"])
    for i in range(2):
        assert row["legs"][i]["npv"] == pytest.approx(ref["legs"][i]["npv"], abs=1e-4), (
            f"legs[{i}]: API {row['legs'][i]['npv']} vs QuantLib {ref['legs'][i]['npv']}"
        )
    for name in FAIR_FIELDS[product][0]:
        assert row[name] == pytest.approx(ref[name], abs=1e-10), (name, row[name], ref[name])


# ---------------------------------------------------------------------------
# Sign convention
# ---------------------------------------------------------------------------

def _flipped(request, product):
    """The same swap seen from the other side: both pay_receive values swapped."""
    out = copy.deepcopy(request)
    for role in LEG_ROLES[product]:
        leg = _swap(out, product)[role]
        leg["pay_receive"] = "Receive" if leg["pay_receive"] == "Pay" else "Pay"
    return out


@pytest.mark.parametrize("product", list(FIXTURES))
def test_flipping_both_sides_negates_npv_and_leg_npvs(client, data_dir, product):
    """`legs[i].npv` is signed from the reported party's view (- for Pay, +
    for Receive), so the counterparty's view is the exact negation."""
    request = load_json(data_dir / FIXTURES[product])
    row = _row(client, product, request)
    other = _row(client, product, _flipped(request, product))

    assert other["npv"] == pytest.approx(-row["npv"], abs=1e-6), (row["npv"], other["npv"])
    for i in range(2):
        assert other["legs"][i]["npv"] == pytest.approx(-row["legs"][i]["npv"], abs=1e-6), (
            f"legs[{i}]: {row['legs'][i]['npv']} vs flipped {other['legs'][i]['npv']}"
        )
    # The fair values are properties of the trade, not of the side.
    for name in FAIR_FIELDS[product][0]:
        assert other[name] == pytest.approx(row[name], abs=1e-12)


# ---------------------------------------------------------------------------
# Vanilla / OIS fair spread
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("product", list(FLOATING_ROLE))
def test_fixed_vs_floating_fair_spread_reprices_the_floating_leg(client, data_dir, product):
    """`fair_spread` on a fixed-vs-floating swap is the floating (overnight)
    leg's spread that zeroes the swap, the fixed leg and the forwards held
    fixed (QuantLib's VanillaSwap / OvernightIndexedSwap fairSpread):
    repricing the floating leg at it through the API gives a zero NPV, and
    the fair rate is unchanged by the spread only through the NPV."""
    request = load_json(data_dir / FIXTURES[product])
    row = _row(client, product, request)
    fair = row["fair_spread"]
    leg = _swap(request, product)[FLOATING_ROLE[product]]
    assert fair != pytest.approx(leg["coupons"].get("spread", 0.0)), (
        "the fixture is not at par; the test needs a non-trivial fair spread"
    )

    at_par = copy.deepcopy(request)
    _swap(at_par, product)[FLOATING_ROLE[product]]["coupons"]["spread"] = fair
    repriced = _row(client, product, at_par)
    notional = _notional(request, product)
    assert abs(repriced["npv"]) < 1e-9 * notional, (
        f"{product}: floating leg repriced at fair_spread {fair}: npv {repriced['npv']}"
    )
    assert repriced["fair_spread"] == pytest.approx(fair, abs=1e-12)
    assert "fair_rate" in repriced


# ---------------------------------------------------------------------------
# Basis fair spread
# ---------------------------------------------------------------------------

def test_basis_fair_spread_lands_on_the_leg_carrying_the_spread(client, data_dir):
    """When only leg1 carries a spread, `fair_spread` is leg1's par spread:
    repricing leg1 at it (leg2 untouched) zeroes the swap."""
    request = load_json(
        data_dir / "ir_swaps/basis_eur_5y_euribor3m_plus5bp_vs_euribor6m.json")
    swap = _swap(request, "basis_swap")
    assert swap["leg1"]["coupons"]["spread"] != 0.0 and swap["leg2"]["coupons"]["spread"] == 0.0

    row = _row(client, "basis_swap", request)
    fair = row["fair_spread"]
    assert fair != pytest.approx(swap["leg1"]["coupons"]["spread"]), (
        "the fixture is not at par; the test needs a non-trivial fair spread"
    )

    at_par = copy.deepcopy(request)
    _swap(at_par, "basis_swap")["leg1"]["coupons"]["spread"] = fair
    repriced = _row(client, "basis_swap", at_par)
    notional = _notional(request, "basis_swap")
    assert abs(repriced["npv"]) < 1e-4 * notional, (
        f"leg1 repriced at fair_spread {fair}: npv {repriced['npv']} is not zero"
    )
    assert repriced["fair_spread"] == pytest.approx(fair, abs=1e-10)


# ---------------------------------------------------------------------------
# Principal exchanges on a vanilla leg
# ---------------------------------------------------------------------------

def test_vanilla_fixed_leg_exchanges_match_quantlib_and_are_flagged(client, data_dir):
    """A vanilla leg with `initial_exchange` / `final_exchange` true pays
    -notional on its schedule start and +notional with its last coupon (own
    frame), priced like every other flow of the leg: the API matches a
    QuantLib reference (coupons + SimpleCashFlows discounted on the leg's
    curve) and the flows carry `kind == "NotionalExchange"` entries."""
    plain = load_json(data_dir / "ir_swaps/vanilla_swap_request.json")
    request = copy.deepcopy(plain)
    request["include_flows"] = True
    fixed = _swap(request, "vanilla_swap")["fixed_leg"]
    fixed["initial_exchange"] = True
    fixed["final_exchange"] = True
    notional = float(fixed["notional"])

    row = _row(client, "vanilla_swap", request)
    ref = swap_legs_ql(request)
    assert row["npv"] == pytest.approx(ref["npv"], abs=1e-4), (row["npv"], ref["npv"])
    for i in range(2):
        assert row["legs"][i]["npv"] == pytest.approx(ref["legs"][i]["npv"], abs=1e-4)

    # The exchanges change the value (a 3.5% fixed leg with its principal
    # is not worth its coupons alone), so the flags are not a no-op.
    assert abs(row["npv"] - _row(client, "vanilla_swap", plain)["npv"]) > 1.0

    flows = row["legs"][0]["flows"]
    exchanges = [f for f in flows if f.get("kind") == "NotionalExchange"]
    coupons = [f for f in flows if f.get("kind") == "Coupon"]
    assert len(exchanges) == 2 and coupons, f"expected 2 exchanges + coupons: {flows}"
    initial, final = sorted(exchanges, key=lambda f: f["payment_date"])
    start = fixed["schedule"]["effective_date"]
    assert initial["payment_date"] == start and initial["amount"] == pytest.approx(-notional)
    assert final["payment_date"] == max(c["payment_date"] for c in coupons)
    assert final["amount"] == pytest.approx(notional)
    # The flows are the leg in its own frame: their PVs sum to npv / sign.
    sign = -1.0 if fixed["pay_receive"] == "Pay" else 1.0
    assert sum(f["present_value"] for f in flows) == pytest.approx(
        row["legs"][0]["npv"] / sign, abs=1e-4)
    # The floating leg carries no exchange: every flow is a coupon.
    assert all(f.get("kind") == "Coupon" for f in row["legs"][1]["flows"])


# ---------------------------------------------------------------------------
# Payment lag / calendar on an IBOR leg
# ---------------------------------------------------------------------------

def test_ibor_leg_payment_lag_and_calendar_match_quantlib(client, data_dir):
    """`payment_lag` / `payment_calendar` are leg-level fields and apply to
    an IBOR leg too (absent = the schedule calendar / no lag): paid two UK
    business days after each period end, every floating coupon's payment
    date moves later than on the plain swap while its accrual and amount
    stay, and the swap matches the QuantLib reference (IborLeg with the same
    payment calendar and lag, through the engine's leg algebra)."""
    plain = load_json(data_dir / "ir_swaps/vanilla_swap_request.json")
    request = copy.deepcopy(plain)
    request["include_flows"] = True
    plain["include_flows"] = True
    floating = _swap(request, "vanilla_swap")["floating_leg"]
    assert floating["coupons_type"] == "IborCoupons"
    assert "payment_lag" not in floating and "payment_calendar" not in floating
    floating["payment_lag"] = 2
    floating["payment_calendar"] = "UnitedKingdom"

    row = _row(client, "vanilla_swap", request)
    ref = swap_legs_ql(request)
    assert row["npv"] == pytest.approx(ref["npv"], abs=1e-4), (row["npv"], ref["npv"])
    for i in range(2):
        assert row["legs"][i]["npv"] == pytest.approx(ref["legs"][i]["npv"], abs=1e-4), (
            f"legs[{i}]: API {row['legs'][i]['npv']} vs QuantLib {ref['legs'][i]['npv']}"
        )
    assert row["fair_rate"] == pytest.approx(ref["fair_rate"], abs=1e-10)

    lagged = row["legs"][1]["flows"]
    unlagged = _row(client, "vanilla_swap", plain)["legs"][1]["flows"]
    assert len(lagged) == len(unlagged) > 0
    for a, b in zip(unlagged, lagged):
        assert b["payment_date"] > a["payment_date"], (a["payment_date"], b["payment_date"])
        assert b["accrual_end_date"] == a["accrual_end_date"]
        assert b["amount"] == pytest.approx(a["amount"], abs=1e-6)
    # The reference lists the same payment dates, flow by flow.
    assert [f["payment_date"] for f in lagged] == [d for d, _, _ in ref["legs"][1]["flows"]]
    # The fixed leg carries no lag: its flows are those of the plain swap.
    assert row["legs"][0]["flows"] == _row(client, "vanilla_swap", plain)["legs"][0]["flows"]


# ---------------------------------------------------------------------------
# CMS LinearTsr leg on an absolute-strike smile cube
# ---------------------------------------------------------------------------

def _flat_absolute_cube(constant_surface, strikes):
    """The constant surface as an absolute-strike smile cube on `strikes`,
    flat at the constant vol, with expiries covering the CMS fixings."""
    wrapper = constant_surface["payload"]
    base = copy.deepcopy(wrapper["payload"]["base"])
    vol = base.pop("constant_vol")
    base["shape"] = "SmileCube3D"
    expiries = [{"n": n, "unit": u} for n, u in ((1, "Months"), (1, "Years"), (5, "Years"),
                                                 (10, "Years"))]
    tenors = [{"n": n, "unit": "Years"} for n in (5, 10, 20)]
    return {
        "id": constant_surface["id"],
        "payload_type": "SwaptionVolSpec",
        "payload": {
            "swap_index_id": wrapper["swap_index_id"],
            "payload_type": "SwaptionVolSmileCubeSpec",
            "payload": {
                "base": base,
                "expiries": expiries,
                "tenors": tenors,
                "strikes": strikes,
                "strike_kind": "Absolute",
                "vols": {
                    "n_1": len(expiries), "n_2": len(tenors), "n_3": len(strikes),
                    "values": [vol] * (len(expiries) * len(tenors) * len(strikes)),
                },
            },
        },
    }


def test_cms_linear_tsr_on_a_flat_absolute_cube_prices_at_the_flat_vol(client, data_dir):
    """An absolute-strike cube carries no ATM level, so LinearTsr replicates
    around the swap rate: a flat cube prices the CMS leg like the constant
    vol, whatever its strike grid (here two grids with middle strikes 4% and
    1.5%, both away from the ~3% swap rates)."""
    request = load_json(data_dir / "ir_swaps/cms_eur_5y_payer_cms10y_floored_lineartsr.json")
    surfaces = request["pricing"]["volatility"]["vol_surfaces"]
    assert len(surfaces) == 1
    assert surfaces[0]["payload"]["payload_type"] == "SwaptionVolConstantSpec"

    constant = _row(client, "vanilla_swap", request)["legs"][1]["npv"]
    cube_npvs = []
    for strikes in ([0.01, 0.02, 0.04, 0.05, 0.06], [0.005, 0.01, 0.015, 0.02, 0.025]):
        cube = copy.deepcopy(request)
        cube["pricing"]["volatility"]["vol_surfaces"] = [_flat_absolute_cube(surfaces[0], strikes)]
        cube_npvs.append(_row(client, "vanilla_swap", cube)["legs"][1]["npv"])

    assert cube_npvs[0] == pytest.approx(cube_npvs[1], rel=1e-12), cube_npvs
    for npv in cube_npvs:
        assert npv == pytest.approx(constant, rel=1e-6), (npv, constant)


# ---------------------------------------------------------------------------
# CMS leg on forward-aware swaption vols (SpreadFromATM cube, SABR)
# ---------------------------------------------------------------------------

def _cms_two_curve_request(data_dir, pricer):
    """The floored CMS example with coupon pricer `pricer` and the CMS leg
    projected off its own forwarding curve (the discount curve's quotes
    + 50bp), so the ATM forwards of a forward-aware surface are only right
    when computed on both of the leg's curves."""
    request = load_json(data_dir / "ir_swaps/cms_eur_5y_payer_cms10y_floored_lineartsr.json")
    curves = request["pricing"]["rates"]["curves"]
    forwarding = copy.deepcopy(next(c for c in curves if c["id"] == "discount"))
    forwarding["id"] = "forwarding"
    for point in forwarding["points"]:
        point["point"]["rate"] += 0.005
    curves.append(forwarding)
    floating = _swap(request, "vanilla_swap")["floating_leg"]
    floating["forwarding_curve"] = "forwarding"
    floating["coupons"]["pricer"]["pricer_type"] = pricer
    return request


def _cms_leg_npv(client, request, surface=None):
    """NPV of the CMS (floating) leg of `request`, on `surface` if given."""
    if surface is not None:
        request = copy.deepcopy(request)
        request["pricing"]["volatility"]["vol_surfaces"] = [surface]
    return _row(client, "vanilla_swap", request)["legs"][1]["npv"]


def _nodes_on_fixings(client, request):
    """A surface grid with a node on every CMS fixing after the as-of date:
    each fixing as `n` business days of the vol calendar (Calendar::advance
    by n Days lands on it) and the CMS swap tenor as the only tenor. Also
    returns the node times on the vol day counter."""
    priced = copy.deepcopy(request)
    priced["include_flows"] = True
    flows = _row(client, "vanilla_swap", priced)["legs"][1]["flows"]
    base = request["pricing"]["volatility"]["vol_surfaces"][0]["payload"]["payload"]["base"]
    calendar = get_calendar(base["calendar"])
    day_counter = get_day_counter(base["day_counter"])
    as_of = parse_date(request["pricing"]["as_of_date"])
    fixings = sorted({parse_date(f["fixing_date"]) for f in flows if f.get("fixing_date")})
    fixings = [d for d in fixings if d > as_of]
    assert len(fixings) == 19, fixings
    expiries = [{"n": calendar.businessDaysBetween(as_of, d), "unit": "Days"} for d in fixings]
    tenor = copy.deepcopy(_swap(request, "vanilla_swap")["floating_leg"]["coupons"]["swap_tenor"])
    times = [day_counter.yearFraction(as_of, d) for d in fixings]
    return expiries, [tenor], times


def _forward_aware_surface(constant_surface, payload_type, grids):
    """`constant_surface` re-expressed as a `payload_type` surface with the
    same id, swap index and conventions, plus the payload's `grids`."""
    wrapper = constant_surface["payload"]
    base = copy.deepcopy(wrapper["payload"]["base"])
    base.pop("constant_vol")
    base.pop("shape", None)
    return {
        "id": constant_surface["id"],
        "payload_type": "SwaptionVolSpec",
        "payload": {
            "swap_index_id": wrapper["swap_index_id"],
            "payload_type": payload_type,
            "payload": dict(base=base, **grids),
        },
    }


@pytest.mark.parametrize("pricer", ["LinearTsr", "HaganAnalytic"])
def test_cms_leg_on_a_flat_spread_from_atm_cube_prices_at_the_flat_vol(client, data_dir, pricer):
    """A SpreadFromATM cube is built on ATM forwards the engine computes for
    its swap index, here on the CMS leg's curves. Flat at the constant vol
    (20%) with a node on every fixing, it prices the leg like that constant
    vol: Hagan reads only variances (20% at any strike), and LinearTsr also
    takes the smile's ATM level as its forward, which on a node is the fair
    rate of the very swap the coupon fixes on (same swap index, same
    curves). What is left is round-off (none here: the prices are equal),
    hence rel 1e-9; a forward off by 0.1bp moves the LinearTsr leg by 7e-5."""
    request = _cms_two_curve_request(data_dir, pricer)
    constant_surface = request["pricing"]["volatility"]["vol_surfaces"][0]
    vol = constant_surface["payload"]["payload"]["base"]["constant_vol"]
    expiries, tenors, _ = _nodes_on_fixings(client, request)
    strikes = [-0.01, 0.0, 0.01]
    cube = _forward_aware_surface(constant_surface, "SwaptionVolSmileCubeSpec", {
        "expiries": expiries,
        "tenors": tenors,
        "strikes": strikes,
        "strike_kind": "SpreadFromATM",
        "vols": {"n_1": len(expiries), "n_2": 1, "n_3": len(strikes),
                 "values": [vol] * (len(expiries) * len(strikes))},
    })

    constant = _cms_leg_npv(client, request)
    assert _cms_leg_npv(client, request, cube) == pytest.approx(constant, rel=1e-9)


@pytest.mark.parametrize("pricer", ["LinearTsr", "HaganAnalytic"])
def test_cms_leg_on_a_near_lognormal_sabr_surface_prices_near_the_flat_vol(
        client, data_dir, pricer):
    """SabrParams on the same nodes with beta 1, rho 0 and nu 0.1%. Hagan's
    lognormal expansion (QuantLib's sabrVolatility) then reads

        sigma(K) = alpha * z / asinh(z) * (1 + nu^2 T / 12),  z = nu / alpha * ln(F / K),

    so alpha = 20% / (1 + nu^2 T / 12) puts every node's ATM vol at 20%, and
    off the money the smile lifts by a relative z / asinh(z) - 1 <= z^2 / 6.
    On the strikes that carry the leg (within 5 standard deviations,
    |ln(F / K)| <= 5 alpha sqrt(T)) that is at most eps = 25 nu^2 T_max / 6
    (2e-5 here). The leg moves at most like sigma^2 with the vol (the CMS
    convexity; the floor is closer to linear), so by less than 2 eps of its
    value: the tolerance."""
    request = _cms_two_curve_request(data_dir, pricer)
    constant_surface = request["pricing"]["volatility"]["vol_surfaces"][0]
    vol = constant_surface["payload"]["payload"]["base"]["constant_vol"]
    expiries, tenors, times = _nodes_on_fixings(client, request)
    nu = 0.001

    def per_node(values):
        values = list(values)
        return {"n_rows": len(values), "n_cols": 1, "values": values}

    sabr = _forward_aware_surface(constant_surface, "SwaptionSabrParamsSpec", {
        "expiries": expiries,
        "tenors": tenors,
        "alpha": per_node(vol / (1.0 + nu * nu * t / 12.0) for t in times),
        "beta": per_node(1.0 for _ in times),
        "rho": per_node(0.0 for _ in times),
        "nu": per_node(nu for _ in times),
    })

    eps = 25.0 * nu * nu * max(times) / 6.0
    constant = _cms_leg_npv(client, request)
    assert _cms_leg_npv(client, request, sabr) == pytest.approx(constant, rel=2.0 * eps)


@pytest.mark.parametrize("pricer", ["LinearTsr", "HaganAnalytic"])
def test_cms_leg_on_a_sabr_fit_to_flat_quotes_prices_near_the_flat_vol(client, data_dir, pricer):
    """SabrCalibrate (beta fixed at 1) fitted to flat 20% quotes on the same
    nodes, plus a 20Y tenor (the QuantLib cube needs two; the 10Y CMS tenor
    stays a node). The fit, read back from /calibrate-swaption-vol, has its
    ATM vols within `atm` of 20% and small nu and rho. To first order in rho,
    Hagan's expansion lifts the smile by at most |rho| z / 2 + z^2 / 6 with
    z = 5 nu sqrt(T_max) on the strikes that carry the leg, so the bound of
    the SabrParams test applies to that lift."""
    request = _cms_two_curve_request(data_dir, pricer)
    constant_surface = request["pricing"]["volatility"]["vol_surfaces"][0]
    vol = constant_surface["payload"]["payload"]["base"]["constant_vol"]
    expiries, tenors, times = _nodes_on_fixings(client, request)
    tenors.append({"n": 20, "unit": "Years"})
    spreads = [-0.01, -0.005, 0.0, 0.005, 0.01]
    sabr = _forward_aware_surface(constant_surface, "SwaptionSabrCalibrateSpec", {
        "expiries": expiries,
        "tenors": tenors,
        "strikes": spreads,
        "vols": {"n_1": len(expiries), "n_2": len(tenors), "n_3": len(spreads),
                 "values": [vol] * (len(expiries) * len(tenors) * len(spreads))},
        "beta_fixed": True,
        "beta_value": 1.0,
    })
    pricing = copy.deepcopy(request["pricing"])
    pricing["volatility"]["vol_surfaces"] = [sabr]
    fit = client.price("calibrate_swaption_vol", {
        "pricing": pricing, "vol_id": sabr["id"],
        "discounting_curve_id": "discount", "forwarding_curve_id": "forwarding",
    })["diagnostics"]
    atm = max(abs(v / vol - 1.0) for v in fit["atm_vol_per_node"])
    z = 5.0 * max(fit["nu_per_node"]) * math.sqrt(max(times))
    lift = atm + max(abs(r) for r in fit["rho_per_node"]) * z / 2.0 + z * z / 6.0
    assert lift < 1e-5, fit

    constant = _cms_leg_npv(client, request)
    assert _cms_leg_npv(client, request, sabr) == pytest.approx(constant, rel=2.0 * lift)


# ---------------------------------------------------------------------------
# 400s: every rule of the leg model names the field
# ---------------------------------------------------------------------------

def _estr_index_def(data_dir):
    """The ESTR overnight IndexDef of the OIS fixture, to plant an overnight
    index into another request."""
    ois = load_json(data_dir / "ir_swaps/ois_swap_request.json")
    return next(i for i in ois["pricing"]["rates"]["indices"] if i["id"] == "ESTR")


def _ibor_on_fixed_leg(req, data_dir):
    """The fixed leg re-tagged IborCoupons with the floating leg's
    kind-specific table (index + spread); the shared fields stay on the leg."""
    swap = _swap(req, "vanilla_swap")
    floating = swap["floating_leg"]["coupons"]
    swap["fixed_leg"]["coupons_type"] = "IborCoupons"
    swap["fixed_leg"]["coupons"] = {"index": copy.deepcopy(floating["index"]),
                                   "spread": floating["spread"]}
    return req


def _both_pay(req, data_dir):
    _swap(req, "vanilla_swap")["floating_leg"]["pay_receive"] = "Pay"
    return req


def _two_currencies(req, data_dir):
    _swap(req, "vanilla_swap")["floating_leg"]["currency"] = "USD"
    return req


def _forwarding_on_fixed(req, data_dir):
    _swap(req, "vanilla_swap")["fixed_leg"]["forwarding_curve"] = "discount"
    return req


def _no_forwarding_on_floating(req, data_dir):
    _swap(req, "vanilla_swap")["floating_leg"].pop("forwarding_curve")
    return req


def _ibor_coupons_on_overnight_index(req, data_dir):
    req["pricing"]["rates"]["indices"].append(_estr_index_def(data_dir))
    _swap(req, "vanilla_swap")["floating_leg"]["coupons"]["index"]["id"] = "ESTR"
    return req


def _notionals_on_ois(req, data_dir):
    """Per-period `notionals` sit on the leg; only the vanilla swap takes them."""
    _swap(req, "ois_swap")["fixed_leg"]["notionals"] = [1000000.0] * 5
    return req


def _payment_lag_on_cms_leg(req, data_dir):
    """A CMS leg pays on its schedule dates: a leg-level lag is a 400."""
    _swap(req, "vanilla_swap")["floating_leg"]["payment_lag"] = 2
    return req


def _payment_calendar_on_cms_leg(req, data_dir):
    _swap(req, "vanilla_swap")["floating_leg"]["payment_calendar"] = "TARGET"
    return req


def _ois_fixed_leg_without_payment_lag(req, data_dir):
    """price-ois-swap requires `payment_lag` and `payment_calendar` on BOTH
    legs (no silent default on an OIS)."""
    _swap(req, "ois_swap")["fixed_leg"].pop("payment_lag")
    return req


def _ois_overnight_leg_without_payment_calendar(req, data_dir):
    _swap(req, "ois_swap")["overnight_leg"].pop("payment_calendar")
    return req


def _swaption_legs_on_two_discount_curves(req, data_dir):
    underlying = req["swaptions"][0]["swaption"]["underlying"]
    underlying["floating_leg"]["discounting_curve"] = "another_curve"
    return req


def _swaption_underlying_with_exchange(req, data_dir):
    req["swaptions"][0]["swaption"]["underlying"]["fixed_leg"]["initial_exchange"] = True
    return req


# (id, product, fixture, mutation, substrings the 400 message must contain)
REJECTIONS = [
    ("ibor_coupons_on_fixed_leg", "vanilla_swap", "ir_swaps/vanilla_swap_request.json",
     _ibor_on_fixed_leg,
     ["VanillaSwap.fixed_leg.coupons must be FixedCoupons", "got IborCoupons"]),
    ("both_legs_pay", "vanilla_swap", "ir_swaps/vanilla_swap_request.json",
     _both_pay,
     ["VanillaSwap.fixed_leg.pay_receive", "VanillaSwap.floating_leg.pay_receive",
      "both Pay"]),
    ("two_currencies_on_vanilla", "vanilla_swap", "ir_swaps/vanilla_swap_request.json",
     _two_currencies,
     ["VanillaSwap.fixed_leg.currency", "VanillaSwap.floating_leg.currency",
      "must be equal on price-vanilla-swap"]),
    ("forwarding_curve_on_fixed_leg", "vanilla_swap", "ir_swaps/vanilla_swap_request.json",
     _forwarding_on_fixed,
     ["VanillaSwap.fixed_leg.forwarding_curve must be absent with FixedCoupons"]),
    ("missing_forwarding_curve_on_floating_leg", "vanilla_swap",
     "ir_swaps/vanilla_swap_request.json", _no_forwarding_on_floating,
     ["VanillaSwap.floating_leg.forwarding_curve is required with IborCoupons"]),
    ("ibor_coupons_on_overnight_index", "vanilla_swap", "ir_swaps/vanilla_swap_request.json",
     _ibor_coupons_on_overnight_index,
     ["VanillaSwap.floating_leg.coupons is IborCoupons", "ESTR", "OvernightCoupons"]),
    ("notionals_on_ois_leg", "ois_swap", "ir_swaps/ois_swap_request.json",
     _notionals_on_ois,
     ["OisSwap.fixed_leg.notionals (price-ois-swap)"]),
    ("payment_lag_on_cms_leg", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json", _payment_lag_on_cms_leg,
     ["VanillaSwap.floating_leg.payment_lag is not supported with CmsCoupons"]),
    ("payment_calendar_on_cms_leg", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json", _payment_calendar_on_cms_leg,
     ["VanillaSwap.floating_leg.payment_calendar is not supported with CmsCoupons"]),
    ("ois_fixed_leg_without_payment_lag", "ois_swap", "ir_swaps/ois_swap_request.json",
     _ois_fixed_leg_without_payment_lag,
     ["OisSwap.fixed_leg.payment_lag is required"]),
    ("ois_overnight_leg_without_payment_calendar", "ois_swap", "ir_swaps/ois_swap_request.json",
     _ois_overnight_leg_without_payment_calendar,
     ["OisSwap.overnight_leg.payment_calendar is required"]),
    ("swaption_legs_on_two_discount_curves", "swaption", "swaption/swaption_request.json",
     _swaption_legs_on_two_discount_curves,
     ["Swaption.underlying.fixed_leg.discounting_curve", "must be equal"]),
    ("swaption_underlying_initial_exchange", "swaption", "swaption/swaption_request.json",
     _swaption_underlying_with_exchange,
     ["Swaption.underlying.fixed_leg.initial_exchange must be absent or false"]),
]


@pytest.mark.parametrize(
    "product,fixture,mutate,expected",
    [(r[1], r[2], r[3], r[4]) for r in REJECTIONS],
    ids=[r[0] for r in REJECTIONS],
)
def test_leg_model_rejections_are_400s_naming_the_field(
        client, data_dir, product, fixture, mutate, expected):
    request = mutate(load_json(data_dir / fixture), data_dir)
    status, body = _post_status(client, product, request)
    assert status == 400, f"expected 400, got {status} :: {body[:300]}"
    for fragment in expected:
        assert fragment in body, f"400 body should mention {fragment!r} :: {body[:300]}"
