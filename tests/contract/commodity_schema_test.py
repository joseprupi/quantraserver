"""Commodity swap schemas round-trip: JSON -> FlatBuffers binary -> read back.

Schema-only coverage (no endpoint yet): the example request under
tests/contract/data/commodity is compiled to a binary with `flatc` against
flatbuffers/fbs/price_commodity_swap_request.fbs and every field is read
back through the generated Python table accessors. A second case does the
same for the response schema.
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FBS_DIR = ROOT / "flatbuffers" / "fbs"
sys.path.insert(0, str(ROOT / "flatbuffers" / "python"))

from quantra.CommodityFixedPrice import CommodityFixedPrice  # noqa: E402
from quantra.CommodityLegPrice import CommodityLegPrice  # noqa: E402
from quantra.FuturesFloatingPrice import FuturesFloatingPrice  # noqa: E402
from quantra.PriceCommoditySwapRequest import PriceCommoditySwapRequest  # noqa: E402
from quantra.PriceCommoditySwapResponse import PriceCommoditySwapResponse  # noqa: E402
from quantra.SpotFloatingPrice import SpotFloatingPrice  # noqa: E402
from quantra.enums.CommodityPriceSource import CommodityPriceSource  # noqa: E402
from quantra.enums.Calendar import Calendar  # noqa: E402
from quantra.enums.CommodityQuantityBasis import CommodityQuantityBasis  # noqa: E402
from quantra.enums.Frequency import Frequency  # noqa: E402
from quantra.enums.Interpolator import Interpolator  # noqa: E402
from quantra.enums.PayReceive import PayReceive  # noqa: E402


DATA = Path(__file__).resolve().parent / "data" / "commodity"


def _flatc():
    exe = shutil.which("flatc") or "/opt/quantra-deps/bin/flatc"
    assert Path(exe).exists(), "flatc not found (run the suite inside the test image)"
    return exe


def _compile(schema, json_path, out_dir):
    subprocess.run(
        [_flatc(), "--binary", "-I", str(FBS_DIR), "-o", str(out_dir), str(FBS_DIR / schema), str(json_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return (Path(out_dir) / (Path(json_path).stem + ".bin")).read_bytes()


def _s(b):
    return b.decode() if isinstance(b, bytes) else b


def _items(table, name):
    """Elements of the vector field `name` (accessor `name(j)` / `nameLength()`)."""
    return [getattr(table, name)(j) for j in range(getattr(table, name + "Length")())]


def _union(leg, cls):
    """The leg's price union member read as `cls`."""
    member = cls()
    member.Init(leg.Price().Bytes, leg.Price().Pos)
    return member


def test_price_commodity_swap_request_round_trip(tmp_path):
    buf = _compile("price_commodity_swap_request.fbs", DATA / "price_commodity_swap_schema_request.json", tmp_path)
    req = PriceCommoditySwapRequest.GetRootAs(buf, 0)

    assert req.IncludeFlows() is True
    assert req.IncludeObservations() is True
    pricing = req.Pricing()
    assert _s(pricing.AsOfDate()) == "2026-11-13"
    assert _s(pricing.Rates().Curves(0).Id()) == "USD_FLAT"

    # market data
    market = pricing.Commodity()
    contracts, spots = _items(market, "Contracts"), _items(market, "SpotPrices")
    assert len(contracts) == 1 and len(spots) == 1
    cl = contracts[0]
    assert _s(cl.Id()) == "NYMEX_CL"
    assert cl.PricingCalendar() == Calendar.UnitedStatesNYSE
    assert [_s(cl.PricingHolidays(j)) for j in range(cl.PricingHolidaysLength())] == ["2026-12-24"]
    assert [(_s(m.Month()), _s(m.LastTradeDate()), m.Price()) for m in _items(cl, "Months")] == [
        ("2026-12", "2026-11-20", 60.00),
        ("2027-01", "2026-12-17", 60.20),
    ]
    assert [(_s(f.Month()), _s(f.Date()), f.Value()) for f in _items(cl, "Fixings")] == [
        ("2026-12", "2026-11-12", 59.90)
    ]

    spot = spots[0]
    assert _s(spot.Id()) == "SPOT_X"
    assert spot.PricingCalendar() == Calendar.UnitedStates
    assert spot.Interpolation() == Interpolator.Linear
    assert [_s(spot.PricingHolidays(j)) for j in range(spot.PricingHolidaysLength())] == ["2026-12-24"]
    assert spot.ExtrapolateFlat() is True
    assert [(_s(p.Date()), p.Price()) for p in _items(spot, "Points")] == [("2026-11-13", 3.10), ("2027-10-29", 3.50)]
    assert [(_s(f.Date()), f.Value()) for f in _items(spot, "Fixings")] == [("2026-11-12", 3.05)]

    # swap 1: fixed vs futures floating
    swaps = [wrapped.CommoditySwap() for wrapped in _items(req, "Swaps")]
    assert len(swaps) == 2
    fixed, floating = swaps[0].Leg1(), swaps[0].Leg2()
    assert fixed.PayReceive() == PayReceive.Pay and floating.PayReceive() == PayReceive.Receive
    for leg in (fixed, floating):
        periods = leg.Periods()
        assert _s(periods.StartDate()) == "2026-11-01"
        assert _s(periods.EndDate()) == "2027-10-31"
        assert periods.Frequency() == Frequency.Monthly
        assert periods.PeriodsLength() == 0
        quantity = leg.Quantity()
        assert quantity.Value() == 10000.0
        assert quantity.ValuesLength() == 0
        assert quantity.Basis() == CommodityQuantityBasis.PerPeriod
        assert leg.PaymentLag() == 5
        assert leg.PaymentCalendar() is not None and leg.PaymentConvention() is not None
        assert _s(leg.DiscountingCurve()) == "USD_FLAT"
    assert fixed.PriceType() == CommodityLegPrice.CommodityFixedPrice
    fixed_price = _union(fixed, CommodityFixedPrice)
    assert fixed_price.Price() == 61.00 and fixed_price.PricesLength() == 0
    assert floating.PriceType() == CommodityLegPrice.FuturesFloatingPrice
    futures = _union(floating, FuturesFloatingPrice)
    assert _s(futures.ContractId()) == "NYMEX_CL"
    assert futures.Nearby() == 1
    assert futures.RollDaysBeforeExpiry() == 0
    assert futures.Spread() == 0.25

    # swap 2: spot floating vs fixed per-period prices, per-pricing-day quantities
    spot_leg, fixed_leg = swaps[1].Leg1(), swaps[1].Leg2()
    assert spot_leg.PriceType() == CommodityLegPrice.SpotFloatingPrice
    spot_price = _union(spot_leg, SpotFloatingPrice)
    assert _s(spot_price.SpotId()) == "SPOT_X"
    assert spot_price.Spread() == 0
    assert fixed_leg.PriceType() == CommodityLegPrice.CommodityFixedPrice
    per_period = _union(fixed_leg, CommodityFixedPrice)
    assert per_period.Price() is None
    assert [per_period.Prices(j) for j in range(per_period.PricesLength())] == [3.20, 3.30, 3.40]
    for leg in (spot_leg, fixed_leg):
        quantity = leg.Quantity()
        assert quantity.Value() is None
        assert [quantity.Values(j) for j in range(quantity.ValuesLength())] == [500.0, 600.0, 700.0]
        assert quantity.Basis() == CommodityQuantityBasis.PerPricingDay
        assert leg.PaymentLag() == 2
        periods = leg.Periods()
        assert periods.StartDate() is None and periods.EndDate() is None and periods.Frequency() is None
        assert [(_s(p.StartDate()), _s(p.EndDate())) for p in _items(periods, "Periods")] == [
            ("2026-11-01", "2026-11-30"),
            ("2026-12-01", "2026-12-31"),
            ("2027-01-01", "2027-01-31"),
        ]


