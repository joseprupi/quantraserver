#!/usr/bin/env python3
"""Generate a synthetic listed-option quote sheet for `/calibrate-implied-forwards`.

Everything is stated and nothing is market data: a spot S, a continuous
zero curve r(T) (linear in Act/365F time, the same interpolation the
engine's `InterpolatedZero` curve uses), a continuous dividend yield q(T)
per expiry, a Black implied-vol smile sigma(k, T) in log-moneyness
k = ln(K / F), and a strike grid. For each (expiry, strike) the script
prices the European call and put with the Black formula on the forward
F = S * exp((r - q) * T) and the discount factor DF = exp(-r * T), then
quotes them as bid/ask = mid * (1 -/+ spread / 2), so the mid is exact and
put-call parity holds exactly: the endpoint must recover F, DF and q(T) to
floating-point precision. With `--noise` the mids are perturbed
multiplicatively (uniform in [-noise, +noise], seeded) before quoting, to
exercise the filters and the least-squares averaging.

The output is a complete request (pricing block + sheet + filter), ready to
POST. The `EquityUnderlyingSpec` carries a flat 0 % placeholder dividend
curve because the schema requires one; the endpoint does not read it.

    python3 tests/tools/make_synthetic_quote_sheet.py \
        --out examples/data/implied_forwards/synthetic_ndx_like_2026-09-30.json

`build_request(SYNTHETIC_NDX_LIKE)` returns the same dict; `truth(preset)`
returns the generating forward / discount factor / yield per expiry so a
test can check the recovery against what was put in.

Stage 2 (`/calibrate-vol-surface`) reuses the generator with two more smile
kinds: `ssvi` (an SSVI power-law surface, Gatheral-Jacquier 2014, written
per expiry as raw SVI parameters: a realistic equity skew with rho = -0.7,
free of butterfly and calendar arbitrage by construction) and `flat` (one
vol everywhere). `--preset svi --endpoint vol-surface --method SviPerExpiry`
writes a complete vol-surface request; `svi_params(preset, expiry)` is the
truth the fit must recover and `smile_vol(preset, k, expiry)` the implied vol
at any log-moneyness. `--spot` / `--underlying-id` re-strike a preset for
another index (the structured-note example uses the Scotia note's levels).
"""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import json
import math
import random
from pathlib import Path

# ---------------------------------------------------------------------------
# Preset: an NDX-like index on 2026-09-30. Round, synthetic numbers.
# ---------------------------------------------------------------------------
SYNTHETIC_NDX_LIKE = {
    "as_of": "2026-09-30",
    "underlying_id": "NDX_SYN",
    "currency": "USD",
    "spot": 24000.0,
    # (date, continuous zero rate) nodes of the discounting curve; linear in
    # Act/365F time between nodes, flat beyond the last one.
    "zero_curve": [
        ("2026-09-30", 0.0400),
        ("2027-09-30", 0.0380),
        ("2029-09-30", 0.0370),
        ("2036-09-30", 0.0400),
    ],
    "discounting_curve_id": "USD_OIS_SYN",
    # (expiry, continuous dividend yield q(T)) — third Fridays.
    "expiries": [
        ("2026-10-16", 0.0080),
        ("2026-11-20", 0.0085),
        ("2026-12-18", 0.0095),
        ("2027-01-15", 0.0100),
        ("2027-03-19", 0.0105),
        ("2027-06-18", 0.0110),
        ("2027-09-17", 0.0115),
        ("2027-12-17", 0.0120),
    ],
    # Black smile in log-moneyness k = ln(K / F): sigma = atm + skew k + curv k^2,
    # clipped to [vol_floor, vol_cap].
    "smile": {"atm": 0.20, "skew": -0.40, "curv": 0.80, "vol_floor": 0.05, "vol_cap": 1.0},
    # Strike grid as multiples of spot: 0.70 S .. 1.30 S step 0.025 S (25 strikes).
    "strike_grid": {"lo": 0.70, "hi": 1.30, "step": 0.025},
    # Relative bid/ask width on every quote (ask - bid) / mid.
    "spread": 0.01,
    # Open interest and volume: 5000 * exp(-4 |k|), rounded — far strikes thin out.
    "liquidity_scale": 5000.0,
    "liquidity_decay": 4.0,
    "sheet_id": "NDX_SYN_2026-09-30",
    "dividend_curve_id": "NDX_SYN_DIV_IMPLIED",
    "filter": {
        "max_relative_spread": 0.05,
        "min_quotes_per_expiry": 4,
        "moneyness_window": 0.25,
    },
    "noise": 0.0,
    "seed": 0,
}


