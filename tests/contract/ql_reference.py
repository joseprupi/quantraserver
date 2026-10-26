#!/usr/bin/env python3
"""
Quantra JSON API vs QuantLib — shared reference harness.

This module is the correctness anchor for the contract/parity pytest suite
(tests/contract/). It holds:

  * ApiClient, which POSTs the example requests to the JSON API verbatim
    (they are stored in the canonical nested schema). A read-only
    _reference_pricing_view() flattens the nested groups for the QuantLib
    reference pricers only — it is never used for an API POST.
  * The get_*(...) QuantLib-enum converters and index/curve builders
    (build_ibor_index, build_overnight_index, build_curve_from_json, ...).
  * The independent per-product QuantLib reference pricers
    (price_fixed_rate_bond_ql ... price_cds_ql) whose NPVs the API is
    compared against — these mirror the legacy monolith (the vanilla swap
    pricer additionally honours the request's floating IndexDef and a
    separate forwarding curve, matching the server) and MUST NOT be
    weakened.
  * _make_multicurve_exogenous_request(), the inline 2-curve bootstrap
    request reused by the bootstrap-curves contract tests.

The pytest test modules (e.g. bond_test.py, swaption_test.py) import the
pricers/helpers from here; conftest.py exposes the client/data-dir fixtures.
"""

import bisect
import collections
import contextlib
import functools
import inspect
import json
import argparse
import math
import requests
import sys
import re
from pathlib import Path
from typing import Tuple, Optional, Dict, Any

try:
    import QuantLib as ql
except ImportError:
    print("ERROR: QuantLib not found. Install with: pip install QuantLib")
    exit(1)


# QuantLib's Null<Natural>() as seen through the Python bindings: Null<int>
# (INT_MAX) cast to Natural. Passing this value for lookbackDays reproduces
# the C++ "no lookback" off-state; a literal 0 does NOT (it forces the fixing
# delay to 0 even when the index carries a non-zero intrinsic fixing delay).
QL_NULL_NATURAL = 2147483647


# =============================================================================
# API Client
# =============================================================================

class ApiClient:
    @staticmethod
    def _load_endpoints() -> Dict[str, str]:
        # This module lives at <repo>/tests/contract/ql_reference.py, so the
        # repo root is parents[2].
        header = Path(__file__).resolve().parents[2] / "src" / "transport" / "product_catalog.h"
        pattern = re.compile(
            r'^\s*X\(\s*\w+,\s*"(?P<key>[^"]+)",\s*"(?P<route>[^"]+)",\s*"[^"]+",\s*"[^"]+"\s*\)'
        )
        endpoints: Dict[str, str] = {}
        with open(header, "r") as f:
            for raw_line in f:
                line = raw_line.strip().rstrip("\\")
                match = pattern.match(line)
                if match:
                    entry = match.groupdict()
                    endpoints[entry["key"]] = entry["route"]
        if not endpoints:
            raise RuntimeError(f"Could not parse endpoint catalog from {header}")
        return endpoints

    ENDPOINTS = _load_endpoints.__func__()
    
    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip('/')
        self.session = requests.Session()
        self.session.headers.update({'Content-Type': 'application/json'})
    
    def health(self) -> bool:
        try:
            return self.session.get(f"{self.base_url}/health").status_code == 200
        except:
            return False
    
    def price(self, product: str, request: dict) -> dict:
        endpoint = self.ENDPOINTS[product]
        # Example payloads are stored in the canonical nested schema, so they are
        # POSTed verbatim — exactly what a real client sends.
        r = self.session.post(f"{self.base_url}/{endpoint}", json=request)
        if r.status_code != 200:
            raise Exception(f"API error ({r.status_code}): {r.text[:200]}")
        return r.json()


def _reference_pricing_view(request: dict) -> dict:
    """Flat read-only view of a request's pricing for the QuantLib reference pricers.

    The example payloads are stored (and POSTed) in the canonical nested shape
    (pricing.rates.*, pricing.volatility.*, pricing.credit.*, ...). The
    independent QuantLib reference pricers below read the legacy flat keys
    (pricing["curves"], pricing.get("vol_surfaces"), ...). This merges the
    nested domain groups back up to the top level so the reference pricers see
    exactly the collections they expect. It is NEVER used for an API POST — the
    server only ever receives the verbatim nested request.
    """
    pricing = request.get("pricing", request)
    if not isinstance(pricing, dict):
        return pricing
    flat = dict(pricing)
    for group in ("rates", "credit", "volatility", "equity", "inflation", "options"):
        section = pricing.get(group)
        if isinstance(section, dict):
            for key, value in section.items():
                flat.setdefault(key, value)
    return flat


def _period_n_unit(container: dict, key: str = "tenor", default_n: int = 0, default_unit: str = "Days"):
    # Canonical Period object
    if "n" in container and "unit" in container:
        return int(container.get("n", default_n)), container.get("unit", default_unit)
    p = container.get(key)
    if isinstance(p, dict):
        return int(p.get("n", default_n)), p.get("unit", default_unit)
    return int(container.get(f"{key}_number", default_n)), container.get(f"{key}_time_unit", default_unit)


# =============================================================================
# JSON Helpers
# =============================================================================

def load_json(filepath: Path) -> dict:
    with open(filepath) as f:
        return json.load(f)


def parse_date(date_str: str) -> ql.Date:
    """Parse an ISO-8601 date string 'YYYY-MM-DD' to a QuantLib Date."""
    parts = date_str.split('-')
    return ql.Date(int(parts[2]), int(parts[1]), int(parts[0]))


def get_day_counter(name: str) -> ql.DayCounter:
    mapping = {
        "Actual360": ql.Actual360(),
        "Actual365Fixed": ql.Actual365Fixed(),
        "ActualActualISDA": ql.ActualActual(ql.ActualActual.ISDA),
        "ActualActualBond": ql.ActualActual(ql.ActualActual.Bond),
        "Thirty360": ql.Thirty360(ql.Thirty360.BondBasis),
    }
    return mapping.get(name, ql.Actual365Fixed())


def get_frequency(name: str):
    mapping = {
        "Annual": ql.Annual,
        "Semiannual": ql.Semiannual,
        "Quarterly": ql.Quarterly,
        "Monthly": ql.Monthly,
    }
    return mapping.get(name, ql.Annual)


def get_convention(name: str):
    mapping = {
        "ModifiedFollowing": ql.ModifiedFollowing,
        "Following": ql.Following,
        "Preceding": ql.Preceding,
        "Unadjusted": ql.Unadjusted,
    }
    return mapping.get(name, ql.ModifiedFollowing)


def get_calendar(name: str):
    mapping = {
        "TARGET": ql.TARGET(),
        # Mirrors the server's CalendarToQL: QuantLib::UnitedKingdom()
        # (default Settlement market).
        "UnitedKingdom": ql.UnitedKingdom(),
        # Mirrors the server's CalendarToQL: the plain "UnitedStates" enum maps
        # to the Settlement market (not NYSE).
        "UnitedStates": ql.UnitedStates(ql.UnitedStates.Settlement),
        "UnitedStatesSettlement": ql.UnitedStates(ql.UnitedStates.Settlement),
        "UnitedStatesNYSE": ql.UnitedStates(ql.UnitedStates.NYSE),
        "UnitedStatesGovernmentBond": ql.UnitedStates(ql.UnitedStates.GovernmentBond),
        "Japan": ql.Japan(),
        # Mirrors the server's CalendarToQL: every day is a business day, so
        # a `Days` period advances calendar days (the vol surfaces returned by
        # /calibrate-vol-surface are keyed this way).
        "NullCalendar": ql.NullCalendar(),
    }
    return mapping.get(name, ql.TARGET())


def get_date_generation(name: str):
    mapping = {
        "Forward": ql.DateGeneration.Forward,
        "Backward": ql.DateGeneration.Backward,
        "ThirdWednesday": ql.DateGeneration.ThirdWednesday,
        "TwentiethIMM": ql.DateGeneration.TwentiethIMM,
    }
    return mapping.get(name, ql.DateGeneration.Forward)


def get_time_unit(name: str):
    mapping = {
        "Days": ql.Days,
        "Weeks": ql.Weeks,
        "Months": ql.Months,
        "Years": ql.Years,
    }
    return mapping.get(name, ql.Days)


def get_rate_averaging(name: str):
    mapping = {
        "Compound": ql.RateAveraging.Compound,
        "Simple": ql.RateAveraging.Simple,
    }
    return mapping.get(name, ql.RateAveraging.Compound)


def get_settlement_method(name: str):
    mapping = {
        "PhysicalOTC": ql.Settlement.PhysicalOTC,
        "PhysicalCleared": ql.Settlement.PhysicalCleared,
        "CollateralizedCashPrice": ql.Settlement.CollateralizedCashPrice,
        "ParYieldCurve": ql.Settlement.ParYieldCurve,
    }
    return mapping.get(name, ql.Settlement.PhysicalOTC)


def get_volatility_type(name: str):
    mapping = {
        "Normal": ql.Normal,
        "Lognormal": ql.ShiftedLognormal,
        "ShiftedLognormal": ql.ShiftedLognormal,
    }
    return mapping.get(name, ql.ShiftedLognormal)


def get_compounding(name: str):
    mapping = {
        "Compounded": ql.Compounded,
        "Continuous": ql.Continuous,
        "Simple": ql.Simple,
        "SimpleThenCompounded": ql.SimpleThenCompounded,
    }
    return mapping.get(name, ql.Continuous)


def get_interpolator(name: str):
    mapping = {
        "Linear": ql.Linear(),
        "LogLinear": ql.LogLinear(),
        "BackwardFlat": ql.BackwardFlat(),
        "ForwardFlat": ql.ForwardFlat(),
        "LogCubic": ql.MonotonicLogCubic(),
    }
    return mapping.get(name, ql.Linear())


def get_currency(code: str):
    mapping = {
        "USD": ql.USDCurrency(),
        "EUR": ql.EURCurrency(),
        "GBP": ql.GBPCurrency(),
        "JPY": ql.JPYCurrency(),
        "CHF": ql.CHFCurrency(),
    }
    return mapping.get(code, ql.USDCurrency())


def get_cds_engine_type(name: str):
    mapping = {
        "MidPoint": "MidPoint",
        "ISDA": "ISDA",
    }
    return mapping.get(name, "MidPoint")


def get_cds_helper_model(name: str):
    mapping = {
        "MidPoint": ql.CreditDefaultSwap.Midpoint,
        "ISDA": ql.CreditDefaultSwap.ISDA,
    }
    return mapping.get(name, ql.CreditDefaultSwap.Midpoint)


def get_isda_numerical_fix(name: str):
    none_fix = getattr(ql.IsdaCdsEngine, "None", ql.IsdaCdsEngine.Taylor)
    mapping = {
        "None": none_fix,
        "Taylor": ql.IsdaCdsEngine.Taylor,
    }
    return mapping.get(name, ql.IsdaCdsEngine.Taylor)


def get_isda_accrual_bias(name: str):
    mapping = {
        "HalfDayBias": ql.IsdaCdsEngine.HalfDayBias,
        "NoBias": ql.IsdaCdsEngine.NoBias,
    }
    return mapping.get(name, ql.IsdaCdsEngine.HalfDayBias)


def get_isda_forwards_in_coupon_period(name: str):
    mapping = {
        "Flat": ql.IsdaCdsEngine.Flat,
        "Piecewise": ql.IsdaCdsEngine.Piecewise,
    }
    return mapping.get(name, ql.IsdaCdsEngine.Piecewise)


def build_ibor_index(idx_def: dict, curve_handle=None):
    tenor_n, tenor_u = _period_n_unit(idx_def, "tenor", 6, "Months")
    period = ql.Period(tenor_n, get_time_unit(tenor_u))
    fixing_days = idx_def.get("fixing_days", 2)
    calendar = get_calendar(idx_def.get("calendar", "TARGET"))
    bdc = get_convention(idx_def.get("business_day_convention", "ModifiedFollowing"))
    eom = idx_def.get("end_of_month", False)
    day_counter = get_day_counter(idx_def.get("day_counter", "Actual360"))
    ccy = get_currency(idx_def.get("currency", "USD"))
    name = idx_def.get("name", idx_def.get("id", "Ibor"))
    handle = curve_handle if curve_handle else ql.YieldTermStructureHandle()
    return ql.IborIndex(name, period, fixing_days, ccy, calendar, bdc, eom, day_counter, handle)


def build_overnight_index(idx_def: dict, curve_handle=None):
    fixing_days = idx_def.get("fixing_days", 0)
    calendar = get_calendar(idx_def.get("calendar", "TARGET"))
    day_counter = get_day_counter(idx_def.get("day_counter", "Actual360"))
    ccy = get_currency(idx_def.get("currency", "USD"))
    name = idx_def.get("name", idx_def.get("id", "ON"))
    handle = curve_handle if curve_handle else ql.YieldTermStructureHandle()
    return ql.OvernightIndex(name, fixing_days, ccy, calendar, day_counter, handle)


# =============================================================================
# Curve Building
# =============================================================================


def find_index_def(idx_id, request_data):
    """Find an IndexDef in the request's indices array by id."""
    pricing = _reference_pricing_view(request_data)
    indices = pricing.get("indices", [])
    for idef in indices:
        if idef.get("id") == idx_id:
            return idef
    # Also check top-level indices (for bootstrap requests)
    for idef in request_data.get("indices", []):
        if idef.get("id") == idx_id:
            return idef
    return None


def resolve_index_from_id(idx_id, request_data, curve_handle=None):
    """Resolve an IndexRef id to a QuantLib index for QuantLib-side comparison."""
    idx_def = find_index_def(idx_id, request_data)
    if idx_def:
        if idx_def.get("index_type") == "Overnight":
            return build_overnight_index(idx_def, curve_handle)
        return build_ibor_index(idx_def, curve_handle)

    # Fallback for legacy ids
    if "3M" in idx_id:
        return ql.Euribor3M()
    if "6M" in idx_id:
        return ql.Euribor6M()
    return ql.Euribor6M()


class _InterpDiscountCurveRef:
    """Faithful stand-in for QuantLib's ``InterpolatedDiscountCurve<Interp>`` for
    the interpolators QuantLib-python does not expose as a discount-curve class
    (Linear, BackwardFlat, ForwardFlat).

    It reproduces the C++ curve's algorithm exactly on QuantLib primitives: the
    discount factors are interpolated in curve time (``discountImpl`` ==
    ``interpolation_(t, true)``) and zero/forward rates come from
    ``InterestRate::impliedRate`` on the resulting discount factors — the same
    path ``YieldTermStructure::zeroRate``/``forwardRate`` take. Because it uses
    QuantLib's own interpolation and implied-rate machinery it matches the server
    curve to machine precision. Only the methods the curve-sampling reference
    calls are implemented; grids stay within the pillar span (the server curve
    does not enable extrapolation), so no extrapolation branch is needed.
    """

    def __init__(self, dates, dfs, day_counter, interp_name):
        self._ref = dates[0]
        self._dc = day_counter
        self._times = [day_counter.yearFraction(self._ref, d) for d in dates]
        self._dfs = list(dfs)
        if interp_name == "Linear":
            self._interp = ql.LinearInterpolation(self._times, self._dfs)
        elif interp_name == "BackwardFlat":
            self._interp = ql.BackwardFlatInterpolation(self._times, self._dfs)
        elif interp_name == "ForwardFlat":
            self._interp = ql.ForwardFlatInterpolation(self._times, self._dfs)
        else:
            raise ValueError(
                f"_InterpDiscountCurveRef: unsupported interpolator {interp_name}")
        # Extrapolation is requested per-call via the interpolation's
        # allowExtrapolation flag (the ``True`` second argument below), matching
        # InterpolatedDiscountCurve::discountImpl.

    def referenceDate(self):
        return self._ref

    def dayCounter(self):
        return self._dc

    def discount(self, d):
        t = d if isinstance(d, float) else self._dc.yearFraction(self._ref, d)
        return self._interp(t, True)

    def zeroRate(self, d, dc, comp, freq):
        compound = 1.0 / self.discount(d)
        t = dc.yearFraction(self._ref, d)
        return ql.InterestRate.impliedRate(compound, dc, comp, freq, t)

    def forwardRate(self, d1, d2, dc, comp, freq):
        compound = self.discount(d1) / self.discount(d2)
        t = dc.yearFraction(d1, d2)
        return ql.InterestRate.impliedRate(compound, dc, comp, freq, t)


def _build_discount_factor_curve(curve_json: dict, points: list, eval_date: ql.Date):
    """Build a genuine discount-factor-interpolated curve for the
    ``InterpolatedDiscount`` bootstrap trait.

    Interpolates the discount factors directly (NOT a zero-from-DF conversion),
    matching the server's ``InterpolatedDiscountCurve`` with the same
    interpolator: LogLinear -> ``ql.DiscountCurve``, LogCubic ->
    ``ql.MonotonicLogCubicDiscountCurve`` (both native QuantLib-python classes),
    and Linear/BackwardFlat/ForwardFlat via ``_InterpDiscountCurveRef`` (a
    faithful replica, since QuantLib-python exposes no such class).
    """
    ref = (parse_date(curve_json["reference_date"])
           if curve_json.get("reference_date") else eval_date)
    dates = []
    dfs = []
    for point_wrapper in points:
        if point_wrapper["point_type"] != "DiscountFactorPoint":
            raise ValueError(
                "DiscountFactorPoint cannot be mixed with other point types")
        point = point_wrapper["point"]
        if "date" in point and point["date"]:
            d = parse_date(point["date"])
        else:
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
            tenor_unit = get_time_unit(tenor_unit_s)
            cal = get_calendar(point.get("calendar", "TARGET"))
            bdc = get_convention(point.get("business_day_convention", "ModifiedFollowing"))
            d = cal.advance(ref, ql.Period(tenor_num, tenor_unit), bdc)
        dates.append(d)
        dfs.append(point["discount_factor"])

    day_counter = get_day_counter(curve_json.get("day_counter", "Actual365Fixed"))
    interp = curve_json.get("interpolator", "LogLinear")

    if interp == "LogLinear":
        curve = ql.DiscountCurve(dates, dfs, day_counter)
        curve.enableExtrapolation()
        return ql.YieldTermStructureHandle(curve)
    if interp == "LogCubic":
        curve = ql.MonotonicLogCubicDiscountCurve(dates, dfs, day_counter)
        curve.enableExtrapolation()
        return ql.YieldTermStructureHandle(curve)
    # Linear / BackwardFlat / ForwardFlat: no native QuantLib-python class.
    return _InterpDiscountCurveRef(dates, dfs, day_counter, interp)


def _build_forward_rate_curve(curve_json: dict, points: list, eval_date: ql.Date):
    """Build a genuine forward-rate-interpolated curve for the ``InterpolatedFwd``
    bootstrap trait.

    Interpolates the INSTANTANEOUS, continuously-compounded forward rates
    directly (NOT a zero/DF conversion), matching the server's
    ``InterpolatedForwardCurve<Interp>``. QuantLib-python exposes native
    forward-curve classes for every interpolator the server supports for this
    trait, so no replica is needed: Linear -> ``ql.LinearForwardCurve``,
    BackwardFlat -> ``ql.ForwardCurve`` (the ``InterpolatedForwardCurve<BackwardFlat>``
    typedef), ForwardFlat -> ``ql.ForwardFlatForwardCurve``. The server rejects
    LogLinear/LogCubic for this trait (QuantLib cannot integrate a
    log-interpolated forward), so they never reach the reference.
    """
    ref = (parse_date(curve_json["reference_date"])
           if curve_json.get("reference_date") else eval_date)
    dates = []
    fwds = []
    for point_wrapper in points:
        if point_wrapper["point_type"] != "ForwardRatePoint":
            raise ValueError(
                "ForwardRatePoint cannot be mixed with other point types")
        point = point_wrapper["point"]
        if "date" in point and point["date"]:
            d = parse_date(point["date"])
        else:
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
            tenor_unit = get_time_unit(tenor_unit_s)
            cal = get_calendar(point.get("calendar", "TARGET"))
            bdc = get_convention(point.get("business_day_convention", "ModifiedFollowing"))
            d = cal.advance(ref, ql.Period(tenor_num, tenor_unit), bdc)
        dates.append(d)
        fwds.append(point["forward_rate"])

    day_counter = get_day_counter(curve_json.get("day_counter", "Actual365Fixed"))
    interp = curve_json.get("interpolator", "Linear")

    if interp == "Linear":
        curve = ql.LinearForwardCurve(dates, fwds, day_counter)
    elif interp == "BackwardFlat":
        curve = ql.ForwardCurve(dates, fwds, day_counter)
    elif interp == "ForwardFlat":
        curve = ql.ForwardFlatForwardCurve(dates, fwds, day_counter)
    else:
        raise ValueError(
            f"InterpolatedFwd reference: unsupported interpolator {interp}")
    curve.enableExtrapolation()
    return ql.YieldTermStructureHandle(curve)


def _joint_calendar(names: list):
    """Mirror of the server's FxSpotSpec.settlement_calendars handling: one
    calendar is used as is, several are joined so that a holiday in any one is
    a holiday of the pair (QuantLib JointCalendar, JoinHolidays)."""
    calendars = [get_calendar(n) for n in names]
    if not calendars:
        raise ValueError("FxSpotSpec.settlement_calendars must list at least one calendar")
    joint = calendars[0]
    for cal in calendars[1:]:
        joint = ql.JointCalendar(joint, cal, ql.JoinHolidays)
    return joint


def build_fx_spot(pricing: dict, spot_id: str, request_data: dict = None) -> dict:
    """Resolve an FxSpotSpec from pricing.fx.spots into the plain values the
    engine derives from it (src/decode/market/fx_spot_parser.cpp):

      {"base": str, "quote": str, "rate": float, "spot_days": int,
       "calendar": ql.Calendar (joint), "bdc": ql BusinessDayConvention}

    `rate` is the spot for delivery on the spot date (quote units per base
    unit); a quote_id resolves through pricing.quotes and must carry the Fx
    quote type, exactly as on the server. `pricing` may be the raw nested
    request pricing or the flat view."""
    fx = (pricing or {}).get("fx") or {}
    spec = next((s for s in fx.get("spots", []) if s.get("id") == spot_id), None)
    if spec is None:
        raise ValueError(f"FX spot not found: {spot_id}")
    if "rate" in spec:
        rate = float(spec["rate"])
    else:
        # pricing.quotes sits at the top of the pricing block in both the
        # nested request and the flat reference view.
        quotes = pricing.get("quotes", [])
        q = next((q for q in quotes if q.get("id") == spec.get("quote_id")), None)
        if q is None:
            raise ValueError(f"FxSpotSpec.quote_id: unknown quote id {spec.get('quote_id')}")
        if q.get("quote_type", "Curve") != "Fx":
            raise ValueError(f"FxSpotSpec.quote_id: quote {q['id']} must have quote_type Fx")
        rate = float(q["value"])
    return {
        "base": spec["base_currency"],
        "quote": spec["quote_currency"],
        "rate": rate,
        "spot_days": int(spec["spot_days"]),
        "calendar": _joint_calendar(spec["settlement_calendars"]),
        "bdc": get_convention(spec["business_day_convention"]),
    }


def build_curve_from_json(curve_json: dict, eval_date: ql.Date, request_data: dict = None) -> ql.YieldTermStructureHandle:
    """Build QuantLib curve from JSON curve definition."""
    points = curve_json.get("points", [])
    quote_values = {}
    quote_types = {}
    if request_data:
        pricing = request_data.get("pricing", {})
        for q in pricing.get("quotes", []):
            if "id" in q:
                quote_values[q["id"]] = q.get("value", 0.0)
                quote_types[q["id"]] = q.get("quote_type", "Curve")

    def resolve_point_rate(point: dict) -> float:
        quote_id = point.get("quote_id")
        if quote_id:
            if quote_types.get(quote_id, "Curve") != "Curve":
                raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
            return quote_values.get(quote_id, 0.0)
        return point["rate"]

    def resolve_bond_quote(point: dict) -> float:
        # Mirrors the server's presence-based selection: price wins over rate.
        quote_id = point.get("quote_id")
        if quote_id:
            if quote_types.get(quote_id, "Curve") != "Curve":
                raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
            return quote_values.get(quote_id, 0.0)
        if "price" in point:
            return point["price"]
        return point["rate"]

    def resolve_future_price(point: dict) -> float:
        # Mirrors the server: FuturesRateHelper's quote is the futures PRICE
        # (0-100 IMM scale). futures_price wins over rate; a quote_id resolves
        # to the price directly. rate is sugar via price = 100*(1-rate).
        quote_id = point.get("quote_id")
        if quote_id:
            if quote_types.get(quote_id, "Curve") != "Curve":
                raise ValueError(f"Quote id '{quote_id}' has wrong type for curve")
            return quote_values.get(quote_id, 0.0)
        if "futures_price" in point:
            return point["futures_price"]
        return 100.0 * (1.0 - point["rate"])
    # Dispatch on the explicit bootstrap_trait, exactly as the server does. The
    # InterpolatedDiscount trait interpolates discount factors directly, giving a
    # genuinely different curve than interpolating zero rates.
    if curve_json.get("bootstrap_trait") == "InterpolatedDiscount":
        return _build_discount_factor_curve(curve_json, points, eval_date)
    # The InterpolatedFwd trait interpolates instantaneous forward rates
    # directly, giving a genuinely different curve than interpolating zero rates
    # or discount factors.
    if curve_json.get("bootstrap_trait") == "InterpolatedFwd":
        return _build_forward_rate_curve(curve_json, points, eval_date)
    if any(p.get("point_type") == "ZeroRatePoint" for p in points):
        dates = []
        rates = []
        compounding = ql.Continuous
        frequency = ql.Annual
        comp_set = False

        for point_wrapper in points:
            if point_wrapper["point_type"] != "ZeroRatePoint":
                raise ValueError("ZeroRatePoint cannot be mixed with bootstrap helpers")
            point = point_wrapper["point"]
            if "date" in point and point["date"]:
                d = parse_date(point["date"])
            else:
                tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
                tenor_unit = get_time_unit(tenor_unit_s)
                cal = get_calendar(point.get("calendar", "TARGET"))
                bdc = get_convention(point.get("business_day_convention", "ModifiedFollowing"))
                d = cal.advance(eval_date, ql.Period(tenor_num, tenor_unit), bdc)
            dates.append(d)
            rates.append(point["zero_rate"])

            if not comp_set:
                compounding = get_compounding(point.get("compounding", "Continuous"))
                frequency = get_frequency(point.get("frequency", "Annual"))
                comp_set = True

        day_counter = get_day_counter(curve_json.get("day_counter", "Actual365Fixed"))
        interpolator = get_interpolator(curve_json.get("interpolator", "Linear"))
        curve = ql.ZeroCurve(dates, rates, day_counter, get_calendar(curve_json.get("calendar", "TARGET")),
                             interpolator, compounding, frequency)
        curve.enableExtrapolation()
        return ql.YieldTermStructureHandle(curve)

    helpers = []
    seen_pillars = set()  # Track pillar dates to avoid duplicates

    for point_wrapper in points:
        point_type = point_wrapper["point_type"]
        point = point_wrapper["point"]
        
        if point_type == "DepositHelper":
            tenor_num, tenor_str = _period_n_unit(point, "tenor", 0, "Days")
            if tenor_str == "Weeks":
                tenor_unit = ql.Weeks
            elif tenor_str == "Months":
                tenor_unit = ql.Months
            else:
                tenor_unit = ql.Years
            
            helper = ql.DepositRateHelper(
                resolve_point_rate(point),
                ql.Period(tenor_num, tenor_unit),
                point.get("fixing_days", 2),
                get_calendar(point.get("calendar", "TARGET")),
                get_convention(point.get("business_day_convention", "ModifiedFollowing")),
                True,
                get_day_counter(point.get("day_counter", "Actual365Fixed"))
            )
            
            # Check for duplicate pillar
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
        
        elif point_type == "FutureHelper":
            # QuantLib FuturesRateHelper: quote is the futures PRICE, convexity
            # is its own argument (mirrors src/decode/market/term_structure_point_parser.cpp).
            helper = ql.FuturesRateHelper(
                ql.QuoteHandle(ql.SimpleQuote(resolve_future_price(point))),
                parse_date(point["future_start_date"]),
                point.get("future_months", 3),
                get_calendar(point.get("calendar", "TARGET")),
                get_convention(point.get("business_day_convention", "ModifiedFollowing")),
                True,
                get_day_counter(point.get("day_counter", "Actual360")),
                ql.QuoteHandle(ql.SimpleQuote(point.get("convexity_adjustment", 0.0))),
            )

            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)

        elif point_type == "SwapHelper":
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Years")
            tenor_unit = ql.Years if tenor_unit_s == "Years" else ql.Months
            index = ql.Euribor6M()
            if request_data and point.get("float_index", {}).get("id"):
                index = resolve_index_from_id(point["float_index"]["id"], request_data)
            
            helper = ql.SwapRateHelper(
                resolve_point_rate(point),
                ql.Period(tenor_num, tenor_unit),
                get_calendar(point.get("calendar", "TARGET")),
                get_frequency(point.get("sw_fixed_leg_frequency", "Annual")),
                get_convention(point.get("sw_fixed_leg_convention", "ModifiedFollowing")),
                get_day_counter(point.get("sw_fixed_leg_day_counter", "Thirty360")),
                index
            )
            
            # Check for duplicate pillar
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
        
        elif point_type == "OISHelper":
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
            tenor_unit = get_time_unit(tenor_unit_s)
            overnight_idx = None
            if request_data and point.get("overnight_index", {}).get("id"):
                overnight_idx = resolve_index_from_id(point["overnight_index"]["id"], request_data)
            if overnight_idx is None:
                overnight_idx = ql.OvernightIndex(
                    "ON", 0, ql.USDCurrency(), ql.TARGET(), ql.Actual360()
                )
            # Mirror the server's OISRateHelper wiring: fixed_leg_convention /
            # fixed_leg_frequency / calendar feed the PAYMENT convention /
            # frequency / calendar slots, and the payment-lag and
            # overnight-coupon parameters are applied verbatim. Wire
            # lookback_days == 0 means "no lookback" = QuantLib Null<Natural>,
            # NOT a zero-day lookback (the two differ whenever the index has a
            # non-zero intrinsic fixing delay).
            lookback = int(point.get("lookback_days", 0))
            helper = ql.OISRateHelper(
                point.get("settlement_days", 2),
                ql.Period(tenor_num, tenor_unit),
                resolve_point_rate(point),
                overnight_idx,
                telescopicValueDates=False,
                paymentLag=int(point.get("payment_lag", 0)),
                paymentConvention=get_convention(point.get("fixed_leg_convention", "Following")),
                paymentFrequency=get_frequency(point.get("fixed_leg_frequency", "Annual")),
                paymentCalendar=get_calendar(point.get("calendar", "TARGET")),
                averagingMethod=get_rate_averaging(point.get("averaging_method", "Compound")),
                lookbackDays=(QL_NULL_NATURAL if lookback <= 0 else lookback),
                lockoutDays=int(point.get("lockout_days", 0)),
                applyObservationShift=bool(point.get("apply_observation_shift", False)),
            )
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
        
        elif point_type == "FxSwapHelper":
            # Mirrors src/decode/market/term_structure_point_parser.cpp: the spot
            # supplies the pair, the spot rate, the lag (fixingDays), the joint
            # settlement calendar and the convention; the collateral currency's
            # discount curve is another curve of the same request, built here
            # recursively (the server orders the bootstrap by that reference).
            # Forward points are curve quotes (quote_id -> Curve type).
            req_pricing = (request_data or {}).get("pricing", {})
            spot = build_fx_spot(req_pricing, point["fx_spot"], request_data)
            collateral_ccy = point["collateral_currency"]
            if collateral_ccy == spot["base"]:
                base_is_collateral = True
            elif collateral_ccy == spot["quote"]:
                base_is_collateral = False
            else:
                raise ValueError(
                    f"FxSwapHelper.collateral_currency {collateral_ccy} is neither "
                    f"currency of FX spot {point['fx_spot']}")
            coll_id = point["collateral_curve"]["id"]
            coll_json = next(c for c in _reference_pricing_view(request_data)["curves"]
                             if c["id"] == coll_id)
            collateral = build_curve_from_json(coll_json, eval_date, request_data)
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
            points_value = (quote_values[point["quote_id"]] if point.get("quote_id")
                            else point["fx_points"])
            if point.get("quote_id") and quote_types.get(point["quote_id"], "Curve") != "Curve":
                raise ValueError(f"Quote id '{point['quote_id']}' has wrong type for curve")
            helper = ql.FxSwapRateHelper(
                ql.QuoteHandle(ql.SimpleQuote(points_value)),
                ql.QuoteHandle(ql.SimpleQuote(spot["rate"])),
                ql.Period(tenor_num, get_time_unit(tenor_unit_s)),
                spot["spot_days"],
                spot["calendar"],
                spot["bdc"],
                bool(point["end_of_month"]),
                base_is_collateral,
                collateral,
            )
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)

        elif point_type == "CrossCcyBasisHelper":
            # Mirrors src/decode/market/term_structure_point_parser.cpp: the spot
            # names the pair and supplies the settlement lag (fixingDays) but
            # its rate never enters a constant-notional bootstrap; both legs'
            # forwarding curves and the collateral side's discount curve are
            # other curves of the request (built here recursively, ordered on
            # the server by the references); the leg whose discount curve is
            # absent is discounted with the curve being bootstrapped. The
            # basis is a curve quote (quote_id -> Curve type). An overnight
            # index needs payment_frequency; two IBOR indices pay at their
            # tenors (QuantLib NoFrequency).
            req_pricing = (request_data or {}).get("pricing", {})
            spot = build_fx_spot(req_pricing, point["fx_spot"], request_data)
            view_curves = _reference_pricing_view(request_data)["curves"]

            def curve_by_id(cid):
                cj = next(c for c in view_curves if c["id"] == cid)
                return build_curve_from_json(cj, eval_date, request_data)

            base_fwd = curve_by_id(point["base_forwarding_curve"]["id"])
            quote_fwd = curve_by_id(point["quote_forwarding_curve"]["id"])
            base_idx = resolve_index_from_id(point["base_index"]["id"], request_data, base_fwd)
            quote_idx = resolve_index_from_id(point["quote_index"]["id"], request_data, quote_fwd)
            base_given = "base_discount_curve" in point
            quote_given = "quote_discount_curve" in point
            if base_given == quote_given:
                raise ValueError("CrossCcyBasisHelper requires exactly one of "
                                 "base_discount_curve or quote_discount_curve")
            collateral = curve_by_id((point["base_discount_curve"] if base_given
                                      else point["quote_discount_curve"])["id"])
            if point.get("quote_id"):
                if quote_types.get(point["quote_id"], "Curve") != "Curve":
                    raise ValueError(f"Quote id '{point['quote_id']}' has wrong type for curve")
                spread_value = quote_values[point["quote_id"]]
            else:
                spread_value = point["spread"]
            tenor_num, tenor_unit_s = _period_n_unit(point, "tenor", 0, "Days")
            frequency = (get_frequency(point["payment_frequency"])
                         if "payment_frequency" in point else ql.NoFrequency)
            helper = ql.ConstNotionalCrossCurrencyBasisSwapRateHelper(
                ql.QuoteHandle(ql.SimpleQuote(spread_value)),
                ql.Period(tenor_num, get_time_unit(tenor_unit_s)),
                spot["spot_days"],
                get_calendar(point["calendar"]),
                get_convention(point["business_day_convention"]),
                bool(point["end_of_month"]),
                base_idx,
                quote_idx,
                collateral,
                base_given,
                bool(point["spread_on_base_leg"]),
                frequency,
                int(point["payment_lag"]),
            )
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)

        elif point_type == "BondHelper":
            # Build bond schedule
            sch = point["schedule"]
            schedule = ql.Schedule(
                parse_date(sch["effective_date"]),
                parse_date(sch["termination_date"]),
                ql.Period(get_frequency(sch["frequency"])),
                get_calendar(sch.get("calendar", "TARGET")),
                get_convention(sch.get("convention", "Unadjusted")),
                get_convention(sch.get("termination_date_convention", "Unadjusted")),
                get_date_generation(sch.get("date_generation_rule", "Backward")),
                False
            )
            
            # Create fixed rate bond helper
            helper = ql.FixedRateBondHelper(
                ql.QuoteHandle(ql.SimpleQuote(resolve_bond_quote(point))),  # Clean price
                point.get("settlement_days", 3),
                point.get("face_amount", 100.0),
                schedule,
                [point["coupon_rate"]],
                get_day_counter(point.get("day_counter", "ActualActualBond")),
                get_convention(point.get("business_day_convention", "Unadjusted")),
                point.get("redemption", 100.0),
                parse_date(point.get("issue_date", sch["effective_date"]))
            )
            
            # Check for duplicate pillar
            pillar = helper.pillarDate()
            if pillar not in seen_pillars:
                helpers.append(helper)
                seen_pillars.add(pillar)
    
    if not helpers:
        # Fallback to flat curve
        return ql.YieldTermStructureHandle(
            ql.FlatForward(eval_date, 0.03, ql.Actual365Fixed())
        )
    
    # The server bootstraps every piecewise yield curve to an accuracy of
    # 1e-15 (src/decode/market/term_structure_parser.cpp, buildCurve); QuantLib's
    # default (1e-12) leaves the discount factors a few ulps apart, which
    # the structured-note standard errors amplify (a variance is a
    # difference of near-equal sums) to ~1e-11 relative.
    curve = ql.PiecewiseLogLinearDiscount(
        eval_date, helpers, get_day_counter(curve_json.get("day_counter", "Actual365Fixed")),
        ql.IterativeBootstrap(accuracy=1.0e-15))
    curve.enableExtrapolation()
    # Remember the helpers' own pillar dates (the server reports them as
    # pillar_dates); see curve_pillar_dates_ql.
    if curve_json.get("id"):
        _LAST_HELPER_PILLARS[curve_json["id"]] = sorted(seen_pillars)
    return ql.YieldTermStructureHandle(curve)


