"""Price a book of fixed-rate bonds over several markets with quantra_client.batch.

Each market is the synthetic bond curve of price_bonds.py with its deposit
rates moved by a few basis points; each market prices its own list of bonds
(different coupons). One request per market, a large market split across
the calls in flight, results read back per bond in input order.

    python price_batch.py --target localhost:50051 --gateway http://localhost:8080 \\
        --markets 20 --bonds 500
"""

import argparse
import copy
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(_REPO_ROOT / "flatbuffers" / "python"))
sys.path.insert(0, str(_REPO_ROOT / "clients" / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from price_bonds import create_market_data, create_target_bond  # noqa: E402
from quantra_client import Client  # noqa: E402
from quantra_client.batch import price_batch, workers_from_meta  # noqa: E402
from quantra.Point import Point  # noqa: E402
from quantra.Pricing import PricingT  # noqa: E402
from quantra.RatesMarketData import RatesMarketDataT  # noqa: E402
from quantra.PriceFixedRateBond import PriceFixedRateBondT  # noqa: E402
from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT  # noqa: E402
from quantra.Yield import YieldT  # noqa: E402
from quantra.enums.Compounding import Compounding  # noqa: E402
from quantra.enums.DayCounter import DayCounter  # noqa: E402
from quantra.enums.Frequency import Frequency  # noqa: E402


def market_request(shift, bonds):
    """One market (the bond curve, deposits moved by `shift`) and its bonds."""
    curve = create_market_data("depos")
    for wrapper in curve.points:
        if wrapper.pointType == Point.DepositHelper:
            wrapper.point.rate += shift
    request = PriceFixedRateBondRequestT()
    request.pricing = PricingT()
    request.pricing.asOfDate = "2008-09-18"
    request.pricing.settlementDate = "2008-09-18"
    request.pricing.rates = RatesMarketDataT()
    request.pricing.rates.curves = [curve]
    yield_def = YieldT()
    yield_def.dayCounter = DayCounter.Actual360
    yield_def.compounding = Compounding.Compounded
    yield_def.frequency = Frequency.Annual
    request.bonds = []
    for i in range(bonds):
        bond = create_target_bond()
        bond.rate = 0.02 + 0.0001 * (i % 300)
        trade = PriceFixedRateBondT()
        trade.fixedRateBond = bond
        trade.discountingCurve = "depos"
        trade.yield_ = yield_def
        request.bonds.append(trade)
    return request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", default="localhost:50051", help="engine gRPC address")
    parser.add_argument("--gateway", default=None,
                        help="JSON gateway URL, to read the worker count from GET /meta")
    parser.add_argument("--markets", type=int, default=20)
    parser.add_argument("--bonds", type=int, default=500, help="bonds per market")
    args = parser.parse_args()

    in_flight = (workers_from_meta(args.gateway) if args.gateway else None) or 8
    # The last market holds most of the book, so it is split across calls.
    sizes = [args.bonds] * (args.markets - 1) + [args.bonds * args.markets]
    groups = [("fixed_rate_bond", market_request(0.0001 * m, n)) for m, n in enumerate(sizes)]

    with Client(args.target) as client:
        result = price_batch(client, groups, in_flight=in_flight)
    print(result)

    for group in result.groups:
        for start, end, error in group.failures:
            print(f"  trades {start}..{end - 1} failed: {error}")
    last = result.groups[-1]
    for i in (0, len(last) - 1):
        if last.error(i) is None:
            print(f"last market, bond {i}: NPV {last.entry(i).Npv():.6f}")


if __name__ == "__main__":
    main()
