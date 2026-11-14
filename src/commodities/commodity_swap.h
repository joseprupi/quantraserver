#ifndef QUANTRA_COMMODITIES_SWAP_H
#define QUANTRA_COMMODITIES_SWAP_H

/**
 * Commodity swap: two legs, one paid and one received, each a fixed price or
 * the average over each period's pricing days of a futures contract or of a
 * spot price, plus a spread.
 *
 * Per period of a leg:
 *   quantity  = the period's quantity; with basis PerPricingDay times the
 *               period's number of pricing days;
 *   price     = the fixed price, or the average of the day prices plus the
 *               spread;
 *   amount    = sign * quantity * price, sign -1 for Pay and +1 for Receive;
 *   payment   = payment calendar advance(period end, lag business days,
 *               convention);
 *   discount  = discount curve at the payment date;
 *   PV        = amount * discount.
 * A period paid on or before the valuation date is left out (no flow, no
 * pricing days counted, no prices read), as for the engine's rate swaps: a
 * flow on the valuation date itself counts as already paid.
 * Leg NPV = sum of its PVs; swap NPV = leg 1 NPV + leg 2 NPV.
 *
 * A fixed leg with basis PerPricingDay counts the pricing days of the
 * floating leg's period with the same first and last day (the fixed leg has
 * no pricing calendar of its own); invalid argument when the other leg is not
 * floating, or when a period still to be paid has no such floating period.
 *
 * Error messages. Every invalid input is an invalid argument. A message about
 * a field of the swap starts with its path relative to the swap, in wire
 * spelling (`leg1.quantity.values has 3 entries for 12 periods`,
 * `leg2.periods[4] (2027-03-01 to 2027-03-31) has no pricing day`); a message
 * about market data read while pricing starts with the contract or spot price
 * it names (`commodity contract 'X': no fixing of 2026-12 on pricing day
 * 2026-11-05 ...`); a message about a result names the result (`npv is not
 * finite ...`). QualifySwapMessage puts the caller's name of the swap in
 * front of any of them.
 */

#include "commodity_futures_contract.h"
#include "commodity_observation.h"
#include "commodity_periods.h"
#include "commodity_spot_price.h"

#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace quantra { namespace commodities {

enum class PayReceive { Pay, Receive };

enum class QuantityBasis { PerPeriod, PerPricingDay };

struct CommodityQuantity {
    /// One quantity for every period. Exactly one of `value` and `values`.
    std::optional<double> value;
    /// One quantity per period, in period order.
    std::vector<double> values;
    QuantityBasis basis = QuantityBasis::PerPeriod;
};

struct FixedPrice {
    /// One price for every period. Exactly one of `price` and `prices`.
    std::optional<double> price;
    /// One price per period, in period order.
    std::vector<double> prices;
};

struct FuturesFloatingPrice {
    std::shared_ptr<const FuturesContract> contract;
    /// 1 = front month.
    int nearby = 1;
    int rollDaysBeforeExpiry = 0;
    double spread = 0.0;
};

struct SpotFloatingPrice {
    std::shared_ptr<const SpotPrice> spot;
    double spread = 0.0;
};

using LegPrice = std::variant<FixedPrice, FuturesFloatingPrice, SpotFloatingPrice>;

struct CommoditySwapLeg {
    PayReceive payReceive = PayReceive::Pay;
    std::vector<CommodityPeriod> periods;
    CommodityQuantity quantity;
    LegPrice price;
    QuantLib::Calendar paymentCalendar;
    int paymentLag = 0;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::Following;
    QuantLib::Handle<QuantLib::YieldTermStructure> discountCurve;
};

struct CommodityFlow {
    QuantLib::Date start;
    QuantLib::Date end;
    QuantLib::Date paymentDate;
    /// Pricing days of the period (floating leg, or a fixed leg quantity per
    /// pricing day); 0 otherwise.
    int pricingDays = 0;
    /// Pricing days whose price is a fixing.
    int realisedDays = 0;
    /// Floating leg: average over all pricing days, before the spread.
    std::optional<double> average;
    /// Average over the fixing days; absent when there are none.
    std::optional<double> realisedAverage;
    /// Average over the days read from today's market data; absent when
    /// there are none.
    std::optional<double> expectedAverage;
    /// Floating leg: the leg's spread.
    std::optional<double> spread;
    /// Fixed leg: the period's fixed price.
    std::optional<double> price;
    /// The period's quantity after the basis is applied.
    double quantity = 0.0;
    double amount = 0.0;
    double discount = 0.0;
    double presentValue = 0.0;
    /// One per pricing day (floating legs).
    std::vector<Observation> observations;
};

struct CommodityLegResult {
    double npv = 0.0;
    std::vector<CommodityFlow> flows;
};

struct CommodityPriceDelta {
    enum class Kind { Futures, Spot };
    Kind kind = Kind::Futures;
    /// Futures: contract id and delivery month.
    std::string contractId;
    std::string month;
    /// Spot: spot id and the date of the forward point.
    std::string spotId;
    QuantLib::Date date;
    /// Change of the swap NPV per unit change of this price.
    double delta = 0.0;
};

struct CommoditySwapResult {
    double npv = 0.0;
    /// Fixed leg against floating leg only: the single fixed price that
    /// zeroes the NPV, the floating leg held.
    std::optional<double> fairFixedPrice;
    /// Floating leg against floating leg only: the spread on leg 2 that
    /// zeroes the NPV.
    std::optional<double> fairSpread;
    std::array<CommodityLegResult, 2> legs;
    /// Sensitivity to every price read from today's market data (fixings
    /// carry none), aggregated over both legs: futures by contract and month
    /// in month order, then spot prices by id and point date.
    std::vector<CommodityPriceDelta> deltas;
};

/// Price a commodity swap as of `asOf`. This is the one place the swap's
/// values are checked; invalid argument on any bad input: legs not opposite,
/// periods invalid (ValidatePeriods), per-period counts not equal to the
/// number of periods, a quantity or fixed price form not exactly one of
/// single / per period, a negative or non-finite quantity, a non-finite price
/// or spread, a negative payment lag, a missing payment calendar, discount
/// curve, contract or spot price, nearby < 1, roll < 0, a fixed leg with
/// quantity per pricing day against a fixed leg, a period still to be paid
/// without pricing days, the market-data errors of ObserveFutures /
/// ObserveSpot / FuturesContract::selectMonth, and a result that is not
/// finite (NPV, flow amount or present value, fair value or delta: the
/// numbers overflowed).
CommoditySwapResult PriceCommoditySwap(
    const CommoditySwapLeg& leg1, const CommoditySwapLeg& leg2, const QuantLib::Date& asOf);

/// An error message of PriceCommoditySwap with the caller's name of the swap
/// in front: `swap` + "." + message for a message starting with a leg path,
/// `swap` + ": " + message otherwise.
std::string QualifySwapMessage(const std::string& swap, const std::string& message);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_SWAP_H