# SSVI power-law surface (Gatheral-Jacquier 2014): total variance
#   w(k, theta) = theta / 2 * (1 + rho phi k + sqrt((phi k + rho)^2 + 1 - rho^2)),
#   theta = atm_vol^2 T,  phi(theta) = eta / theta^gamma,
# which is the raw SVI slice a = theta (1 - rho^2) / 2, b = theta phi / 2,
# sigma = sqrt(1 - rho^2) / phi, m = -rho / phi, rho = rho. With eta = 1,
# gamma = 1/2 the ATM vol skew is rho / (4 sqrt(T)) and the surface is free
# of calendar arbitrage (theta increasing in T) and of butterfly arbitrage
# (theta phi (1 + |rho|) < 4 and theta phi^2 (1 + |rho|) <= 4 at every expiry).
SYNTHETIC_SVI_NDX_LIKE = copy.deepcopy(SYNTHETIC_NDX_LIKE)
SYNTHETIC_SVI_NDX_LIKE.update({
    "sheet_id": "NDX_SYN_SVI_2026-09-30",
    "smile": {
        "type": "ssvi", "rho": -0.70, "eta": 1.0, "gamma": 0.5,
        # ATM vol per expiry (same order as "expiries"); mildly downward.
        "atm_vols": [0.230, 0.225, 0.220, 0.215, 0.210, 0.210, 0.210, 0.210],
    },
    "vol_surface_id": "NDX_SYN_VOL",
})

# RTY-like: higher level, slightly less skew; used re-struck at the Scotia
# note's RTY level for the structured-note example.
SYNTHETIC_SVI_RTY_LIKE = copy.deepcopy(SYNTHETIC_SVI_NDX_LIKE)
SYNTHETIC_SVI_RTY_LIKE.update({
    "underlying_id": "RTY_SYN",
    "spot": 2800.0,
    "sheet_id": "RTY_SYN_SVI_2026-09-30",
    "dividend_curve_id": "RTY_SYN_DIV_IMPLIED",
    "vol_surface_id": "RTY_SYN_VOL",
    "expiries": [(d, q + 0.004) for d, q in SYNTHETIC_NDX_LIKE["expiries"]],
    "smile": {
        "type": "ssvi", "rho": -0.65, "eta": 1.0, "gamma": 0.5,
        "atm_vols": [0.270, 0.265, 0.260, 0.255, 0.250, 0.248, 0.246, 0.245],
    },
})

# Flat 20 % everywhere: both methods must return 0.20 on the whole grid.
SYNTHETIC_FLAT_20 = copy.deepcopy(SYNTHETIC_NDX_LIKE)
SYNTHETIC_FLAT_20.update({
    "sheet_id": "NDX_SYN_FLAT20_2026-09-30",
    "smile": {"type": "flat", "vol": 0.20},
    "vol_surface_id": "NDX_SYN_FLAT20_VOL",
})

PRESETS = {
    "ndx_like": SYNTHETIC_NDX_LIKE,
    "svi": SYNTHETIC_SVI_NDX_LIKE,
    "svi_rty": SYNTHETIC_SVI_RTY_LIKE,
    "flat": SYNTHETIC_FLAT_20,
}


# ---------------------------------------------------------------------------
# Black-Scholes (forward form) with the standard normal CDF via erf.
# ---------------------------------------------------------------------------
def norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def black_call_put(forward: float, strike: float, sigma: float, t: float, df: float):
    """Undiscounted-forward Black formula; returns (call, put) discounted by df."""
    sd = sigma * math.sqrt(t)
    d1 = (math.log(forward / strike) + 0.5 * sd * sd) / sd
    d2 = d1 - sd
    call = df * (forward * norm_cdf(d1) - strike * norm_cdf(d2))
    put = df * (strike * norm_cdf(-d2) - forward * norm_cdf(-d1))
    return call, put


def year_fraction(as_of: str, date: str) -> float:
    """Act/365F."""
    a = dt.date.fromisoformat(as_of)
    d = dt.date.fromisoformat(date)
    return (d - a).days / 365.0


def zero_rate(preset: dict, date: str) -> float:
    """Linear interpolation of the continuous zero rate in Act/365F time
    (flat beyond the last node), exactly like InterpolatedZero + Linear."""
    as_of = preset["as_of"]
    nodes = [(year_fraction(as_of, d), r) for d, r in preset["zero_curve"]]
    t = year_fraction(as_of, date)
    if t <= nodes[0][0]:
        return nodes[0][1]
    for (t0, r0), (t1, r1) in zip(nodes, nodes[1:]):
        if t <= t1:
            return r0 + (r1 - r0) * (t - t0) / (t1 - t0)
    return nodes[-1][1]