# curve id -> pillar dates of the helpers of the last build_curve_from_json
# call for that curve (the reference twin of the server's pillar_dates).
_LAST_HELPER_PILLARS: Dict[str, list] = {}


def curve_pillar_dates_ql(request: dict) -> list:
    """Reference for the `pillar_dates` of a /bootstrap-curves result: the
    curve's reference date followed by each QuantLib helper's pillarDate()
    (what the server reports, sorted, as ISO strings). The curve is built
    with build_curve_from_json so the dates are the helpers' own, not a
    re-derivation."""
    pricing = _reference_pricing_view(request)
    as_of = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = as_of
    query = request["queries"][0]
    curve_json = next(c for c in pricing["curves"] if c["id"] == query["curve_id"])
    build_curve_from_json(curve_json, as_of, request)
    ref = (parse_date(curve_json["reference_date"]) if curve_json.get("reference_date")
           else as_of)
    dates = sorted({ref, *_LAST_HELPER_PILLARS[curve_json["id"]]})
    return [_iso_date(d) for d in dates]


# =============================================================================
# QuantLib Pricing Functions
# =============================================================================

def _schedule_stub_dates(sch: dict):
    """Mirror the server's ScheduleParser stub-period control: an optional
    first_date / next_to_last_date, ISO-8601, passed straight into QuantLib's
    Schedule firstDate / nextToLastDate arguments. Absent -> ql.Date(), which is
    the constructor's own default (no stub), so parity holds for legacy cases."""
    first_date = parse_date(sch["first_date"]) if sch.get("first_date") else ql.Date()
    next_to_last = (parse_date(sch["next_to_last_date"])
                    if sch.get("next_to_last_date") else ql.Date())
    return first_date, next_to_last


def price_fixed_rate_bond_ql(request: dict) -> float:
    """Price fixed rate bond using QuantLib."""
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["fixed_rate_bond"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build curve
    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Build schedule
    sch = bond["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        # Mirrors the server's ScheduleParser: schedule->end_of_month()
        # (FlatBuffers schema default false).
        sch.get("end_of_month", False),
        # Optional stub-period control (firstDate / nextToLastDate).
        *_schedule_stub_dates(sch)
    )

    # Build bond. Mirrors the server's FixedRateBondParser, which passes
    # payment_convention, redemption and issue_date straight into the
    # QuantLib::FixedRateBond constructor. The fallbacks are the FlatBuffers
    # wire defaults (payment_convention=Following, redemption=0.0); issue_date
    # is effectively required on the wire (the server dereferences it).
    issue_date = bond.get("issue_date")
    ql_bond = ql.FixedRateBond(
        bond.get("settlement_days", 2),
        bond.get("face_amount", 100.0),
        schedule,
        [bond["rate"]],
        get_day_counter(bond.get("accrual_day_counter", "Thirty360")),
        get_convention(bond.get("payment_convention", "Following")),
        bond.get("redemption", 0.0),
        parse_date(issue_date) if issue_date else ql.Date()
    )
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(curve))
    
    return ql_bond.NPV()


def price_callable_fixed_rate_bond_ql(request: dict) -> float:
    """Price a callable (or puttable) fixed-rate bond using QuantLib.

    Mirrors the server's CallableFixedRateBondEvaluator: build the same
    QuantLib::CallableFixedRateBond with an identical CallabilitySchedule (clean
    call/put prices, Bond::Price::Clean), resolve the short-rate model by id from
    pricing.volatility.models (a SwaptionModelSpec of model_type
    HullWhiteLattice) exactly as the swaption reference does, and price on a
    TreeCallableFixedRateBondEngine over the discount curve with the request's
    tree_steps. Only param_mode=Explicit is reproduced here (the catalog cases
    use explicit Hull-White parameters for deterministic parity); Calibrate would
    require the swaption calibration routine replayed here.
    """
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["callable_fixed_rate_bond"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)

    sch = bond["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        sch.get("end_of_month", False)
    )

    # Build the call schedule with clean prices per 100 (Bond::Price::Clean),
    # mirroring CallableFixedRateBondParser.
    call_schedule = ql.CallabilitySchedule()
    for entry in bond["call_schedule"]:
        price = ql.BondPrice(entry["price"], ql.BondPrice.Clean)
        ctype = (ql.Callability.Call if entry["callability_type"] == "Call"
                 else ql.Callability.Put)
        call_schedule.push_back(ql.Callability(price, ctype, parse_date(entry["date"])))

    ql_bond = ql.CallableFixedRateBond(
        bond.get("settlement_days", 2),
        bond.get("face_amount", 100.0),
        schedule,
        [bond["rate"]],
        get_day_counter(bond.get("accrual_day_counter", "Thirty360")),
        get_convention(bond.get("payment_convention", "Following")),
        bond.get("redemption", 100.0),
        parse_date(bond["issue_date"]),
        call_schedule
    )

    # Resolve the short-rate model by id (same mechanism as the swaption).
    model_spec = {}
    for m in pricing.get("models", []):
        if m.get("id") == bond_data.get("model"):
            model_spec = m.get("payload", {})
            break
    if model_spec.get("model_type") != "HullWhiteLattice":
        raise ValueError(
            "callable bond reference requires a HullWhiteLattice model")
    if model_spec.get("param_mode", "Explicit") != "Explicit":
        raise ValueError(
            "callable bond reference only supports HullWhiteLattice with "
            "param_mode=Explicit")

    hw_model = ql.HullWhite(curve,
                            model_spec.get("hw_a", 0.03),
                            model_spec.get("hw_sigma", 0.01))
    tree_steps = int(bond_data.get("tree_steps", 400))
    ql_bond.setPricingEngine(
        ql.TreeCallableFixedRateBondEngine(hw_model, tree_steps, curve))

    return ql_bond.NPV()


def price_floating_rate_bond_ql(request: dict) -> float:
    """Price floating rate bond using QuantLib."""
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["floating_rate_bond"]
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    
    # Build discount curve
    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    discount_curve = build_curve_from_json(curve_json, eval_date, request)
    
    # Build forwarding curve (may be different)
    forward_id = bond_data.get("forwarding_curve", curve_id)
    forward_json = next((c for c in pricing["curves"] if c["id"] == forward_id), curve_json)
    forward_curve = build_curve_from_json(forward_json, eval_date, request)
    
    # Build schedule
    sch = bond["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        # Mirrors the server's ScheduleParser: schedule->end_of_month()
        # (FlatBuffers schema default false).
        sch.get("end_of_month", False)
    )

    # Get index info
    idx_ref = bond.get("index", {})
    idx_id = idx_ref.get("id", "EUR_6M") if isinstance(idx_ref, dict) else "EUR_6M"

    # Resolve the index exactly as the server does: its IndexRegistryBuilder
    # constructs a generic IborIndex from the request's IndexDef (name, tenor,
    # fixing days, currency, calendar, convention, end-of-month, day count),
    # projecting off the forwarding curve. Falls back to the Euribor family
    # when the request carries no IndexDef (legacy requests).
    idx_def = find_index_def(idx_id, request)
    if idx_def:
        index = build_ibor_index(idx_def, forward_curve)
    else:
        period_months = 6
        if "3M" in idx_id:
            period_months = 3
        if period_months == 3:
            index = ql.Euribor3M(forward_curve)
        else:
            index = ql.Euribor6M(forward_curve)

    # Add any fixings from the IndexDef
    if idx_def:
        for fixing in idx_def.get("fixings", []):
            fixing_date = parse_date(fixing["date"])
            index.addFixing(fixing_date, fixing["value"])
    
    # Build floating rate bond
    ql_bond = ql.FloatingRateBond(
        bond.get("settlement_days", 2),
        bond.get("face_amount", 100.0),
        schedule,
        index,
        get_day_counter(bond.get("accrual_day_counter", "Actual360")),
        get_convention(bond.get("payment_convention", "ModifiedFollowing")),
        bond.get("fixing_days", 2),
        [1.0],  # gearings
        [bond.get("spread", 0.0)],  # spreads
        [],  # caps
        [],  # floors
        bond.get("in_arrears", False),
        bond.get("redemption", 100.0),
        parse_date(bond.get("issue_date", sch["effective_date"]))
    )
    
    # Set up coupon pricer for floating rate coupons
    pricer = ql.BlackIborCouponPricer()
    volatility = ql.ConstantOptionletVolatility(
        bond.get("settlement_days", 2),
        get_calendar((idx_def or {}).get("calendar", "TARGET")),
        get_convention((idx_def or {}).get("business_day_convention", "ModifiedFollowing")),
        0.0,  # zero volatility for simple pricing
        ql.Actual365Fixed()
    )
    pricer.setCapletVolatility(ql.OptionletVolatilityStructureHandle(volatility))
    ql.setCouponPricer(ql_bond.cashflows(), pricer)
    
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(discount_curve))
    
    return ql_bond.NPV()


# =============================================================================
# Swap legs — the `SwapLeg` model shared by price-vanilla-swap, price-ois-swap,
# price-basis-swap and the swaption underlying (0.10.0)
# =============================================================================
#
# Every single-currency swap endpoint is two `SwapLeg`s under role-named
# fields (VanillaSwap.fixed_leg / floating_leg, OisSwap.fixed_leg /
# overnight_leg, BasisSwap.leg1 / leg2). The leg is flat: the fields every
# coupon kind shares (`notional` / `notionals`, `schedule`, `day_counter`,
# `payment_convention`, `payment_calendar`, `payment_lag`) sit on the leg
# itself, as in FpML / ORE, and `coupons` (the `coupons_type` union member)
# carries only the kind-specific ones. The reference reads each leg exactly
# as the engine does (src/decode/products/swap_leg_mapper.cpp decodeShared +
# decodeFixed/Ibor/Overnight/Cms, src/evaluators/swap_leg_pricer.cpp): the
# side from `pay_receive`, the curves from the leg itself, the principal
# exchanges from `initial_exchange` / `final_exchange`. No converter sits in
# between.

_SwapLegView = collections.namedtuple(
    "_SwapLegView",
    "coupons pay discounting_curve forwarding_curve currency kind "
    "notional notionals amortizing schedule day_counter payment_convention "
    "payment_calendar payment_lag initial_exchange final_exchange")


def _swap_leg(leg: dict) -> _SwapLegView:
    """The one adapter from a wire `SwapLeg` to what the reference pricers
    read. The leg is flat: the fields every coupon kind shares sit on the
    leg itself and `coupons` holds only the kind-specific ones (the engine's
    decodeShared / decodeFixed|Ibor|Overnight|Cms split). The view carries:

      coupons             the body of the union member (`rate`; `index`,
                          `spread`, `fixing_days`, `in_arrears`; the OIS
                          conventions; the CMS fields) -- never a shared field
      pay                 True for `pay_receive == "Pay"`
      discounting_curve, forwarding_curve (None on a fixed leg), currency
      kind                = `coupons_type`
      notional            the exchanged notional (`_swap_leg_notionals`)
      notionals           the per-period vector the leg builders take (one
                          entry = constant)
      amortizing          True when the leg states a `notionals` vector
      schedule            the wire schedule dict (`_wire_schedule_from`)
      day_counter         the enum name, or None (an overnight leg may omit it)
      payment_convention  the enum name
      payment_calendar    the enum name, or None (absent = the schedule's)
      payment_lag         int, or None (absent = 0)
      initial_exchange, final_exchange (absent = false)

    A field the engine requires on every leg is read with [] here, so a
    malformed request fails loudly in the reference."""
    side = leg["pay_receive"]
    if side not in ("Pay", "Receive"):
        raise ValueError(f"SwapLeg.pay_receive must be Pay or Receive, got {side!r}")
    kind = leg["coupons_type"]
    if kind not in ("FixedCoupons", "IborCoupons", "OvernightCoupons", "CmsCoupons"):
        raise ValueError(f"SwapLeg.coupons_type not supported: {kind!r}")
    notionals, notional = _swap_leg_notionals(leg)
    lag = leg.get("payment_lag")
    return _SwapLegView(
        coupons=leg["coupons"],
        pay=(side == "Pay"),
        discounting_curve=leg["discounting_curve"],
        forwarding_curve=leg.get("forwarding_curve"),
        currency=leg["currency"],
        kind=kind,
        notional=notional,
        notionals=notionals,
        amortizing=bool(leg.get("notionals")),
        schedule=leg["schedule"],
        day_counter=leg.get("day_counter"),
        payment_convention=leg["payment_convention"],
        payment_calendar=leg.get("payment_calendar"),
        payment_lag=None if lag is None else int(lag),
        initial_exchange=bool(leg.get("initial_exchange", False)),
        final_exchange=bool(leg.get("final_exchange", False)),
    )


def _swap_leg_payment_calendar(view: _SwapLegView, schedule: ql.Schedule) -> ql.Calendar:
    """The calendar a leg's coupons are paid on: `payment_calendar` when the
    leg states one, else the schedule's own (QuantLib's and the engine's
    default: swap_leg_builder.cpp)."""
    if view.payment_calendar is not None:
        return get_calendar(view.payment_calendar)
    return schedule.calendar()


def _swap_curve_resolver(request: dict, eval_date: ql.Date):
    """`curve(id)` -> YieldTermStructureHandle built once per id from
    `pricing.rates.curves`: every leg naming the same id gets the same handle,
    as the engine's registry hands out."""
    pricing = _reference_pricing_view(request)
    built = {}

    def curve(curve_id: str):
        if curve_id not in built:
            spec = next((c for c in pricing["curves"] if c["id"] == curve_id), None)
            if spec is None:
                raise ValueError(f"curve not found: {curve_id!r}")
            built[curve_id] = build_curve_from_json(spec, eval_date, request)
        return built[curve_id]

    return curve


def _swap_leg_notionals(leg: dict):
    """(notionals vector, exchanged notional) of a wire `SwapLeg`, read as
    the engine's decodeNotionals reads them off the leg: the per-period
    `notionals` vector when present (the scalar is then optional; the
    exchanged notional is the scalar when given, else the first period's),
    the constant `notional` otherwise."""
    vector = leg.get("notionals")
    if vector:
        notionals = [float(n) for n in vector]
        constant = float(leg["notional"]) if leg.get("notional") is not None else notionals[0]
        return notionals, constant
    constant = float(leg["notional"])
    return [constant], constant


def _zero_vol_ibor_pricer(index) -> None:
    """The zero-vol Black pricer the engine attaches to every IBOR coupon leg
    (BuildIborLeg): reproduces VanillaSwap's own in-advance coupon pricing
    and, in arrears, reads the shifted fixing with no convexity adjustment."""
    vol = ql.ConstantOptionletVolatility(
        0, index.fixingCalendar(), ql.ModifiedFollowing, 0.0, ql.Actual365Fixed())
    pricer = ql.BlackIborCouponPricer()
    pricer.setCapletVolatility(ql.OptionletVolatilityStructureHandle(vol))
    return pricer


def _swap_leg_coupons(leg: dict, request: dict, forward_curve, discount_curve=None):
    """Coupon leg of one `SwapLeg`, built exactly as the engine's shared leg
    builder (src/evaluators/swap_leg_builder.cpp + swap_leg_pricer.cpp) builds
    it: the shared fields (`notional` / `notionals`, `schedule`,
    `day_counter`, `payment_convention`, `payment_calendar`, `payment_lag`)
    from the leg, the kind-specific ones from `coupons`:

      FixedCoupons      FixedRateLeg at `coupons.rate`, paid with the leg's
                        payment convention, `payment_calendar` (absent = the
                        schedule's) and `payment_lag` (absent = 0);
      IborCoupons       IborLeg with the zero-vol Black pricer, paid on the
                        same leg-level calendar / lag; `coupons.fixing_days`
                        absent = the index's own fixing days (QuantLib's
                        default, what VanillaSwap always did), present = the
                        override; `coupons.in_arrears` honoured;
      OvernightCoupons  OvernightLeg with the OIS conventions (the leg's
                        payment lag and calendar; averaging, lookback,
                        lockout, observation shift from `coupons`) -- the
                        chain QuantLib's OvernightIndexedSwap uses; the leg's
                        `day_counter` is not read (the index's own applies);
      CmsCoupons        CmsLeg on the swap index with the requested pricer
                        (`_cms_coupons`; needs `discount_curve`).

    `forward_curve` is the leg's forwarding curve handle (None for a fixed
    leg). Returns (ql.Leg, schedule, exchanged notional, rate_or_spread,
    is_fixed)."""
    view = _swap_leg(leg)
    f = view.coupons
    schedule = _wire_schedule_from(view.schedule)
    payment_calendar = _swap_leg_payment_calendar(view, schedule)
    payment_lag = view.payment_lag or 0
    conv = get_convention(view.payment_convention)
    if view.kind == "FixedCoupons":
        coupons = ql.FixedRateLeg(
            schedule, get_day_counter(view.day_counter), view.notionals, [float(f["rate"])],
            conv, paymentCalendar=payment_calendar, paymentLag=payment_lag)
        return coupons, schedule, view.notional, float(f["rate"]), True
    if view.kind == "CmsCoupons":
        if discount_curve is None:
            raise ValueError("a CMS leg needs its discounting curve (the swap index's)")
        coupons = _cms_coupons(leg, request, forward_curve, discount_curve)
        return coupons, schedule, view.notional, float(f.get("spread", 0.0)), False
    spread = float(f.get("spread", 0.0))
    idx_def = find_index_def(f["index"]["id"], request)
    if idx_def is None:
        raise ValueError(f"Index not found: {f['index']['id']}")
    if view.kind == "OvernightCoupons":
        if idx_def.get("index_type") != "Overnight":
            raise ValueError("OvernightCoupons needs an overnight index")
        index = build_overnight_index(idx_def, forward_curve)
        lookback = int(f.get("lookback_days", 0))
        coupons = ql.OvernightLeg(
            view.notionals, schedule, index, index.dayCounter(), conv, [1.0], [spread],
            bool(f.get("telescopic_value_dates", False)),
            get_rate_averaging(f.get("averaging_method", "Compound")),
            payment_calendar, payment_lag,
            QL_NULL_NATURAL if lookback <= 0 else lookback, int(f.get("lockout_days", 0)),
            bool(f.get("apply_observation_shift", False)))
        return coupons, schedule, view.notional, spread, False
    # IborCoupons
    if idx_def.get("index_type") == "Overnight":
        raise ValueError("IborCoupons needs an IBOR index")
    index = build_ibor_index(idx_def, forward_curve)
    # An absent override leaves IborLeg's fixing days unset (empty vector), so
    # every coupon fixes on the index's own fixing days.
    fixing_days = [int(f["fixing_days"])] if f.get("fixing_days") is not None else []
    coupons = ql.IborLeg(view.notionals, schedule, index, get_day_counter(view.day_counter),
                         conv, fixing_days, [1.0], [spread], [], [],
                         bool(f.get("in_arrears", False)),
                         paymentCalendar=payment_calendar, paymentLag=payment_lag)
    ql.setCouponPricer(coupons, _zero_vol_ibor_pricer(index))
    return coupons, schedule, view.notional, spread, False


# Role-named leg fields of each single-currency swap endpoint, in `legs` order.
_SWAP_LEG_ROLES = {
    "vanilla_swap": ("fixed_leg", "floating_leg"),
    "ois_swap": ("fixed_leg", "overnight_leg"),
    "basis_swap": ("leg1", "leg2"),
}


def swap_legs_ql(request: dict, index: int = 0) -> dict:
    """Price one swap of a price-vanilla-swap / price-ois-swap /
    price-basis-swap request with the leg algebra of the engine
    (src/evaluators/swap_leg_pricer.cpp and the three evaluators) on QuantLib
    curves built from the request. For leg i (coupons C_i, exchanged notional
    N_i, discounting curve D_i, own frame = as the receiver of the coupons
    sees it, s_i = -1 for pay_receive Pay / +1 for Receive):

      L_i = C_i + [initial] SimpleCashFlow(-N_i, schedule start)
                + [final]   SimpleCashFlow(+N_i, last coupon payment date)
      legs[i].npv = legs[i].npv_reporting = s_i * CashFlows.npv(L_i, D_i)
      npv         = legs[0].npv + legs[1].npv
      B_i         = s_i * CashFlows.bps(C_i, D_i)          (exchanges excluded)
      fair_rate   = r_0 - npv / (B_0 / 1e-4)       vanilla / OIS (leg 0 = fixed)
      fair_spread = q_1 - npv / (B_1 / 1e-4)       vanilla / OIS (leg 1 = floating)
      fair_spread = q_k - npv * 1e-4 / B_k         basis; k = leg1 when only
                    leg1 carries a spread, leg2 otherwise

    Every value is as of the evaluation date, flows on that date excluded (as
    QuantLib's DiscountingSwapEngine values a leg). A CMS floating leg goes
    through the same algebra (the CMS coupons held fixed for `fair_rate`, the
    CMS coupons' annuity for `fair_spread`). As on every swap endpoint,
    `fair_rate` is present iff exactly one leg is fixed and `fair_spread` iff
    a leg is floating.

    Returns {npv, legs: [{npv, npv_reporting, pv_own, flows}], fair_rate?,
    fair_spread?} with `flows` = (ISO payment date, amount, discount) of every
    future flow of the full leg, in leg order (coupons first, then the
    exchanges, as the engine lists them)."""
    pricing = _reference_pricing_view(request)
    entry = request["swaps"][index]
    product = next((k for k in _SWAP_LEG_ROLES if k in entry), None)
    if product is None:
        raise ValueError("swap_legs_ql: entry holds no vanilla_swap / ois_swap / basis_swap")
    swap = entry[product]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    curve = _swap_curve_resolver(request, eval_date)

    def build(leg: dict):
        view = _swap_leg(leg)
        disc = curve(view.discounting_curve)
        fwd = curve(view.forwarding_curve) if view.kind != "FixedCoupons" else None
        coupons, schedule, notional, level, is_fixed = _swap_leg_coupons(
            leg, request, fwd, disc)
        full = list(coupons)
        if view.initial_exchange:
            full.append(ql.SimpleCashFlow(-notional, schedule.dates()[0]))
        if view.final_exchange:
            full.append(ql.SimpleCashFlow(notional, coupons[-1].date()))
        yts = _xccy_term_structure(disc)
        sign = -1.0 if view.pay else 1.0
        pv_own = ql.CashFlows.npv(full, yts, False, eval_date, eval_date)
        signed_bps = sign * ql.CashFlows.bps(coupons, yts, False, eval_date, eval_date)
        flows = [(cf.date().ISO(), cf.amount(), yts.discount(cf.date()))
                 for cf in full if not cf.hasOccurred(eval_date)]
        return {"npv": sign * pv_own, "npv_reporting": sign * pv_own, "pv_own": pv_own,
                "flows": flows, "signed_bps": signed_bps, "level": level,
                "is_fixed": is_fixed}

    legs = [build(swap[role]) for role in _SWAP_LEG_ROLES[product]]
    npv = legs[0]["npv"] + legs[1]["npv"]
    out = {"npv": npv,
           "legs": [{k: leg[k] for k in ("npv", "npv_reporting", "pv_own", "flows")}
                    for leg in legs]}
    if product == "basis_swap":
        k = 0 if (legs[0]["level"] != 0.0 and legs[1]["level"] == 0.0) else 1
        if abs(legs[k]["signed_bps"]) > 1e-16:
            fair = legs[k]["level"] - npv * 1e-4 / legs[k]["signed_bps"]
            if math.isfinite(fair):
                out["fair_spread"] = fair
    else:
        if legs[0]["signed_bps"] != 0.0:
            fair = legs[0]["level"] - npv / (legs[0]["signed_bps"] / 1e-4)
            if math.isfinite(fair):
                out["fair_rate"] = fair
        if legs[1]["signed_bps"] != 0.0:
            fair = legs[1]["level"] - npv / (legs[1]["signed_bps"] / 1e-4)
            if math.isfinite(fair):
                out["fair_spread"] = fair
    return out


def price_vanilla_swap_ql(request: dict) -> float:
    """Price a vanilla swap (`VanillaSwap { fixed_leg, floating_leg }`) using
    QuantLib.

    The native QuantLib::VanillaSwap is used whenever it expresses the request
    exactly -- one discount curve for both legs, no principal exchange, no
    per-period notionals, in-advance fixing on the index's own fixing days,
    one notional, both legs paid on the floating schedule's convention (the
    one VanillaSwap applies to both legs), no payment calendar / lag override
    on either leg -- with the index projecting off the floating leg's
    forwarding curve and the swap discounted on the legs' discounting curve.
    Anything else (an in-arrears leg, amortizing notionals, a CMS floating
    leg, exchanges, per-leg discount curves, a fixing-days override, a
    payment lag) takes the engine's own leg algebra, `swap_legs_ql`."""
    pricing = _reference_pricing_view(request)
    swap = request["swaps"][0]["vanilla_swap"]
    fixed = _swap_leg(swap["fixed_leg"])
    floating = _swap_leg(swap["floating_leg"])

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    fc, xc = fixed.coupons, floating.coupons
    exchanges = (fixed.initial_exchange or fixed.final_exchange or
                 floating.initial_exchange or floating.final_exchange)
    if floating.kind != "IborCoupons" or exchanges:
        return swap_legs_ql(request)["npv"]
    float_schedule = _wire_schedule_from(floating.schedule)
    float_convention = float_schedule.businessDayConvention()
    native = (
        fixed.discounting_curve == floating.discounting_curve
        and not fixed.amortizing and not floating.amortizing
        and fixed.notional == floating.notional
        and not xc.get("in_arrears", False)
        and xc.get("fixing_days") is None
        and fixed.payment_calendar is None and not fixed.payment_lag
        and floating.payment_calendar is None and not floating.payment_lag
        and get_convention(fixed.payment_convention) == float_convention
        and get_convention(floating.payment_convention) == float_convention
    )
    if not native:
        return swap_legs_ql(request)["npv"]

    curve_of = _swap_curve_resolver(request, eval_date)
    curve = curve_of(fixed.discounting_curve)
    forward_curve = curve_of(floating.forwarding_curve)
    fixed_schedule = _wire_schedule_from(fixed.schedule)
    # The floating index exactly as the engine resolves it: from the request's
    # IndexDef (tenor / calendar / conventions), projecting off the leg's
    # forwarding curve.
    idx_id = xc["index"]["id"]
    if find_index_def(idx_id, request) is None:
        raise ValueError(f"Index not found: {idx_id}")
    index = resolve_index_from_id(idx_id, request, forward_curve)
    swap_type = ql.VanillaSwap.Payer if fixed.pay else ql.VanillaSwap.Receiver

    ql_swap = ql.VanillaSwap(
        swap_type,
        fixed.notional,
        fixed_schedule,
        float(fc["rate"]),
        get_day_counter(fixed.day_counter),
        float_schedule,
        index,
        float(xc.get("spread", 0.0)),
        get_day_counter(floating.day_counter)
    )
    ql_swap.setPricingEngine(ql.DiscountingSwapEngine(curve))

    return ql_swap.NPV()


# =============================================================================
# CMS swap (fixed vs constant-maturity-swap floating leg)
# =============================================================================


def _cms_frequency_to_period(freq_name: str) -> ql.Period:
    """Mirror swap_index_registry.cpp frequencyToPeriod: the swap index's fixed
    leg tenor is derived from the SwapIndexDef fixed-leg Frequency enum."""
    return {
        "Annual": ql.Period(1, ql.Years),
        "Semiannual": ql.Period(6, ql.Months),
        "Quarterly": ql.Period(3, ql.Months),
        "Monthly": ql.Period(1, ql.Months),
        "Bimonthly": ql.Period(2, ql.Months),
        "EveryFourthMonth": ql.Period(4, ql.Months),
    }.get(freq_name, ql.Period(1, ql.Years))


def _cms_gff_model(name: str):
    """Mirror cms_leg_parser.cpp toYieldCurveModel (Hagan-family only)."""
    return {
        "Standard": ql.GFunctionFactory.Standard,
        "ExactYield": ql.GFunctionFactory.ExactYield,
        "ParallelShifts": ql.GFunctionFactory.ParallelShifts,
        "NonParallelShifts": ql.GFunctionFactory.NonParallelShifts,
    }.get(name, ql.GFunctionFactory.Standard)


def _build_swaption_vol_for_cms(pricing: dict, vol_id: str, eval_date: ql.Date):
    """Build the SwaptionVolatilityStructureHandle the CMS coupon pricer reads,
    mirroring vol_surface_parsers.cpp for the Constant and AtmMatrix shapes.

    reference_date in the cataloged surfaces equals as_of_date, so the moving-
    reference (calendar-based) matrix constructor QuantLib-python exposes builds
    the identical structure as the server's fixed-reference constructor.
    """
    for v in pricing.get("vol_surfaces", []):
        if v.get("id") != vol_id:
            continue
        if v.get("payload_type") != "SwaptionVolSpec":
            raise ValueError(f"vol surface {vol_id!r} is not a SwaptionVolSpec")
        payload = v.get("payload", {})
        inner_type = payload.get("payload_type", "SwaptionVolConstantSpec")
        inner = payload.get("payload", {})
        base = inner.get("base", {})
        cal = get_calendar(base.get("calendar", "TARGET"))
        bdc = get_convention(base.get("business_day_convention", "ModifiedFollowing"))
        dc = get_day_counter(base.get("day_counter", "Actual365Fixed"))
        vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
        disp = base.get("displacement", 0.0)
        ref = parse_date(base["reference_date"]) if base.get("reference_date") else eval_date

        if inner_type == "SwaptionVolConstantSpec":
            vol = base.get("constant_vol", 0.20)
            return ql.SwaptionVolatilityStructureHandle(
                ql.ConstantSwaptionVolatility(ref, cal, bdc, vol, dc, vol_type, disp))

        if inner_type == "SwaptionVolAtmMatrixSpec":
            expiries = inner.get("expiries", [])
            tenors = inner.get("tenors", [])
            vols = inner.get("vols", {})
            n_rows = vols.get("n_rows", len(expiries))
            n_cols = vols.get("n_cols", len(tenors))
            values = vols.get("values", [])

            def to_period(p):
                num, unit = _period_n_unit(p, "tenor", 0, "Months")
                return ql.Period(num, get_time_unit(unit))

            ql_expiries = [to_period(p) for p in expiries]
            ql_tenors = [to_period(p) for p in tenors]
            matrix = ql.Matrix(n_rows, n_cols)
            for i in range(n_rows):
                for j in range(n_cols):
                    matrix[i][j] = values[i * n_cols + j]
            shifts = ql.Matrix(n_rows, n_cols, disp) if disp else ql.Matrix()
            vm = ql.SwaptionVolatilityMatrix(
                cal, bdc, ql_expiries, ql_tenors, matrix, dc, False, vol_type, shifts)
            return ql.SwaptionVolatilityStructureHandle(vm)

        raise ValueError(
            f"CMS reference supports Constant/AtmMatrix swaption vol only, "
            f"got {inner_type!r} for vol {vol_id!r}")
    raise ValueError(f"swaption vol surface not found: {vol_id!r}")


def _cms_coupons(leg: dict, request: dict, fwd_curve, disc_curve) -> ql.Leg:
    """The CmsLeg of a `CmsCoupons` swap leg, mirroring the engine
    (swap_leg_pricer.cpp buildCmsCoupons, swap_index_registry.cpp): a
    QuantLib::SwapIndex (family name = swap_index_id, fixed-leg tenor from the
    SwapIndexDef fixed frequency, IBOR leg projecting off the leg's forwarding
    curve, discounted on the leg's discounting curve) with a Hagan-family or
    LinearTsr coupon pricer fed by the request's swaption vol surface. `leg`
    is the wire `SwapLeg`: notional, schedule, day counter and payment
    convention come from the leg (a CMS leg takes no payment calendar / lag
    and one constant notional), the CMS fields from `coupons`."""
    pricing = _reference_pricing_view(request)
    eval_date = ql.Settings.instance().evaluationDate
    view = _swap_leg(leg)
    cms = view.coupons

    # Swap index (SwapIndexRegistry::getIborSwapIndexWithCurves).
    swap_index_id = cms["swap_index_id"]
    sidef = next((s for s in pricing.get("swap_indices", [])
                  if s.get("id") == swap_index_id), None)
    if sidef is None:
        raise ValueError(f"swap index not found: {swap_index_id!r}")
    spot_days = sidef.get("spot_days", 2)
    fixed_spec = sidef.get("fixed_leg", {})
    fixed_leg_tenor = _cms_frequency_to_period(fixed_spec.get("fixed_frequency", "Annual"))
    fixed_index_cal = get_calendar(fixed_spec.get("fixed_calendar", "TARGET"))
    fixed_index_bdc = get_convention(fixed_spec.get("fixed_bdc", "ModifiedFollowing"))
    fixed_index_dc = get_day_counter(fixed_spec.get("fixed_day_counter", "Thirty360"))

    ibor = resolve_index_from_id(sidef["float_index_id"], request, fwd_curve)

    tenor_n, tenor_u = _period_n_unit(cms["swap_tenor"], "tenor", 0, "Years")
    swap_tenor = ql.Period(tenor_n, get_time_unit(tenor_u))
    swap_index = ql.SwapIndex(
        swap_index_id, swap_tenor, spot_days, ibor.currency(),
        fixed_index_cal, fixed_leg_tenor, fixed_index_bdc, fixed_index_dc,
        ibor, disc_curve)

    # CMS leg (the engine's CmsLeg builder).
    cms_schedule = _wire_schedule_from(view.schedule)
    fixing_days = cms.get("fixing_days", -1)
    if fixing_days is None or fixing_days < 0:
        fixing_days = spot_days
    # Optional per-coupon cap/floor (presence, not sign, decides — mirrors the
    # server's withCaps/withFloors). Absent => empty vector => plain CMS coupons.
    caps = [cms["cap"]] if cms.get("cap") is not None else []
    floors = [cms["floor"]] if cms.get("floor") is not None else []
    cms_leg = ql.CmsLeg(
        [view.notional],
        cms_schedule,
        swap_index,
        get_day_counter(view.day_counter),
        get_convention(view.payment_convention),
        [int(fixing_days)],
        [cms.get("gear", 1.0)],
        [cms.get("spread", 0.0)],
        caps,
        floors)

    # Coupon pricer (BuildCmsPricer).
    vol_handle = _build_swaption_vol_for_cms(pricing, cms["swaption_vol_id"], eval_date)
    pricer_spec = cms.get("pricer") or {}
    pricer_type = pricer_spec.get("pricer_type", "LinearTsr")
    mr_quote = ql.QuoteHandle(ql.SimpleQuote(pricer_spec.get("mean_reversion", 0.03)))
    yc_model = _cms_gff_model(pricer_spec.get("yield_curve_model", "Standard"))

    if pricer_type == "LinearTsr":
        pricer = ql.LinearTsrPricer(vol_handle, mr_quote, disc_curve)
    elif pricer_type == "HaganAnalytic":
        pricer = ql.AnalyticHaganPricer(vol_handle, yc_model, mr_quote)
    elif pricer_type == "HaganNumeric":
        lower = pricer_spec.get("hagan_lower_limit", 0.0)
        upper = pricer_spec.get("hagan_upper_limit", 1.0)
        precision = pricer_spec.get("hagan_precision", 1.0e-6)
        # The server also carries a hard upper integration limit; when the
        # request leaves hagan_hard_upper_limit at its default (<= 0) the server
        # substitutes QL_MAX_REAL, which is exactly QuantLib's default in this
        # 6-argument constructor (QuantLib-python does not expose the 7th arg).
        pricer = ql.NumericHaganPricer(vol_handle, yc_model, mr_quote, lower, upper, precision)
    else:
        raise ValueError(f"unsupported CMS pricer type: {pricer_type!r}")
    ql.setCouponPricer(cms_leg, pricer)
    return cms_leg


