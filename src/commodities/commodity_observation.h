#ifndef QUANTRA_COMMODITIES_OBSERVATION_H
#define QUANTRA_COMMODITIES_OBSERVATION_H

/**
 * The price observed on one pricing day, given the valuation date:
 *   day before the valuation date: the fixing (missing = invalid argument
 *     naming the day and the contract month or spot id);
 *   day on the valuation date: the fixing when given, otherwise today's price;
 *   day after the valuation date: today's price (futures: the price of the
 *     month read, missing = invalid argument naming the month; spot: the
 *     interpolated forward price).
 */

#include "commodity_futures_contract.h"
#include "commodity_spot_price.h"

#include <ql/time/date.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace quantra { namespace commodities {

enum class PriceSource {
    /// A published price on or before the valuation date.
    Fixing,
    /// Read from today's market data (futures price or spot forward price).
    Curve
};

struct Observation {
    QuantLib::Date date;
    /// Delivery month read, "YYYY-MM"; empty for a spot price.
    std::string contractMonth;
    double price = 0.0;
    PriceSource source = PriceSource::Curve;
};

/// A spot observation and the points it reads.
struct SpotObservation {
    Observation observation;
    /// Read from today's market data: the points and weights of the
    /// interpolation (the price is SpotPrice::weightedPrice of them). Empty
    /// for a fixing.
    std::vector<SpotPointWeight> weights;
};

/// Price of the month `monthIndex` of `contract` on `day`. A Curve source
/// means the price is months()[monthIndex].price.
Observation ObserveFutures(
    const FuturesContract& contract,
    std::size_t monthIndex,
    const QuantLib::Date& day,
    const QuantLib::Date& asOf);

/// Price of `spot` on `day`.
SpotObservation ObserveSpot(
    const SpotPrice& spot, const QuantLib::Date& day, const QuantLib::Date& asOf);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_OBSERVATION_H
