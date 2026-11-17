#ifndef QUANTRA_COMMODITY_PARSERS_H
#define QUANTRA_COMMODITY_PARSERS_H

/**
 * Commodity market data: `pricing.commodity` (wire) -> futures contracts and
 * spot prices by id (plain domain objects of src/commodities).
 *
 * Who checks what. This decoder checks only what the wire alone can tell,
 * and a violation is a 400 naming the field
 * (`pricing.commodity.contracts[0] ('SYN_CL').fixings[2].value is required`):
 *   - presence: ids, months, month strings, last trading dates, fixing
 *     dates and values, spot point dates and prices, `months`, `points`,
 *     `pricing_calendar`, spot `interpolation`;
 *   - dates parse as YYYY-MM-DD (requireIsoDate);
 *   - enums are known values: `pricing_calendar` goes through the same
 *     factory every product uses, so `pricing.calendar_overrides` (already
 *     applied to the process calendars) hold; spot `interpolation` is
 *     Linear, BackwardFlat or ForwardFlat (LogLinear / LogCubic refused:
 *     prices can be zero or negative);
 *   - ids unique within contracts / within spot prices.
 * Every rule about the values belongs to the domain objects' constructors
 * (FuturesContract, SpotPrice, PricingCalendar), also 400s: at least one
 * month / point, month format (IsYearMonth), months and last trading dates
 * strictly ascending, point dates ascending, no fixing given twice, finite
 * prices and fixing values. `pricing_holidays` become the object's own
 * holidays and change no other use of the calendar.
 */

#include "commodity_futures_contract.h"
#include "commodity_spot_price.h"

#include "commodity_generated.h"

#include <map>
#include <memory>
#include <string>

namespace quantra {

/// Commodity market data of one request, by id.
struct CommodityRegistry {
    std::map<std::string, std::shared_ptr<const commodities::FuturesContract>> contracts;
    std::map<std::string, std::shared_ptr<const commodities::SpotPrice>> spotPrices;

    /// The contract with the given id; QuantraNotFound (404) naming `field`
    /// (the request field that holds the id) when absent.
    std::shared_ptr<const commodities::FuturesContract> contract(
        const std::string& id, const std::string& field) const;
    /// The spot price with the given id; QuantraNotFound (404) naming
    /// `field` when absent.
    std::shared_ptr<const commodities::SpotPrice> spotPrice(
        const std::string& id, const std::string& field) const;
};

/// Parse `pricing.commodity`; a null block yields an empty registry.
CommodityRegistry BuildCommodityRegistry(const quantra::CommodityMarketData* commodity);

} // namespace quantra

#endif // QUANTRA_COMMODITY_PARSERS_H