def price_cms_swap_ql(request: dict) -> float:
    """Price a fixed-vs-CMS vanilla swap (`VanillaSwap.floating_leg` carrying
    `CmsCoupons`) using QuantLib: the engine's leg algebra (`swap_legs_ql`)
    with the CMS leg of `_cms_coupons`. A CMS swap reports a real `fair_rate`
    (the CMS coupons held fixed), which `swap_legs_ql` also returns."""
    swap = request["swaps"][0]["vanilla_swap"]
    if swap["floating_leg"]["coupons_type"] != "CmsCoupons":
        raise ValueError("price_cms_swap_ql: floating_leg.coupons_type must be CmsCoupons")
    return swap_legs_ql(request)["npv"]


def price_zero_coupon_bond_ql(request: dict) -> float:
    """Price a zero-coupon bond using QuantLib.

    Mirrors the server's ZeroCouponBondParser/Evaluator: a single-redemption
    QuantLib::ZeroCouponBond discounted on the request's curve. redemption
    defaults to 100.0 (par) and issue_date to a null Date when omitted, exactly
    as the parser does.
    """
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["zero_coupon_bond"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id),
                      pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)

    issue_date = bond.get("issue_date")
    ql_bond = ql.ZeroCouponBond(
        bond.get("settlement_days", 2),
        get_calendar(bond["calendar"]),
        bond["face_amount"],
        parse_date(bond["maturity_date"]),
        get_convention(bond["payment_convention"]),
        bond.get("redemption", 100.0),
        parse_date(issue_date) if issue_date else ql.Date(),
    )
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(curve))

    return ql_bond.NPV()


def price_zero_coupon_swap_ql(request: dict) -> float:
    """Price a zero-coupon swap (QuantLib::ZeroCouponSwap) using QuantLib.

    Mirrors the server's ZeroCouponSwapMapper/Evaluator: a single fixed
    cashflow against a single compounded-floating cashflow. The fixed side is
    quoted either as a known payment amount or as a fixed rate compounded over
    the contract life under its own day counter. Discounts on the discounting
    curve; the floating leg projects off the forwarding curve.
    """
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    trade = swap_data["zero_coupon_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    disc_id = swap_data["discounting_curve"]
    disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id),
                     pricing["curves"][0])
    discount_curve = build_curve_from_json(disc_json, eval_date, request)

    fwd_id = swap_data.get("forwarding_curve", disc_id)
    if fwd_id == disc_id:
        forward_curve = discount_curve
    else:
        fwd_json = next((c for c in pricing["curves"] if c["id"] == fwd_id),
                        disc_json)
        forward_curve = build_curve_from_json(fwd_json, eval_date, request)

    idx_id = trade["index"]["id"]
    index = resolve_index_from_id(idx_id, request, forward_curve)

    swap_type = ql.ZeroCouponSwap.Payer \
        if trade.get("swap_type", "Payer") == "Payer" \
        else ql.ZeroCouponSwap.Receiver

    payment_calendar = get_calendar(trade["payment_calendar"])
    payment_convention = get_convention(trade["payment_convention"])
    payment_delay = trade.get("payment_delay", 0)

    if "fixed_rate" in trade:
        swap = ql.ZeroCouponSwap(
            swap_type,
            trade["base_nominal"],
            parse_date(trade["start_date"]),
            parse_date(trade["maturity_date"]),
            trade["fixed_rate"],
            get_day_counter(trade["fixed_rate_day_counter"]),
            index,
            payment_calendar,
            payment_convention,
            payment_delay,
        )
    else:
        swap = ql.ZeroCouponSwap(
            swap_type,
            trade["base_nominal"],
            parse_date(trade["start_date"]),
            parse_date(trade["maturity_date"]),
            trade["fixed_payment"],
            index,
            payment_calendar,
            payment_convention,
            payment_delay,
        )

    swap.setPricingEngine(ql.DiscountingSwapEngine(discount_curve))

    return swap.NPV()


def _wire_schedule_from(sch: dict) -> ql.Schedule:
    """Build a QuantLib Schedule from a wire schedule dict, mirroring the
    server's ScheduleParser (same convention fallbacks used elsewhere here)."""
    return ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        sch.get("end_of_month", False),
        # Optional stub-period control (first_date / next_to_last_date).
        *_schedule_stub_dates(sch)
    )


def price_amortizing_fixed_rate_bond_ql(request: dict) -> float:
    """Price an amortizing / step-up fixed-rate bond using QuantLib.

    Mirrors the server's FixedRateBondParser amortizing path: per-period
    notionals feed QuantLib::AmortizingFixedRateBond. redemptions default to
    {100.0}, which matches the server passing {redemption} for a redemption=100
    request.
    """
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["fixed_rate_bond"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)

    schedule = _wire_schedule_from(bond["schedule"])
    issue_date = bond.get("issue_date")
    ql_bond = ql.AmortizingFixedRateBond(
        bond.get("settlement_days", 2),
        [float(n) for n in bond["notionals"]],
        schedule,
        [bond["rate"]],
        get_day_counter(bond.get("accrual_day_counter", "Thirty360")),
        get_convention(bond.get("payment_convention", "Following")),
        parse_date(issue_date) if issue_date else ql.Date(),
    )
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(curve))
    return ql_bond.NPV()


def price_amortizing_floating_rate_bond_ql(request: dict) -> float:
    """Price an amortizing / step-up floating-rate bond using QuantLib.

    Mirrors the server's amortizing FloatingRateBond path: per-period notionals
    feed QuantLib::AmortizingFloatingRateBond, a zero-vol Black coupon pricer is
    attached exactly as the constant path does, and redemptions default to
    {100.0} (matching the server's {redemption} for a redemption=100 request).
    """
    pricing = _reference_pricing_view(request)
    bond_data = request["bonds"][0]
    bond = bond_data["floating_rate_bond"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    curve_id = bond_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    discount_curve = build_curve_from_json(curve_json, eval_date, request)

    forward_id = bond_data.get("forwarding_curve", curve_id)
    forward_json = next((c for c in pricing["curves"] if c["id"] == forward_id), curve_json)
    forward_curve = build_curve_from_json(forward_json, eval_date, request)

    schedule = _wire_schedule_from(bond["schedule"])

    idx_ref = bond.get("index", {})
    idx_id = idx_ref.get("id", "EUR_6M") if isinstance(idx_ref, dict) else "EUR_6M"
    idx_def = find_index_def(idx_id, request)
    if idx_def:
        index = build_ibor_index(idx_def, forward_curve)
    else:
        index = ql.Euribor3M(forward_curve) if "3M" in idx_id else ql.Euribor6M(forward_curve)
    if idx_def:
        for fixing in idx_def.get("fixings", []):
            index.addFixing(parse_date(fixing["date"]), fixing["value"])

    ql_bond = ql.AmortizingFloatingRateBond(
        bond.get("settlement_days", 2),
        [float(n) for n in bond["notionals"]],
        schedule,
        index,
        get_day_counter(bond.get("accrual_day_counter", "Actual360")),
        get_convention(bond.get("payment_convention", "ModifiedFollowing")),
        bond.get("fixing_days", 2),
        [1.0],
        [bond.get("spread", 0.0)],
        [],
        [],
        bond.get("in_arrears", False),
        parse_date(bond.get("issue_date", bond["schedule"]["effective_date"])),
    )

    pricer = ql.BlackIborCouponPricer()
    volatility = ql.ConstantOptionletVolatility(
        bond.get("settlement_days", 2),
        get_calendar((idx_def or {}).get("calendar", "TARGET")),
        get_convention((idx_def or {}).get("business_day_convention", "ModifiedFollowing")),
        0.0,
        ql.Actual365Fixed(),
    )
    pricer.setCapletVolatility(ql.OptionletVolatilityStructureHandle(volatility))
    ql.setCouponPricer(ql_bond.cashflows(), pricer)
    ql_bond.setPricingEngine(ql.DiscountingBondEngine(discount_curve))
    return ql_bond.NPV()


def price_amortizing_vanilla_swap_ql(request: dict) -> float:
    """Price an amortizing / step-up vanilla swap using QuantLib.

    QuantLib::VanillaSwap cannot carry per-period notionals, so both legs are
    built explicitly (FixedRateLeg / IborLeg with the leg's `notionals`
    vector, a leg without one on its constant notional) and valued with the
    engine's leg algebra -- exactly the path the engine takes for every swap
    (`swap_legs_ql`)."""
    return swap_legs_ql(request)["npv"]


def price_ois_swap_ql(request: dict) -> float:
    """Price an OIS (`OisSwap { fixed_leg, overnight_leg }`) using QuantLib's
    native OvernightIndexedSwap.

    QuantLib::OvernightIndexedSwap applies ONE payment lag / calendar /
    convention to both legs, so the request's two legs must state the same
    leg-level `payment_calendar` / `payment_lag` / `payment_convention`
    (price-ois-swap requires the calendar and the lag on both legs); the
    reference passes the overnight leg's values and refuses a request where
    the two legs differ (the engine prices such a leg pair with per-leg
    conventions: see `swap_legs_ql`). Per-leg discount curves or principal
    exchanges, which the native instrument cannot carry, take `swap_legs_ql`
    as well."""
    pricing = _reference_pricing_view(request)
    swap = request["swaps"][0]["ois_swap"]
    fixed = _swap_leg(swap["fixed_leg"])
    overnight = _swap_leg(swap["overnight_leg"])
    fc, oc = fixed.coupons, overnight.coupons

    fixed_payment_calendar = fixed.payment_calendar or fixed.schedule["calendar"]
    overnight_payment_calendar = overnight.payment_calendar or overnight.schedule["calendar"]
    if (fixed_payment_calendar != overnight_payment_calendar
            or (fixed.payment_lag or 0) != (overnight.payment_lag or 0)
            or fixed.payment_convention != overnight.payment_convention):
        raise ValueError(
            "price_ois_swap_ql: QuantLib::OvernightIndexedSwap takes one payment "
            "calendar / lag / convention for both legs; the fixed leg must state the "
            "overnight leg's (fixed %r/%r/%r vs overnight %r/%r/%r)" % (
                fixed_payment_calendar, fixed.payment_lag, fixed.payment_convention,
                overnight_payment_calendar, overnight.payment_lag,
                overnight.payment_convention))

    exchanges = (fixed.initial_exchange or fixed.final_exchange or
                 overnight.initial_exchange or overnight.final_exchange)
    if fixed.discounting_curve != overnight.discounting_curve or exchanges:
        return swap_legs_ql(request)["npv"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    curve_of = _swap_curve_resolver(request, eval_date)
    discount_curve = curve_of(fixed.discounting_curve)
    forward_curve = curve_of(overnight.forwarding_curve)

    fixed_schedule = _wire_schedule_from(fixed.schedule)
    overnight_schedule = _wire_schedule_from(overnight.schedule)

    idx_def = find_index_def(oc["index"]["id"], request)
    if idx_def is None:
        raise ValueError(f"Index not found: {oc['index']['id']}")
    overnight_index = build_overnight_index(idx_def, forward_curve)

    swap_type = ql.OvernightIndexedSwap.Payer if fixed.pay else ql.OvernightIndexedSwap.Receiver
    # Wire lookback_days == 0 means "no lookback" = QuantLib Null<Natural>,
    # NOT a zero-day lookback — mirrors the server's mapping.
    lookback = int(oc.get("lookback_days", 0))
    lookback = QL_NULL_NATURAL if lookback <= 0 else lookback
    # Two-schedule constructor: each leg on its own schedule and notional.
    ql_swap = ql.OvernightIndexedSwap(
        swap_type,
        [fixed.notional],
        fixed_schedule,
        float(fc["rate"]),
        get_day_counter(fixed.day_counter),
        [overnight.notional],
        overnight_schedule,
        overnight_index,
        float(oc.get("spread", 0.0)),
        overnight.payment_lag or 0,
        get_convention(overnight.payment_convention),
        get_calendar(overnight_payment_calendar),
        bool(oc.get("telescopic_value_dates", False)),
        get_rate_averaging(oc["averaging_method"]),
        lookback,
        int(oc.get("lockout_days", 0)),
        bool(oc.get("apply_observation_shift", False)),
    )
    ql_swap.setPricingEngine(ql.DiscountingSwapEngine(discount_curve))
    return ql_swap.NPV()


def price_basis_swap_ql(request: dict) -> float:
    """Price a basis swap (`BasisSwap { leg1, leg2 }`, two floating legs each
    projected off its own forwarding curve and discounted on its own
    discounting curve) with the engine's leg algebra, `swap_legs_ql`."""
    return swap_legs_ql(request)["npv"]


def price_fra_ql(request: dict) -> float:
    """Price FRA using QuantLib (mirrors src/evaluators/fra_evaluator.cpp).

    The server resolves the forwarding and discounting curve ids
    independently, clones the request's IndexDef onto the forwarding curve
    and passes the request's explicit maturity date to
    QuantLib::ForwardRateAgreement — this reference does the same.
    """
    pricing = _reference_pricing_view(request)
    fra_data = request["fras"][0]
    fra = fra_data["fra"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Forwarding curve projects the index; discounting curve discounts the
    # payoff (defaults keep single-curve requests working as before).
    fwd_id = fra_data.get("forwarding_curve", "discount")
    fwd_json = next((c for c in pricing["curves"] if c["id"] == fwd_id), pricing["curves"][0])
    fwd_curve = build_curve_from_json(fwd_json, eval_date, request)
    disc_id = fra_data.get("discounting_curve", fwd_id)
    if disc_id == fwd_id:
        disc_curve = fwd_curve
    else:
        disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id), fwd_json)
        disc_curve = build_curve_from_json(disc_json, eval_date, request)

    # Build the index from its IndexDef (tenor, fixing days, calendar,
    # conventions, day count) exactly like the server's IndexRegistry, and
    # link it to the forwarding curve.
    idx = fra.get("index", {})
    if isinstance(idx, dict) and "id" in idx:
        idx_def = find_index_def(idx["id"], request)
        if idx_def:
            index = build_ibor_index(idx_def, fwd_curve)
        else:
            index = ql.Euribor3M(fwd_curve) if "3M" in idx["id"] else ql.Euribor6M(fwd_curve)
    else:
        # Legacy inline-index shape.
        period_months = idx.get("period_number", 3)
        index = ql.Euribor3M(fwd_curve) if period_months == 3 else ql.Euribor6M(fwd_curve)

    start_date = parse_date(fra["start_date"])
    position = ql.Position.Long if fra["fra_type"] == "Long" else ql.Position.Short

    if fra.get("maturity_date"):
        # Explicit accrual end date, as the server passes it.
        ql_fra = ql.ForwardRateAgreement(
            index, start_date, parse_date(fra["maturity_date"]), position,
            fra["strike"], fra["notional"], disc_curve
        )
    else:
        ql_fra = ql.ForwardRateAgreement(
            index, start_date, position, fra["strike"], fra["notional"], disc_curve
        )

    return ql_fra.NPV()


# =============================================================================
# FX forward
# =============================================================================

def fx_spot_date_ql(spot: dict, eval_date: ql.Date) -> ql.Date:
    """Spot date of a resolved FX spot (build_fx_spot): the evaluation date
    adjusted on the joint settlement calendar, then advanced `spot_days`
    business days — QuantLib FxSwapRateHelper::initializeDates, which the
    server's FxSpotDate mirrors."""
    cal = spot["calendar"]
    return cal.advance(cal.adjust(eval_date), ql.Period(spot["spot_days"], ql.Days))


def fx_as_of_exchange_rate_ql(spot_rate: float, spot_date: ql.Date, base_curve, quote_curve) -> float:
    """As-of exchange rate X = S * DF_quote(spot) / DF_base(spot), quote units
    per base unit at the curves' reference date (docs/http-api.md, FX spot
    market data)."""
    return spot_rate * quote_curve.discount(spot_date) / base_curve.discount(spot_date)


def fx_forward_ql(request: dict, index: int = 0) -> dict:
    """Price one FX forward of a PriceFxForwardRequest with the discount-factor
    algebra of src/evaluators/fx_forward_evaluator.cpp, on QuantLib curves built
    from the request:

      base_leg_pv  = sign * N * DF_b(T)              (base ccy)
      quote_leg_pv = -sign * N * K * DF_q(T)         (quote ccy)
      X            = S * DF_q(t_s) / DF_b(t_s)
      npv          = base_leg_pv * X + quote_leg_pv  (reporting = quote)
                   = base_leg_pv + quote_leg_pv / X  (reporting = base)
      fair         = X * DF_b(T) / DF_q(T)
      forward_points = fair - S

    t_s is the spot date (fx_spot_date_ql) and T the maturity adjusted with
    the spot's joint calendar and convention. Returns every response field:
    {npv, fair_forward_rate, forward_points, base_leg_pv, quote_leg_pv,
    spot_date, maturity_date, fx_spot_used} plus "as_of_exchange_rate" (X,
    not on the wire) for the reporting-currency identity test."""
    pricing = _reference_pricing_view(request)
    entry = request["fx_forwards"][index]
    fwd = entry["fx_forward"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    spot = build_fx_spot(request["pricing"], entry["fx_spot"], request)
    if (fwd["base_currency"], fwd["quote_currency"]) != (spot["base"], spot["quote"]):
        raise ValueError("FxForward pair does not match the spot's pair")

    def curve(curve_id):
        curve_json = next(c for c in pricing["curves"] if c["id"] == curve_id)
        return build_curve_from_json(curve_json, eval_date, request)

    base_curve = curve(entry["base_discounting_curve"])
    quote_curve = curve(entry["quote_discounting_curve"])

    spot_date = fx_spot_date_ql(spot, eval_date)
    maturity = spot["calendar"].adjust(parse_date(fwd["maturity_date"]), spot["bdc"])
    if maturity <= spot_date:
        raise ValueError("FxForward.maturity_date must fall after the spot date")

    S = spot["rate"]
    N = float(fwd["base_notional"])
    K = float(fwd["forward_rate"])
    sign = 1.0 if fwd["direction"] == "BuyBase" else -1.0

    df_b = base_curve.discount(maturity)
    df_q = quote_curve.discount(maturity)
    base_leg_pv = sign * N * df_b
    quote_leg_pv = -sign * N * K * df_q
    X = fx_as_of_exchange_rate_ql(S, spot_date, base_curve, quote_curve)
    fair = X * df_b / df_q

    reporting = entry["reporting_currency"]
    if reporting == fwd["quote_currency"]:
        npv = base_leg_pv * X + quote_leg_pv
    elif reporting == fwd["base_currency"]:
        npv = base_leg_pv + quote_leg_pv / X
    else:
        raise ValueError("PriceFxForward.reporting_currency must be the base or quote currency")

    return {
        "npv": npv,
        "fair_forward_rate": fair,
        "forward_points": fair - S,
        "base_leg_pv": base_leg_pv,
        "quote_leg_pv": quote_leg_pv,
        "spot_date": spot_date.ISO(),
        "maturity_date": maturity.ISO(),
        "fx_spot_used": S,
        "as_of_exchange_rate": X,
    }


FX_FORWARD_COMPARED_FIELDS = ("npv", "fair_forward_rate", "forward_points",
                              "base_leg_pv", "quote_leg_pv", "fx_spot_used")


def price_fx_forward_ql(request: dict) -> dict:
    """Functional-catalog reference for /price-fx-forward (compare="fields"):
    every numeric response field of the first forward, keyed by its dotted
    response path."""
    values = fx_forward_ql(request, 0)
    return {f"fx_forwards.0.{name}": values[name] for name in FX_FORWARD_COMPARED_FIELDS}


# =============================================================================
# Cross-currency swap
# =============================================================================


def _xccy_term_structure(curve):
    """The QuantLib term structure behind a handle (CashFlows.npv / bps take
    the structure itself)."""
    return curve.currentLink() if hasattr(curve, "currentLink") else curve


def cross_currency_swap_ql(request: dict, index: int = 0) -> dict:
    """Price one swap of a PriceCrossCurrencySwapRequest with the cash-flow
    algebra of src/evaluators/cross_currency_swap_evaluator.cpp on QuantLib
    curves built from the request. For leg i (coupons C_i, notional N_i,
    discounting curve D_i, own frame = as received by the receiver of the
    coupons, s_i = -1 for pay_receive Pay / +1 for Receive):

      L_i   = C_i + [initial] SimpleCashFlow(-N_i, schedule start)
                  + [final]   SimpleCashFlow(+N_i, last coupon payment date)
      legs[i].npv = s_i * CashFlows.npv(L_i, D_i)
      X     = S * DF_quote(t_s) / DF_base(t_s)        (fx_as_of_exchange_rate_ql)
      npv   = legs[0].npv_reporting + legs[1].npv_reporting
      A_i   = CashFlows.bps(C_i, D_i) / 1e-4
      fair_rate   = r_f - NPV[fixed ccy]   / (s_f * A_f)   (exactly one fixed leg)
      fair_spread = q_k - NPV[leg k ccy]   / (s_k * A_k)   (k = the floating leg of
                    a fixed/float swap, legs[1] of a float/float one)

    Returns the scalar response fields ({npv, fair_rate?, fair_spread?,
    fx_spot_used, fx_as_of_rate, spot_date}) and "legs": one dict per leg
    ({npv, npv_reporting} as on the wire, plus "pv_own" -- the own-frame PV,
    not on the wire -- and "flows": the (date, amount, discount) of every
    future flow of the full leg, in leg order, for the flows test)."""
    pricing = _reference_pricing_view(request)
    entry = request["swaps"][index]
    swap = entry["cross_currency_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    spot = build_fx_spot(request["pricing"], entry["fx_spot"], request)

    curves = {}

    def curve(curve_id):
        if curve_id not in curves:
            curve_json = next(c for c in pricing["curves"] if c["id"] == curve_id)
            curves[curve_id] = build_curve_from_json(curve_json, eval_date, request)
        return curves[curve_id]

    def build(leg):
        disc = curve(leg["discounting_curve"])
        fwd = curve(leg["forwarding_curve"]) if leg["coupons_type"] != "FixedCoupons" else None
        coupons, schedule, notional, level, is_fixed = _swap_leg_coupons(leg, request, fwd)
        full = list(coupons)
        if leg["initial_exchange"]:
            full.append(ql.SimpleCashFlow(-notional, schedule.dates()[0]))
        if leg["final_exchange"]:
            full.append(ql.SimpleCashFlow(notional, coupons[-1].date()))
        yts = _xccy_term_structure(disc)
        pv_own = ql.CashFlows.npv(full, yts, False, eval_date, eval_date)
        annuity = ql.CashFlows.bps(coupons, yts, False, eval_date, eval_date) / 1e-4
        flows = [(cf.date().ISO(), cf.amount(), yts.discount(cf.date()))
                 for cf in full if not cf.hasOccurred(eval_date)]
        return {"ccy": leg["currency"], "disc": disc, "pv_own": pv_own, "annuity": annuity,
                "level": level, "is_fixed": is_fixed, "flows": flows,
                "sign": -1.0 if leg["pay_receive"] == "Pay" else 1.0}

    legs = [build(swap["leg1"]), build(swap["leg2"])]
    pair = {legs[0]["ccy"], legs[1]["ccy"]}
    if pair != {spot["base"], spot["quote"]}:
        raise ValueError("PriceCrossCurrencySwap.fx_spot does not quote the legs' pair")
    base_leg, quote_leg = (legs[0], legs[1]) if legs[0]["ccy"] == spot["base"] else (legs[1], legs[0])

    spot_date = fx_spot_date_ql(spot, eval_date)
    S = spot["rate"]
    X = fx_as_of_exchange_rate_ql(S, spot_date, base_leg["disc"], quote_leg["disc"])

    def convert(amount, from_ccy, to_ccy):
        if from_ccy == to_ccy:
            return amount
        return amount * X if from_ccy == spot["base"] else amount / X

    reporting = entry["reporting_currency"]
    if reporting not in pair:
        raise ValueError("PriceCrossCurrencySwap.reporting_currency must be a leg currency")
    for leg in legs:
        leg["npv"] = leg["sign"] * leg["pv_own"]
        leg["npv_reporting"] = convert(leg["npv"], leg["ccy"], reporting)

    def npv_in(ccy):
        return sum(convert(leg["npv"], leg["ccy"], ccy) for leg in legs)

    out = {
        "npv": legs[0]["npv_reporting"] + legs[1]["npv_reporting"],
        "legs": [{"npv": leg["npv"], "npv_reporting": leg["npv_reporting"],
                  "pv_own": leg["pv_own"], "flows": leg["flows"]} for leg in legs],
        "fx_spot_used": S,
        "fx_as_of_rate": X,
        "spot_date": spot_date.ISO(),
    }
    if legs[0]["is_fixed"] != legs[1]["is_fixed"]:
        f = legs[0] if legs[0]["is_fixed"] else legs[1]
        out["fair_rate"] = f["level"] - npv_in(f["ccy"]) / (f["sign"] * f["annuity"])
    if not legs[0]["is_fixed"] or not legs[1]["is_fixed"]:
        q = legs[0] if legs[1]["is_fixed"] else legs[1]
        out["fair_spread"] = q["level"] - npv_in(q["ccy"]) / (q["sign"] * q["annuity"])
    return out


CROSS_CURRENCY_SWAP_COMPARED_FIELDS = ("npv", "fair_rate", "fair_spread", "fx_spot_used",
                                       "fx_as_of_rate")


def price_cross_currency_swap_ql(request: dict) -> dict:
    """Functional-catalog reference for /price-cross-currency-swap
    (compare="fields"): every numeric response field of the first swap
    (fair_rate / fair_spread when the swap shape defines them) and both legs'
    NPVs in their own and in the reporting currency, keyed by their dotted
    response paths (the framework indexes lists: swaps.0.legs.1.npv)."""
    values = cross_currency_swap_ql(request, 0)
    out = {f"swaps.0.{name}": values[name]
           for name in CROSS_CURRENCY_SWAP_COMPARED_FIELDS if name in values}
    for i, leg in enumerate(values["legs"]):
        out[f"swaps.0.legs.{i}.npv"] = leg["npv"]
        out[f"swaps.0.legs.{i}.npv_reporting"] = leg["npv_reporting"]
    return out


def price_cap_floor_ql(request: dict) -> float:
    """Price cap/floor using QuantLib.

    Mirrors the server's cap/floor evaluator:
      * the index projects off the trade's forwarding_curve while the engine
        discounts on the trade's discounting_curve,
      * the IborLeg carries the trade-level payment day counter and payment
        adjustment (withPaymentDayCounter / withPaymentAdjustment),
      * the volatility is the referenced OptionletVolSpec rebuilt as a
        ConstantOptionletVolatility with the spec's own reference date,
        calendar, convention, day count, volatility type and displacement,
      * the engine follows the referenced model: Bachelier for normal vols,
        BlackCapFloorEngine for Black and ShiftedBlack (QuantLib reads the
        displacement from the vol structure),
      * a Collar is ql.Collar(leg, [strike], [floor_strike]).
    """
    pricing = _reference_pricing_view(request)
    cf_data = request["cap_floors"][0]
    cf = cf_data["cap_floor"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Discounting and forwarding curves (may be the same id)
    disc_id = cf_data.get("discounting_curve", "discount")
    disc_json = next((c for c in pricing["curves"] if c["id"] == disc_id), pricing["curves"][0])
    disc_curve = build_curve_from_json(disc_json, eval_date, request)
    fwd_id = cf_data.get("forwarding_curve", disc_id)
    if fwd_id == disc_id:
        fwd_curve = disc_curve
    else:
        fwd_json = next((c for c in pricing["curves"] if c["id"] == fwd_id), pricing["curves"][0])
        fwd_curve = build_curve_from_json(fwd_json, eval_date, request)

    # Build schedule
    sch = cf["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention", "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        False
    )

    # Index resolved from its IndexDef, projecting off the forwarding curve
    # (mirrors IndexRegistry::getIborWithCurve on the server).
    index = resolve_index_from_id(cf["index"]["id"], request, fwd_curve)

    # Floating leg mirrors the server's IborLeg: trade-level payment day
    # counter and payment adjustment, index-level fixing days.
    leg = ql.IborLeg(
        [cf["notional"]],
        schedule,
        index,
        get_day_counter(cf.get("day_counter", "Actual360")),
        get_convention(cf.get("business_day_convention", "ModifiedFollowing")),
    )
    cf_type = cf["cap_floor_type"]
    if cf_type == "Cap":
        ql_cf = ql.Cap(leg, [cf["strike"]])
    elif cf_type == "Floor":
        ql_cf = ql.Floor(leg, [cf["strike"]])
    elif cf_type == "Collar":
        # Long the cap at `strike`, short the floor at `floor_strike` on the
        # same leg (the server's QuantLib::Collar).
        ql_cf = ql.Collar(leg, [cf["strike"]], [cf["floor_strike"]])
    else:
        raise ValueError(f"Unsupported cap/floor type: {cf_type}")

    # Volatility: rebuild the referenced constant optionlet vol spec.
    vol_base = {}
    for v in pricing.get("vol_surfaces", []):
        if v.get("id") == cf_data.get("volatility"):
            vol_base = v.get("payload", {}).get("base", {})
            break
    vol_value = vol_base.get("constant_vol", 0.20)
    displacement = vol_base.get("displacement", 0.0)
    # QuantLib encodes Black lognormal vols under ShiftedLognormal; pure
    # lognormal is displacement == 0 (same mapping as the server).
    vol_type = ql.Normal if vol_base.get("volatility_type") == "Normal" else ql.ShiftedLognormal
    vol_ref = parse_date(vol_base.get("reference_date", pricing["as_of_date"]))
    vol_handle = ql.OptionletVolatilityStructureHandle(
        ql.ConstantOptionletVolatility(
            vol_ref,
            get_calendar(vol_base.get("calendar", "TARGET")),
            get_convention(vol_base.get("business_day_convention", "ModifiedFollowing")),
            vol_value,
            get_day_counter(vol_base.get("day_counter", "Actual365Fixed")),
            vol_type,
            displacement,
        )
    )

    # Engine follows the referenced model (Black / ShiftedBlack / Bachelier).
    model_type = "Black"
    for m in pricing.get("models", []):
        if m.get("id") == cf_data.get("model"):
            model_type = m.get("payload", {}).get("model_type", "Black")
            break
    if model_type == "Bachelier":
        engine = ql.BachelierCapFloorEngine(disc_curve, vol_handle)
    else:
        engine = ql.BlackCapFloorEngine(disc_curve, vol_handle)
    ql_cf.setPricingEngine(engine)

    return ql_cf.NPV()


def price_swaption_ql(request: dict) -> float:
    """Price swaption using QuantLib."""
    return build_swaption_ql(request)["swaption"].NPV()


def build_swaption_ql(request: dict) -> dict:
    """Build the native QuantLib swaption for a request.

    Returns ``{"swaption", "swap", "curve", "vol", "vol_type", "displacement",
    "eval_date"}`` with the pricing engine already attached, so callers can
    read diagnostics (annuity, forward, exercise date) besides the NPV. ``vol``
    is the constant volatility for constant specs, else None.
    """
    pricing = _reference_pricing_view(request)
    sw_data = request["swaptions"][0]
    sw = sw_data["swaption"]
    underlying_type = sw.get("underlying_type")
    underlying = sw.get("underlying")
    if not underlying_type or underlying is None:
        raise ValueError("Swaption underlying_type and underlying are required")
    
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # The swaption's curves are the underlying legs' (both legs must name one
    # discount curve, as the engine checks): discounting from the fixed leg,
    # forwarding from the floating / overnight leg. The underlying is the
    # native QuantLib::VanillaSwap / OvernightIndexedSwap the engine builds
    # (swaption_evaluator.cpp buildUnderlyingSwap): the fixed leg's notional
    # for both legs, the fixed leg's side as the swap type, both legs paid on
    # the floating schedule's convention, the IBOR leg fixing on the index's
    # own fixing days.
    floating_role = "overnight_leg" if underlying_type == "OisSwap" else "floating_leg"
    fixed = _swap_leg(underlying["fixed_leg"])
    floating = _swap_leg(underlying[floating_role])
    if fixed.discounting_curve != floating.discounting_curve:
        raise ValueError(
            "Swaption.underlying.fixed_leg.discounting_curve and "
            f"{floating_role}.discounting_curve must be equal "
            f"({fixed.discounting_curve!r} vs {floating.discounting_curve!r})")
    curve_of = _swap_curve_resolver(request, eval_date)
    curve = curve_of(fixed.discounting_curve)
    forward_curve = curve_of(floating.forwarding_curve)

    # Kind-specific fields (`rate`, `index`, `spread`, the OIS conventions)
    # come from each leg's `coupons`; notional, schedule, day counter and the
    # payment conventions from the leg itself.
    fixed_leg = fixed.coupons
    fixed_schedule = _wire_schedule_from(fixed.schedule)

    if underlying_type == "OisSwap":
        on_leg = floating.coupons
        on_schedule = _wire_schedule_from(floating.schedule)
        on_index = resolve_index_from_id(on_leg["index"]["id"], request, forward_curve)
        swap_type = (ql.OvernightIndexedSwap.Payer if fixed.pay
                     else ql.OvernightIndexedSwap.Receiver)
        # Wire lookback_days == 0 means "no lookback" = QuantLib Null<Natural>,
        # NOT a zero-day lookback — mirrors the server's mapping.
        lookback = int(on_leg.get("lookback_days", 0))
        lookback = QL_NULL_NATURAL if lookback <= 0 else lookback
        notional = fixed.notional
        swap = ql.OvernightIndexedSwap(
            swap_type,
            [notional],
            fixed_schedule,
            float(fixed_leg["rate"]),
            get_day_counter(fixed.day_counter),
            [notional],
            on_schedule,
            on_index,
            float(on_leg.get("spread", 0.0)),
            floating.payment_lag or 0,
            get_convention(floating.payment_convention),
            _swap_leg_payment_calendar(floating, on_schedule),
            bool(on_leg.get("telescopic_value_dates", False)),
            get_rate_averaging(on_leg["averaging_method"]),
            lookback,
            int(on_leg.get("lockout_days", 0)),
            bool(on_leg.get("apply_observation_shift", False)),
        )
    else:
        float_leg = floating.coupons
        float_schedule = _wire_schedule_from(floating.schedule)
        index = resolve_index_from_id(float_leg["index"]["id"], request, forward_curve)
        swap_type = ql.VanillaSwap.Payer if fixed.pay else ql.VanillaSwap.Receiver

        swap = ql.VanillaSwap(
            swap_type,
            fixed.notional,
            fixed_schedule,
            float(fixed_leg["rate"]),
            get_day_counter(fixed.day_counter),
            float_schedule,
            index,
            float(float_leg.get("spread", 0.0)),
            get_day_counter(floating.day_counter)
        )

    # Exercise construction mirrors the server's swaption evaluator
    # (buildSwaptionInstrument): European takes the single exercise_date,
    # Bermudan the exercise_dates list, and American opens the exercise
    # window at the evaluation date — the server builds
    # AmericanExercise(Settings::evaluationDate(), exercise_date).
    exercise_type = sw.get("exercise_type", "European")
    if exercise_type == "Bermudan":
        exercise = ql.BermudanExercise(
            [parse_date(d) for d in sw["exercise_dates"]])
    elif exercise_type == "American":
        exercise = ql.AmericanExercise(eval_date, parse_date(sw["exercise_date"]))
    else:
        exercise = ql.EuropeanExercise(parse_date(sw["exercise_date"]))
    settlement_type = sw.get("settlement_type", "Physical")
    settlement_method = sw.get("settlement_method", "PhysicalOTC")
    ql_settlement_type = ql.Settlement.Cash if settlement_type == "Cash" else ql.Settlement.Physical
    ql_settlement_method = get_settlement_method(settlement_method)
    swaption = ql.Swaption(swap, exercise, ql_settlement_type, ql_settlement_method)
    
    # Get volatility
    vol = 0.20
    vol_type = ql.ShiftedLognormal
    displacement = 0.0
    vol_handle = None
    inner_is_constant = False

    quote_values = {}
    quote_types = {}
    for q in pricing.get("quotes", []):
        quote_values[q.get("id")] = q.get("value")
        quote_types[q.get("id")] = q.get("quote_type")

    for v in pricing.get("vol_surfaces", []):
        if v.get("id") == sw_data.get("volatility"):
            payload = v.get("payload", {})
            if v.get("payload_type") != "SwaptionVolSpec":
                break

            inner_type = payload.get("payload_type", "SwaptionVolConstantSpec")
            inner = payload.get("payload", {})

            if inner_type == "SwaptionVolConstantSpec":
                inner_is_constant = True
                base = inner.get("base", {})
                vol = base.get("constant_vol", vol)
                vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
                displacement = base.get("displacement", 0.0)
                break

            if inner_type == "SwaptionVolAtmMatrixSpec":
                base = inner.get("base", {})
                vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
                displacement = base.get("displacement", 0.0)
                expiries = inner.get("expiries", [])
                tenors = inner.get("tenors", [])
                vols = inner.get("vols", {})
                n_rows = vols.get("n_rows", 0)
                n_cols = vols.get("n_cols", 0)
                values = vols.get("values", [])
                quote_ids = vols.get("quote_ids", [])

                def to_period(p):
                    num, unit = _period_n_unit(p, "tenor", 0, "Months")
                    return ql.Period(num, get_time_unit(unit))

                ql_expiries = [to_period(p) for p in expiries]
                ql_tenors = [to_period(p) for p in tenors]

                matrix = ql.Matrix(n_rows, n_cols)
                for i in range(n_rows):
                    for j in range(n_cols):
                        idx = i * n_cols + j
                        if idx < len(quote_ids) and quote_ids[idx]:
                            qid = quote_ids[idx]
                            if quote_types.get(qid) != "Volatility":
                                raise ValueError("Quote type mismatch for vol quote")
                            vval = quote_values.get(qid)
                        else:
                            vval = values[idx]
                        matrix[i][j] = vval

                # Python bindings are stricter for overloaded constructors; use
                # the stable calendar-based constructor and default interpolation settings.
                vol_matrix = ql.SwaptionVolatilityMatrix(
                    ql.TARGET(), ql.ModifiedFollowing,
                    ql_expiries, ql_tenors, matrix,
                    ql.Actual365Fixed()
                )
                vol_handle = ql.SwaptionVolatilityStructureHandle(vol_matrix)
                break

            # Smile cube and SABR are not yet supported in the Python reference pricer
            base = inner.get("base", {})
            vol = base.get("constant_vol", vol)
            vol_type = get_volatility_type(base.get("volatility_type", "Lognormal"))
            displacement = base.get("displacement", 0.0)
            break

    if "vol_surfaces" not in pricing:
        for v in pricing.get("volatilities", []):
            if v["id"] == sw_data.get("volatility"):
                vol = v.get("constant_vol", 0.20)
                break

    if vol_handle is None:
        if displacement and displacement != 0.0:
            vol_handle = ql.SwaptionVolatilityStructureHandle(
                ql.ConstantSwaptionVolatility(
                    eval_date, ql.TARGET(), ql.ModifiedFollowing, vol,
                    ql.Actual365Fixed(), vol_type, displacement
                )
            )
        else:
            vol_handle = ql.SwaptionVolatilityStructureHandle(
                ql.ConstantSwaptionVolatility(
                    eval_date, ql.TARGET(), ql.ModifiedFollowing, vol,
                    ql.Actual365Fixed(), vol_type
                )
            )

    model_spec = {}
    for m in pricing.get("models", []):
        if m.get("id") == sw_data.get("model"):
            model_spec = m.get("payload", {})
            break
    model_type = model_spec.get("model_type", "Black")

    if model_type == "HullWhiteLattice":
        # Mirrors the server's HullWhiteLattice engine branch: a Hull-White
        # one-factor model on the discount curve with the explicit hw_a /
        # hw_sigma parameters, priced on a TreeSwaptionEngine with
        # lattice_steps time steps (schema defaults: a=0.03, sigma=0.01,
        # steps=50). param_mode=Calibrate would need the server's
        # calibration routine reproduced here, so it is not supported.
        if model_spec.get("param_mode", "Explicit") != "Explicit":
            raise ValueError(
                "Reference pricer only supports HullWhiteLattice with "
                "param_mode=Explicit")
        hw_model = ql.HullWhite(curve,
                                model_spec.get("hw_a", 0.03),
                                model_spec.get("hw_sigma", 0.01))
        swaption.setPricingEngine(
            ql.TreeSwaptionEngine(hw_model, int(model_spec.get("lattice_steps", 50))))
    elif model_type == "Bachelier":
        swaption.setPricingEngine(ql.BachelierSwaptionEngine(curve, vol_handle))
    else:
        swaption.setPricingEngine(ql.BlackSwaptionEngine(curve, vol_handle))

    return {
        "swaption": swaption,
        "swap": swap,
        "curve": curve,
        "vol": vol if vol_handle is None or inner_is_constant else None,
        "vol_type": vol_type,
        "displacement": displacement,
        "eval_date": eval_date,
    }


def price_cds_ql(request: dict) -> float:
    """Price CDS using QuantLib."""
    pricing = _reference_pricing_view(request)
    cds_data = request["cds_list"][0]
    cds = cds_data["cds"]
    quote_values = {q["id"]: q.get("value", 0.0) for q in pricing.get("quotes", []) if "id" in q}
    quote_types = {q["id"]: q.get("quote_type", "Curve") for q in pricing.get("quotes", []) if "id" in q}

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    # Build discount curve for pricing
    curve_id = cds_data.get("discounting_curve", "discount")
    curve_json = next((c for c in pricing["curves"] if c["id"] == curve_id), pricing["curves"][0])
    curve = build_curve_from_json(curve_json, eval_date, request)

    # Build schedule
    sch = cds["schedule"]
    schedule = ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch["frequency"])),
        get_calendar(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "Following")),
        get_convention(sch.get("termination_date_convention", "Unadjusted")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        False
    )

    protection = ql.Protection.Buyer if cds["side"] == "Buyer" else ql.Protection.Seller

    settles_accrual = cds.get("settles_accrual", True)
    pays_at_default = cds.get("pays_at_default_time", True)
    rebates_accrual = cds.get("rebates_accrual", True)
    last_period_dc = get_day_counter(cds.get("last_period_day_counter", "Actual360"))
    cash_settlement_days = cds.get("cash_settlement_days", 3)

    protection_start = parse_date(cds["protection_start"]) if cds.get("protection_start") else ql.Date()
    upfront_date = parse_date(cds["upfront_date"]) if cds.get("upfront_date") else ql.Date()
    trade_date = parse_date(cds["trade_date"]) if cds.get("trade_date") else ql.Date()

    upfront = cds.get("upfront", 0.0)
    running_coupon = cds.get("running_coupon", 0.0)
    if upfront != 0.0 or cds.get("upfront_date"):
        try:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                upfront,
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360")),
                settles_accrual,
                pays_at_default,
                protection_start,
                upfront_date,
                None,
                last_period_dc,
                rebates_accrual,
                trade_date,
                cash_settlement_days
            )
        except Exception:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                upfront,
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360"))
            )
    else:
        try:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360")),
                settles_accrual,
                pays_at_default,
                protection_start,
                None,
                last_period_dc,
                rebates_accrual,
                trade_date,
                cash_settlement_days
            )
        except Exception:
            ql_cds = ql.CreditDefaultSwap(
                protection,
                cds["notional"],
                running_coupon,
                schedule,
                get_convention(cds.get("business_day_convention", "Following")),
                get_day_counter(cds.get("day_counter", "Actual360"))
            )

    # Credit curve
    credit_curve_id = cds_data["credit_curve_id"]
    credit = next((c for c in pricing.get("credit_curves", []) if c["id"] == credit_curve_id), None)
    if credit is None:
        raise ValueError(f"credit_curve_id not found: {credit_curve_id}")

    recovery = credit.get("recovery_rate", 0.4)
    hazard = credit.get("flat_hazard_rate", 0.0)
    quotes = credit.get("quotes", [])

    # Use trade discount curve for credit helpers
    credit_discount_curve = curve

    if quotes:
        helper_conv = credit.get("helper_conventions", {})
        settlement_days = helper_conv.get("settlement_days", 0)
        calendar = get_calendar(credit.get("calendar", "TARGET"))
        freq = get_frequency(helper_conv.get("frequency", "Quarterly"))
        bdc = get_convention(helper_conv.get("business_day_convention", "Following"))
        rule = get_date_generation(helper_conv.get("date_generation_rule", "TwentiethIMM"))
        curve_dc = get_day_counter(credit.get("day_counter", "Actual365Fixed"))
        last_period_dc = get_day_counter(helper_conv.get("last_period_day_counter", "Actual365Fixed"))
        settles_accrual = helper_conv.get("settles_accrual", True)
        pays_at_default = helper_conv.get("pays_at_default_time", True)
        rebates_accrual = helper_conv.get("rebates_accrual", True)
        helper_model = get_cds_helper_model(helper_conv.get("helper_model", "MidPoint"))

        helpers = []
        for q in quotes:
            q_n, q_u = _period_n_unit(q, "tenor", 0, "Days")
            tenor = ql.Period(q_n, get_time_unit(q_u))
            quote_type = q.get("quote_type", "ParSpread")
            quote_id = q.get("quote_id")
            if quote_id:
                if quote_types.get(quote_id, "Curve") != "Credit":
                    raise ValueError(f"Quote id '{quote_id}' has wrong type for credit")
                quote_value = quote_values.get(quote_id, 0.0)
            else:
                quote_value = None

            if quote_type == "Upfront":
                upfront_value = quote_value if quote_value is not None else q.get("quoted_upfront", 0.0)
                upfront_quote = ql.QuoteHandle(ql.SimpleQuote(upfront_value))
                running = q.get("running_coupon", 0.0)
                helper = ql.UpfrontCdsHelper(
                    upfront_quote,
                    running,
                    tenor,
                    settlement_days,
                    calendar,
                    freq,
                    bdc,
                    rule,
                    curve_dc,
                    recovery,
                    credit_discount_curve,
                    settles_accrual,
                    pays_at_default,
                    ql.Date(),
                    last_period_dc,
                    rebates_accrual,
                    helper_model
                )
            else:
                spread_value = quote_value if quote_value is not None else q.get("quoted_par_spread", 0.0)
                spread_quote = ql.QuoteHandle(ql.SimpleQuote(spread_value))
                helper = ql.SpreadCdsHelper(
                    spread_quote,
                    tenor,
                    settlement_days,
                    calendar,
                    freq,
                    bdc,
                    rule,
                    curve_dc,
                    recovery,
                    credit_discount_curve,
                    settles_accrual,
                    pays_at_default,
                    ql.Date(),
                    last_period_dc,
                    rebates_accrual,
                    helper_model
                )
            helpers.append(helper)

        interp = get_interpolator(credit.get("curve_interpolator", "LogLinear"))
        if hasattr(ql, "PiecewiseDefaultCurve"):
            try:
                default_curve = ql.PiecewiseDefaultCurve(
                    ql.SurvivalProbability, helpers, curve_dc, interp
                )
            except Exception:
                default_curve = ql.PiecewiseDefaultCurve(
                    ql.SurvivalProbability, helpers, curve_dc
                )
        else:
            # Fallback for QuantLib Python builds without PiecewiseDefaultCurve
            default_curve = ql.PiecewiseFlatHazardRate(
                eval_date, helpers, curve_dc
            )
    else:
        hazard = hazard if hazard > 0.0 else 0.01
        default_curve = ql.FlatHazardRate(
            eval_date,
            ql.QuoteHandle(ql.SimpleQuote(hazard)),
            get_day_counter(credit.get("day_counter", "Actual365Fixed"))
        )

    # Model/engine
    model_id = cds_data["model"]
    model = next((m for m in pricing.get("models", []) if m["id"] == model_id), None)
    if model is None:
        raise ValueError(f"model not found: {model_id}")

    cds_model = model.get("payload", {})
    engine_type = get_cds_engine_type(cds_model.get("engine_type", "MidPoint"))
    if engine_type == "ISDA":
        engine = ql.IsdaCdsEngine(
            ql.DefaultProbabilityTermStructureHandle(default_curve),
            recovery,
            curve,
            cds_model.get("include_settlement_date_flows"),
            get_isda_numerical_fix(cds_model.get("isda_numerical_fix", "Taylor")),
            get_isda_accrual_bias(cds_model.get("isda_accrual_bias", "HalfDayBias")),
            get_isda_forwards_in_coupon_period(cds_model.get("isda_forwards_in_coupon_period", "Piecewise"))
        )
    else:
        engine = ql.MidPointCdsEngine(
            ql.DefaultProbabilityTermStructureHandle(default_curve),
            recovery,
            curve
        )

    ql_cds.setPricingEngine(engine)

    return ql_cds.NPV()


