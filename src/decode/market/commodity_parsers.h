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
#include <vector>

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

/// One `CommodityContractSpec` decoded (presence, dates, calendar) and not
/// yet validated: the FuturesContract constructor owns the value rules.
struct CommodityContractData {
    std::string id;
    commodities::PricingCalendar calendar;
    std::vector<commodities::ContractMonth> months;
    std::vector<commodities::ContractFixing> fixings;
};

/// Decode one contract; `base` is its path in the request
/// (`pricing.commodity.contracts[0]`, `curves[1].contract`), messages name
/// `<base> ('<id>').<field>`. Used by the registry and by the commodity
/// curve build.
CommodityContractData DecodeCommodityContract(
    const quantra::CommodityContractSpec* spec, const std::string& base);

/// A present interpolation enum as a commodity price interpolation:
/// Linear, BackwardFlat or ForwardFlat; any other value is a 400 naming
/// `field` and the accepted values. Presence is the caller's.
commodities::SpotInterpolation DecodeCommodityInterpolation(
    quantra::enums::Interpolator value, const std::string& field);

} // namespace quantra

#endif // QUANTRA_COMMODITY_PARSERS_H
