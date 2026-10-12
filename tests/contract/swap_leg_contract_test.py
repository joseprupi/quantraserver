"""The `SwapLeg` contract of the single-currency swap endpoints (0.10.0).

price-vanilla-swap, price-ois-swap and price-basis-swap all take two
`SwapLeg`s (pay_receive, currency, the shared notional / schedule / day
counter / payment conventions on the leg, coupons_type + the kind-specific
coupons table, discounting_curve, forwarding_curve, exchange flags) and
answer with one `SwapLegResult` per leg in request order. These tests pin,
end to end against the running JSON API:

  - the response shape: `legs` of length 2 whose signed NPVs sum to `npv`,
    `npv_reporting == npv` on a one-currency swap, and the fair value a swap
    solves for (`fair_rate` on fixed-vs-floating, `fair_spread` on basis);
  - the sign convention: flipping both `pay_receive` values negates `npv`
    and both leg NPVs;
  - the basis `fair_spread` lands on the leg that carries the request's
    spread, and repricing that leg at it zeroes the swap;
  - principal exchanges on a vanilla leg (`initial_exchange` /
    `final_exchange`) are priced as `NotionalExchange` flows and match a
    QuantLib reference built from the same request;
  - a leg-level `payment_lag` / `payment_calendar` on an IBOR leg moves the
    payment dates and matches the QuantLib reference;
  - the 400s the leg model promises, each naming the offending field.
"""

import copy

import pytest

from ql_reference import load_json, swap_legs_ql

# Role-named leg fields of each endpoint, in `legs` order.
LEG_ROLES = {
    "vanilla_swap": ("fixed_leg", "floating_leg"),
    "ois_swap": ("fixed_leg", "overnight_leg"),
    "basis_swap": ("leg1", "leg2"),
}

FIXTURES = {
    "vanilla_swap": "vanilla_swap_request.json",
    "ois_swap": "ois_swap_request.json",
    "basis_swap": "basis_swap_request.json",
}

# The fair value each endpoint solves for, and the one it must NOT report.
FAIR_FIELD = {
    "vanilla_swap": ("fair_rate", "fair_spread"),
    "ois_swap": ("fair_rate", "fair_spread"),
    "basis_swap": ("fair_spread", "fair_rate"),
}


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

    present, absent = FAIR_FIELD[product]
    assert present in row, f"{product}: response lacks {present}: {row}"
    assert absent not in row, f"{product}: response must not carry {absent}: {row}"


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
    present = FAIR_FIELD[product][0]
    assert row[present] == pytest.approx(ref[present], abs=1e-10), (row[present], ref[present])


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
    # The fair value is a property of the trade, not of the side.
    present = FAIR_FIELD[product][0]
    assert other[present] == pytest.approx(row[present], abs=1e-12)


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
    plain = load_json(data_dir / "vanilla_swap_request.json")
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
    plain = load_json(data_dir / "vanilla_swap_request.json")
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
# 400s: every rule of the leg model names the field
# ---------------------------------------------------------------------------

def _estr_index_def(data_dir):
    """The ESTR overnight IndexDef of the OIS fixture, to plant an overnight
    index into another request."""
    ois = load_json(data_dir / "ois_swap_request.json")
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
    ("ibor_coupons_on_fixed_leg", "vanilla_swap", "vanilla_swap_request.json",
     _ibor_on_fixed_leg,
     ["VanillaSwap.fixed_leg.coupons must be FixedCoupons", "got IborCoupons"]),
    ("both_legs_pay", "vanilla_swap", "vanilla_swap_request.json",
     _both_pay,
     ["VanillaSwap.fixed_leg.pay_receive", "VanillaSwap.floating_leg.pay_receive",
      "both Pay"]),
    ("two_currencies_on_vanilla", "vanilla_swap", "vanilla_swap_request.json",
     _two_currencies,
     ["VanillaSwap.fixed_leg.currency", "VanillaSwap.floating_leg.currency",
      "must be equal on price-vanilla-swap"]),
    ("forwarding_curve_on_fixed_leg", "vanilla_swap", "vanilla_swap_request.json",
     _forwarding_on_fixed,
     ["VanillaSwap.fixed_leg.forwarding_curve must be absent with FixedCoupons"]),
    ("missing_forwarding_curve_on_floating_leg", "vanilla_swap",
     "vanilla_swap_request.json", _no_forwarding_on_floating,
     ["VanillaSwap.floating_leg.forwarding_curve is required with IborCoupons"]),
    ("ibor_coupons_on_overnight_index", "vanilla_swap", "vanilla_swap_request.json",
     _ibor_coupons_on_overnight_index,
     ["VanillaSwap.floating_leg.coupons is IborCoupons", "ESTR", "OvernightCoupons"]),
    ("notionals_on_ois_leg", "ois_swap", "ois_swap_request.json",
     _notionals_on_ois,
     ["OisSwap.fixed_leg.notionals (price-ois-swap)"]),
    ("payment_lag_on_cms_leg", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json", _payment_lag_on_cms_leg,
     ["VanillaSwap.floating_leg.payment_lag is not supported with CmsCoupons"]),
    ("payment_calendar_on_cms_leg", "vanilla_swap",
     "ir_swaps/cms_eur_5y_payer_cms10y_vs_fixed_lineartsr.json", _payment_calendar_on_cms_leg,
     ["VanillaSwap.floating_leg.payment_calendar is not supported with CmsCoupons"]),
    ("ois_fixed_leg_without_payment_lag", "ois_swap", "ois_swap_request.json",
     _ois_fixed_leg_without_payment_lag,
     ["OisSwap.fixed_leg.payment_lag is required"]),
    ("ois_overnight_leg_without_payment_calendar", "ois_swap", "ois_swap_request.json",
     _ois_overnight_leg_without_payment_calendar,
     ["OisSwap.overnight_leg.payment_calendar is required"]),
    ("swaption_legs_on_two_discount_curves", "swaption", "swaption_request.json",
     _swaption_legs_on_two_discount_curves,
     ["Swaption.underlying.fixed_leg.discounting_curve", "must be equal"]),
    ("swaption_underlying_initial_exchange", "swaption", "swaption_request.json",
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