# =============================================================================
# Inflation swaps (ZCIIS / YYIIS)
# =============================================================================
#
# These mirror the server's inflation stack field for field:
#   * src/decode/market/inflation_curve_parsers.cpp — the InflationIndexSpec /
#     InflationCurveSpec builders (index construction, helper dates, base
#     fixing, PiecewiseZero/YoYInflationCurve<Linear> bootstrap),
#   * src/decode/products/zero_coupon_inflation_swap_mapper.cpp and
#     year_on_year_inflation_swap_mapper.cpp — the trade construction,
#   * src/evaluators/*_inflation_swap_evaluator.cpp — engine selection
#     (DiscountingSwapEngine on the trade's discounting curve; the YoY leg
#     additionally gets a BlackYoYInflationCouponPricer holding the same
#     nominal curve and no volatility surface).


def get_cpi_interpolation(name: str):
    """Mirror the server's CPIInterpolationType mapping (fbs default AsIndex)."""
    mapping = {
        "AsIndex": ql.CPI.AsIndex,
        "Flat": ql.CPI.Flat,
        "Linear": ql.CPI.Linear,
    }
    return mapping.get(name, ql.CPI.AsIndex)


def _inflation_region(currency: str):
    """QuantLib Region for the server's currency->region mapping.

    The Python bindings expose only CustomRegion (not EURegion/USRegion/...),
    but a Region contributes nothing numerical — only the index's name, i.e.
    its fixing-store key — so a CustomRegion carrying the same name as the
    server's region is an exact behavioural match.
    """
    names = {
        "EUR": ("EU", "EU"),
        "USD": ("USA", "US"),
        "GBP": ("UK", "UK"),
        "AUD": ("Australia", "AU"),
        "ZAR": ("South Africa", "ZA"),
    }
    name, code = names.get(currency, ("N/A", "N/A"))
    return ql.CustomRegion(name, code)


def _find_inflation_index_spec(pricing: dict, index_id: str) -> dict:
    for spec in pricing.get("inflation_indices", []):
        if spec.get("id") == index_id:
            return spec
    raise ValueError(f"Inflation index spec not found: {index_id}")


def _find_inflation_curve_spec(pricing: dict, curve_id: str) -> dict:
    for spec in pricing.get("inflation_curves", []):
        if spec.get("id") == curve_id:
            return spec
    raise ValueError(f"Inflation curve spec not found: {curve_id}")


def _wire_period(container: dict, key: str) -> ql.Period:
    n, unit = _period_n_unit(container, key)
    return ql.Period(n, get_time_unit(unit))


def _apply_inflation_fixings(index, spec: dict):
    """Mirror the server: clearFixings(), then add every supplied fixing."""
    index.clearFixings()
    for fixing in spec.get("fixings", []) or []:
        index.addFixing(parse_date(fixing["date"]), fixing["value"])


def _build_inflation_index(spec: dict, zero_handle=None, yoy_handle=None):
    """Inflation index from an InflationIndexSpec (mirrors buildInflationIndex).

    Note the server never passes the spec's `interpolated` flag into the
    QuantLib index constructor (QuantLib 1.41 dropped that parameter), so the
    reference does not either. Ratio-based YoY indices
    (underlying_zero_index_id) are not supported by the reference yet — do
    not add a catalog case that needs one.
    """
    family = spec.get("family_name") or spec["id"]
    ccy_code = spec.get("currency") or "EUR"
    region = _inflation_region(ccy_code)
    ccy = get_currency(ccy_code)
    freq = get_frequency(spec.get("frequency", "Monthly"))
    avail_lag = _wire_period(spec, "availability_lag")
    revised = spec.get("revised", False)
    if spec.get("kind", "ZeroInflation") == "ZeroInflation":
        handle = zero_handle if zero_handle is not None \
            else ql.ZeroInflationTermStructureHandle()
        index = ql.ZeroInflationIndex(
            family, region, revised, freq, avail_lag, ccy, handle)
    else:
        if spec.get("underlying_zero_index_id"):
            raise ValueError(
                "Ratio-based YoY inflation indices are not supported by the "
                "reference yet")
        handle = yoy_handle if yoy_handle is not None \
            else ql.YoYInflationTermStructureHandle()
        index = ql.YoYInflationIndex(
            family, region, revised, freq, avail_lag, ccy, handle)
    _apply_inflation_fixings(index, spec)
    return index


def _inflation_helper_maturity(helper: dict, reference_date, calendar, bdc,
                               label: str):
    """Helper maturity, mirroring resolveHelperDates (tenor or end_date).

    The server also accepts explicit start_date+end_date pairs (a dated
    helper constructor the Python bindings do not expose), so those raise
    here instead of being silently mispriced.
    """
    if helper.get("start_date"):
        raise ValueError(
            f"{label}: explicit start/end dated helpers are not supported by "
            "the reference yet")
    if helper.get("end_date"):
        return parse_date(helper["end_date"])
    return calendar.advance(reference_date, _wire_period(helper, "tenor"), bdc)


def _resolve_inflation_quote(helper: dict, request: dict) -> float:
    """Inline quote_value, or quote_id resolved from pricing.quotes (Curve)."""
    quote_id = helper.get("quote_id")
    if quote_id:
        pricing = request.get("pricing", {})
        for q in pricing.get("quotes", []):
            if q.get("id") == quote_id:
                if q.get("quote_type", "Curve") != "Curve":
                    raise ValueError(
                        f"Quote id '{quote_id}' has wrong type for an "
                        "inflation curve")
                return q.get("value", 0.0)
        raise ValueError(f"Quote id not found: {quote_id}")
    return helper["quote_value"]


def _build_zero_inflation_curve(curve_spec: dict, index_spec: dict,
                                request: dict):
    """PiecewiseZeroInflation curve from an InflationCurveSpec (ZeroInflation).

    Mirrors the server's build: ZCIIS helpers with per-helper calendar /
    convention / day counter / observation lag / CPI interpolation, base date
    = inflationPeriod(reference - availability_lag).start, Linear
    interpolation (the only interpolator the server accepts), no seasonality.
    """
    if curve_spec.get("interpolator", "Linear") != "Linear":
        raise ValueError("Inflation curves only support the Linear interpolator")
    ref = parse_date(curve_spec["reference_date"])
    dc = get_day_counter(curve_spec.get("day_counter", "Actual365Fixed"))
    accuracy = curve_spec.get("bootstrap_accuracy", 1.0e-12)
    freq = get_frequency(index_spec.get("frequency", "Monthly"))
    avail_lag = _wire_period(index_spec, "availability_lag")
    bare_index = _build_inflation_index(index_spec)

    helpers = []
    for wrapper in curve_spec["points"]:
        if wrapper.get("point_type") != "ZeroCouponInflationSwapHelper":
            raise ValueError(
                "Zero inflation curves take only ZeroCouponInflationSwapHelper "
                "points")
        point = wrapper["point"]
        calendar = _calendar_from_enum(point.get("calendar", "TARGET"))
        bdc = get_convention(point.get("payment_convention",
                                       "ModifiedFollowing"))
        maturity = _inflation_helper_maturity(
            point, ref, calendar, bdc, "ZeroCouponInflationSwapHelper")
        helpers.append(ql.ZeroCouponInflationSwapHelper(
            ql.QuoteHandle(ql.SimpleQuote(_resolve_inflation_quote(point,
                                                                   request))),
            _wire_period(point, "swap_observation_lag"),
            maturity,
            calendar,
            bdc,
            get_day_counter(point.get("day_counter", "Actual365Fixed")),
            bare_index,
            get_cpi_interpolation(point.get("observation_interpolation",
                                            "AsIndex"))))

    base_date = ql.inflationPeriod(ref - avail_lag, freq)[0]
    ts = ql.PiecewiseZeroInflation(ref, base_date, freq, dc, helpers, None,
                                   accuracy)
    if curve_spec.get("allow_extrapolation", True):
        ts.enableExtrapolation()
    else:
        ts.disableExtrapolation()
    return ts


def _build_yoy_inflation_curve(curve_spec: dict, index_spec: dict,
                               request: dict, nominal_curve):
    """PiecewiseYoYInflation curve from an InflationCurveSpec (YoYInflation).

    Mirrors the server: YoY swap helpers carrying the nominal curve named by
    each helper's nominal_curve_id, base YoY rate read from the index's own
    stored fixing at inflationPeriod(reference - availability_lag).start.
    `nominal_curve` maps a curve id to its built YieldTermStructureHandle.
    """
    if curve_spec.get("interpolator", "Linear") != "Linear":
        raise ValueError("Inflation curves only support the Linear interpolator")
    ref = parse_date(curve_spec["reference_date"])
    dc = get_day_counter(curve_spec.get("day_counter", "Actual365Fixed"))
    accuracy = curve_spec.get("bootstrap_accuracy", 1.0e-12)
    freq = get_frequency(index_spec.get("frequency", "Monthly"))
    avail_lag = _wire_period(index_spec, "availability_lag")
    bare_index = _build_inflation_index(index_spec)

    helpers = []
    for wrapper in curve_spec["points"]:
        if wrapper.get("point_type") != "YearOnYearInflationSwapHelper":
            raise ValueError(
                "YoY inflation curves take only YearOnYearInflationSwapHelper "
                "points")
        point = wrapper["point"]
        calendar = _calendar_from_enum(point.get("calendar", "TARGET"))
        bdc = get_convention(point.get("payment_convention",
                                       "ModifiedFollowing"))
        maturity = _inflation_helper_maturity(
            point, ref, calendar, bdc, "YearOnYearInflationSwapHelper")
        helpers.append(ql.YearOnYearInflationSwapHelper(
            ql.QuoteHandle(ql.SimpleQuote(_resolve_inflation_quote(point,
                                                                   request))),
            _wire_period(point, "swap_observation_lag"),
            maturity,
            calendar,
            bdc,
            get_day_counter(point.get("day_counter", "Actual365Fixed")),
            bare_index,
            get_cpi_interpolation(point.get("observation_interpolation",
                                            "AsIndex")),
            nominal_curve(point["nominal_curve_id"])))

    base_date = ql.inflationPeriod(ref - avail_lag, freq)[0]
    base_yoy_rate = bare_index.fixing(base_date)
    ts = ql.PiecewiseYoYInflation(ref, base_date, base_yoy_rate, freq, dc,
                                  helpers, None, accuracy)
    if curve_spec.get("allow_extrapolation", True):
        ts.enableExtrapolation()
    else:
        ts.disableExtrapolation()
    return ts


def _nominal_curve_resolver(pricing: dict, eval_date, request: dict):
    """id -> YieldTermStructureHandle over pricing.rates.curves, cached."""
    cache = {}

    def resolve(curve_id: str):
        if curve_id not in cache:
            curve_json = next(
                (c for c in pricing.get("curves", []) if c["id"] == curve_id),
                None)
            if curve_json is None:
                raise ValueError(f"Rates curve not found: {curve_id}")
            cache[curve_id] = build_curve_from_json(curve_json, eval_date,
                                                    request)
        return cache[curve_id]

    return resolve


def _build_wire_schedule(sch: dict) -> ql.Schedule:
    """QuantLib Schedule from a wire Schedule table (mirrors ScheduleParser)."""
    return ql.Schedule(
        parse_date(sch["effective_date"]),
        parse_date(sch["termination_date"]),
        ql.Period(get_frequency(sch.get("frequency", "Annual"))),
        _calendar_from_enum(sch.get("calendar", "TARGET")),
        get_convention(sch.get("convention", "ModifiedFollowing")),
        get_convention(sch.get("termination_date_convention",
                               "ModifiedFollowing")),
        get_date_generation(sch.get("date_generation_rule", "Forward")),
        sch.get("end_of_month", False))


def price_zero_coupon_inflation_swap_ql(request: dict) -> float:
    """Price a zero-coupon inflation swap (ZCIIS) using QuantLib."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    trade = swap_data["zero_coupon_inflation_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    nominal_curve = _nominal_curve_resolver(pricing, eval_date, request)
    discount = nominal_curve(swap_data["discounting_curve"])

    curve_spec = _find_inflation_curve_spec(pricing,
                                            swap_data["inflation_curve"])
    if curve_spec.get("kind", "ZeroInflation") != "ZeroInflation":
        raise ValueError("ZCIIS needs a ZeroInflation curve")
    index_spec = _find_inflation_index_spec(pricing, curve_spec["index_id"])
    if index_spec["id"] != trade["inflation_index_id"]:
        raise ValueError(
            "ZCIIS inflation_index_id does not match the inflation curve's "
            "index")
    ts = _build_zero_inflation_curve(curve_spec, index_spec, request)
    index = _build_inflation_index(
        index_spec, zero_handle=ql.ZeroInflationTermStructureHandle(ts))

    swap_type = ql.Swap.Payer if trade.get("swap_type", "Payer") == "Payer" \
        else ql.Swap.Receiver
    swap = ql.ZeroCouponInflationSwap(
        swap_type,
        trade["notional"],
        parse_date(trade["start_date"]),
        parse_date(trade["maturity_date"]),
        _calendar_from_enum(trade.get("fixed_calendar", "TARGET")),
        get_convention(trade.get("fixed_convention", "ModifiedFollowing")),
        get_day_counter(trade.get("day_counter", "Actual365Fixed")),
        trade["fixed_rate"],
        index,
        _wire_period(trade, "observation_lag"),
        get_cpi_interpolation(trade.get("observation_interpolation",
                                        "AsIndex")),
        trade.get("adjust_observation_dates", False),
        _calendar_from_enum(trade.get("inflation_calendar", "NullCalendar")),
        get_convention(trade.get("inflation_convention", "Following")))
    swap.setPricingEngine(ql.DiscountingSwapEngine(discount))
    return swap.NPV()


def price_year_on_year_inflation_swap_ql(request: dict) -> float:
    """Price a year-on-year inflation swap (YYIIS) using QuantLib."""
    pricing = _reference_pricing_view(request)
    swap_data = request["swaps"][0]
    trade = swap_data["year_on_year_inflation_swap"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    nominal_curve = _nominal_curve_resolver(pricing, eval_date, request)
    discount = nominal_curve(swap_data["discounting_curve"])

    curve_spec = _find_inflation_curve_spec(pricing,
                                            swap_data["inflation_curve"])
    if curve_spec.get("kind") != "YoYInflation":
        raise ValueError("YYIIS needs a YoYInflation curve")
    index_spec = _find_inflation_index_spec(pricing, curve_spec["index_id"])
    if index_spec["id"] != trade["inflation_index_id"]:
        raise ValueError(
            "YYIIS inflation_index_id does not match the inflation curve's "
            "index")
    ts = _build_yoy_inflation_curve(curve_spec, index_spec, request,
                                    nominal_curve)
    index = _build_inflation_index(
        index_spec, yoy_handle=ql.YoYInflationTermStructureHandle(ts))

    swap_type = ql.Swap.Payer if trade.get("swap_type", "Payer") == "Payer" \
        else ql.Swap.Receiver
    swap = ql.YearOnYearInflationSwap(
        swap_type,
        trade["notional"],
        _build_wire_schedule(trade["fixed_schedule"]),
        trade["fixed_rate"],
        get_day_counter(trade.get("fixed_day_counter", "Actual365Fixed")),
        _build_wire_schedule(trade["yoy_schedule"]),
        index,
        _wire_period(trade, "observation_lag"),
        get_cpi_interpolation(trade.get("observation_interpolation",
                                        "AsIndex")),
        trade.get("spread", 0.0),
        get_day_counter(trade.get("yoy_day_counter", "Actual365Fixed")),
        _calendar_from_enum(trade.get("payment_calendar", "TARGET")),
        get_convention(trade.get("payment_convention", "ModifiedFollowing")))
    # The server prices the YoY leg with a BlackYoYInflationCouponPricer that
    # carries only the nominal discount curve (no optionlet vol surface); the
    # vol is never touched for plain (uncapped) YoY coupons.
    ql.setCouponPricer(
        swap.yoyLeg(),
        ql.BlackYoYInflationCouponPricer(
            ql.YoYOptionletVolatilitySurfaceHandle(), discount))
    swap.setPricingEngine(ql.DiscountingSwapEngine(discount))
    return swap.NPV()


def price_year_on_year_inflation_cap_floor_ql(request: dict) -> float:
    """Price a year-on-year inflation cap/floor/collar using QuantLib.

    Mirrors the server's evaluator field for field:
      * the YoY index projects off the same PiecewiseYoYInflation curve the
        year-on-year swap path builds, and the engine's nominal term structure
        is the trade's discounting curve,
      * the optionlet strip is a yoyInflationLeg over the trade schedule with
        the trade notional / payment day counter / payment adjustment and, when
        present, per-optionlet gearing/spread; the observation interpolation is
        fixed to CPI.AsIndex,
      * the volatility is the referenced YoYOptionletVolSpec rebuilt as a
        ConstantYoYOptionletVolatility whose frequency and index-interpolation
        flag come from the YoY inflation index (so the surface stays
        convention-consistent with its index), settlement days 0,
      * the engine follows the vol spec's vol_type: Black / UnitDisplacedBlack /
        Bachelier.

    Note: QuantLib-Python's ConstantYoYOptionletVolatility constructor does not
    expose the vol-type/displacement arguments, so the surface uses QuantLib's
    default vol type on both sides; the pricing formula is entirely determined
    by the engine, matching the server (which constructs the C++ surface with
    the same defaults).
    """
    pricing = _reference_pricing_view(request)
    cf_data = request["cap_floors"][0]
    trade = cf_data["year_on_year_inflation_cap_floor"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    nominal_curve = _nominal_curve_resolver(pricing, eval_date, request)
    discount = nominal_curve(cf_data["discounting_curve"])

    curve_spec = _find_inflation_curve_spec(pricing, cf_data["inflation_curve"])
    if curve_spec.get("kind") != "YoYInflation":
        raise ValueError("YoY inflation cap/floor needs a YoYInflation curve")
    index_spec = _find_inflation_index_spec(pricing, curve_spec["index_id"])
    if index_spec["id"] != trade["inflation_index_id"]:
        raise ValueError(
            "YoY inflation cap/floor inflation_index_id does not match the "
            "inflation curve's index")
    ts = _build_yoy_inflation_curve(curve_spec, index_spec, request, nominal_curve)
    index = _build_inflation_index(
        index_spec, yoy_handle=ql.YoYInflationTermStructureHandle(ts))

    # Resolve the referenced YoYOptionletVolSpec payload.
    vol_id = cf_data["volatility"]
    vol_payload = None
    for v in pricing.get("vol_surfaces", []):
        if v.get("id") == vol_id:
            vol_payload = v.get("payload", {})
            break
    if vol_payload is None:
        raise ValueError(f"YoY optionlet vol not found: {vol_id}")

    schedule = _build_wire_schedule(trade["schedule"])
    obs_lag = _wire_period(trade, "observation_lag")
    notional = trade["notional"]
    payment_dc = get_day_counter(trade["day_counter"])
    payment_bdc = get_convention(trade["payment_convention"])
    calendar = schedule.calendar()

    gearings = [trade["gearing"]] if "gearing" in trade else []
    spreads = [trade["spread"]] if "spread" in trade else []

    leg = ql.yoyInflationLeg(
        schedule, calendar, index, obs_lag, ql.CPI.AsIndex,
        [notional], payment_dc, payment_bdc, 0, gearings, spreads)

    cf_type = trade["cap_floor_type"]
    if cf_type == "Cap":
        instrument = ql.YoYInflationCap(leg, [trade["cap_rate"]])
    elif cf_type == "Floor":
        instrument = ql.YoYInflationFloor(leg, [trade["floor_rate"]])
    elif cf_type == "Collar":
        instrument = ql.YoYInflationCollar(
            leg, [trade["cap_rate"]], [trade["floor_rate"]])
    else:
        raise ValueError(f"Unknown cap_floor_type: {cf_type}")

    freq = get_frequency(index_spec.get("frequency", "Monthly"))
    interpolated = index_spec.get("interpolated", True)
    vol_handle = ql.YoYOptionletVolatilitySurfaceHandle(
        ql.ConstantYoYOptionletVolatility(
            vol_payload["constant_vol"],
            0,
            _calendar_from_enum(vol_payload["calendar"]),
            get_convention(vol_payload["business_day_convention"]),
            get_day_counter(vol_payload["day_counter"]),
            _wire_period(vol_payload, "observation_lag"),
            freq,
            interpolated))

    engine_type = vol_payload["vol_type"]
    yoy_index = index
    if engine_type == "Bachelier":
        engine = ql.YoYInflationBachelierCapFloorEngine(
            yoy_index, vol_handle, discount)
    elif engine_type == "UnitDisplacedBlack":
        engine = ql.YoYInflationUnitDisplacedBlackCapFloorEngine(
            yoy_index, vol_handle, discount)
    else:
        engine = ql.YoYInflationBlackCapFloorEngine(
            yoy_index, vol_handle, discount)
    instrument.setPricingEngine(engine)
    return instrument.NPV()


def _make_multicurve_exogenous_request() -> dict:
    """
    Build a 2-curve request:
      1) EUR_OIS: OIS discount curve built from OIS helpers
      2) EUR_6M: 6M forwarding curve using swap helper discounting off EUR_OIS (exogenous discount)
    """
    return json.loads(r"""
{
    "pricing": {
        "as_of_date": "2026-01-15",
        "rates": {
            "indices": [
                {
                    "id": "EUR_6M",
                    "name": "Euribor",
                    "index_type": "Ibor",
                    "fixing_days": 2,
                    "calendar": "TARGET",
                    "business_day_convention": "ModifiedFollowing",
                    "day_counter": "Actual360",
                    "end_of_month": false,
                    "currency": "EUR",
                    "tenor": {
                        "n": 6,
                        "unit": "Months"
                    }
                },
                {
                    "id": "EUR_ESTR",
                    "name": "ESTR",
                    "index_type": "Overnight",
                    "fixing_days": 0,
                    "calendar": "TARGET",
                    "business_day_convention": "Following",
                    "day_counter": "Actual360",
                    "end_of_month": true,
                    "currency": "EUR",
                    "tenor": {
                        "n": 0,
                        "unit": "Days"
                    }
                }
            ],
            "curves": [
                {
                    "id": "EUR_OIS",
                    "day_counter": "Actual360",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.03,
                                "overnight_index": {
                                    "id": "EUR_ESTR"
                                },
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "Following",
                                "fixed_leg_day_counter": "Actual360",
                                "payment_lag": 0,
                                "averaging_method": "Compound",
                                "lookback_days": 0,
                                "lockout_days": 0,
                                "apply_observation_shift": false,
                                "tenor": {
                                    "n": 1,
                                    "unit": "Years"
                                }
                            }
                        },
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.029,
                                "overnight_index": {
                                    "id": "EUR_ESTR"
                                },
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "Following",
                                "fixed_leg_day_counter": "Actual360",
                                "payment_lag": 0,
                                "averaging_method": "Compound",
                                "lookback_days": 0,
                                "lockout_days": 0,
                                "apply_observation_shift": false,
                                "tenor": {
                                    "n": 5,
                                    "unit": "Years"
                                }
                            }
                        },
                        {
                            "point_type": "OISHelper",
                            "point": {
                                "rate": 0.028,
                                "overnight_index": {
                                    "id": "EUR_ESTR"
                                },
                                "settlement_days": 2,
                                "calendar": "TARGET",
                                "fixed_leg_frequency": "Annual",
                                "fixed_leg_convention": "Following",
                                "fixed_leg_day_counter": "Actual360",
                                "payment_lag": 0,
                                "averaging_method": "Compound",
                                "lookback_days": 0,
                                "lockout_days": 0,
                                "apply_observation_shift": false,
                                "tenor": {
                                    "n": 10,
                                    "unit": "Years"
                                }
                            }
                        }
                    ]
                },
                {
                    "id": "EUR_6M",
                    "day_counter": "Actual360",
                    "interpolator": "LogLinear",
                    "bootstrap_trait": "Discount",
                    "points": [
                        {
                            "point_type": "DepositHelper",
                            "point": {
                                "rate": 0.032,
                                "fixing_days": 2,
                                "calendar": "TARGET",
                                "business_day_convention": "ModifiedFollowing",
                                "day_counter": "Actual360",
                                "tenor": {
                                    "n": 6,
                                    "unit": "Months"
                                }
                            }
                        },
                        {
                            "point_type": "SwapHelper",
                            "point": {
                                "rate": 0.031,
                                "calendar": "TARGET",
                                "sw_fixed_leg_frequency": "Annual",
                                "sw_fixed_leg_convention": "ModifiedFollowing",
                                "sw_fixed_leg_day_counter": "Thirty360",
                                "float_index": {
                                    "id": "EUR_6M"
                                },
                                "spread": 0.0,
                                "fwd_start_days": 0,
                                "deps": {
                                    "discount_curve": {
                                        "id": "EUR_OIS"
                                    }
                                },
                                "tenor": {
                                    "n": 5,
                                    "unit": "Years"
                                }
                            }
                        }
                    ]
                }
            ]
        }
    },
    "queries": [
        {
            "curve_id": "EUR_OIS",
            "measures": [
                "DF",
                "ZERO",
                "FWD"
            ],
            "grid": {
                "grid_type": "TenorGrid",
                "grid": {
                    "tenors": [
                        {
                            "n": 1,
                            "unit": "Days"
                        },
                        {
                            "n": 1,
                            "unit": "Weeks"
                        },
                        {
                            "n": 1,
                            "unit": "Months"
                        },
                        {
                            "n": 3,
                            "unit": "Months"
                        },
                        {
                            "n": 6,
                            "unit": "Months"
                        },
                        {
                            "n": 1,
                            "unit": "Years"
                        },
                        {
                            "n": 2,
                            "unit": "Years"
                        },
                        {
                            "n": 5,
                            "unit": "Years"
                        },
                        {
                            "n": 10,
                            "unit": "Years"
                        }
                    ],
                    "calendar": "TARGET",
                    "business_day_convention": "Following"
                }
            },
            "zero": {
                "use_curve_day_counter": true,
                "compounding": "Continuous",
                "frequency": "Annual"
            },
            "fwd": {
                "use_curve_day_counter": true,
                "compounding": "Simple",
                "frequency": "Annual",
                "forward_type": "Period",
                "use_grid_calendar_for_advance": true,
                "tenor": {
                    "n": 6,
                    "unit": "Months"
                }
            }
        },
        {
            "curve_id": "EUR_6M",
            "measures": [
                "DF",
                "ZERO",
                "FWD"
            ],
            "grid": {
                "grid_type": "TenorGrid",
                "grid": {
                    "tenors": [
                        {
                            "n": 1,
                            "unit": "Days"
                        },
                        {
                            "n": 1,
                            "unit": "Weeks"
                        },
                        {
                            "n": 1,
                            "unit": "Months"
                        },
                        {
                            "n": 3,
                            "unit": "Months"
                        },
                        {
                            "n": 6,
                            "unit": "Months"
                        },
                        {
                            "n": 1,
                            "unit": "Years"
                        },
                        {
                            "n": 2,
                            "unit": "Years"
                        },
                        {
                            "n": 5,
                            "unit": "Years"
                        },
                        {
                            "n": 10,
                            "unit": "Years"
                        }
                    ],
                    "calendar": "TARGET",
                    "business_day_convention": "Following"
                }
            },
            "zero": {
                "use_curve_day_counter": true,
                "compounding": "Continuous",
                "frequency": "Annual"
            },
            "fwd": {
                "use_curve_day_counter": true,
                "compounding": "Simple",
                "frequency": "Annual",
                "forward_type": "Period",
                "use_grid_calendar_for_advance": true,
                "tenor": {
                    "n": 6,
                    "unit": "Months"
                }
            }
        }
    ]
}
""")


# =============================================================================
# Bootstrap-curves series reference (/bootstrap-curves sampling parity)
# =============================================================================

def _curve_query_grid_dates(grid_spec: dict, reference_date: ql.Date,
                            as_of_date: ql.Date) -> list:
    """Mirror the server's date-grid construction (src/market/grid_utils.cpp).

    TenorGrid: when the grid names a calendar, each tenor is
    calendar.advance(reference_date, tenor, grid bdc); otherwise it is a plain
    reference_date + tenor. RangeGrid: literal day/week stepping (calendar
    advance for month/year steps), optionally filtered to business days
    (WeekendsOnly when no calendar is named).
    """
    grid_type = grid_spec["grid_type"]
    g = grid_spec["grid"]

    if grid_type == "TenorGrid":
        cal_name = g.get("calendar", "NullCalendar")
        use_calendar = cal_name != "NullCalendar"
        if use_calendar:
            cal = get_calendar(cal_name)
            bdc = get_convention(g.get("business_day_convention", "Following"))
        dates = []
        for t in g["tenors"]:
            period = ql.Period(int(t.get("n", 0)), get_time_unit(t.get("unit", "Days")))
            if use_calendar:
                dates.append(cal.advance(reference_date, period, bdc))
            else:
                dates.append(reference_date + period)
        return dates

    if grid_type == "RangeGrid":
        start = parse_date(g["start_date"]) if g.get("start_date") else as_of_date
        end = parse_date(g["end_date"])
        step_n = max(1, int(g.get("step_number", 1)))
        step_unit = g.get("step_time_unit", "Days")
        business_only = g.get("business_days_only", False)
        cal_name = g.get("calendar", "NullCalendar")
        if cal_name == "NullCalendar":
            cal = ql.WeekendsOnly() if business_only else ql.NullCalendar()
        else:
            cal = get_calendar(cal_name)
        bdc = get_convention(g.get("business_day_convention", "Following"))
        dates = []
        current = start
        while current <= end:
            if not business_only or cal.isBusinessDay(current):
                dates.append(current)
            if step_unit == "Days":
                current = current + step_n
            elif step_unit == "Weeks":
                current = current + 7 * step_n
            else:
                current = cal.advance(
                    current, ql.Period(step_n, get_time_unit(step_unit)), bdc)
        return dates

    raise ValueError(f"Unsupported grid_type: {grid_type}")


def _curve_fallback_calendar(curve_json: dict):
    """Mirror the server's calendarFromTermStructure: the first helper's
    calendar when it has one, TARGET otherwise."""
    points = curve_json.get("points", [])
    if points:
        cal_name = points[0].get("point", {}).get("calendar")
        if cal_name:
            return get_calendar(cal_name)
    return ql.TARGET()


def _resolve_fwd_calendar_bdc(query: dict, curve_json: dict):
    """Mirror grid_utils ResolveCalendar / ResolveBusinessDayConvention:
    options override > grid's own calendar/bdc > curve fallback calendar."""
    options = query.get("options")
    grid = query.get("grid", {}).get("grid", {})
    if options and options.get("calendar", "NullCalendar") != "NullCalendar":
        cal = get_calendar(options["calendar"])
    elif grid.get("calendar", "NullCalendar") != "NullCalendar":
        cal = get_calendar(grid["calendar"])
    else:
        cal = _curve_fallback_calendar(curve_json)
    if options is not None:
        bdc = get_convention(options.get("business_day_convention", "Following"))
    else:
        bdc = get_convention(grid.get("business_day_convention", "Following"))
    return cal, bdc