def _expiry_index(preset: dict, expiry: str) -> int:
    for i, (d, _q) in enumerate(preset["expiries"]):
        if d == expiry:
            return i
    raise KeyError(expiry)


def svi_params(preset: dict, expiry: str):
    """Raw SVI (a, b, sigma, rho, m) of the generating slice at `expiry` for
    an `ssvi` smile; None for the other smile kinds."""
    s = preset["smile"]
    if s.get("type", "poly") != "ssvi":
        return None
    t = year_fraction(preset["as_of"], expiry)
    theta = s["atm_vols"][_expiry_index(preset, expiry)] ** 2 * t
    phi = s["eta"] / theta ** s["gamma"]
    rho = s["rho"]
    return {
        "a": theta * (1.0 - rho * rho) / 2.0,
        "b": theta * phi / 2.0,
        "sigma": math.sqrt(1.0 - rho * rho) / phi,
        "rho": rho,
        "m": -rho / phi,
    }


def svi_total_variance(p: dict, k: float) -> float:
    d = k - p["m"]
    return p["a"] + p["b"] * (p["rho"] * d + math.sqrt(d * d + p["sigma"] * p["sigma"]))


def smile_vol(preset: dict, k: float, expiry: str = None) -> float:
    """Black implied vol at log-moneyness k = ln(K / F). The polynomial
    smile does not depend on the expiry; `ssvi` and `flat` take it."""
    s = preset["smile"]
    kind = s.get("type", "poly")
    if kind == "flat":
        return s["vol"]
    if kind == "ssvi":
        t = year_fraction(preset["as_of"], expiry)
        return math.sqrt(svi_total_variance(svi_params(preset, expiry), k) / t)
    v = s["atm"] + s["skew"] * k + s["curv"] * k * k
    return min(max(v, s["vol_floor"]), s["vol_cap"])


def strikes(preset: dict) -> list:
    g = preset["strike_grid"]
    n = int(round((g["hi"] - g["lo"]) / g["step"])) + 1
    return [round(preset["spot"] * (g["lo"] + i * g["step"]), 6) for i in range(n)]


def truth(preset: dict) -> dict:
    """{expiry: {"time", "rate", "forward", "discount_factor", "dividend_yield"}}."""
    out = {}
    for expiry, q in preset["expiries"]:
        t = year_fraction(preset["as_of"], expiry)
        r = zero_rate(preset, expiry)
        out[expiry] = {
            "time": t,
            "rate": r,
            "forward": preset["spot"] * math.exp((r - q) * t),
            "discount_factor": math.exp(-r * t),
            "dividend_yield": q,
        }
    return out