def test_price_commodity_swap_response_round_trip(tmp_path):
    response = {
        "swaps": [
            {
                "npv": 123.5,
                "fair_fixed_price": 60.4,
                "legs": [
                    {
                        "npv": -1.0,
                        "flows": [
                            {
                                "start_date": "2026-11-01",
                                "end_date": "2026-11-30",
                                "payment_date": "2026-12-07",
                                "pricing_days": 20,
                                "realised_days": 8,
                                "average": 60.1,
                                "realised_average": 59.9,
                                "expected_average": 60.2,
                                "spread": 0.25,
                                "quantity": 10000.0,
                                "amount": 601000.0,
                                "discount": 0.99,
                                "present_value": 594990.0,
                                "observations": [
                                    {"date": "2026-11-12", "contract_month": "2026-12", "price": 59.9, "source": "Fixing"},
                                    {"date": "2026-11-16", "price": 60.0, "source": "Curve"},
                                ],
                            }
                        ],
                    }
                ],
                "deltas": [
                    {"contract_id": "NYMEX_CL", "month": "2026-12", "delta": 7000.0},
                    {"spot_id": "SPOT_X", "date": "2026-11-16", "delta": 12.5},
                ],
            }
        ]
    }
    json_path = tmp_path / "price_commodity_swap_schema_response.json"
    json_path.write_text(json.dumps(response))
    buf = _compile("commodity_swap_response.fbs", json_path, tmp_path)
    resp = PriceCommoditySwapResponse.GetRootAs(buf, 0)

    swap = _items(resp, "Swaps")[0]
    assert swap.Npv() == 123.5 and swap.FairFixedPrice() == 60.4 and swap.FairSpread() is None
    leg = _items(swap, "Legs")[0]
    assert leg.Npv() == -1.0
    flow = _items(leg, "Flows")[0]
    assert (_s(flow.StartDate()), _s(flow.EndDate()), _s(flow.PaymentDate())) == ("2026-11-01", "2026-11-30", "2026-12-07")
    assert (flow.PricingDays(), flow.RealisedDays()) == (20, 8)
    assert (flow.Average(), flow.RealisedAverage(), flow.ExpectedAverage()) == (60.1, 59.9, 60.2)
    assert flow.Spread() == 0.25 and flow.Price() is None
    assert (flow.Quantity(), flow.Amount(), flow.Discount(), flow.PresentValue()) == (
        10000.0, 601000.0, 0.99, 594990.0)
    first, second = _items(flow, "Observations")
    assert (_s(first.Date()), _s(first.ContractMonth()), first.Price(), first.Source()) == (
        "2026-11-12", "2026-12", 59.9, CommodityPriceSource.Fixing)
    assert second.ContractMonth() is None and second.Source() == CommodityPriceSource.Curve
    futures_delta, spot_delta = _items(swap, "Deltas")
    assert (_s(futures_delta.ContractId()), _s(futures_delta.Month()), futures_delta.Delta()) == (
        "NYMEX_CL", "2026-12", 7000.0)
    assert (_s(spot_delta.SpotId()), _s(spot_delta.Date()), spot_delta.Delta()) == ("SPOT_X", "2026-11-16", 12.5)
    assert spot_delta.ContractId() is None