def bootstrap_curves_ql(request: dict) -> dict:
    """Reference for /bootstrap-curves: build the queried curve independently
    and sample it exactly the way the server does.

    Returns {measure_name: [values]} for the request's single query, one value
    per grid date, in the query's measure order. The curve is built with the
    same machinery the NPV families use (build_curve_from_json); the grid and
    the per-measure sampling mirror src/market/grid_utils.cpp and
    src/evaluators/bootstrap_curves_evaluator.cpp:

      * DF    -> curve.discount(d)
      * ZERO  -> curve.zeroRate(d', dc, compounding, frequency) with d'
                 clamped to referenceDate+1 when d <= referenceDate; dc is the
                 curve's own day counter unless use_curve_day_counter=false
      * FWD   -> curve.forwardRate(d, end, dc, compounding, frequency) where
                 end is d + eps (Instantaneous) or a calendar advance by the
                 query tenor (Period), floored at d+1
    """
    pricing = _reference_pricing_view(request)
    as_of = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = as_of

    queries = request.get("queries") or []
    if len(queries) != 1:
        raise ValueError("bootstrap_curves_ql expects exactly one curve query")
    query = queries[0]

    curve_id = query["curve_id"]
    curve_json = next(c for c in pricing["curves"] if c["id"] == curve_id)
    curve = build_curve_from_json(curve_json, as_of, request)

    # The server anchors tenor grids on the curve's own reference date
    # (TermStructure.reference_date, falling back to as_of_date).
    grid_ref = (parse_date(curve_json["reference_date"])
                if curve_json.get("reference_date") else as_of)
    grid_dates = _curve_query_grid_dates(query["grid"], grid_ref, as_of)

    curve_ref = curve.referenceDate()
    out = {}
    for measure in query["measures"]:
        if measure == "DF":
            values = [curve.discount(d) for d in grid_dates]
        elif measure == "ZERO":
            zq = query.get("zero") or {}
            dc = (curve.dayCounter() if zq.get("use_curve_day_counter", True)
                  else get_day_counter(zq.get("day_counter", "Actual365Fixed")))
            comp = get_compounding(zq.get("compounding", "Continuous"))
            freq = get_frequency(zq.get("frequency", "Annual"))
            values = []
            for d in grid_dates:
                dd = curve_ref + 1 if d <= curve_ref else d
                values.append(curve.zeroRate(dd, dc, comp, freq).rate())
        elif measure == "FWD":
            fq = query.get("fwd") or {}
            dc = (curve.dayCounter() if fq.get("use_curve_day_counter", True)
                  else get_day_counter(fq.get("day_counter", "Actual365Fixed")))
            comp = get_compounding(fq.get("compounding", "Simple"))
            freq = get_frequency(fq.get("frequency", "Annual"))
            instantaneous = (
                fq.get("forward_type", "Instantaneous") == "Instantaneous")
            if fq.get("use_grid_calendar_for_advance", True):
                cal, bdc = _resolve_fwd_calendar_bdc(query, curve_json)
            else:
                cal, bdc = _curve_fallback_calendar(curve_json), ql.Following
            values = []
            for d in grid_dates:
                if instantaneous:
                    eps_n = int(fq.get("instantaneous_eps_number", 1))
                    eps_unit = fq.get("instantaneous_eps_time_unit", "Days")
                    if eps_unit == "Days":
                        end = d + eps_n
                    else:
                        end = cal.advance(
                            d, ql.Period(eps_n, get_time_unit(eps_unit)), bdc)
                else:
                    t_n, t_unit = _period_n_unit(fq, "tenor", 0, "Days")
                    end = cal.advance(d, ql.Period(t_n, get_time_unit(t_unit)), bdc)
                if end <= d:
                    end = d + 1
                values.append(curve.forwardRate(d, end, dc, comp, freq).rate())
        else:
            raise ValueError(f"Unsupported curve measure: {measure}")
        out[measure] = values
    return out


# =============================================================================
# Calendar Utility Reference (exact-match parity cases)
# =============================================================================

def _calendar_from_enum(name: str):
    """QuantLib calendar for a wire ``enums.Calendar`` value.

    Mirrors the server's CalendarToQL (src/common/enum_convert.cpp) entry for
    entry, including the market variants QuantLib defaults to when the C++
    side default-constructs a calendar (e.g. UnitedKingdom -> Settlement,
    Germany -> FrankfurtStockExchange, China -> SSE). Note in particular that
    the plain "UnitedStates" enum maps to the Settlement market on the
    server, so it does here too.

    Raises on anything unmapped instead of falling back to a default —
    exact-match calendar cases must never silently compare against the wrong
    calendar. (BespokeCalendar is deliberately unmapped: an empty bespoke
    calendar has no holiday content worth pinning.)
    """
    mapping = {
        "Argentina": ql.Argentina,
        "Australia": ql.Australia,
        "Brazil": ql.Brazil,
        "Canada": ql.Canada,
        "China": ql.China,
        "CzechRepublic": ql.CzechRepublic,
        "Denmark": ql.Denmark,
        "Finland": ql.Finland,
        "Germany": ql.Germany,
        "HongKong": ql.HongKong,
        "Hungary": ql.Hungary,
        "Iceland": ql.Iceland,
        "India": ql.India,
        "Indonesia": ql.Indonesia,
        "Israel": ql.Israel,
        "Italy": ql.Italy,
        "Japan": ql.Japan,
        "Mexico": ql.Mexico,
        "NewZealand": ql.NewZealand,
        "Norway": ql.Norway,
        "NullCalendar": ql.NullCalendar,
        "Poland": ql.Poland,
        "Romania": ql.Romania,
        "Russia": ql.Russia,
        "SaudiArabia": ql.SaudiArabia,
        "Singapore": ql.Singapore,
        "Slovakia": ql.Slovakia,
        "SouthAfrica": ql.SouthAfrica,
        "SouthKorea": ql.SouthKorea,
        "Sweden": ql.Sweden,
        "Switzerland": ql.Switzerland,
        "TARGET": ql.TARGET,
        "Taiwan": ql.Taiwan,
        "Turkey": ql.Turkey,
        "Ukraine": ql.Ukraine,
        "UnitedKingdom": ql.UnitedKingdom,
        "WeekendsOnly": ql.WeekendsOnly,
    }
    if name in mapping:
        return mapping[name]()
    us_markets = {
        "UnitedStates": ql.UnitedStates.Settlement,
        "UnitedStatesSettlement": ql.UnitedStates.Settlement,
        "UnitedStatesNYSE": ql.UnitedStates.NYSE,
        "UnitedStatesGovernmentBond": ql.UnitedStates.GovernmentBond,
        "UnitedStatesNERC": ql.UnitedStates.NERC,
    }
    if name in us_markets:
        return ql.UnitedStates(us_markets[name])
    raise ValueError(f"Calendar enum value not mapped by the reference: {name}")


def _iso_date(d: ql.Date) -> str:
    """Format a QuantLib date the way the server does (io::iso_date)."""
    return f"{d.year():04d}-{int(d.month()):02d}-{d.dayOfMonth():02d}"


def calendar_business_days_ql(request: dict) -> list:
    """Business dates in [start_date, end_date], as ISO strings.

    Mirrors CalendarBusinessDaysEvaluator: walk every calendar day from
    start_date to end_date inclusive, skip the endpoints when
    include_start / include_end are false, and keep the dates the calendar
    calls business days.
    """
    cal = _calendar_from_enum(request.get("calendar", "TARGET"))
    start = parse_date(request["start_date"])
    end = parse_date(request["end_date"])
    include_start = request.get("include_start", True)
    include_end = request.get("include_end", True)
    dates = []
    d = start
    while d <= end:
        if not (d == start and not include_start) \
                and not (d == end and not include_end) \
                and cal.isBusinessDay(d):
            dates.append(_iso_date(d))
        d = d + 1
    return dates


def calendar_holidays_ql(request: dict) -> list:
    """Holiday dates in [start_date, end_date], as ISO strings.

    Mirrors CalendarHolidaysEvaluator: walk every calendar day in the
    inclusive range, keep the non-business days, and drop plain weekends
    unless include_weekends is set (weekend dates that are also listed
    holidays still count as weekends for this filter, exactly like the
    server's isWeekend check).
    """
    cal = _calendar_from_enum(request.get("calendar", "TARGET"))
    start = parse_date(request["start_date"])
    end = parse_date(request["end_date"])
    include_weekends = request.get("include_weekends", False)
    dates = []
    d = start
    while d <= end:
        if not cal.isBusinessDay(d):
            if include_weekends or not cal.isWeekend(d.weekday()):
                dates.append(_iso_date(d))
        d = d + 1
    return dates


def calendar_advance_ql(request: dict) -> str:
    """The advanced date, as an ISO string.

    Mirrors CalendarAdvanceEvaluator/Mapper: Calendar::advance(date,
    Period(tenor_number, tenor_unit), convention, end_of_month) with the
    schema defaults (tenor_number 0, Days, Following, end_of_month false).
    """
    cal = _calendar_from_enum(request.get("calendar", "TARGET"))
    date = parse_date(request["date"])
    period = ql.Period(int(request.get("tenor_number", 0)),
                       get_time_unit(request.get("tenor_unit", "Days")))
    convention = get_convention(request.get("convention", "Following"))
    end_of_month = request.get("end_of_month", False)
    return _iso_date(cal.advance(date, period, convention, end_of_month))


# =============================================================================
# Equity Option Reference Pricer
# =============================================================================

def price_equity_option_ql(request: dict) -> float:
    """Price an equity option using QuantLib, mirroring the server's engine
    selection across exercise styles and payoffs.

    Mirrors the server's equity-option evaluator field for field:
      * the underlying spec resolves its spot from pricing.quotes
        (spot_quote_id) and its dividend yield from the referenced curve in
        pricing.rates.curves, and the trade's discounting_curve is the
        risk-free leg — the three feed a BlackScholesMertonProcess together
        with the referenced Black vol,
      * the volatility is the referenced BlackVolSpec (shape=Constant only,
        like the server's equity path) rebuilt as a BlackConstantVol with the
        spec's own reference date, calendar and day count,
      * the payoff is the wire-selected EquityPayoff (plain vanilla or a
        cash-/asset-or-nothing digital) and the exercise the wire-selected
        EquityExercise (European, American or Bermudan). The engine follows
        the same combination the server routes on: analytic European for
        European vanilla, the finite-difference Black-Scholes engine for
        American/Bermudan vanilla, the analytic European engine for European
        digitals and the analytic digital-American engine for at-hit American
        digitals,
      * discrete cash dividends declared on the underlying spec
        (EquityUnderlyingSpec.discrete_dividends: ex_date + amount) are
        turned into a FixedDividend schedule (via DividendVector) and priced
        with AnalyticDividendEuropeanEngine — the same escrowed-dividend
        analytic European engine the server now uses. They are layered on
        top of the continuous dividend-yield curve, which stays in the
        process. With no discrete dividends the plain no-dividend engines
        are used, exactly like the server,
      * the engine follows the referenced model: AnalyticEuropeanEngine
        (or AnalyticDividendEuropeanEngine when discrete dividends are
        present) for BlackScholesAnalytic, BinomialCRRVanillaEngine with the
        spec's own binomial_steps for BinomialCRR (the server builds
        BinomialVanillaEngine<CoxRossRubinstein> with the same step count),
      * the returned NPV is the per-option NPV scaled by the trade's
        quantity, matching the server's response field.

    Deliberately refuses (raises) what it cannot faithfully reproduce:
    barrier features, and discrete dividends combined with the binomial
    model (the server restricts discrete dividends to the analytic
    BlackScholesAnalytic path).
    """
    pricing = _reference_pricing_view(request)
    opt_data = request["options"][0]
    opt = opt_data["option"]

    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    if opt.get("barrier"):
        raise ValueError("Equity barrier options are not reference-priced yet")

    underlying = next(
        (u for u in pricing.get("equity_underlyings", [])
         if u.get("id") == opt["underlying_id"]), None)
    if underlying is None:
        raise ValueError(f"Equity underlying not found: {opt['underlying_id']}")

    # Discrete cash dividends declared on the underlying spec (ex_date +
    # amount). Escrowed cash events layered on top of the continuous dividend
    # curve; empty when the spec carries none.
    div_dates = []
    div_amounts = []
    for dd in underlying.get("discrete_dividends", []) or []:
        div_dates.append(parse_date(dd["ex_date"]))
        div_amounts.append(dd["amount"])

    # Spot resolved from pricing.quotes by spot_quote_id (QuoteRegistry).
    spot_value = next(
        (q.get("value") for q in pricing.get("quotes", [])
         if q.get("id") == underlying["spot_quote_id"]), None)
    if spot_value is None:
        raise ValueError(f"Unknown quote id: {underlying['spot_quote_id']}")
    spot = ql.QuoteHandle(ql.SimpleQuote(spot_value))

    # Risk-free = the trade's discounting curve; dividend yield = the
    # underlying spec's curve. Both are ordinary pricing.rates.curves entries.
    disc_id = opt_data["discounting_curve"]
    disc_json = next(c for c in pricing["curves"] if c["id"] == disc_id)
    risk_free = build_curve_from_json(disc_json, eval_date, request)
    div_id = underlying["dividend_yield_curve_id"]
    div_json = next(c for c in pricing["curves"] if c["id"] == div_id)
    dividend = build_curve_from_json(div_json, eval_date, request)

    # Volatility: the referenced BlackVolSpec rebuilt as a BlackConstantVol
    # (the server's shape=Constant branch: BlackConstantVol(ref, cal, vol, dc)).
    vol_spec = next(
        (v for v in pricing.get("vol_surfaces", [])
         if v.get("id") == opt_data.get("volatility")), None)
    if vol_spec is None:
        raise ValueError(f"Black vol not found: {opt_data.get('volatility')}")
    vol_base = vol_spec.get("payload", {}).get("base", {})
    if vol_base.get("shape", "Constant") != "Constant":
        raise ValueError(
            "Only shape=Constant BlackVolSpec is reference-priced for equity")
    black_vol = ql.BlackVolTermStructureHandle(ql.BlackConstantVol(
        parse_date(vol_base.get("reference_date", pricing["as_of_date"])),
        get_calendar(vol_base.get("calendar", "TARGET")),
        vol_base["constant_vol"],
        get_day_counter(vol_base.get("day_counter", "Actual365Fixed")),
    ))

    process = ql.BlackScholesMertonProcess(spot, dividend, risk_free, black_vol)

    # Payoff: EquityPayoff union — plain vanilla or one of the two digital
    # payoffs. The server builds the matching QL StrikedTypePayoff.
    po = opt["payoff"]
    opt_type = ql.Option.Put if po.get("option_type", "Call") == "Put" else ql.Option.Call
    payoff_type = opt.get("payoff_type")
    if payoff_type == "EquityPlainVanillaPayoff":
        payoff = ql.PlainVanillaPayoff(opt_type, po["strike"])
        is_plain = True
    elif payoff_type == "EquityCashOrNothingPayoff":
        payoff = ql.CashOrNothingPayoff(opt_type, po["strike"], po["cash"])
        is_plain = False
    elif payoff_type == "EquityAssetOrNothingPayoff":
        payoff = ql.AssetOrNothingPayoff(opt_type, po["strike"])
        is_plain = False
    else:
        raise ValueError(f"Unsupported EquityPayoff: {payoff_type}")

    # Exercise: EquityExercise union — European / American / Bermudan. The
    # server reads the style back from the QL exercise object.
    exercise_type = opt.get("exercise_type")
    ex = opt["exercise"]
    if exercise_type == "EquityEuropeanExercise":
        exercise = ql.EuropeanExercise(parse_date(ex["expiry_date"]))
    elif exercise_type == "EquityAmericanExercise":
        end = parse_date(ex["end_date"])
        if ex.get("start_date"):
            exercise = ql.AmericanExercise(parse_date(ex["start_date"]), end)
        else:
            exercise = ql.AmericanExercise(end)
    elif exercise_type == "EquityBermudanExercise":
        dates = sorted(parse_date(d) for d in ex["exercise_dates"])
        exercise = ql.BermudanExercise(dates)
    else:
        raise ValueError(f"Unsupported EquityExercise: {exercise_type}")

    option = ql.VanillaOption(payoff, exercise)

    # Engine follows the referenced model spec (required on the server) and the
    # exercise/payoff combination, mirroring the server's selection exactly.
    model_spec = next(
        (m for m in pricing.get("models", [])
         if m.get("id") == opt_data.get("model")), None)
    if model_spec is None:
        raise ValueError(f"Model not found: {opt_data.get('model')}")
    model_payload = model_spec.get("payload", {})
    model_type = model_payload.get("model_type", "BlackScholesAnalytic")

    if is_plain:
        if exercise_type == "EquityEuropeanExercise":
            if model_type == "BlackScholesAnalytic":
                if div_dates:
                    # Escrowed discrete-dividend analytic European engine —
                    # mirrors the server's AnalyticDividendEuropeanEngine.
                    schedule = ql.DividendVector(div_dates, div_amounts)
                    engine = ql.AnalyticDividendEuropeanEngine(process, schedule)
                else:
                    engine = ql.AnalyticEuropeanEngine(process)
            elif model_type == "BinomialCRR":
                if div_dates:
                    raise ValueError(
                        "Discrete cash dividends currently require "
                        "model_type=BlackScholesAnalytic")
                steps = int(model_payload.get("binomial_steps", 500))
                if steps <= 0:
                    raise ValueError(
                        "EquityVanillaModelSpec.binomial_steps must be > 0")
                engine = ql.BinomialCRRVanillaEngine(process, steps)
            else:
                raise ValueError(f"Unsupported EquityModelType: {model_type}")
        else:
            # American / Bermudan plain vanilla → finite-difference BS engine,
            # with the same default grid the server uses.
            if model_type != "BlackScholesAnalytic":
                raise ValueError(
                    "American and Bermudan equity options require "
                    "model_type=BlackScholesAnalytic")
            if div_dates:
                raise ValueError(
                    "Discrete cash dividends are not supported for "
                    "American or Bermudan equity options")
            engine = ql.FdBlackScholesVanillaEngine(process)
    else:
        # Digital payoffs.
        if model_type != "BlackScholesAnalytic":
            raise ValueError(
                "Digital equity payoffs require model_type=BlackScholesAnalytic")
        if div_dates:
            raise ValueError(
                "Discrete cash dividends are not supported for digital "
                "equity payoffs")
        if exercise_type == "EquityEuropeanExercise":
            engine = ql.AnalyticEuropeanEngine(process)
        elif exercise_type == "EquityAmericanExercise":
            engine = ql.AnalyticDigitalAmericanEngine(process)
        else:
            raise ValueError(
                "Bermudan exercise is not supported for digital equity payoffs")

    option.setPricingEngine(engine)

    return option.NPV() * opt.get("quantity", 1.0)


# =============================================================================
# Vol surface sampling + calibration references
# =============================================================================

def _find_vol_surface_spec(pricing: dict, vol_id: str) -> dict:
    spec = next((v for v in pricing.get("vol_surfaces", [])
                 if v.get("id") == vol_id), None)
    if spec is None:
        raise ValueError(f"Vol surface not found: {vol_id}")
    return spec


def _find_swap_index_def(pricing: dict, swap_index_id: str) -> dict:
    sidx = next((s for s in pricing.get("swap_indices", [])
                 if s.get("id") == swap_index_id), None)
    if sidx is None:
        raise ValueError(f"Swap index not found: {swap_index_id}")
    return sidx


def _wire_period_to_ql(p: dict) -> ql.Period:
    n, unit = _period_n_unit(p, "tenor", 0, "Months")
    return ql.Period(n, get_time_unit(unit))


def _frequency_to_period(freq) -> ql.Period:
    """Mirror of the server's frequencyToPeriod for swap-index fixed legs."""
    return {
        ql.Annual: ql.Period(1, ql.Years),
        ql.Semiannual: ql.Period(6, ql.Months),
        ql.Quarterly: ql.Period(3, ql.Months),
        ql.Monthly: ql.Period(1, ql.Months),
    }[freq]


def _ir_vol_base(base: dict):
    """Decode an IrVolBaseSpec: (ref_date, calendar, bdc, day_counter,
    vol_type, displacement). Refuses quote-referenced vols (no catalog case
    exercises the quote indirection)."""
    if base.get("quote_id"):
        raise ValueError(
            "quote-referenced vols are not reference-priced yet (inline "
            "constant_vol / matrix values only)")
    return (
        parse_date(base["reference_date"]),
        get_calendar(base.get("calendar", "TARGET")),
        get_convention(base.get("business_day_convention", "ModifiedFollowing")),
        get_day_counter(base.get("day_counter", "Actual365Fixed")),
        get_volatility_type(base.get("volatility_type", "Lognormal")),
        base.get("displacement", 0.0),
    )


def _build_swaption_atm_matrix(inner: dict, ref, cal, bdc, dc, vol_type,
                               displacement):
    """Rebuild the server's SwaptionVolatilityMatrix for an AtmMatrixSpec.

    The server constructs the matrix from Periods; the Python bindings only
    expose the date-based constructor, so the option dates are derived the
    exact way QuantLib's Period constructor does internally:
    calendar.advance(reference_date, period, bdc).
    """
    expiries = [_wire_period_to_ql(p) for p in inner["expiries"]]
    tenors = [_wire_period_to_ql(p) for p in inner["tenors"]]
    vols = inner["vols"]
    if vols.get("quote_ids"):
        raise ValueError("quote-referenced matrix vols are not supported here")
    n_rows, n_cols = vols["n_rows"], vols["n_cols"]
    matrix = ql.Matrix(n_rows, n_cols)
    for i in range(n_rows):
        for j in range(n_cols):
            matrix[i][j] = vols["values"][i * n_cols + j]
    option_dates = [cal.advance(ref, p, bdc) for p in expiries]
    shifts = (ql.Matrix(n_rows, n_cols, displacement)
              if displacement != 0.0 else ql.Matrix())
    svm = ql.SwaptionVolatilityMatrix(
        ref, cal, bdc, option_dates, tenors, matrix, dc, False, vol_type,
        shifts)
    return svm, expiries, tenors


def _tenor_grid_dates(grid_spec: dict, ref, fallback_cal, fallback_bdc):
    """Build a TenorGrid's dates the way the sampling evaluator does for
    optionlet / equity surfaces: calendar.advance(reference_date, p, bdc),
    with the grid's own calendar/bdc taking effect when the grid sets an
    explicit calendar, else the surface's own conventions.

    Only TenorGrid is supported (the catalog cases use tenor grids); the
    reference deliberately raises on RangeGrid rather than approximating the
    server's stepping loop untested.
    """
    if grid_spec.get("grid_type", "TenorGrid") != "TenorGrid":
        raise ValueError("Only TenorGrid expiry grids are reference-sampled")
    grid = grid_spec["grid"]
    if grid.get("calendar"):
        cal = get_calendar(grid["calendar"])
        bdc = get_convention(grid.get("business_day_convention", "Following"))
    else:
        cal, bdc = fallback_cal, fallback_bdc
    return [cal.advance(ref, _wire_period_to_ql(p), bdc)
            for p in grid["tenors"]]


def sample_vol_surfaces_ql(request: dict) -> list:
    """Sample a vol surface exactly like the server's /sample-vol-surfaces.

    Returns the flat `vols` list the response's results[0] must carry,
    computed from an independently rebuilt QuantLib vol structure sampled at
    independently recomputed query points. Supported subset (everything else
    raises rather than approximating):

      * Swaption surfaces of kind Constant (ConstantSwaptionVolatility) and
        AtmMatrix2D (SwaptionVolatilityMatrix, bilinear in option time and
        swap length), Cube and ExpirySlice output modes, TenorGrid expiry
        and tenor grids, AbsoluteStrike axis. Exercise dates, effective swap
        start/end dates (spot lag + fixed-leg schedule end) and the
        option-time / swap-length year fractions mirror the evaluator's
        computation node for node.
      * Optionlet surfaces (constant vol only — the only shape on the wire),
        sampled per (grid date, strike).
      * EquityBlack surfaces of shape Constant (BlackConstantVol) and
        AtmMatrix2D term vols (BlackVarianceCurve, linear in variance,
        forceMonotoneVariance=true like the server's parser), sampled per
        (grid date, strike).

    Not supported (deliberately): smile cubes / SABR sampling (needs the
    forward-aware cube the swaption family also defers), SpreadFromATM strike
    axes, RangeGrid grids, SmileSlice/TermSlice modes, quote-referenced vols
    and multi-query requests.
    """
    pricing = _reference_pricing_view(request)
    queries = request.get("queries", [])
    if len(queries) != 1:
        raise ValueError("Reference samples exactly one query per request")
    q = queries[0]
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    options = q.get("options", {})
    if options.get("calendar"):
        raise ValueError("QueryOptions.calendar overrides are not supported")
    allow_extrapolation = options.get("allow_extrapolation", True)
    strike_grid = q.get("strike_grid", {})
    if strike_grid.get("axis", "AbsoluteStrike") != "AbsoluteStrike":
        raise ValueError("Only AbsoluteStrike sampling is reference-supported")
    strikes = strike_grid["strikes"]

    surface_type = q.get("surface_type", "Swaption")
    vol_spec = _find_vol_surface_spec(pricing, q["vol_id"])
    payload = vol_spec.get("payload", {})

    if surface_type == "Swaption":
        if vol_spec.get("payload_type") != "SwaptionVolSpec":
            raise ValueError("vol_id does not reference a SwaptionVolSpec")
        inner_type = payload.get("payload_type")
        inner = payload.get("payload", {})
        base = inner.get("base", {})
        ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)

        if inner_type == "SwaptionVolConstantSpec":
            handle = ql.ConstantSwaptionVolatility(
                ref, cal, bdc, base["constant_vol"], dc, vol_type,
                displacement)
        elif inner_type == "SwaptionVolAtmMatrixSpec":
            handle, _, _ = _build_swaption_atm_matrix(
                inner, ref, cal, bdc, dc, vol_type, displacement)
        else:
            raise ValueError(
                f"Swaption sampling of {inner_type} is not reference-supported"
                " (smile cubes and SABR surfaces are deferred)")
        if allow_extrapolation:
            handle.enableExtrapolation()

        # Swaption grid dates come from the swap index's fixed-leg
        # conventions, not the grid's own calendar fields (the evaluator's
        # TenorGrid branch): exercise = fixed_cal.advance(ref, p, fixed_bdc).
        sidx = _find_swap_index_def(pricing, payload["swap_index_id"])
        if q.get("swap_index_id") and q["swap_index_id"] != sidx["id"]:
            raise ValueError("Query swap_index_id must match the surface's")
        fixed = sidx["fixed_leg"]
        fixed_cal = get_calendar(fixed["fixed_calendar"])
        fixed_bdc = get_convention(fixed["fixed_bdc"])
        fixed_term_bdc = get_convention(fixed["fixed_term_bdc"])
        fixed_rule = get_date_generation(fixed["fixed_date_rule"])
        fixed_eom = fixed.get("fixed_eom", False)
        fixed_freq = get_frequency(fixed["fixed_frequency"])
        spot_days = sidx.get("spot_days", 0)

        exp_grid = q["expiry_grid"]
        if exp_grid.get("grid_type", "TenorGrid") != "TenorGrid":
            raise ValueError("Only TenorGrid expiry grids are supported")
        exercises = [fixed_cal.advance(ref, _wire_period_to_ql(p), fixed_bdc)
                     for p in exp_grid["grid"]["tenors"]]
        ten_grid = q["tenor_grid"]
        if ten_grid.get("grid_type", "TenorGrid") != "TenorGrid":
            raise ValueError("Only TenorGrid tenor grids are supported")
        tenors = [_wire_period_to_ql(p) for p in ten_grid["grid"]["tenors"]]

        def node_times(exercise, tenor):
            # Mirrors the evaluator's computeSwaptionDates + sampleVol:
            # spot-lagged start, fixed-leg schedule end date, option time
            # from the evaluation date and swap length between start/end,
            # all on the surface's own day counter.
            start = exercise
            if spot_days > 0:
                start = fixed_cal.advance(exercise, spot_days, ql.Days,
                                          fixed_bdc)
            tentative_end = fixed_cal.advance(start, tenor, fixed_term_bdc)
            schedule = ql.Schedule(
                start, tentative_end, ql.Period(fixed_freq), fixed_cal,
                fixed_bdc, fixed_term_bdc, fixed_rule, fixed_eom)
            option_time = max(1.0e-8, dc.yearFraction(eval_date, exercise))
            swap_length = max(1.0e-8,
                              dc.yearFraction(start, schedule.endDate()))
            return option_time, swap_length

        mode = q.get("output_mode", "Cube")
        out = []
        if mode == "Cube":
            for exercise in exercises:
                for tenor in tenors:
                    option_time, swap_length = node_times(exercise, tenor)
                    for strike in strikes:
                        out.append(handle.volatility(
                            option_time, swap_length, strike, True))
        elif mode == "ExpirySlice":
            tenor = tenors[q["slice_tenor_index"]]
            if not q.get("slice_strike_is_set"):
                raise ValueError("ExpirySlice requires slice_strike_is_set")
            strike = q["slice_strike"]
            for exercise in exercises:
                option_time, swap_length = node_times(exercise, tenor)
                out.append(handle.volatility(
                    option_time, swap_length, strike, True))
        else:
            raise ValueError(
                f"Output mode {mode} is not reference-supported")
        return out

    if surface_type == "Optionlet":
        if vol_spec.get("payload_type") != "OptionletVolSpec":
            raise ValueError("vol_id does not reference an OptionletVolSpec")
        base = payload.get("base", {})
        ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)
        handle = ql.ConstantOptionletVolatility(
            ref, cal, bdc, base["constant_vol"], dc, vol_type, displacement)
        if allow_extrapolation:
            handle.enableExtrapolation()
        dates = _tenor_grid_dates(q["expiry_grid"], ref, cal, bdc)
        return [handle.volatility(d, strike, True)
                for d in dates for strike in strikes]

    if surface_type == "EquityBlack":
        if vol_spec.get("payload_type") != "BlackVolSpec":
            raise ValueError("vol_id does not reference a BlackVolSpec")
        base = payload.get("base", {})
        if base.get("quote_id"):
            raise ValueError("quote-referenced vols are not supported here")
        ref = parse_date(base["reference_date"])
        cal = get_calendar(base.get("calendar", "TARGET"))
        bdc = get_convention(
            base.get("business_day_convention", "ModifiedFollowing"))
        dc = get_day_counter(base.get("day_counter", "Actual365Fixed"))
        shape = base.get("shape", "Constant")
        if shape == "Constant":
            handle = ql.BlackConstantVol(ref, cal, base["constant_vol"], dc)
        elif shape == "AtmMatrix2D":
            # Server: pillar dates via cal.advance(ref, p, bdc), then a
            # BlackVarianceCurve (linear total variance) with
            # forceMonotoneVariance=true.
            term_vols = payload["term_vols"]
            if term_vols.get("quote_ids"):
                raise ValueError("quote-referenced term vols not supported")
            pillar_dates = [cal.advance(ref, _wire_period_to_ql(p), bdc)
                            for p in payload["expiries"]]
            handle = ql.BlackVarianceCurve(
                ref, pillar_dates, term_vols["values"], dc, True)
        else:
            raise ValueError(
                f"EquityBlack shape {shape} is not reference-supported")
        if allow_extrapolation:
            handle.enableExtrapolation()
        dates = _tenor_grid_dates(q["expiry_grid"], ref, cal, bdc)
        return [handle.blackVol(d, strike, True)
                for d in dates for strike in strikes]

    raise ValueError(f"Unknown surface_type: {surface_type}")