def quote_lines(preset: dict) -> list:
    rng = random.Random(preset.get("seed", 0))
    noise = float(preset.get("noise", 0.0))
    half = preset["spread"] / 2.0
    lines = []
    tr = truth(preset)
    for expiry, _q in preset["expiries"]:
        tv = tr[expiry]
        f, df, t = tv["forward"], tv["discount_factor"], tv["time"]
        for k_abs in strikes(preset):
            k = math.log(k_abs / f)
            call, put = black_call_put(f, k_abs, smile_vol(preset, k, expiry), t, df)
            if noise > 0.0:
                call *= 1.0 + rng.uniform(-noise, noise)
                put *= 1.0 + rng.uniform(-noise, noise)
            liq = round(preset["liquidity_scale"] * math.exp(-preset["liquidity_decay"] * abs(k)))
            lines.append({
                "expiry": expiry,
                "strike": k_abs,
                "call_bid": call * (1.0 - half),
                "call_ask": call * (1.0 + half),
                "put_bid": put * (1.0 - half),
                "put_ask": put * (1.0 + half),
                "call_volume": float(liq // 2),
                "put_volume": float(liq // 2),
                "call_open_interest": float(liq),
                "put_open_interest": float(liq),
            })
    return lines


def zero_point(date: str, rate: float) -> dict:
    return {
        "point_type": "ZeroRatePoint",
        "point": {
            "date": date,
            "zero_rate": rate,
            "compounding": "Continuous",
            "frequency": "Annual",
        },
    }


def build_request(preset: dict = SYNTHETIC_NDX_LIKE) -> dict:
    p = copy.deepcopy(preset)
    uid = p["underlying_id"]
    spot_id = uid + "_SPOT"
    placeholder = uid + "_DIV_PLACEHOLDER"
    last = p["zero_curve"][-1][0]
    return {
        "pricing": {
            "as_of_date": p["as_of"],
            "quotes": [
                {"id": spot_id, "kind": "Price", "value": p["spot"], "quote_type": "Curve"},
            ],
            "rates": {
                "curves": [
                    {
                        "id": p["discounting_curve_id"],
                        "day_counter": "Actual365Fixed",
                        "interpolator": "Linear",
                        "reference_date": p["as_of"],
                        "bootstrap_trait": "InterpolatedZero",
                        "points": [zero_point(d, r) for d, r in p["zero_curve"]],
                    },
                    {
                        "id": placeholder,
                        "day_counter": "Actual365Fixed",
                        "interpolator": "Linear",
                        "reference_date": p["as_of"],
                        "bootstrap_trait": "InterpolatedZero",
                        "points": [zero_point(p["as_of"], 0.0), zero_point(last, 0.0)],
                    },
                ]
            },
            "equity": {
                "equity_underlyings": [
                    {
                        "id": uid,
                        "currency": p["currency"],
                        "spot_quote_id": spot_id,
                        "dividend_yield_curve_id": placeholder,
                    }
                ]
            },
        },
        "sheet": {
            "id": p["sheet_id"],
            "underlying_id": uid,
            "option_style": "European",
            "quotes": quote_lines(p),
        },
        "discounting_curve": p["discounting_curve_id"],
        "filter": dict(p["filter"]),
        "dividend_curve_id": p["dividend_curve_id"],
    }


def build_vol_surface_request(preset: dict = SYNTHETIC_SVI_NDX_LIKE,
                              method: str = "AndreasenHuge",
                              forwards: list = None) -> dict:
    """A complete `/calibrate-vol-surface` request on the preset's sheet.
    `forwards` (the `expiries` array of an implied-forwards response) is
    pasted when given; otherwise the endpoint fits the forwards itself."""
    base = build_request(preset)
    req = {
        "pricing": base["pricing"],
        "sheet": base["sheet"],
        "discounting_curve": base["discounting_curve"],
        "filter": base["filter"],
        "method": method,
        "vol_surface_id": preset.get("vol_surface_id", preset["underlying_id"] + "_VOL"),
    }
    if forwards is not None:
        req["forwards"] = forwards
    return req


def restrike(preset: dict, spot: float = None, underlying_id: str = None) -> dict:
    """Copy of the preset at another spot / underlying id (ids derived from
    the underlying id: <id>_SPOT, <id>_DIV_PLACEHOLDER, <id>_VOL, sheet
    <id>_<kind>_<as_of>)."""
    p = copy.deepcopy(preset)
    if spot is not None:
        p["spot"] = float(spot)
    if underlying_id is not None:
        old = p["underlying_id"]
        p["underlying_id"] = underlying_id
        p["sheet_id"] = p["sheet_id"].replace(old, underlying_id, 1)
        p["dividend_curve_id"] = p["dividend_curve_id"].replace(old, underlying_id, 1)
        if "vol_surface_id" in p:
            p["vol_surface_id"] = p["vol_surface_id"].replace(old, underlying_id, 1)
    return p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, help="write the request JSON here (default: stdout)")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="ndx_like",
                    help="ndx_like (polynomial smile, stage 1), svi / svi_rty (SSVI "
                         "surface), flat (20 %% everywhere)")
    ap.add_argument("--endpoint", choices=["implied-forwards", "vol-surface"],
                    default="implied-forwards", help="which request to write")
    ap.add_argument("--method", choices=["AndreasenHuge", "SviPerExpiry"],
                    default="AndreasenHuge", help="vol-surface fit method")
    ap.add_argument("--spot", type=float, default=None, help="re-strike the preset at this spot")
    ap.add_argument("--underlying-id", default=None, help="rename the underlying (ids follow)")
    ap.add_argument("--noise", type=float, default=0.0,
                    help="multiplicative price noise, uniform in [-noise, +noise] (default 0)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--spread", type=float, default=None,
                    help="relative bid/ask width (default: the preset's 0.01)")
    ap.add_argument("--truth", action="store_true",
                    help="print the generating forward / DF / yield per expiry instead")
    args = ap.parse_args()

    preset = restrike(PRESETS[args.preset], args.spot, args.underlying_id)
    preset["noise"] = args.noise
    preset["seed"] = args.seed
    if args.spread is not None:
        preset["spread"] = args.spread
    if args.truth:
        out = truth(preset)
        for expiry in out:
            out[expiry]["svi"] = svi_params(preset, expiry)
        print(json.dumps(out, indent=2))
        return 0
    if args.endpoint == "vol-surface":
        request = build_vol_surface_request(preset, args.method)
    else:
        request = build_request(preset)
    text = json.dumps(request, indent=2) + "\n"
    if args.out:
        args.out.write_text(text)
        print(f"wrote {args.out}")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