def calibrate_swaption_model_ql(request: dict) -> dict:
    """Run the server's Hull-White swaption calibration independently.

    Mirrors the server's calibration routine step for step: one SwaptionHelper
    per (expiry, tenor) grid node quoted at the surface's vol for that node
    (constant vol, or the ATM matrix interpolated at the node), priced on a
    JamshidianSwaptionEngine over HullWhite(discount_curve, a_init,
    sigma_init), calibrated with LevenbergMarquardt under
    EndCriteria(function_evaluations, max_iterations, eps, eps, eps) and the
    spec's calibrate_a/calibrate_sigma fixed-parameter flags. The helper error
    metric follows the server's vol-type pairing: PriceError for Normal vols,
    ImpliedVolError for Black/shifted Black.

    Returns the response fields to compare: hw_a, hw_sigma, rmse (root mean
    square of the helpers' calibrationError, same metric) and num_helpers.

    Both sides execute the same QuantLib C++ code, so on a well-conditioned
    problem the calibrated parameters agree to ~1e-11 (see the catalog
    tolerance note). Ill-conditioned setups — both parameters free against
    vols no Hull-White model can fit — leave a flat valley in `a` where the
    two builds stop at measurably different (equally valid) points; such
    cases are deliberately not in the catalog.
    """
    pricing = _reference_pricing_view(request)
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    model = next((m for m in pricing.get("models", [])
                  if m.get("id") == request["model_id"]), None)
    if model is None:
        raise ValueError(f"Model not found: {request['model_id']}")
    calib = request.get("calibration") or model["payload"].get(
        "hw_calibration")
    if not calib:
        raise ValueError("No calibration spec on request or model")

    disc_json = next(c for c in pricing["curves"]
                     if c["id"] == calib["discount_curve_id"])
    discount = build_curve_from_json(disc_json, eval_date, request)
    fwd_json = next(c for c in pricing["curves"]
                    if c["id"] == calib["forwarding_curve_id"])
    forwarding = build_curve_from_json(fwd_json, eval_date, request)

    sidx = _find_swap_index_def(pricing, calib["swap_index_id"])
    if sidx.get("kind", "IborSwapIndex") != "IborSwapIndex":
        raise ValueError("Hull-White calibration supports Ibor swap indices")
    fixed = sidx["fixed_leg"]
    ibor = resolve_index_from_id(sidx["float_index_id"], request, forwarding)

    vol_spec = _find_vol_surface_spec(pricing, calib["swaption_vol_id"])
    wrapper = vol_spec["payload"]
    inner_type = wrapper.get("payload_type")
    inner = wrapper.get("payload", {})
    base = inner.get("base", {})
    ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)

    matrix_expiries = matrix_tenors = None
    if inner_type == "SwaptionVolConstantSpec":
        constant_vol = base["constant_vol"]

        def market_vol(expiry, tenor):
            return constant_vol
    elif inner_type == "SwaptionVolAtmMatrixSpec":
        svm, matrix_expiries, matrix_tenors = _build_swaption_atm_matrix(
            inner, ref, cal, bdc, dc, vol_type, displacement)

        def market_vol(expiry, tenor):
            # The server calls volatility(Period, Period, 0, extrapolate);
            # QuantLib resolves the option Period through the structure's own
            # calendar/bdc, which the Python bindings only expose via the
            # (Date, Period) overload.
            return svm.volatility(cal.advance(ref, expiry, bdc), tenor, 0.0,
                                  True)
    else:
        raise ValueError(
            "Hull-White calibration supports Constant and AtmMatrix2D vols")

    expiries = ([_wire_period_to_ql(p) for p in calib.get("expiries", [])]
                or matrix_expiries)
    tenors = ([_wire_period_to_ql(p) for p in calib.get("tenors", [])]
              or matrix_tenors)
    if not expiries or not tenors:
        raise ValueError("Calibration grid is empty")

    error_type = (ql.BlackCalibrationHelper.PriceError
                  if vol_type == ql.Normal
                  else ql.BlackCalibrationHelper.ImpliedVolError)
    fixed_leg_tenor = _frequency_to_period(
        get_frequency(fixed["fixed_frequency"]))
    fixed_dc = get_day_counter(fixed["fixed_day_counter"])
    spot_days = sidx.get("spot_days", 0)

    helpers = []
    for expiry in expiries:
        for tenor in tenors:
            vol_quote = ql.QuoteHandle(
                ql.SimpleQuote(market_vol(expiry, tenor)))
            helpers.append(ql.SwaptionHelper(
                expiry, tenor, vol_quote, ibor, fixed_leg_tenor, fixed_dc,
                ibor.dayCounter(), discount, error_type, ql.nullDouble(),
                1.0, vol_type, displacement, spot_days))

    hw = ql.HullWhite(discount, calib.get("a_init", 0.03),
                      calib.get("sigma_init", 0.01))
    engine = ql.JamshidianSwaptionEngine(hw)
    for helper in helpers:
        helper.setPricingEngine(engine)

    eps = calib.get("end_criteria_eps", 1.0e-8)
    # Server arg order: EndCriteria(function_evaluations, max_iterations,
    # eps, eps, eps).
    end_criteria = ql.EndCriteria(
        calib.get("function_evaluations", 1000),
        calib.get("max_iterations", 200), eps, eps, eps)
    fix_params = [not calib.get("calibrate_a", True),
                  not calib.get("calibrate_sigma", True)]
    # The server keeps a joint fit inside [a_lower, a_upper] (defaults 1e-3 /
    # 1.0, a box on "a" only). QuantLib-Python exposes no per-parameter
    # constraint, so the reference runs unconstrained; every catalog case that
    # calibrates "a" round-trips a generating a=0.05 well inside the box, where
    # the two fits coincide to the catalog tolerance.
    hw.calibrate(helpers, ql.LevenbergMarquardt(), end_criteria,
                 ql.NoConstraint(), [], fix_params)

    errors = [h.calibrationError() for h in helpers]
    rmse = math.sqrt(sum(e * e for e in errors) / len(errors))
    return {
        "hw_a": hw.params()[0],
        "hw_sigma": hw.params()[1],
        "rmse": rmse,
        "num_helpers": len(helpers),
    }


def calibrate_swaption_vol_ql(request: dict) -> dict:
    """Run the server's per-node SABR swaption-cube calibration independently.

    Rebuilds everything the server's SABR-calibrate finalize path builds:

      * per-node ATM forwards as the fair rates of forward-starting vanilla
        swaps generated from the swap index conventions (spot lag, fixed-leg
        schedule) on the request's discounting/forwarding curves,
      * the spread-zero ATM vol matrix (linear interpolation of each node's
        market vols at spread 0) wrapped in a SwaptionVolatilityMatrix,
      * the per-node vol spreads, the {alpha 0.04, beta beta_value, nu 0.4,
        rho 0.0} parameter guesses and the {false, beta_fixed, false, false}
        fixed flags,
      * QuantLib's SabrSwaptionVolatilityCube with the same SwapIndex (built
        on the first grid tenor) and default end criteria, whose
        sparseSabrParameters() runs the same per-node Levenberg-Marquardt
        fits the server runs.

    Returns the response diagnostics fields to compare (dotted paths into the
    response): per-node forward, ATM vol (Hagan vol at the forward on the
    calibrated parameters), alpha/beta/rho/nu grids, per-node fit RMSE and
    the overall RMSE. Lognormal, displacement-free surfaces only — the
    catalog cases keep the smile data well inside SABR's parameter bounds,
    where the two builds' optimizers agree to ~1e-10 (boundary-pinned fits,
    e.g. rho -> 1, diverge across builds and are deliberately not cataloged).
    """
    pricing = _reference_pricing_view(request)
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date

    disc_json = next(c for c in pricing["curves"]
                     if c["id"] == request["discounting_curve_id"])
    discount = build_curve_from_json(disc_json, eval_date, request)
    fwd_json = next(c for c in pricing["curves"]
                    if c["id"] == request["forwarding_curve_id"])
    forwarding = build_curve_from_json(fwd_json, eval_date, request)

    vol_spec = _find_vol_surface_spec(pricing, request["vol_id"])
    wrapper = vol_spec["payload"]
    if wrapper.get("payload_type") != "SwaptionSabrCalibrateSpec":
        raise ValueError("vol_id must reference a SwaptionSabrCalibrateSpec")
    inner = wrapper["payload"]
    base = inner["base"]
    ref, cal, bdc, dc, vol_type, displacement = _ir_vol_base(base)
    if vol_type != ql.ShiftedLognormal or displacement != 0.0:
        raise ValueError(
            "Only displacement-free lognormal SABR calibration is "
            "reference-supported")

    sidx = _find_swap_index_def(pricing, wrapper["swap_index_id"])
    fixed = sidx["fixed_leg"]
    float_leg = sidx["float_leg"]
    fixed_cal = get_calendar(fixed["fixed_calendar"])
    fixed_bdc = get_convention(fixed["fixed_bdc"])
    fixed_term_bdc = get_convention(fixed["fixed_term_bdc"])
    fixed_rule = get_date_generation(fixed["fixed_date_rule"])
    fixed_eom = fixed.get("fixed_eom", False)
    fixed_freq = get_frequency(fixed["fixed_frequency"])
    fixed_dc = get_day_counter(fixed["fixed_day_counter"])
    spot_days = sidx.get("spot_days", 0)
    ibor = resolve_index_from_id(sidx["float_index_id"], request, forwarding)

    expiries = [_wire_period_to_ql(p) for p in inner["expiries"]]
    tenors = [_wire_period_to_ql(p) for p in inner["tenors"]]
    spreads = inner["strikes"]
    tensor = inner["vols"]
    if tensor.get("quote_ids"):
        raise ValueError("quote-referenced SABR vols are not supported here")
    n_exp, n_ten, n_str = tensor["n_1"], tensor["n_2"], tensor["n_3"]
    values = tensor["values"]

    # Per-node ATM forwards: the server's computeServerAtmForwards — a
    # forward-starting vanilla swap per node whose fair rate is the forward.
    swap_engine = ql.DiscountingSwapEngine(discount)
    float_tenor = _wire_period_to_ql(float_leg["float_tenor"])
    forwards = []
    for expiry in expiries:
        exercise = fixed_cal.advance(ref, expiry, fixed_bdc)
        start = exercise
        if spot_days > 0:
            start = fixed_cal.advance(exercise, spot_days, ql.Days, fixed_bdc)
        for tenor in tenors:
            tentative_end = fixed_cal.advance(start, tenor, fixed_term_bdc)
            fixed_schedule = ql.Schedule(
                start, tentative_end, ql.Period(fixed_freq), fixed_cal,
                fixed_bdc, fixed_term_bdc, fixed_rule, fixed_eom)
            float_schedule = ql.Schedule(
                start, fixed_schedule.endDate(), float_tenor,
                get_calendar(float_leg["float_calendar"]),
                get_convention(float_leg["float_bdc"]),
                get_convention(float_leg["float_term_bdc"]),
                get_date_generation(float_leg["float_date_rule"]),
                float_leg.get("float_eom", False))
            swap = ql.VanillaSwap(
                ql.VanillaSwap.Payer, 1.0, fixed_schedule, 0.0, fixed_dc,
                float_schedule, ibor, 0.0, ibor.dayCounter())
            swap.setPricingEngine(swap_engine)
            forwards.append(swap.fairRate())

    # Spread-zero ATM vols: linear interpolation of each node's market vols
    # at spread 0 (flat-extended when the grid is one-sided), exactly the
    # server's interpolateAtmVolAtSpreadZero.
    def atm_at_spread_zero(node_vols):
        if len(node_vols) == 1 or spreads[0] >= 0.0:
            return node_vols[0]
        if spreads[-1] <= 0.0:
            return node_vols[-1]
        hi = bisect.bisect_left(spreads, 0.0)
        lo = hi - 1
        w = (0.0 - spreads[lo]) / (spreads[hi] - spreads[lo])
        return node_vols[lo] * (1.0 - w) + node_vols[hi] * w

    atm_matrix = ql.Matrix(n_exp, n_ten)
    for i in range(n_exp):
        for j in range(n_ten):
            node = values[(i * n_ten + j) * n_str:(i * n_ten + j + 1) * n_str]
            atm_matrix[i][j] = atm_at_spread_zero(node)
    option_dates = [cal.advance(ref, p, bdc) for p in expiries]
    atm_structure = ql.SwaptionVolatilityMatrix(
        ref, cal, bdc, option_dates, tenors, atm_matrix, dc, False, vol_type,
        ql.Matrix())
    atm_handle = ql.SwaptionVolatilityStructureHandle(atm_structure)

    vol_spreads = []
    for i in range(n_exp):
        for j in range(n_ten):
            atm = atm_matrix[i][j]
            vol_spreads.append([
                ql.QuoteHandle(ql.SimpleQuote(
                    values[(i * n_ten + j) * n_str + k] - atm))
                for k in range(n_str)])

    beta_fixed = inner.get("beta_fixed", True)
    beta_value = inner.get("beta_value", 0.5)
    guesses = [[ql.QuoteHandle(ql.SimpleQuote(0.04)),
                ql.QuoteHandle(ql.SimpleQuote(beta_value)),
                ql.QuoteHandle(ql.SimpleQuote(0.4)),
                ql.QuoteHandle(ql.SimpleQuote(0.0))]
               for _ in range(n_exp * n_ten)]
    is_fixed = [False, beta_fixed, False, False]

    swap_index = ql.SwapIndex(
        wrapper["swap_index_id"], tenors[0], spot_days, ibor.currency(),
        fixed_cal, _frequency_to_period(fixed_freq), fixed_bdc, fixed_dc,
        ibor, discount)
    cube = ql.SabrSwaptionVolatilityCube(
        atm_handle, expiries, tenors, spreads, vol_spreads, swap_index,
        swap_index, inner.get("vega_weighted_smile_fit", False), guesses,
        is_fixed, False)
    cube.enableExtrapolation()

    # browse() row layout is (swapLengthIdx * nOptionTimes + optionIdx) with
    # QuantLib's internal parameter order {alpha, beta, nu, rho}; re-indexed
    # into the response's row-major (expiry, tenor) layout and schema field
    # order, exactly like the server's finalize.
    browsed = cube.sparseSabrParameters()
    n_nodes = n_exp * n_ten
    alpha = [0.0] * n_nodes
    beta = [0.0] * n_nodes
    rho = [0.0] * n_nodes
    nu = [0.0] * n_nodes
    per_node_rmse = [0.0] * n_nodes
    for i in range(n_exp):
        for j in range(n_ten):
            row = j * n_exp + i
            k = i * n_ten + j
            alpha[k] = browsed[row][2]
            beta[k] = browsed[row][3]
            nu[k] = browsed[row][4]
            rho[k] = browsed[row][5]
            per_node_rmse[k] = browsed[row][7]
    overall_rmse = math.sqrt(sum(r * r for r in per_node_rmse) / n_nodes)

    # Per-node ATM vol: Hagan vol at the forward on the calibrated params,
    # with the node's time-to-expiry on the surface's own conventions —
    # the diagnostics builder's SabrSmileSection.volatility(F).
    atm_vol = []
    for i in range(n_exp):
        exercise = cal.advance(ref, expiries[i], bdc)
        tte = dc.yearFraction(ref, exercise)
        for j in range(n_ten):
            k = i * n_ten + j
            atm_vol.append(ql.sabrVolatility(
                forwards[k], forwards[k], tte, alpha[k], beta[k], nu[k],
                rho[k]))

    return {
        "diagnostics.forward_per_node": forwards,
        "diagnostics.atm_vol_per_node": atm_vol,
        "diagnostics.alpha_per_node": alpha,
        "diagnostics.beta_per_node": beta,
        "diagnostics.rho_per_node": rho,
        "diagnostics.nu_per_node": nu,
        "diagnostics.calibration.per_node_rmse": per_node_rmse,
        "diagnostics.calibration.overall_rmse": overall_rmse,
    }


# =============================================================================
# Structured notes (worst-of autocallables on index baskets, Monte Carlo)
# =============================================================================
#
# Same-stream parity: the reference does not merely
# re-price the note, it reproduces the engine's Monte Carlo estimate on the
# SAME random stream, so the comparison is bit-level up to summation order
# and the 15 significant digits the JSON gateway prints. Everything below is
# built from the request JSON only; the payoff walk is written from the
# schema documentation in flatbuffers/fbs/structured_note.fbs, not from the
# engine.
#
# Model (src/mc/basket_model.h, src/market/quanto_builder.h): one
# GeneralizedBlackScholesProcess(spot, dividend', r_note, black vol) per
# underlying, r_note = the note's discounting curve WITHOUT the funding
# spread, dividend' = the underlying's dividend curve (note-currency
# underlying) or QuantoTermStructure(dividend, riskFree = r_note, foreign =
# quanto.discounting_curve, equity vol at initial_level, FX vol at the FX
# spot as quoted, rho) for a quanto one. StochasticProcessArray with the
# correlation sub-matrix permuted to the note's underlying order.
#
# Grid (src/mc/time_grid.h): Actual/365 (Fixed) years from the valuation
# date to every future dated observation (accrual-only events are not
# observed); with steps_per_year every interval is split into
# ceil(length * steps_per_year) equal steps.
#
# Random numbers (src/mc/path_engine.h): dimension = assets x steps, consumed
# time-major (element (step - 1) * assets + asset). Sobol + bridge:
# SobolBrownianBridgeRsg(assets, steps) with QuantLib's defaults (Diagonal
# ordering, JoeKuoD7 direction integers, seed 0) -- the engine passes the
# request's seed, which QuantLib only uses for Sobol dimensions beyond the
# 1898 tabulated ones, so the stream is the same for every seed at these
# dimensions. Sobol without bridge: InverseCumulativeRsg<SobolRsg(dim, seed,
# JoeKuoD7), InverseCumulativeNormal>. PseudoRandom: InverseCumulativeRsg<
# RandomSequenceGenerator<MersenneTwister(seed)>, InverseCumulativeNormal>.
# No Python MultiPathGenerator accepts the bridge RSG, so the paths are
# evolved by hand exactly as QuantLib's MultiPathGenerator::next() does:
# one Gaussian vector per path, StochasticProcessArray.evolve(t, x, dt, dw)
# per step with the step's slice; the antithetic path uses the negated
# vector. Antithetic variates: the pair's accumulators are averaged and the
# pair is one sample of the mean / standard-error statistics (standard
# error = sqrt(unbiased sample variance / samples)).
#
# Discounting: ZeroSpreadedTermStructure(discounting_curve, funding_spread_bp
# / 1e4, Continuous, Annual, Actual/365 (Fixed)) at each cash flow's payment
# date; a payment on or before the valuation date is not discounted.
#
# Summation order follows the engine so the sums are bit-identical: per path
# the cash flows are added event by event (a coupon PV before the redemption
# PV of the same event, the final redemption and loss as one term), the pair
# average is 0.5 * (path + antithetic), and the sample sums are accumulated
# in simulation order.

_SN_PERF_RULES = {"WorstOf": min, "BestOf": max,
                  "Average": lambda perfs: sum(perfs) / float(len(perfs))}


def _sn_find(items, key, value, what):
    hit = next((it for it in items or [] if it.get(key) == value), None)
    if hit is None:
        raise ValueError(f"{what} not found: {value}")
    return hit


class _SliceSurface:
    """Python twin of the engine's SmileSliceVolSurface
    (src/market/smile_slice_vol_surface.h): one total-variance smile per
    expiry (`slices`: callables strike -> w), linear in total variance between
    the bracketing expiries at fixed strike, w(T1, K) t / T1 before the first
    and w(Tn, K) t / Tn after the last. Same expressions in the same order,
    so the values are the engine's bit for bit. Offers what the reference
    reads from a QuantLib Black vol handle (blackVol / blackVariance /
    minStrike / maxStrike); the extrapolation flags are no-ops here (every
    reference read extrapolates, as the engine's simulation does)."""

    def __init__(self, ref, dc, dates, slices, min_strike, max_strike):
        self.ref, self.dc, self.dates, self.slices = ref, dc, list(dates), list(slices)
        self.times = [dc.yearFraction(ref, d) for d in self.dates]
        self._min, self._max = min_strike, max_strike

    def referenceDate(self):
        return self.ref

    def dayCounter(self):
        return self.dc

    def maxDate(self):
        return self.dates[-1]

    def minStrike(self):
        return self._min

    def maxStrike(self):
        return self._max

    def enableExtrapolation(self):
        pass

    def disableExtrapolation(self):
        pass

    def blackVariance(self, t, strike, extrapolate=False):
        if not isinstance(t, float):
            t = self.dc.yearFraction(self.ref, t)
        times = self.times
        if t <= times[0]:
            return self.slices[0](strike) * t / times[0]
        if t >= times[-1]:
            return self.slices[-1](strike) * t / times[-1]
        i = bisect.bisect_right(times, t) - 1
        w0 = self.slices[i](strike)
        w1 = self.slices[i + 1](strike)
        return w0 + (w1 - w0) * (t - times[i]) / (times[i + 1] - times[i])

    def blackVol(self, t, strike, extrapolate=False):
        # BlackVarianceTermStructure::blackVolImpl.
        if not isinstance(t, float):
            t = self.dc.yearFraction(self.ref, t)
        nz = 0.00001 if t == 0.0 else t
        return math.sqrt(self.blackVariance(nz, strike) / nz)


class _ShiftedSurface:
    """Twin of the engine's ShiftedBlackVolSurface: every Black vol of
    `inner` plus `shift`, w = (sigma + shift)^2 t (the vega bump of an SVI
    payload)."""

    def __init__(self, inner, shift):
        self.inner, self.shift = inner, shift
        self.ref, self.dc = inner.ref, inner.dc

    def minStrike(self):
        return self.inner.minStrike()

    def maxStrike(self):
        return self.inner.maxStrike()

    def blackVariance(self, t, strike, extrapolate=False):
        if not isinstance(t, float):
            t = self.dc.yearFraction(self.ref, t)
        v = self.inner.blackVol(t, strike, True) + self.shift
        if not v > 0.0:
            raise ValueError(f"Black vol shift {self.shift} makes the volatility non-positive")
        return v * v * t

    def blackVol(self, t, strike, extrapolate=False):
        if not isinstance(t, float):
            t = self.dc.yearFraction(self.ref, t)
        nz = 0.00001 if t == 0.0 else t
        return math.sqrt(self.blackVariance(nz, strike) / nz)


def _cubic_variance_slice(strikes, total_variances):
    """CubicVarianceSlice: QuantLib's CubicNaturalSpline of total variance in
    ln K over the nodes (the same class the engine instantiates), constant
    beyond the first and last strike."""
    xs = [math.log(k) for k in strikes]
    spline = ql.CubicNaturalSpline(xs, list(total_variances))
    lo, hi = xs[0], xs[-1]

    def w(strike):
        return spline(min(max(math.log(strike), lo), hi), True)
    return w


def _svi_slice(p):
    """SviSlice: the raw SVI total variance about the slice's forward, the
    strike floored at 1e-6 as QuantLib's SviSmileSection does; same
    expression order as the engine's sviRawTotalVariance."""
    fwd, a, b, sigma, rho, m = (float(p["forward"]), float(p["a"]), float(p["b"]),
                                float(p["sigma"]), float(p["rho"]), float(p["m"]))

    def w(strike):
        k = math.log(max(strike, 1.0e-6) / fwd)
        d = k - m
        return a + b * (rho * d + math.sqrt(d * d + sigma * sigma))
    return w


class _PyLocalVol:
    """QuantLib's LocalVolSurface::localVolImpl (ql/termstructures/volatility/
    equityfx/localvolsurface.cpp, 1.44) on a Python surface: the same
    finite-difference stencil in log-strike (dy = 1e-4 |y| or 1e-6 near the
    money) and in time (dt = min(1e-4, t/2), the strikes carried along the
    forward) with the same discount reads, so that a structured note on a
    CubicStrikeLinearTime grid or an SVI payload is reproduced to the bit
    without subclassing a term structure through the bindings."""

    def __init__(self, surface, risk_free, dividend, spot_handle):
        self.bs, self.rf, self.dq, self.spot = surface, risk_free, dividend, spot_handle

    def localVol(self, t, level, extrapolate=True):
        dr = self.rf.discount(t, True)
        dq = self.dq.discount(t, True)
        forward = self.spot.value() * dq / dr
        strike = level
        y = math.log(strike / forward)
        dy = y * 0.0001 if abs(y) > 0.001 else 0.000001
        strikep = strike * math.exp(dy)
        strikem = strike / math.exp(dy)
        w = self.bs.blackVariance(t, strike, True)
        wp = self.bs.blackVariance(t, strikep, True)
        wm = self.bs.blackVariance(t, strikem, True)
        dwdy = (wp - wm) / (2.0 * dy)
        d2wdy2 = (wp - 2.0 * w + wm) / (dy * dy)
        if t == 0.0:
            dt = 0.0001
            drpt = self.rf.discount(t + dt, True)
            dqpt = self.dq.discount(t + dt, True)
            strikept = strike * dr * dqpt / (drpt * dq)
            wpt = self.bs.blackVariance(t + dt, strikept, True)
            if not wpt >= w:
                raise ValueError(f"decreasing variance at strike {strike} between {t} and {t + dt}")
            dwdt = (wpt - w) / dt
        else:
            dt = min(0.0001, t / 2.0)
            drpt = self.rf.discount(t + dt, True)
            drmt = self.rf.discount(t - dt, True)
            dqpt = self.dq.discount(t + dt, True)
            dqmt = self.dq.discount(t - dt, True)
            strikept = strike * dr * dqpt / (drpt * dq)
            strikemt = strike * dr * dqmt / (drmt * dq)
            wpt = self.bs.blackVariance(t + dt, strikept, True)
            wmt = self.bs.blackVariance(t - dt, strikemt, True)
            if not wpt >= w or not w >= wmt:
                raise ValueError(f"decreasing variance at strike {strike} around time {t}")
            dwdt = (wpt - wmt) / (2.0 * dt)
        if dwdy == 0.0 and d2wdy2 == 0.0:
            return math.sqrt(dwdt)
        den1 = 1.0 - y / w * dwdy
        den2 = 0.25 * (-0.25 - 1.0 / w + y * y / w / w) * dwdy * dwdy
        den3 = 0.5 * d2wdy2
        den = den1 + den2 + den3
        result = dwdt / den
        if not result >= 0.0:
            raise ValueError(f"negative local vol^2 at strike {strike} and time {t}")
        return math.sqrt(result)


def _sn_black_vol(pricing: dict, vol_id: str, shift: float = 0.0):
    """The referenced Black vol as the server builds it
    (src/decode/market/vol_surface_parsers.cpp): Constant -> BlackConstantVol,
    AtmMatrix2D -> BlackVarianceCurve (pillars = calendar.advance(reference,
    expiry, bdc), forceMonotoneVariance), SmileCube3D -> BlackVarianceSurface
    (rows = strikes, columns = expiries, bilinear or bicubic) or, with
    `surface_interpolator: CubicStrikeLinearTime`, the Python twin of the
    engine's SmileSliceVolSurface (one CubicNaturalSpline of total variance
    in log-strike per expiry, linear in time); an SviVolSpec payload -> the
    same twin on SVI slices (shape reported as SviSlices). `shift` is added
    to every Black vol (the constant, every term node, every surface entry;
    for SVI slices at every read, the engine's ShiftedBlackVolSurface)
    before the object is built: the engine's `shiftedBlackVol` of the vega
    bump. Returns (handle, shape, exact_step) where exact_step is True for
    the two strike-independent shapes that the process steps exactly
    (anything else is Dupire local vol on a fine grid; a Python twin cannot
    be handed to a QuantLib process and is stepped by hand, see
    structured_note_ql)."""
    spec = _find_vol_surface_spec(pricing, vol_id)
    if spec.get("payload_type") == "SviVolSpec":
        payload = spec["payload"]
        base = payload["base"]
        ref = parse_date(base["reference_date"])
        dc = get_day_counter(base["day_counter"])
        dates = [parse_date(sl["expiry"]) for sl in payload["slices"]]
        ts = _SliceSurface(ref, dc, dates, [_svi_slice(sl) for sl in payload["slices"]],
                           0.0, sys.float_info.max)
        if shift != 0.0:
            ts = _ShiftedSurface(ts, shift)
        return ts, "SviSlices", False
    if spec.get("payload_type") != "BlackVolSpec":
        raise ValueError(f"vol {vol_id}: a BlackVolSpec or SviVolSpec payload is required")
    payload = spec["payload"]
    base = payload["base"]
    ref = parse_date(base["reference_date"])
    cal = get_calendar(base["calendar"])
    bdc = get_convention(base["business_day_convention"])
    dc = get_day_counter(base["day_counter"])
    shape = base.get("shape", "Constant")
    if shape == "Constant":
        ts = ql.BlackConstantVol(ref, cal, float(base["constant_vol"]) + shift, dc)
    elif shape == "AtmMatrix2D":
        vols = payload["term_vols"]
        if vols.get("quote_ids"):
            raise ValueError(f"vol {vol_id}: quote-referenced term vols not supported")
        dates = [cal.advance(ref, _wire_period_to_ql(p), bdc) for p in payload["expiries"]]
        ts = ql.BlackVarianceCurve(ref, dates, [float(v) + shift for v in vols["values"]], dc,
                                   True)
    elif shape == "SmileCube3D":
        vols = payload["surface_vols"]
        if vols.get("quote_ids"):
            raise ValueError(f"vol {vol_id}: quote-referenced surface vols not supported")
        dates = [cal.advance(ref, _wire_period_to_ql(p), bdc) for p in payload["expiries"]]
        strikes = [float(k) for k in payload["strikes"]]
        n_exp, n_str = len(dates), len(strikes)
        values = vols["values"]
        if len(values) != n_exp * n_str:
            raise ValueError(f"vol {vol_id}: surface_vols has {len(values)} values, "
                             f"expected {n_exp} x {n_str}")
        legacy_cubic = (payload.get("expiry_interpolator", "Linear") == "LogCubic")
        interp = payload.get("surface_interpolator", "Bilinear")
        if interp == "CubicStrikeLinearTime" and not legacy_cubic:
            # The engine's SmileSliceVolSurface: per expiry the nodes'
            # (shifted vol)^2 T in ln K through a natural cubic spline,
            # constant outside; linear in total variance between expiries.
            slices = []
            for i, d in enumerate(dates):
                t = dc.yearFraction(ref, d)
                ws = []
                for j in range(n_str):
                    v = float(values[i * n_str + j]) + shift
                    ws.append(v * v * t)
                slices.append(_cubic_variance_slice(strikes, ws))
            return (_SliceSurface(ref, dc, dates, slices, strikes[0], strikes[-1]), shape,
                    False)
        # Wire order is row-major by expiry then strike; QuantLib wants a
        # (strikes x expiries) matrix.
        matrix = ql.Matrix(n_str, n_exp)
        for i in range(n_exp):
            for j in range(n_str):
                matrix[j][i] = float(values[i * n_str + j]) + shift
        # Constant strike extrapolation on both sides (the edge strike's vol),
        # as the server builds every BlackVarianceSurface; beyond the last
        # expiry QuantLib scales the last total variance linearly in t.
        ts = ql.BlackVarianceSurface(ref, cal, dates, strikes, matrix, dc,
                                     ql.BlackVarianceSurface.ConstantExtrapolation,
                                     ql.BlackVarianceSurface.ConstantExtrapolation)
        if legacy_cubic or interp == "Bicubic":
            ts.setInterpolation("bicubic")
        else:
            ts.setInterpolation("bilinear")
    else:
        raise ValueError(f"vol {vol_id}: shape {shape} is not reference-supported")
    if payload.get("allow_extrapolation", False):
        ts.enableExtrapolation()
    else:
        ts.disableExtrapolation()
    return ql.BlackVolTermStructureHandle(ts), shape, shape in ("Constant", "AtmMatrix2D")


def _sn_simulation_times(as_of: ql.Date, dates: list, steps_per_year):
    """The engine's grid rule (src/mc/time_grid.cpp): Actual/365 (Fixed)
    times of the distinct mandatory dates, each interval refined to
    ceil(length * steps_per_year) equal steps when steps_per_year is given.
    Returns (times, {date: grid index})."""
    dc = ql.Actual365Fixed()
    distinct = [0.0]
    mandatory_times = []
    for d in dates:
        t = dc.yearFraction(as_of, d)
        mandatory_times.append(t)
        if t > distinct[-1]:
            distinct.append(t)
    if len(distinct) < 2:
        raise ValueError("structured note: no observation after the valuation date")
    times = [0.0]
    distinct_index = [0]
    for k in range(1, len(distinct)):
        a, b = distinct[k - 1], distinct[k]
        n = 1
        if steps_per_year is not None:
            n = max(1, int(math.ceil((b - a) * float(steps_per_year) - 1e-9)))
        for s in range(1, n):
            times.append(a + (b - a) * float(s) / float(n))
        times.append(b)
        distinct_index.append(len(times) - 1)
    index_of = {}
    for d, t in zip(dates, mandatory_times):
        k = distinct.index(t)
        index_of[d] = distinct_index[k]
    return times, index_of


def _sn_gaussian_source(sequence: str, brownian_bridge: bool, seed: int,
                        assets: int, steps: int):
    """The engine's Gaussian sequence generator (src/mc/path_engine.h);
    returns a callable producing one time-major vector of dimension
    assets x steps per call."""
    dimension = assets * steps
    if sequence == "Sobol":
        if brownian_bridge:
            # QuantLib defaults = Diagonal ordering, seed 0, JoeKuoD7; the
            # seed only enters beyond the tabulated dimensions.
            rsg = ql.SobolBrownianBridgeRsg(assets, steps)
        else:
            rsg = ql.InvCumulativeSobolGaussianRsg(
                ql.SobolRsg(dimension, seed, ql.SobolRsg.JoeKuoD7))
    elif sequence == "PseudoRandom":
        rsg = ql.InvCumulativeMersenneTwisterGaussianRsg(
            ql.MersenneTwisterUniformRsg(dimension, seed))
    else:
        raise ValueError(f"unknown random sequence: {sequence}")
    return lambda: list(rsg.nextSequence().value())


def _sn_spreaded(curve_handle, spread: float):
    """The engine's funding / rate shift: ZeroSpreadedTermStructure(curve,
    spread, Continuous, Annual, Actual/365 (Fixed)), extrapolating."""
    ts = ql.ZeroSpreadedTermStructure(
        curve_handle, ql.QuoteHandle(ql.SimpleQuote(spread)), ql.Continuous, ql.Annual,
        ql.Actual365Fixed())
    ts.enableExtrapolation()
    return ts


def _sn_bump_correlation(c, delta: float):
    """The engine's bumpCorrelation (src/evaluators/structured_note_evaluator.cpp):
    every off-diagonal entry + delta clipped to [-1, 1]; when the smallest
    eigenvalue (SymmetricSchurDecomposition, QuantLib's Jacobi) is below
    -1e-10 the matrix is repaired as V diag(max(lambda, 0)) V^T rescaled to
    unit diagonal, the sums in the engine's order. Returns (matrix, repaired)."""
    n = c.rows()
    m = ql.Matrix(n, n)
    for i in range(n):
        for j in range(n):
            m[i][j] = c[i][j] if i == j else min(1.0, max(-1.0, c[i][j] + delta))
    schur = ql.SymmetricSchurDecomposition(m)
    ev = schur.eigenvalues()
    smallest = ev[0]
    for l in range(1, n):
        smallest = min(smallest, ev[l])
    if smallest >= -1.0e-10:
        return m, False
    v = schur.eigenvectors()
    r = [[0.0] * n for _ in range(n)]
    for i in range(n):
        for j in range(n):
            total = 0.0
            for l in range(n):
                total += v[i][l] * max(ev[l], 0.0) * v[j][l]
            r[i][j] = total
    out = ql.Matrix(n, n)
    for i in range(n):
        for j in range(n):
            out[i][j] = 1.0 if i == j else r[i][j] / math.sqrt(r[i][i] * r[j][j])
    return out, True


def structured_note_ql(request: dict, index: int = 0) -> dict:
    """Reference price of `request["notes"][index]` on the engine's random
    stream (see the section comment above for every convention).

    Returns the fields of a StructuredNoteResult the parity tests compare:
    npv, npv_pct_of_notional, standard_error (absent for a Called / Matured
    note), paths, status, legs {bond_redemption, coupons, capital_loss:
    {pv, standard_error}}, p_barrier_breach, expected_life_years,
    event_probabilities [{index, observation_date, p_call?, p_coupon?}] and
    seasoning {memory_carried, status, known_payment?}; with the request's
    `diagnostics` flags the market / decomposition / checks / distribution
    blocks, and with `greeks` the Greeks block (bump-and-reprice on the same
    stream, every bumped market rebuilt from the request JSON exactly as the
    engine rebuilds it: spot quote x (1 +/- h), every Black vol + dv (the
    quanto curve rebuilt with it), the off-diagonal correlations + dr clipped
    and repaired by eigenvalue clipping when indefinite, the funding spread
    + bp on the discounting, a zero spread of + bp on the note curve as the
    processes' risk-free curve and under the funding spread; each
    sensitivity is the mean of the per-sample difference of the two runs,
    scaled to the unit move, with the engine's standard-error estimator on
    those differences).
    """
    pricing = _reference_pricing_view(request)
    note = request["notes"][index]
    as_of = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = as_of
    notional = float(note["notional"])
    perf_rule = _SN_PERF_RULES[note["performance_rule"]]
    coupon_rule = note["coupon_rule"]
    memory_rule = bool(coupon_rule["memory"])
    pay_only_on_call = bool(coupon_rule["pay_only_on_call"])
    capital = note["capital"]
    barrier = float(capital["barrier"])
    daily_close = capital["monitoring"] == "DailyClose"
    put_strike = float(capital.get("put_strike", 1.0))
    participation = float(capital.get("participation", 1.0))
    upside = note.get("upside")
    maturity = parse_date(note["maturity_payment_date"])
    mc = note["monte_carlo"]
    paths = int(mc["paths"])
    seed = int(mc.get("seed", 42))
    sequence = mc.get("sequence", "Sobol")
    antithetic = bool(mc.get("antithetic", True))
    brownian_bridge = bool(mc.get("brownian_bridge", True))
    steps_per_year = mc.get("steps_per_year")

    # --- market -------------------------------------------------------------
    def curve(curve_id, what):
        spec = _sn_find(pricing["curves"], "id", curve_id, what)
        return build_curve_from_json(spec, as_of, request)

    note_curve = curve(note["discounting_curve"], "Discounting curve")
    spreaded = _sn_spreaded(note_curve, float(note.get("funding_spread_bp", 0.0)) * 1.0e-4)

    def df(d):
        return 1.0 if d <= as_of else spreaded.discount(d, True)

    exact_step = True
    initial = []
    ids = []
    und = []  # per underlying, what the market diagnostics and the limit runs reuse
    for u in note["underlyings"]:
        eq = _sn_find(pricing.get("equity_underlyings"), "id", u["underlying_id"],
                      "Equity underlying")
        spot_value = _sn_find(pricing.get("quotes"), "id", eq["spot_quote_id"], "Quote")["value"]
        spot = ql.QuoteHandle(ql.SimpleQuote(float(spot_value)))
        dividend = curve(eq["dividend_yield_curve_id"], "Dividend yield curve")
        plain_dividend = dividend
        local_curve = note_curve
        vol, _shape, exact = _sn_black_vol(pricing, u["volatility_id"])
        exact_step = exact_step and exact
        level = float(u["initial_level"])
        quanto = u.get("quanto")
        if (eq.get("currency") != note["currency"]) != (quanto is not None):
            raise ValueError(f"underlying {u['underlying_id']}: quanto block presence "
                             f"must match a currency different from the note's")
        fx_vol, fx_rate, rho = None, 0.0, 0.0
        if quanto is not None:
            if isinstance(vol, _SliceSurface):
                raise NotImplementedError("quanto underlying on a CubicStrikeLinearTime grid or "
                                          "an SVI payload: no QuantLib handle for the "
                                          "QuantoTermStructure in the reference")
            local_curve = curve(quanto["discounting_curve"], "Discounting curve")
            fx = build_fx_spot(pricing, quanto["fx_spot_id"], request)
            fx_vol, _fx_shape, fx_exact = _sn_black_vol(pricing, quanto["fx_volatility_id"])
            if not fx_exact:
                raise ValueError("quanto FX vol must be Constant or AtmMatrix2D")
            fx_rate = float(fx["rate"])
            rho = float(quanto["fx_underlying_correlation"])
            dividend = ql.YieldTermStructureHandle(ql.QuantoTermStructure(
                dividend, note_curve, local_curve, vol, level, fx_vol, fx["rate"], rho))
        initial.append(level)
        ids.append(u["underlying_id"])
        und.append({"spot": float(spot_value), "spot_handle": spot, "dividend": dividend,
                    "plain_dividend": plain_dividend, "local_curve": local_curve, "vol": vol,
                    "vol_id": u["volatility_id"], "level": level, "exact": exact,
                    "quanto": quanto is not None, "fx_vol": fx_vol, "fx_spot": fx_rate,
                    "rho": rho})

    n_u = len(ids)
    correlation = ql.Matrix(n_u, n_u)
    if n_u == 1:
        # A single underlying is simulated on the 1 x 1 unit matrix and
        # carries no correlation_id (the engine's rule).
        if note.get("correlation_id"):
            raise ValueError("correlation_id must be absent with a single underlying")
        correlation[0][0] = 1.0
    else:
        corr_spec = _sn_find(((pricing.get("correlations") or {}).get("matrices")), "id",
                             note["correlation_id"], "Correlation matrix")
        labels = list(corr_spec["labels"])
        if sorted(labels) != sorted(ids):
            raise ValueError("correlation labels must be exactly the note's underlying ids")
        values = corr_spec["values"]
        n_lab = len(labels)
        if values.get("quote_ids"):
            quotes = {q["id"]: q["value"] for q in pricing.get("quotes", [])}
            flat = [quotes[qid] for qid in values["quote_ids"]]
        else:
            flat = list(values["values"])
        for a, ia in enumerate(ids):
            for b, ib in enumerate(ids):
                correlation[a][b] = float(flat[labels.index(ia) * n_lab + labels.index(ib)])

    def make_asset(spot_handle, dividend, risk_free, vol, exact):
        """One simulated asset: the GeneralizedBlackScholesProcess(spot,
        dividend, riskFree, vol) and, for a strike x expiry surface, the
        engine's edge-clamped Dupire local vol (src/mc/basket_model.h):
        QuantLib's LocalVolSurface on the same handles, read at the level
        clamped to [minStrike e^1e-3, maxStrike e^-1e-3]."""
        python_surface = isinstance(vol, (_SliceSurface, _ShiftedSurface))
        a = {"process": None if python_surface else
             ql.GeneralizedBlackScholesProcess(spot_handle, dividend, risk_free, vol),
             "exact": exact, "x0": spot_handle.value()}
        if not exact:
            # A CubicStrikeLinearTime grid or an SVI payload has no QuantLib
            # object behind it: the local vol is QuantLib's stencil in Python
            # (_PyLocalVol) on the twin surface; anything else is QuantLib's.
            a["lv"] = (_PyLocalVol(vol, risk_free, dividend, spot_handle) if python_surface
                       else ql.LocalVolSurface(vol, risk_free, dividend, spot_handle))
            a["risk_free"] = risk_free
            a["dividend"] = dividend
            a["lower"] = vol.minStrike() * math.exp(1.0e-3)
            a["upper"] = vol.maxStrike() * math.exp(-1.0e-3)
            if not a["lower"] < a["upper"]:
                a["lower"], a["upper"] = vol.minStrike(), vol.maxStrike()
        return a

    def base_assets():
        return [make_asset(u["spot_handle"], u["dividend"], note_curve, u["vol"], u["exact"])
                for u in und]

    # --- events ---------------------------------------------------------------
    events = note["events"]
    n_events = len(events)
    parsed = []
    for i, ev in enumerate(events):
        parsed.append({
            "index": i,
            "observation": parse_date(ev["observation_date"]),
            "observation_iso": ev["observation_date"],
            "payment": parse_date(ev["payment_date"]) if ev.get("payment_date") else None,
            "autocall": ev.get("autocall_trigger"),
            "coupon_trigger": ev.get("coupon_trigger"),
            "coupon": float(ev.get("coupon", 0.0)),
            # `is_final`: a maturity-only observation (capital block and upside
            # only; no coupon test, no call test, the memory is not paid).
            "is_final": bool(ev.get("is_final", False)),
            "accrual_only": (not ev.get("is_final", False)
                             and ev.get("autocall_trigger") is None
                             and ev.get("coupon_trigger") is None),
            "final": i + 1 == n_events,
        })

    def step_event(ev, perf, state):
        """One event of the walk on one path. `state` = {memory, called,
        knocked_in}; returns the event's outcome dict (fractions of the
        notional)."""
        out = {"called": None, "earned": None, "coupon_paid": 0.0,
               "call_redemption": 0.0, "breached": False,
               "final_redemption": 0.0, "final_loss": 0.0, "final_upside": 0.0}
        if ev["accrual_only"]:
            state["memory"] += ev["coupon"]
            return out
        if not ev["is_final"]:
            calls = ev["autocall"] is not None and perf >= ev["autocall"]
            if ev["coupon_trigger"] is not None:
                earned = perf >= ev["coupon_trigger"]
            else:
                earned = calls
            out["earned"] = earned
            if earned:
                amount = ev["coupon"] + state["memory"]
                state["memory"] = 0.0
                if calls or ev["final"] or not pay_only_on_call:
                    out["coupon_paid"] += amount
                else:
                    state["memory"] = amount
            elif memory_rule:
                state["memory"] += ev["coupon"]
            if ev["autocall"] is not None:
                out["called"] = calls
                if calls:
                    out["coupon_paid"] += state["memory"]
                    state["memory"] = 0.0
                    out["call_redemption"] = 1.0
                    state["called"] = True
                    return out
        if ev["final"]:
            breached = state["knocked_in"] if daily_close else perf < barrier
            out["breached"] = breached
            out["final_redemption"] = 1.0
            if breached:
                out["final_loss"] = -participation * max(put_strike - perf, 0.0)
            if upside:
                gain = float(upside["participation"]) * max(perf - float(upside["strike"]), 0.0)
                if upside.get("cap") is not None:
                    gain = min(gain, float(upside["cap"]))
                out["final_upside"] = gain
        return out

    # --- seasoning: the past on the historical levels ----------------------------
    state = {"memory": 0.0, "called": False,
             "knocked_in": bool(capital.get("knocked_in", False))}
    known = []  # (date, amount, bucket)
    status = "Alive"
    first_future = 0
    for ev in parsed:
        if ev["observation"] > as_of:
            break
        first_future = ev["index"] + 1
        perf = 0.0
        if not ev["accrual_only"]:
            perfs = []
            for k, u in enumerate(note["underlyings"]):
                level = next((h["value"] for h in u.get("historical_levels") or []
                              if parse_date(h["date"]) == ev["observation"]), None)
                if level is None:
                    raise ValueError(f"no historical level of {ids[k]} on {ev['observation_iso']}")
                perfs.append(float(level) / initial[k])
            perf = perf_rule(perfs)
            if daily_close and perf < barrier:
                state["knocked_in"] = True
        out = step_event(ev, perf, state)
        if out["coupon_paid"] > 0.0:
            known.append((ev["payment"], out["coupon_paid"], "coupons"))
        if state["called"]:
            known.append((ev["payment"], out["call_redemption"], "bond_redemption"))
            status = "Called"
            break
        if ev["final"]:
            known.append((maturity, out["final_redemption"] + out["final_upside"], "bond_redemption"))
            if out["final_loss"] != 0.0:
                known.append((maturity, out["final_loss"], "capital_loss"))
            status = "Matured"
    memory_carried = state["memory"]

    def known_on(curve):
        """PV of the flows the past fixed (dated after as_of) on `curve`:
        the engine's knownOn (per bucket, and the total in flow order)."""
        pv = {"bond_redemption": 0.0, "coupons": 0.0, "capital_loss": 0.0}
        total = 0.0
        for d, amount, bucket in known:
            if d > as_of:
                x = amount * (1.0 if d <= as_of else curve.discount(d, True))
                total += x
                pv[bucket] += x
        return pv, total

    known_pv, known_total = known_on(spreaded)

    result = {
        "trade_id": note["trade_id"],
        "status": status,
        "paths": 0,
        "seasoning": {"status": status, "memory_carried": memory_carried},
        "event_probabilities": [],
    }
    if status != "Alive":
        result["npv"] = known_total * notional
        result["npv_pct_of_notional"] = known_total * 100.0
        result["seasoning"]["known_payment"] = sum(a for _, a, _ in known)
        return result

    # --- simulation set-up ----------------------------------------------------------
    future = parsed[first_future:]
    observed = [ev for ev in future if not ev["accrual_only"]]
    if not observed:
        raise ValueError("structured note: no dated event after the valuation date")
    if (not exact_step or daily_close) != (steps_per_year is not None):
        raise ValueError("steps_per_year must be present exactly when a vol is a surface "
                         "or the barrier is DailyClose")
    times, index_of = _sn_simulation_times(as_of, [ev["observation"] for ev in observed],
                                           steps_per_year)
    grid = ql.TimeGrid(times)
    steps = len(times) - 1
    dc365 = ql.Actual365Fixed()
    for ev in observed:
        ev["grid_index"] = index_of[ev["observation"]]
        ev["t_observation"] = dc365.yearFraction(as_of, ev["observation"])

    n_future = len(future)

    def discounting_on(curve):
        """The discounting of one run on a (funding-spreaded) curve: the
        discount factor of every future event's payment date (0 for an
        accrual-only event), of the maturity payment date, and the known
        PVs on that curve (the engine's Discounting)."""
        def dfc(d):
            return 1.0 if d <= as_of else curve.discount(d, True)
        pv, total = known_on(curve)
        return {"df_payment": [0.0 if ev["accrual_only"] else dfc(ev["payment"])
                               for ev in future],
                "df_maturity": dfc(maturity), "known_pv": pv, "known_total": total}

    base = discounting_on(spreaded)
    df_maturity = base["df_maturity"]
    observed_index = 0
    for ev in observed:
        ev["observed_index"] = observed_index
        observed_index += 1
    n_observed = len(observed)
    # Accumulators (the engine's layout, src/evaluators/structured_note_evaluator.cpp):
    # the three buckets, the indicators, the undiscounted final loss (for
    # E[loss | breach]) and four per future event.
    (A_TOTAL, A_BOND, A_COUPON, A_LOSS, A_BREACH, A_SURVIVE, A_LIFE,
     A_LOSS_UNDISCOUNTED, A_BASE) = range(9)
    per_event = 4  # p_call, p_coupon, pv coupons, pv redemption
    n_acc = A_BASE + per_event * n_future
    known_undiscounted = 0.0
    for d, amount, _bucket in known:
        if d > as_of:
            known_undiscounted += amount

    def perf_at(path, j):
        return perf_rule([path[k][j] / initial[k] for k in range(n_u)])

    def walk(path, disc, rec=None, slot=0, grid_index=None):
        """One path through the state machine, discounted on `disc`
        (discounting_on); returns its accumulators. With `rec` (the
        distribution block) the basket performance is recorded at every
        observed event whether or not the note is still alive, together
        with the path's undiscounted payout, its PV, the call event and the
        breach flag, at slot `slot`. `grid_index` (per future event, the
        vanilla-repricing grid) replaces the events' own grid indices."""
        acc = [0.0] * n_acc
        st = {"memory": state["memory"], "called": False, "knocked_in": state["knocked_in"]}
        prev = 0
        payout = known_undiscounted
        alive = True
        for e, ev in enumerate(future):
            if ev["accrual_only"]:
                if alive:
                    step_event(ev, 0.0, st)
                continue
            gi = ev["grid_index"] if grid_index is None else grid_index[e]
            perf = perf_at(path, gi)
            if rec is not None:
                rec["perf"][ev["observed_index"]][slot] = perf
            if not alive:
                continue
            if daily_close and not st["knocked_in"]:
                for j in range(prev + 1, gi + 1):
                    if perf_at(path, j) < barrier:
                        st["knocked_in"] = True
                        break
            prev = gi
            out = step_event(ev, perf, st)
            b = A_BASE + per_event * e
            if out["earned"]:
                acc[b + 1] += 1.0
            if out["coupon_paid"] > 0.0:
                pv = out["coupon_paid"] * disc["df_payment"][e]
                acc[A_COUPON] += pv
                acc[A_TOTAL] += pv
                acc[b + 2] += pv
                payout += out["coupon_paid"]
            if out["called"]:
                acc[b] += 1.0
                pv = out["call_redemption"] * disc["df_payment"][e]
                acc[A_BOND] += pv
                acc[A_TOTAL] += pv
                acc[b + 3] += pv
                acc[A_LIFE] += ev["t_observation"]
                payout += out["call_redemption"]
                if rec is not None:
                    rec["called_event"][slot] = ev["index"]
                alive = False
                if rec is None:
                    break
                continue
            if ev["final"]:
                acc[A_SURVIVE] += 1.0
                if out["breached"]:
                    acc[A_BREACH] += 1.0
                bond = (out["final_redemption"] + out["final_upside"]) * disc["df_maturity"]
                loss = out["final_loss"] * disc["df_maturity"]
                acc[A_BOND] += bond
                acc[A_LOSS] += loss
                acc[A_TOTAL] += bond + loss
                acc[b + 3] += bond
                acc[A_LIFE] += ev["t_observation"]
                acc[A_LOSS_UNDISCOUNTED] += -out["final_loss"]
                payout += out["final_redemption"] + out["final_upside"] + out["final_loss"]
                if rec is not None:
                    rec["breached"][slot] = out["breached"]
        if rec is not None:
            rec["payout"][slot] = payout
            rec["payout_pv"][slot] = acc[A_TOTAL] + disc["known_total"]
        return acc

    # --- paths on the engine's stream --------------------------------------------
    if antithetic and paths % 2 != 0:
        raise ValueError("paths must be even with antithetic variates")
    samples = paths // 2 if antithetic else paths

    def make_simulate(grid, steps, n_acc, walk, extra=None):
        """The engine's RunPaths (src/mc/path_engine.cpp) over `grid`
        (`steps` = grid points - 1) with `n_acc` accumulators filled by
        `walk(path, disc, rec, slot)` and, when given, `extra(path, acc)`
        (the vanilla repricing's option payoffs beyond the walk's): a fresh
        generator per run, the paths evolved by hand, the pair averaged
        into one sample, the running means at 1/4, 1/2 and 3/4 of the
        samples with the same estimator on the first k samples (an entry is
        skipped when its count is 0 or repeats the previous one). The
        returned simulate(assets, matrix, disc, rec, capture) takes `capture`
        (a list) that receives every simulated path's acc[TOTAL] in
        simulation order (the Greeks' common-random-number differences)."""

        def one_path(path, disc, rec, slot):
            acc = walk(path, disc, rec, slot)
            if extra is not None:
                acc = acc + [0.0] * (n_acc - len(acc))
                extra(path, acc)
            return acc

        def simulate(assets, matrix, disc, rec=None, capture=None):
            draw = _sn_gaussian_source(sequence, brownian_bridge, seed, n_u, steps)
            all_exact = all(a["exact"] for a in assets)
            if all_exact:
                proc_array = ql.StochasticProcessArray([a["process"] for a in assets], matrix)
                x0 = proc_array.initialValues()
            else:
                # The engine's local-vol path is QuantLib's StochasticProcessArray
                # evolve (dz = spectral pseudo-square-root x dw, then each
                # process's Euler step) with the edge-clamped local vol as the
                # external local vol of the process; reproduced step by step:
                # GeneralizedBlackScholesProcess::evolve (Euler branch) is
                # x exp(drift(t, x) dt + sigma(t, x) sqrt(dt) dz) with
                # drift = r(t, t + 1e-4) - q(t, t + 1e-4) - sigma^2 / 2 on the
                # continuous forward rates and sigma = localVol(t, clamp(x)).
                sqrt_corr = ql.pseudoSqrt(matrix, ql.SalvagingAlgorithm.Spectral)
                x0 = [a["x0"] for a in assets]

            def step(a, t0, x, dt, dz):
                if a["exact"]:
                    return a["process"].evolve(t0, x, dt, dz)
                sigma = a["lv"].localVol(t0, min(max(x, a["lower"]), a["upper"]), True)
                t1 = t0 + 0.0001
                drift = (a["risk_free"].forwardRate(t0, t1, ql.Continuous, ql.NoFrequency,
                                                    True).rate()
                         - a["dividend"].forwardRate(t0, t1, ql.Continuous, ql.NoFrequency,
                                                     True).rate()
                         - 0.5 * sigma * sigma)
                return x * math.exp(drift * dt + (sigma * math.sqrt(dt)) * dz)

            def evolve(dw):
                path = [[0.0] * (steps + 1) for _ in range(n_u)]
                asset = x0
                for j in range(n_u):
                    path[j][0] = asset[j]
                for i in range(1, steps + 1):
                    offset = (i - 1) * n_u
                    if all_exact:
                        asset = proc_array.evolve(grid[i - 1], asset, grid.dt(i - 1),
                                                  ql.Array(dw[offset:offset + n_u]))
                    else:
                        dz = sqrt_corr * ql.Array(dw[offset:offset + n_u])
                        t0, dt = grid[i - 1], grid.dt(i - 1)
                        asset = [step(assets[j], t0, asset[j], dt, dz[j]) for j in range(n_u)]
                    for j in range(n_u):
                        path[j][i] = asset[j]
                return path

            total = [0.0] * n_acc
            total_sq = [0.0] * n_acc
            running = []
            quarter, half, three_quarters = samples // 4, samples // 2, (3 * samples) // 4

            def statistics(count):
                n = float(count)
                means = [s / n for s in total]
                ses = []
                for k in range(n_acc):
                    if count < 2:
                        ses.append(float("nan"))
                        continue
                    variance = (total_sq[k] - n * means[k] * means[k]) / (n - 1.0)
                    if variance < 0.0:
                        variance = 0.0  # rounding on a constant accumulator
                    ses.append(math.sqrt(variance / n))
                return means, ses

            def maybe_record(done, at, fraction):
                if done != at or at == 0:
                    return
                if running and running[-1]["samples"] == at:
                    return
                means, ses = statistics(at)
                running.append({"fraction": fraction, "samples": at,
                                "means": means, "standard_errors": ses})

            slot = 0
            for s in range(samples):
                dw = draw()
                acc = one_path(evolve(dw), disc, rec, slot)
                slot += 1
                if capture is not None:
                    capture.append(acc[A_TOTAL])
                if antithetic:
                    acc_anti = one_path(evolve([-x for x in dw]), disc, rec, slot)
                    slot += 1
                    if capture is not None:
                        capture.append(acc_anti[A_TOTAL])
                    acc = [0.5 * (a + b) for a, b in zip(acc, acc_anti)]
                for k in range(n_acc):
                    total[k] += acc[k]
                    total_sq[k] += acc[k] * acc[k]
                done = s + 1
                maybe_record(done, quarter, 0.25)
                maybe_record(done, half, 0.5)
                maybe_record(done, three_quarters, 0.75)
            mean, se = statistics(samples)
            return {"mean": mean, "standard_error": se, "running_means": running}

        return simulate

    simulate = make_simulate(grid, steps, n_acc, walk)

    diag = request.get("diagnostics") or {}
    want_market = bool(diag.get("market", False))
    want_decomposition = bool(diag.get("decomposition", False))
    want_checks = bool(diag.get("checks", False))
    want_distribution = bool(diag.get("distribution", False))
    sample_paths = int(diag.get("sample_paths", 0) or 0)
    rec = None
    if want_distribution:
        rec = {"perf": [[0.0] * paths for _ in range(n_observed)],
               "payout": [0.0] * paths, "payout_pv": [0.0] * paths,
               "called_event": [None] * paths, "breached": [False] * paths}
    greeks_spec = request.get("greeks")
    base_totals = [] if greeks_spec is not None else None
    sim = simulate(base_assets(), correlation, base, rec, base_totals)
    mean, se = sim["mean"], sim["standard_error"]

    result["paths"] = paths
    result["npv"] = (mean[A_TOTAL] + known_total) * notional
    result["npv_pct_of_notional"] = (mean[A_TOTAL] + known_total) * 100.0
    result["standard_error"] = se[A_TOTAL] * notional
    result["legs"] = {
        "bond_redemption": {"pv": (mean[A_BOND] + known_pv["bond_redemption"]) * notional,
                            "standard_error": se[A_BOND] * notional},
        "coupons": {"pv": (mean[A_COUPON] + known_pv["coupons"]) * notional,
                    "standard_error": se[A_COUPON] * notional},
        "capital_loss": {"pv": (mean[A_LOSS] + known_pv["capital_loss"]) * notional,
                         "standard_error": se[A_LOSS] * notional},
    }
    result["p_barrier_breach"] = mean[A_BREACH]
    result["expected_life_years"] = mean[A_LIFE]
    for e, ev in enumerate(future):
        if ev["accrual_only"]:
            continue
        row = {"index": ev["index"], "observation_date": ev["observation_iso"]}
        b = A_BASE + per_event * e
        if ev["autocall"] is not None:
            row["p_call"] = mean[b]
        if not ev["is_final"]:  # an is_final event has no coupon test
            row["p_coupon"] = mean[b + 1]
        result["event_probabilities"].append(row)

    # --- diagnostics (flatbuffers/fbs/structured_note_response.fbs doc comments) ---
    # Forward of underlying k at grid point j on the simulation's time axis:
    # spot x D_div'(t) / D_note(t), D_div' the process's dividend curve (the
    # QuantoTermStructure of a quanto underlying), D_note the note curve
    # without the funding spread.
    def forward_at(k, j):
        t = times[j]
        return und[k]["spot"] * und[k]["dividend"].discount(t, True) / note_curve.discount(t, True)

    if want_market:
        market = {"observation_dates": [ev["observation_iso"] for ev in observed],
                  "underlyings": []}
        obs_times = [times[ev["grid_index"]] for ev in observed]
        t_last = obs_times[-1]
        for k in range(n_u):
            u = und[k]
            mu = {"underlying_id": ids[k], "spot": u["spot"], "forwards": [],
                  "implied_dividend_yields": [], "implied_rates": [], "atm_vols": [],
                  "barrier_vols": []}
            t_prev = 0.0
            for ev, t in zip(observed, obs_times):
                fwd = forward_at(k, ev["grid_index"])
                mu["forwards"].append(fwd)
                dt = t - t_prev

                def implied_rate(c):
                    return -math.log(c.discount(t, True) / c.discount(t_prev, True)) / dt

                mu["implied_dividend_yields"].append(implied_rate(u["plain_dividend"]))
                mu["implied_rates"].append(implied_rate(u["local_curve"]))
                mu["atm_vols"].append(u["vol"].blackVol(t, fwd, True))
                mu["barrier_vols"].append(u["vol"].blackVol(t, barrier * initial[k], True))
                t_prev = t
            if u["quanto"]:
                s_s = u["vol"].blackVol(t_last, initial[k], True)
                s_fx = u["fx_vol"].blackVol(t_last, u["fx_spot"], True)
                mu["quanto_drift_bp"] = -u["rho"] * s_s * s_fx / 1.0e-4
            market["underlyings"].append(mu)
        pay_dates = [ev["payment"] for ev in observed]
        if not pay_dates or pay_dates[-1] != maturity:
            pay_dates.append(maturity)
        market["payment_dates"] = [_iso_date(d) for d in pay_dates]
        market["discount_factors"] = [note_curve.discount(d, True) for d in pay_dates]
        market["discount_factors_with_funding"] = [df(d) for d in pay_dates]
        market["correlation_labels"] = list(ids)
        market["correlation"] = [correlation[i][j] for i in range(n_u) for j in range(n_u)]
        result["market"] = market

    if want_decomposition:
        dec = {"events": []}
        for e, ev in enumerate(future):
            if ev["accrual_only"]:
                continue
            b = A_BASE + per_event * e
            row = {"index": ev["index"], "observation_date": ev["observation_iso"]}
            if ev["autocall"] is not None:
                row["p_call"] = mean[b]
            if not ev["is_final"]:
                row["p_coupon"] = mean[b + 1]
            row["pv_coupons"] = mean[b + 2] * notional
            row["pv_redemption"] = mean[b + 3] * notional
            dec["events"].append(row)
        dec["p_survive_to_maturity"] = mean[A_SURVIVE]
        dec["p_breach"] = mean[A_BREACH]
        dec["expected_life_years"] = mean[A_LIFE]
        if mean[A_BREACH] > 0.0:
            dec["expected_loss_given_breach"] = mean[A_LOSS_UNDISCOUNTED] / mean[A_BREACH]
        result["decomposition"] = dec

    if want_checks:
        per_sample = 2 if antithetic else 1
        checks = {"running_means": []}
        for rm in sim["running_means"]:
            checks["running_means"].append({
                "fraction": rm["fraction"], "paths": rm["samples"] * per_sample,
                "npv": (rm["means"][A_TOTAL] + known_total) * notional,
                "standard_error": rm["standard_errors"][A_TOTAL] * notional})
        checks["running_means"].append({"fraction": 1.0, "paths": paths,
                                        "npv": result["npv"],
                                        "standard_error": result["standard_error"]})
        # Every underlying at its forward on every grid point (fine grid
        # included), one path through the same walk.
        forward_path = [[forward_at(k, j) for j in range(steps + 1)] for k in range(n_u)]
        acc = walk(forward_path, base)
        checks["deterministic_forward_npv"] = (acc[A_TOTAL] + known_total) * notional
        checks["bond_only_npv"] = notional * df_maturity
        if n_u == 1:
            checks["correlation_one_npv"] = result["npv"]
            checks["correlation_one_standard_error"] = result["standard_error"]
            checks["correlation_zero_npv"] = result["npv"]
            checks["correlation_zero_standard_error"] = result["standard_error"]
        else:
            # The same stream on fresh processes (the engine rebuilds the
            # process array for each limit run); StochasticProcessArray
            # takes the spectral pseudo-square-root, which handles the
            # rank-1 all-ones matrix without perturbation.
            for name, fill in (("one", lambda i, j: 1.0),
                               ("zero", lambda i, j: 1.0 if i == j else 0.0)):
                m = ql.Matrix(n_u, n_u)
                for i in range(n_u):
                    for j in range(n_u):
                        m[i][j] = fill(i, j)
                limit = simulate(base_assets(), m, base)
                checks[f"correlation_{name}_npv"] = (limit["mean"][A_TOTAL] + known_total) * notional
                checks[f"correlation_{name}_standard_error"] = limit["standard_error"][A_TOTAL] * notional
        result["checks"] = checks

    if want_distribution:
        def quantile(sorted_values, p):
            n = len(sorted_values)
            h = float(n - 1) * p
            lo = int(math.floor(h))
            hi = min(lo + 1, n - 1)
            return sorted_values[lo] + (h - float(lo)) * (sorted_values[hi] - sorted_values[lo])

        dist = {"basket_performance_quantiles": []}
        for ev in observed:
            s = sorted(rec["perf"][ev["observed_index"]])
            dist["basket_performance_quantiles"].append({
                "observation_date": ev["observation_iso"],
                "q05": quantile(s, 0.05), "q25": quantile(s, 0.25), "q50": quantile(s, 0.50),
                "q75": quantile(s, 0.75), "q95": quantile(s, 0.95)})
        s = sorted(rec["payout"])
        for p, name in ((0.05, "q05"), (0.25, "q25"), (0.50, "q50"), (0.75, "q75"),
                        (0.95, "q95")):
            dist[f"payout_{name}"] = quantile(s, p)
        n_bins = 20
        lo, hi = s[0], s[-1]
        width = (hi - lo) / float(n_bins)
        bins = [{"lower": lo + float(b) * width,
                 "upper": hi if b + 1 == n_bins else lo + float(b + 1) * width,
                 "paths": 0} for b in range(n_bins)]
        for v in rec["payout"]:
            b = int((v - lo) / width) if width > 0.0 else 0
            if b >= n_bins:
                b = n_bins - 1
            bins[b]["paths"] += 1
        dist["payout_histogram"] = bins
        dist["sample_paths"] = []
        for i in range(min(sample_paths, paths)):
            sp = {"basket_performances": [rec["perf"][j][i] for j in range(n_observed)],
                  "payout": rec["payout"][i], "payout_pv": rec["payout_pv"][i],
                  "breached": bool(rec["breached"][i])}
            if rec["called_event"][i] is not None:
                sp["called_on"] = parsed[rec["called_event"][i]]["observation_iso"]
            dist["sample_paths"].append(sp)
        result["distribution"] = dist

    # --- vanilla repricing (StructuredNoteVanillaRepricing doc comments) ---------
    # One extra simulation on the note's grid plus every used sheet expiry:
    # the out-of-the-money side of every usable quote (the engine's
    # selectOutOfTheMoneyQuotes on the model forward spot x D_div(T) /
    # D_note(T) and the plain note curve's DF) as a European payoff at its
    # expiry grid point, D_note(T) x mean, with the note walked on the same
    # paths (note_npv).
    if bool(diag.get("vanilla_repricing", False)):
        flt = diag.get("vanilla_repricing_filter") or {}
        rep_dates = [ev["observation"] for ev in observed]
        rep_unds = []
        options = []  # (k, expiry date, strike, is_call)
        for sheet in diag.get("vanilla_repricing_sheets") or []:
            if sheet["underlying_id"] not in ids:
                continue
            k = ids.index(sheet["underlying_id"])
            if und[k]["quanto"]:
                raise ValueError("vanilla repricing: a sheet on a quanto underlying")
            if sheet.get("option_style", "European") != "European":
                raise ValueError("vanilla repricing: option_style must be European")
            spot_k = und[k]["spot"]
            by_expiry = {}
            for q in sheet["quotes"]:
                by_expiry.setdefault(q["expiry"], []).append(q)
            ru = {"underlying_id": ids[k], "sheet_id": sheet["id"], "spot": spot_k,
                  "expiries": [], "options": []}
            for expiry in sorted(by_expiry, key=parse_date):
                d = parse_date(expiry)
                t = dc365.yearFraction(as_of, d)
                if d <= as_of:
                    ru["expiries"].append({"expiry": expiry, "time": t, "quotes_used": 0})
                    continue
                df = note_curve.discount(t, True)
                fwd = spot_k * und[k]["dividend"].discount(t, True) / note_curve.discount(t, True)
                used = _otm_quotes(by_expiry[expiry], flt, spot_k, fwd, df, t)
                ru["expiries"].append({"expiry": expiry, "time": t, "forward": fwd,
                                       "discount_factor": df, "quotes_used": len(used)})
                if used:
                    rep_dates.append(d)
                for u in used:
                    ru["options"].append({"expiry": expiry, "time": t, "strike": u["strike"],
                                          "option_type": "Call" if u["is_call"] else "Put",
                                          "bid": u["bid"], "ask": u["ask"], "mid": u["mid"],
                                          "market_vol": u["vol"], "forward": fwd,
                                          "discount_factor": df})
                    options.append((k, d, u["strike"], u["is_call"]))
            if not ru["options"]:
                raise ValueError(f"vanilla repricing: no usable quote for {ids[k]}")
            rep_unds.append(ru)
        if not rep_unds:
            raise ValueError("vanilla repricing: no sheet for any underlying of the note")
        rep_dates.sort()
        rep_times, rep_index = _sn_simulation_times(as_of, rep_dates, steps_per_year)
        rep_grid = ql.TimeGrid(rep_times)
        rep_steps = len(rep_times) - 1
        rep_gi = [None if ev["accrual_only"] else rep_index[ev["observation"]] for ev in future]
        opt_gi = [rep_index[d] for (_k, d, _strike, _is_call) in options]
        n_opt = len(options)

        def rep_walk(path, disc, rec=None, slot=0):
            return walk(path, disc, rec, slot, rep_gi)

        def rep_extra(path, acc):
            for i, (k, _d, strike, is_call) in enumerate(options):
                level = path[k][opt_gi[i]]
                acc[n_acc + i] = max(level - strike if is_call else strike - level, 0.0)

        rep_simulate = make_simulate(rep_grid, rep_steps, n_acc + n_opt, rep_walk, rep_extra)
        rep = rep_simulate(base_assets(), correlation, base)
        vr = {"paths": paths, "time_steps": rep_steps,
              "note_npv": (rep["mean"][A_TOTAL] + known_total) * notional,
              "note_standard_error": rep["standard_error"][A_TOTAL] * notional,
              "count": 0, "inside_bid_ask": 0, "max_abs_price_error": 0.0,
              "max_abs_vol_error": None, "max_standard_error": 0.0, "underlyings": rep_unds}
        i = 0
        for ru in rep_unds:
            ru.update({"count": 0, "inside_bid_ask": 0, "max_abs_price_error": 0.0,
                       "max_abs_vol_error": None, "max_standard_error": 0.0})
            for o in ru["options"]:
                df, fwd = o.pop("discount_factor"), o.pop("forward")
                o["model_price"] = df * rep["mean"][n_acc + i]
                o["standard_error"] = df * rep["standard_error"][n_acc + i]
                o["inside_bid_ask"] = o["bid"] <= o["model_price"] <= o["ask"]
                o["price_error"] = o["model_price"] - o["mid"]
                mv = _black_implied_vol(o["option_type"] == "Call", o["strike"], fwd,
                                        o["model_price"], df, o["time"])
                if mv is not None:
                    o["model_vol"] = mv
                    o["vol_error"] = mv - o["market_vol"]
                ru["count"] += 1
                if o["inside_bid_ask"]:
                    ru["inside_bid_ask"] += 1
                ru["max_abs_price_error"] = max(ru["max_abs_price_error"], abs(o["price_error"]))
                ru["max_standard_error"] = max(ru["max_standard_error"], o["standard_error"])
                if "vol_error" in o:
                    ru["max_abs_vol_error"] = max(ru["max_abs_vol_error"] or 0.0,
                                                  abs(o["vol_error"]))
                i += 1
            vr["count"] += ru["count"]
            vr["inside_bid_ask"] += ru["inside_bid_ask"]
            vr["max_abs_price_error"] = max(vr["max_abs_price_error"], ru["max_abs_price_error"])
            vr["max_standard_error"] = max(vr["max_standard_error"], ru["max_standard_error"])
            if ru["max_abs_vol_error"] is not None:
                vr["max_abs_vol_error"] = max(vr["max_abs_vol_error"] or 0.0,
                                              ru["max_abs_vol_error"])
            if ru["max_abs_vol_error"] is None:
                del ru["max_abs_vol_error"]
        if vr["max_abs_vol_error"] is None:
            del vr["max_abs_vol_error"]
        result["vanilla_repricing"] = vr

    # --- Greeks (StructuredNoteGreeks doc comments; the engine's arithmetic) ---
    if greeks_spec is not None:
        h = float(greeks_spec.get("spot_bump", 0.01))
        dv = float(greeks_spec.get("vol_bump", 0.01))
        dr = float(greeks_spec.get("correlation_bump", 0.01))
        funding_bump_bp = float(greeks_spec.get("funding_bump_bp", 1.0))
        rate_bump_bp = float(greeks_spec.get("rate_bump_bp", 1.0))
        funding_bp = float(note.get("funding_spread_bp", 0.0))

        def sample_values(totals, disc):
            return [(0.5 * (totals[2 * s] + totals[2 * s + 1]) if antithetic else totals[s])
                    + disc["known_total"] for s in range(samples)]

        def difference_statistics(d):
            n = float(len(d))
            total = 0.0
            total_sq = 0.0
            for x in d:
                total += x
                total_sq += x * x
            m = total / n
            if len(d) < 2:
                return m * notional, float("nan")
            variance = (total_sq - n * m * m) / (n - 1.0)
            if variance < 0.0:
                variance = 0.0
            return m * notional, math.sqrt(variance / n) * notional

        def bumped_run(spot_of, spot_factor, vol_of, risk_free, note_curve_moved, matrix, disc):
            assets = []
            for k, u in enumerate(und):
                spot = u["spot_handle"]
                if k == spot_of:
                    spot = ql.QuoteHandle(ql.SimpleQuote(u["spot"] * spot_factor))
                vol = u["vol"]
                if k == vol_of:
                    vol = _sn_black_vol(pricing, u["vol_id"], dv)[0]
                dividend = u["dividend"]
                if u["quanto"] and (k == vol_of or note_curve_moved):
                    dividend = ql.YieldTermStructureHandle(ql.QuantoTermStructure(
                        u["plain_dividend"], risk_free, u["local_curve"], vol, u["level"],
                        u["fx_vol"], u["fx_spot"], u["rho"]))
                assets.append(make_asset(spot, dividend, risk_free, vol, u["exact"]))
            totals = []
            simulate(assets, matrix, disc, None, totals)
            return sample_values(totals, disc)

        def difference(a, b, scale):
            return difference_statistics([(x - y) * scale for x, y in zip(a, b)])

        base_values = sample_values(base_totals, base)
        greeks = {"underlyings": [], "spot_bump": h, "vol_bump": dv, "correlation_bump": dr,
                  "funding_bump_bp": funding_bump_bp, "rate_bump_bp": rate_bump_bp,
                  "correlation_bump_repaired": False}
        for k in range(n_u):
            up = bumped_run(k, 1.0 + h, None, note_curve, False, correlation, base)
            down = bumped_run(k, 1.0 - h, None, note_curve, False, correlation, base)
            delta, delta_se = difference(up, down, 0.01 / (2.0 * h))
            gamma_scale = (0.01 / h) * (0.01 / h)
            gamma, gamma_se = difference_statistics(
                [(x - 2.0 * y + z) * gamma_scale for x, y, z in zip(up, base_values, down)])
            vol_up = bumped_run(None, 1.0, k, note_curve, False, correlation, base)
            vega, vega_se = difference(vol_up, base_values, 0.01 / dv)
            greeks["underlyings"].append({
                "underlying_id": ids[k], "delta": delta, "delta_standard_error": delta_se,
                "gamma": gamma, "gamma_standard_error": gamma_se,
                "vega": vega, "vega_standard_error": vega_se})
        if n_u > 1:
            bumped, repaired = _sn_bump_correlation(correlation, dr)
            greeks["correlation_bump_repaired"] = repaired
            greeks["correlation_bumped"] = [bumped[i][j] for i in range(n_u) for j in range(n_u)]
            corr_up = bumped_run(None, 1.0, None, note_curve, False, bumped, base)
            cs, cs_se = difference(corr_up, base_values, 0.01 / dr)
            greeks["correlation_sensitivity"] = cs
            greeks["correlation_sensitivity_standard_error"] = cs_se
        funded = discounting_on(_sn_spreaded(note_curve, (funding_bp + funding_bump_bp) * 1.0e-4))
        fund_up = bumped_run(None, 1.0, None, note_curve, False, correlation, funded)
        greeks["funding_dv01"], greeks["funding_dv01_standard_error"] = difference(
            fund_up, base_values, 1.0 / funding_bump_bp)
        shifted_note = ql.YieldTermStructureHandle(
            _sn_spreaded(note_curve, rate_bump_bp * 1.0e-4))
        shifted = discounting_on(_sn_spreaded(shifted_note, funding_bp * 1.0e-4))
        rate_up = bumped_run(None, 1.0, None, shifted_note, True, correlation, shifted)
        greeks["rho"], greeks["rho_standard_error"] = difference(
            rate_up, base_values, 1.0 / rate_bump_bp)
        result["greeks"] = greeks
    return result


def price_structured_note_ql(request: dict) -> dict:
    """Functional-catalog entry point (compare="fields"): the headline
    scalars of notes[0] as dotted response paths, from structured_note_ql on
    the same random stream -- npv, standard error, the PV of the three
    cash-flow buckets (bond_redemption, coupons, capital_loss, in that
    order), barrier-breach probability, expected life and the coupon memory
    carried in. The per-event probabilities and the bucket standard errors
    are compared field by field in tests/contract/structured_note_parity_test.py."""
    r = structured_note_ql(request, 0)
    out = {"notes.0.npv": r["npv"]}
    if "standard_error" in r:
        out["notes.0.standard_error"] = r["standard_error"]
    if "legs" in r:
        for bucket in ("bond_redemption", "coupons", "capital_loss"):
            out[f"notes.0.legs.{bucket}.pv"] = r["legs"][bucket]["pv"]
    if "p_barrier_breach" in r:
        out["notes.0.p_barrier_breach"] = r["p_barrier_breach"]
        out["notes.0.expected_life_years"] = r["expected_life_years"]
    out["notes.0.seasoning.memory_carried"] = r["seasoning"]["memory_carried"]
    if "vanilla_repricing" in r:
        vr = r["vanilla_repricing"]
        out["notes.0.vanilla_repricing.note_npv"] = vr["note_npv"]
        out["notes.0.vanilla_repricing.note_standard_error"] = vr["note_standard_error"]
        out["notes.0.vanilla_repricing.count"] = vr["count"]
        out["notes.0.vanilla_repricing.inside_bid_ask"] = vr["inside_bid_ask"]
        out["notes.0.vanilla_repricing.max_abs_price_error"] = vr["max_abs_price_error"]
        out["notes.0.vanilla_repricing.max_standard_error"] = vr["max_standard_error"]
        if "max_abs_vol_error" in vr:
            out["notes.0.vanilla_repricing.max_abs_vol_error"] = vr["max_abs_vol_error"]
        # A sample of the repriced options per sheet: the first, the middle
        # and the last used quote (model price, standard error, model vol).
        for i, u in enumerate(vr["underlyings"]):
            n = len(u["options"])
            for j in sorted({0, n // 2, n - 1}):
                o = u["options"][j]
                stem = f"notes.0.vanilla_repricing.underlyings.{i}.options.{j}."
                out[stem + "model_price"] = o["model_price"]
                out[stem + "standard_error"] = o["standard_error"]
                if "model_vol" in o:
                    out[stem + "model_vol"] = o["model_vol"]
    return out


# =============================================================================
# Response NPV extraction (API side of the parity comparison)
# =============================================================================


# =============================================================================
# Implied forwards and dividends from option quotes (/calibrate-implied-forwards)
# =============================================================================

def _implied_forwards_weighted_line(pairs):
    """Weighted least squares of y on K, same algebra and the same summation
    order as the engine's weightedLine (header of
    src/evaluators/implied_forwards_evaluator.h): returns (a, b) or None when
    the strikes do not span."""
    W = sK = sY = 0.0
    for k, y, w in pairs:
        W += w
        sK += w * k
        sY += w * y
    k_bar = sK / W
    y_bar = sY / W
    sxy = sxx = 0.0
    for k, y, w in pairs:
        dk = k - k_bar
        sxy += w * dk * (y - y_bar)
        sxx += w * dk * dk
    if not sxx > 0.0:
        return None
    b = sxy / sxx
    return y_bar - b * k_bar, b


def _quote_filter_pass(lines, flt):
    """The per-line rules of filterExpiryLines (implied_forwards_evaluator.cpp)
    in the same order; returns the survivors as dicts with both sides' mids
    and spreads (the dropped lines are not needed by the references)."""
    max_rel = float(flt.get("max_relative_spread", 0.05))
    min_oi = flt.get("min_open_interest")
    out = []
    seen = set()
    for q in lines:
        k = q.get("strike")
        if k is None or not k > 0.0 or not math.isfinite(k):
            continue
        sides = [q.get("call_bid"), q.get("call_ask"), q.get("put_bid"), q.get("put_ask")]
        if any(v is None for v in sides):
            continue
        cb, ca, pb, pa = sides
        if any(not math.isfinite(v) for v in sides):
            continue
        if cb < 0.0 or pb < 0.0 or not ca > 0.0 or not pa > 0.0:
            continue
        if cb > ca or pb > pa:
            continue
        if min_oi is not None:
            coi, poi = q.get("call_open_interest"), q.get("put_open_interest")
            if coi is None or poi is None or coi < min_oi or poi < min_oi:
                continue
        c_mid, p_mid = 0.5 * (cb + ca), 0.5 * (pb + pa)
        c_spr, p_spr = ca - cb, pa - pb
        if c_spr / c_mid > max_rel or p_spr / p_mid > max_rel:
            continue
        if k in seen:
            continue
        seen.add(k)
        out.append({"strike": k, "call_bid": cb, "call_ask": ca, "put_bid": pb, "put_ask": pa,
                    "call_mid": c_mid, "put_mid": p_mid, "call_spread": c_spr,
                    "put_spread": p_spr})
    return out


def _black_implied_vol(is_call, strike, fwd, price, df, t):
    """The engine's impliedBlackVol: blackFormulaImpliedStdDev (1e-12, 1000
    iterations) / sqrt(t); None when it does not solve or is not > 0."""
    try:
        sd = ql.blackFormulaImpliedStdDev(
            ql.Option.Call if is_call else ql.Option.Put, strike, fwd, price, df, 0.0,
            ql.nullDouble(), 1e-12, 1000)
    except RuntimeError:
        return None
    vol = sd / math.sqrt(t)
    if not math.isfinite(vol) or not vol > 0.0:
        return None
    return vol


def _otm_quotes(lines, flt, spot, fwd, df, t):
    """The engine's selectOutOfTheMoneyQuotes (implied_forwards_evaluator.cpp):
    the out-of-the-money side (call iff strike >= fwd) of every line that
    passes the per-line rules, kept when its mid is above intrinsic, at least
    1e-5 * spot (quote resolution) and has a Black-76 implied vol on (fwd, df,
    t); sorted by strike."""
    used = []
    for q in _quote_filter_pass(lines, flt):
        k_abs = q["strike"]
        is_call = k_abs >= fwd
        bid = q["call_bid"] if is_call else q["put_bid"]
        ask = q["call_ask"] if is_call else q["put_ask"]
        mid = q["call_mid"] if is_call else q["put_mid"]
        intrinsic = df * max(fwd - k_abs if is_call else k_abs - fwd, 0.0)
        if not mid > intrinsic or mid < 1e-5 * spot:
            continue
        vol = _black_implied_vol(is_call, k_abs, fwd, mid, df, t)
        if vol is None:
            continue
        used.append({"strike": k_abs, "is_call": is_call, "bid": bid, "ask": ask,
                     "mid": mid, "vol": vol})
    used.sort(key=lambda u: u["strike"])
    return used


def _implied_forwards_fit(sheet, spot, curve, flt, eval_date):
    """The per-expiry fit of calibrate_implied_forwards_ql: returns
    [(expiry, usable, values)] over the sheet's expiries in date order."""
    dc = ql.Actual365Fixed()
    min_quotes = int(flt.get("min_quotes_per_expiry", 4))
    window = float(flt.get("moneyness_window", 0.25))
    by_expiry = {}
    for q in sheet["quotes"]:
        by_expiry.setdefault(q["expiry"], []).append(q)
    expiries = []   # (expiry, usable, values)
    for expiry in sorted(by_expiry, key=parse_date):
        lines = by_expiry[expiry]
        exp_date = parse_date(expiry)
        t = dc.yearFraction(eval_date, exp_date)
        if exp_date <= eval_date:
            expiries.append((expiry, False, None))
            continue
        pairs = []
        zero_spread = False
        for q in _quote_filter_pass(lines, flt):
            total = q["call_spread"] + q["put_spread"]
            if not total > 0.0:
                zero_spread = True
            pairs.append([q["strike"], q["call_mid"] - q["put_mid"],
                          1.0 / total if total > 0.0 else 1.0])
        if zero_spread:
            for p_ in pairs:
                p_[2] = 1.0
        if len(pairs) < min_quotes:
            expiries.append((expiry, False, None))
            continue
        fit1 = _implied_forwards_weighted_line(pairs)
        if fit1 is None or not fit1[1] < 0.0:
            expiries.append((expiry, False, None))
            continue
        f1 = -fit1[0] / fit1[1]
        if not f1 > 0.0 or not math.isfinite(f1):
            expiries.append((expiry, False, None))
            continue
        lo, hi = f1 * (1.0 - window), f1 * (1.0 + window)
        inside = [p_ for p_ in pairs if not (p_[0] < lo or p_[0] > hi)]
        if len(inside) < min_quotes:
            expiries.append((expiry, False, None))
            continue
        fit2 = _implied_forwards_weighted_line(inside)
        if fit2 is None or not fit2[1] < 0.0:
            expiries.append((expiry, False, None))
            continue
        a, b = fit2
        df_implied = -b
        forward = a / df_implied
        if not forward > 0.0 or not math.isfinite(forward):
            expiries.append((expiry, False, None))
            continue
        ss = 0.0
        max_abs = 0.0
        for k, y, _w in inside:
            r_ = y - (a + b * k)
            ss += r_ * r_
            max_abs = max(max_abs, abs(r_))
        df_curve = curve.discount(exp_date)
        r = -math.log(df_curve) / t
        q_ = r - math.log(forward / spot) / t
        expiries.append((expiry, True, {
            "time": t,
            "quotes_used": float(len(inside)),
            "forward": forward,
            "implied_discount_factor": df_implied,
            "curve_discount_factor": df_curve,
            "implied_funding_bp": -math.log(df_implied / df_curve) / t * 1e4,
            "implied_dividend_yield": q_,
            "fit_residual_rmse": math.sqrt(ss / len(inside)),
            "max_abs_residual": max_abs,
        }))
    return expiries


def calibrate_implied_forwards_ql(request: dict) -> dict:
    """Independent re-run of the engine's per-expiry put-call-parity fit.

    Mirrors src/evaluators/implied_forwards_evaluator.cpp rule for rule, in
    plain Python (no numpy): the spot from the underlying's spot quote, the
    discount factors from the request's discounting curve rebuilt in
    QuantLib, the per-line filters in the same order (strike > 0, both sides
    present, finite, non-negative bid / positive ask, not crossed, open
    interest, relative spread on each side, duplicate strike), weights
    1 / (call spread + put spread) with the equal-weights fallback when any
    pair of the expiry has a zero total spread, the first pass over every
    surviving pair, the moneyness window around the first-pass forward, the
    second pass, and the curve-relative quantities with T = Act/365F:

        DF_implied = -b, F = -a / b, r = -ln(DF_curve) / T,
        q = r - ln(F / S) / T, funding_bp = -ln(DF_implied / DF_curve) / T * 1e4.

    Returns {dotted.response.path: value} for every fitted expiry (the
    per-expiry report, the plain forward table and the zero points of the
    returned dividend curve) so the functional catalog's "fields" compare and
    the contract test check the whole response. Non-European sheets and
    sheets with no surviving expiry raise, like the engine's 400.
    """
    pricing = _reference_pricing_view(request)
    as_of = pricing["as_of_date"]
    eval_date = parse_date(as_of)
    ql.Settings.instance().evaluationDate = eval_date

    sheet = request["sheet"]
    if sheet.get("option_style", "European") != "European":
        raise ValueError("option_style must be European")
    underlying = next(u for u in pricing["equity_underlyings"]
                      if u["id"] == sheet["underlying_id"])
    spot = next(q["value"] for q in pricing["quotes"] if q["id"] == underlying["spot_quote_id"])
    disc_json = next(c for c in pricing["curves"] if c["id"] == request["discounting_curve"])
    curve = build_curve_from_json(disc_json, eval_date, request)
    dc = ql.Actual365Fixed()

    flt = dict(request.get("filter") or {})
    max_rel = float(flt.get("max_relative_spread", 0.05))
    min_quotes = int(flt.get("min_quotes_per_expiry", 4))
    window = float(flt.get("moneyness_window", 0.25))
    min_oi = flt.get("min_open_interest")

    expiries = _implied_forwards_fit(sheet, spot, curve, flt, eval_date)
    fields = {}
    usable = [(e, v) for e, ok, v in expiries if ok]
    if not usable:
        raise ValueError("no expiry survived the implied-forward fit")
    for i, (_e, ok, v) in enumerate(expiries):
        if not ok:
            continue
        for name, value in v.items():
            fields[f"expiries.{i}.{name}"] = value
    for j, (_e, v) in enumerate(usable):
        fields[f"forwards.{j}.time"] = v["time"]
        fields[f"forwards.{j}.forward"] = v["forward"]
        fields[f"forwards.{j}.implied_dividend_yield"] = v["implied_dividend_yield"]
    # Dividend curve: as-of node with the first fitted yield, then one per expiry.
    yields = [usable[0][1]["implied_dividend_yield"]] + \
        [v["implied_dividend_yield"] for _e, v in usable]
    for k_, y_ in enumerate(yields):
        fields[f"dividend_curve_spec.points.{k_}.point.zero_rate"] = y_
    return fields


# ---------------------------------------------------------------------------
# Black vol surface from option quotes (/calibrate-vol-surface)
# ---------------------------------------------------------------------------

def _svi_total_variance(p, k):
    d = k - p["m"]
    return p["a"] + p["b"] * (p["rho"] * d + math.sqrt(d * d + p["sigma"] ** 2))


def _svi_durrleman_g(p, k):
    d = k - p["m"]
    big_r = math.sqrt(d * d + p["sigma"] ** 2)
    w = _svi_total_variance(p, k)
    if not w > 0.0:
        return -math.inf
    w1 = p["b"] * (p["rho"] + d / big_r)
    w2 = p["b"] * p["sigma"] ** 2 / big_r ** 3
    t1 = 1.0 - k * w1 / (2.0 * w)
    return t1 * t1 - (w1 * w1 / 4.0) * (1.0 / w + 0.25) + w2 / 2.0


def calibrate_vol_surface_ql(request: dict) -> dict:
    """Independent re-run of src/evaluators/vol_surface_calibration_evaluator.cpp
    with the Python bindings of the same QuantLib classes.

    Forwards: the request's `forwards` (usable entries) or the implied-forward
    fit above (same filter). Per expiry: the per-line filter, the
    out-of-the-money mid (call for K >= F, put below), its Black-76 implied
    vol via blackFormulaImpliedStdDev on the fitted forward and implied
    discount factor (dropped when not above intrinsic, below the quote
    resolution 1e-5 * spot, or not solvable), strikes sorted. Then

      * AndreasenHuge: AndreasenHugeVolatilityInterpl on CalibrationPairs
        (out-of-the-money VanillaOption, SimpleQuote(vol)), spot handle,
        DiscountCurve of the implied DFs (log-linear, Act/365F) and a
        dividend DiscountCurve with F DF / S, CubicSpline, CallPut, 500 grid
        points; AndreasenHugeVolatilityAdapter.blackVol at every grid point
        whose model price (interpl.optionPrice) is at least 1e-4 * spot, the
        others filled from the nearest inverted strike (lowest index on a
        tie), as the engine does;
      * SviPerExpiry: SviInterpolatedSmileSection per expiry (all five
        parameters free, vega-weighted, EndCriteria(500, 100, 1e-12, 1e-10,
        1e-10), LevenbergMarquardt(1e-10, 1e-10, 1e-10)) — the same
        XABRInterpolationImpl the engine's SviInterpolation runs — then the
        butterfly check and the calendar clip. The engine's penalized
        butterfly refit is not reproduced here (no CostFunction subclassing
        in the bindings): a violation with enforce_butterfly on raises, so
        the catalog only carries arbitrage-free sheets for this method.

    The grid: strikes = moneyness * spot (default 0.50 .. 1.50 step 0.025),
    expiries = the fitted sheet expiries (or the request's calendar-day
    periods). The returned surface is rebuilt as the pricers read it
    (BlackVarianceSurface, NullCalendar, Act/365F, bilinear, extrapolation)
    and every used quote repriced on it.

    Returns {dotted.response.path: value}: the forwards used, per-expiry
    quotes_used / rmse_vol / max_abs_error_vol (and the SVI parameters), the
    grid strikes, every surface vol of vol_surface_spec, the repricing
    summary and the per-quote model vol and price.
    """
    pricing = _reference_pricing_view(request)
    eval_date = parse_date(pricing["as_of_date"])
    ql.Settings.instance().evaluationDate = eval_date
    dc = ql.Actual365Fixed()

    sheet = request["sheet"]
    if sheet.get("option_style", "European") != "European":
        raise ValueError("option_style must be European")
    underlying = next(u for u in pricing["equity_underlyings"]
                      if u["id"] == sheet["underlying_id"])
    spot = next(q["value"] for q in pricing["quotes"] if q["id"] == underlying["spot_quote_id"])
    disc_json = next(c for c in pricing["curves"] if c["id"] == request["discounting_curve"])
    curve = build_curve_from_json(disc_json, eval_date, request)
    flt = dict(request.get("filter") or {})
    method = request.get("method", "AndreasenHuge")
    svi_opts = dict(request.get("svi") or {})
    enforce_butterfly = bool(svi_opts.get("enforce_butterfly", True))
    enforce_calendar = bool(svi_opts.get("enforce_calendar", True))

    # 1. Forwards.
    forwards = {}
    if request.get("forwards") is not None:
        for f in request["forwards"]:
            if not f.get("usable"):
                continue
            forwards[f["expiry"]] = (float(f["forward"]), float(f["implied_discount_factor"]))
    else:
        for expiry, ok, v in _implied_forwards_fit(sheet, spot, curve, flt, eval_date):
            if ok:
                forwards[expiry] = (v["forward"], v["implied_discount_factor"])

    # 2. Per expiry: filter, out-of-the-money mids, implied vols.
    min_used = max(int(flt.get("min_quotes_per_expiry", 4)),
                   5 if method == "SviPerExpiry" else 1)
    by_expiry = {}
    for q in sheet["quotes"]:
        by_expiry.setdefault(q["expiry"], []).append(q)
    work = []        # usable expiries: dicts
    report_idx = {}  # expiry -> index in the response's expiries array
    for i, expiry in enumerate(sorted(by_expiry, key=parse_date)):
        report_idx[expiry] = i
        exp_date = parse_date(expiry)
        if exp_date <= eval_date or expiry not in forwards:
            continue
        fwd, df = forwards[expiry]
        t = dc.yearFraction(eval_date, exp_date)
        used = _otm_quotes(by_expiry[expiry], flt, spot, fwd, df, t)
        if len(used) < min_used:
            continue
        work.append({"expiry": expiry, "date": exp_date, "t": t, "fwd": fwd, "df": df,
                     "used": used})
    if not work:
        raise ValueError("no usable expiry")

    # Grid.
    grid = dict(request.get("output_grid") or {})
    moneyness = list(grid.get("strikes_as_moneyness") or []) or \
        [round(0.50 + 0.025 * i, 6) for i in range(41)]
    strikes = [m * spot for m in moneyness]
    if grid.get("expiries"):
        cal = ql.NullCalendar()
        grid_dates = [cal.advance(eval_date, _wire_period_to_ql(p), ql.Unadjusted)
                      for p in grid["expiries"]]
    else:
        grid_dates = [w["date"] for w in work]
    n_exp, n_str = len(grid_dates), len(strikes)
    vols = [[math.nan] * n_str for _ in range(n_exp)]
    fields = {}

    if method == "AndreasenHuge":
        dates = [eval_date] + [w["date"] for w in work]
        r_dfs = [1.0] + [w["df"] for w in work]
        q_dfs = [1.0] + [w["fwd"] * w["df"] / spot for w in work]
        r_ts = ql.YieldTermStructureHandle(ql.DiscountCurve(dates, r_dfs, dc))
        q_ts = ql.YieldTermStructureHandle(ql.DiscountCurve(dates, q_dfs, dc))
        cal_set = ql.CalibrationSet()
        for w in work:
            exercise = ql.EuropeanExercise(w["date"])
            for u in w["used"]:
                payoff = ql.PlainVanillaPayoff(ql.Option.Call if u["is_call"] else ql.Option.Put,
                                               u["strike"])
                cal_set.push_back(ql.CalibrationPair(ql.VanillaOption(payoff, exercise),
                                                     ql.SimpleQuote(u["vol"])))
        interpl = ql.AndreasenHugeVolatilityInterpl(
            cal_set, ql.QuoteHandle(ql.SimpleQuote(spot)), r_ts, q_ts,
            ql.AndreasenHugeVolatilityInterpl.CubicSpline,
            ql.AndreasenHugeVolatilityInterpl.CallPut, 500)
        adapter = ql.AndreasenHugeVolatilityAdapter(interpl)
        for w in work:
            errs = []
            for u in w["used"]:
                try:
                    errs.append(adapter.blackVol(w["date"], u["strike"], True) - u["vol"])
                except RuntimeError:
                    pass
            if errs:
                i = report_idx[w["expiry"]]
                fields[f"expiries.{i}.rmse_vol"] = math.sqrt(sum(e * e for e in errs) / len(errs))
                fields[f"expiries.{i}.max_abs_error_vol"] = max(abs(e) for e in errs)
        for i, d in enumerate(grid_dates):
            t = dc.yearFraction(eval_date, d)
            fwd_t = interpl.fwd(t)
            row = []
            for k_abs in strikes:
                try:
                    price = interpl.optionPrice(
                        t, k_abs, ql.Option.Call if k_abs >= fwd_t else ql.Option.Put)
                    if not price >= 1e-4 * spot:
                        row.append(math.nan)
                        continue
                    v = adapter.blackVol(d, k_abs, True)
                    row.append(v if math.isfinite(v) and v > 0.0 else math.nan)
                except RuntimeError:
                    row.append(math.nan)
            solved = [j for j in range(n_str) if not math.isnan(row[j])]
            if not solved:
                raise ValueError("Andreasen-Huge inversion failed on a whole row")
            for j in range(n_str):
                if math.isnan(row[j]):
                    best = min(solved, key=lambda l: (abs(l - j), l))
                    vols[i][j] = row[best]
                else:
                    vols[i][j] = row[j]
    elif method == "SviPerExpiry":
        for w in work:
            strikes_w = [u["strike"] for u in w["used"]]
            vols_w = [u["vol"] for u in w["used"]]
            nd = ql.nullDouble()
            sec = ql.SviInterpolatedSmileSection(
                w["date"], w["fwd"], strikes_w, False, vols_w[0], vols_w,
                nd, nd, nd, nd, nd, False, False, False, False, False, True,
                ql.EndCriteria(500, 100, 1e-12, 1e-10, 1e-10),
                ql.LevenbergMarquardt(1e-10, 1e-10, 1e-10), dc)
            p = {"a": sec.a(), "b": sec.b(), "sigma": sec.sigma(), "rho": sec.rho(), "m": sec.m()}
            k_lo, k_hi = math.log(strikes[0] / w["fwd"]), math.log(strikes[-1] / w["fwd"])
            ks = [k_lo + (k_hi - k_lo) * j / 200 for j in range(201)]
            min_g = min(_svi_durrleman_g(p, k) for k in ks)
            if min_g < 0.0 and enforce_butterfly:
                raise NotImplementedError(
                    f"{w['expiry']}: butterfly violation (min g {min_g}); the penalized refit "
                    "is engine-only")
            w["svi"] = p
            i = report_idx[w["expiry"]]
            errs = [math.sqrt(max(0.0, _svi_total_variance(p, math.log(u["strike"] / w["fwd"])))
                              / w["t"]) - u["vol"] for u in w["used"]]
            fields[f"expiries.{i}.rmse_vol"] = math.sqrt(sum(e * e for e in errs) / len(errs))
            fields[f"expiries.{i}.max_abs_error_vol"] = max(abs(e) for e in errs)
            fields[f"expiries.{i}.butterfly_min_g"] = min_g
            for name in ("a", "b", "sigma", "rho", "m"):
                fields[f"expiries.{i}.svi.{name}"] = p[name]
        for i, d in enumerate(grid_dates):
            w = next(w for w in work if w["date"] == d)
            for j, k_abs in enumerate(strikes):
                tv = max(0.0, _svi_total_variance(w["svi"], math.log(k_abs / w["fwd"])))
                vols[i][j] = math.sqrt(tv / w["t"])
    else:
        raise ValueError(f"unknown method {method}")

    # Calendar check (clip for SVI when enforced).
    clip = enforce_calendar if method == "SviPerExpiry" else False
    times = [dc.yearFraction(eval_date, d) for d in grid_dates]
    for j in range(n_str):
        prev = -1.0
        for i in range(n_exp):
            tv = vols[i][j] ** 2 * times[i]
            if i > 0 and tv < prev + 1e-8:
                if clip:
                    vols[i][j] = math.sqrt((prev + 1e-8) / times[i])
                    tv = prev + 1e-8
            prev = tv

    # Fields: forwards used, quotes used, grid, surface, the SVI slices.
    for j, w in enumerate(work):
        fields[f"forwards.{j}.time"] = w["t"]
        fields[f"forwards.{j}.forward"] = w["fwd"]
        fields[f"forwards.{j}.implied_discount_factor"] = w["df"]
        fields[f"expiries.{report_idx[w['expiry']]}.quotes_used"] = float(len(w["used"]))
        if method == "SviPerExpiry":
            stem = f"vol_surface_spec_svi.payload.slices.{j}."
            fields[stem + "forward"] = w["fwd"]
            for name in ("a", "b", "sigma", "rho", "m"):
                fields[stem + name] = w["svi"][name]
    fields["grid_strikes"] = strikes
    fields["vol_surface_spec.payload.surface_vols.values"] = \
        [vols[i][j] for i in range(n_exp) for j in range(n_str)]

    # Vanilla repricing on the returned grid as the pricers read it
    # (CubicStrikeLinearTime: the engine's SmileSliceVolSurface twin).
    surface = _SliceSurface(
        eval_date, dc, grid_dates,
        [_cubic_variance_slice(strikes, [vols[i][j] * vols[i][j] * times[i]
                                         for j in range(n_str)]) for i in range(n_exp)],
        strikes[0], strikes[-1])
    n = inside = 0
    max_p = max_v = 0.0
    for w in work:
        for u in w["used"]:
            mv = surface.blackVol(w["date"], u["strike"], True)
            mp = ql.blackFormula(ql.Option.Call if u["is_call"] else ql.Option.Put, u["strike"],
                                 w["fwd"], mv * math.sqrt(w["t"]), w["df"])
            fields[f"vanilla_repricing.quotes.{n}.model_vol"] = mv
            fields[f"vanilla_repricing.quotes.{n}.model_price"] = mp
            if u["bid"] <= mp <= u["ask"]:
                inside += 1
            max_p = max(max_p, abs(mp - u["mid"]))
            max_v = max(max_v, abs(mv - u["vol"]))
            n += 1
    fields["vanilla_repricing.count"] = float(n)
    fields["vanilla_repricing.inside_bid_ask"] = float(inside)
    fields["vanilla_repricing.max_abs_price_error"] = max_p
    fields["vanilla_repricing.max_abs_vol_error"] = max_v
    return fields


def api_npv(response: dict, list_key: str) -> float:
    """Pull the single-instrument NPV out of a pricing response.

    Mirrors the per-product extraction the legacy monolith did inline; every
    single-product endpoint returns its instrument under one list key
    (bonds/swaps/fras/cap_floors/swaptions/cds_list) with an "npv" field.
    """
    return response[list_key][0]["npv"]


# =============================================================================
# Per-request calendar holiday overrides (applied centrally to every *_ql
# reference entry point)
# =============================================================================
#
# QuantLib keeps added/removed holidays in process-global state shared by every
# instance of a calendar, so the reference applies a request's
# `calendar_overrides` to the Python-side calendars BEFORE building anything
# (schedules, helpers, indices) and restores the previous state afterwards.
# Restoration is manual and exact — each date is put back to the business-day
# status it had before the request — rather than a blanket
# resetAddedAndRemovedHolidays(), so state that predates the call survives and
# the undo does not depend on which Calendar methods the binding exposes.

_calendar_override_depth = 0


def request_calendar_overrides(request: dict) -> list:
    """The request's calendar_overrides entries (possibly empty).

    Pricing / bootstrap requests carry them inside `pricing`; the three
    calendar-utility requests carry them at the top level.
    """
    if not isinstance(request, dict):
        return []
    pricing = request.get("pricing")
    if isinstance(pricing, dict) and pricing.get("calendar_overrides"):
        return pricing["calendar_overrides"]
    return request.get("calendar_overrides") or []


@contextlib.contextmanager
def applied_calendar_overrides(request: dict):
    """Apply the request's holiday overrides for the duration of the block.

    added_holidays -> Calendar.addHoliday, removed_holidays ->
    Calendar.removeHoliday, exactly like the server. On exit (also on error)
    every touched date is restored to its previous business-day status, in
    reverse order. Re-entrant: only the outermost block applies and restores,
    so reference pricers that call each other see one consistent state.
    """
    global _calendar_override_depth
    overrides = request_calendar_overrides(request) \
        if _calendar_override_depth == 0 else []
    undo = []  # (calendar, date, was_business_day)
    _calendar_override_depth += 1
    try:
        for entry in overrides:
            calendar = _calendar_from_enum(entry["calendar"])
            for iso in entry.get("added_holidays") or []:
                d = parse_date(iso)
                undo.append((calendar, d, calendar.isBusinessDay(d)))
                calendar.addHoliday(d)
            for iso in entry.get("removed_holidays") or []:
                d = parse_date(iso)
                undo.append((calendar, d, calendar.isBusinessDay(d)))
                calendar.removeHoliday(d)
        yield
    finally:
        _calendar_override_depth -= 1
        for calendar, d, was_business_day in reversed(undo):
            if was_business_day:
                calendar.removeHoliday(d)
            else:
                calendar.addHoliday(d)


def _with_calendar_overrides(pricer):
    @functools.wraps(pricer)
    def wrapper(request, *args, **kwargs):
        with applied_calendar_overrides(request):
            return pricer(request, *args, **kwargs)
    return wrapper


# Wrap every public reference entry point once, so no product can forget to
# honour (or to undo) the overrides.
for _name, _obj in list(globals().items()):
    if _name.endswith("_ql") and not _name.startswith("_") \
            and inspect.isfunction(_obj):
        globals()[_name] = _with_calendar_overrides(_obj)
del _name, _obj
