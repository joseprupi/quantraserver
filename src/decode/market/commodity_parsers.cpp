#include "commodity_parsers.h"

#include "commodity_pricing_days.h"
#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

#include <set>
#include <utility>
#include <vector>

namespace quantra {

namespace {

std::string at(const std::string& base, std::size_t i) {
    return base + "[" + std::to_string(i) + "]";
}

/// Named calendar (through the shared factory, so request calendar overrides
/// apply) minus the object's own holidays.
commodities::PricingCalendar parsePricingCalendar(
    flatbuffers::Optional<quantra::enums::Calendar> calendar,
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* holidays,
    const std::string& base) {
    const QuantLib::Calendar named =
        CalendarToQL(requireEnum(calendar, base + ".pricing_calendar"));
    std::vector<QuantLib::Date> own;
    if (holidays) {
        own.reserve(holidays->size());
        for (flatbuffers::uoffset_t i = 0; i < holidays->size(); ++i) {
            own.push_back(requireIsoDate(holidays->Get(i), at(base + ".pricing_holidays", i)));
        }
    }
    return commodities::PricingCalendar(named, own);
}

std::shared_ptr<const commodities::FuturesContract> parseContract(
    const quantra::CommodityContractSpec* spec, const std::string& base) {
    auto data = DecodeCommodityContract(spec, base);
    return std::make_shared<const commodities::FuturesContract>(
        std::move(data.id), std::move(data.calendar), std::move(data.months), data.fixings);
}

std::shared_ptr<const commodities::SpotPrice> parseSpot(
    const quantra::CommoditySpotSpec* spec, const std::string& base) {
    if (!spec) {
        QUANTRA_INVALID_ARGUMENT(base + " is null");
    }
    const std::string id = requireNonEmptyString(spec->id(), base + ".id");
    const std::string named = base + " ('" + id + "')";
    auto calendar = parsePricingCalendar(spec->pricing_calendar(), spec->pricing_holidays(), named);
    const std::string interpolationField = named + ".interpolation";
    const auto interpolation = DecodeCommodityInterpolation(
        requireEnum(spec->interpolation(), interpolationField), interpolationField);

    const auto* points = spec->points();
    if (!points) {
        QUANTRA_INVALID_ARGUMENT(named + ".points is required");
    }
    std::vector<commodities::SpotPoint> parsedPoints;
    parsedPoints.reserve(points->size());
    for (flatbuffers::uoffset_t i = 0; i < points->size(); ++i) {
        const auto* p = points->Get(i);
        const std::string field = at(named + ".points", i);
        if (!p) {
            QUANTRA_INVALID_ARGUMENT(field + " is null");
        }
        commodities::SpotPoint point;
        point.date = requireIsoDate(p->date(), field + ".date");
        point.price = requireScalarPresent(p->price(), field + ".price");
        parsedPoints.push_back(point);
    }

    std::vector<commodities::SpotFixing> fixings;
    if (const auto* list = spec->fixings()) {
        fixings.reserve(list->size());
        for (flatbuffers::uoffset_t i = 0; i < list->size(); ++i) {
            const auto* f = list->Get(i);
            const std::string field = at(named + ".fixings", i);
            if (!f) {
                QUANTRA_INVALID_ARGUMENT(field + " is null");
            }
            commodities::SpotFixing fixing;
            fixing.date = requireIsoDate(f->date(), field + ".date");
            fixing.value = requireScalarPresent(f->value(), field + ".value");
            fixings.push_back(fixing);
        }
    }
    return std::make_shared<const commodities::SpotPrice>(
        id, std::move(calendar), interpolation, spec->extrapolate_flat(), std::move(parsedPoints),
        fixings);
}

} // namespace

CommodityContractData DecodeCommodityContract(
    const quantra::CommodityContractSpec* spec, const std::string& base) {
    if (!spec) {
        QuantraInvalidArgumentAt(base, base + " is null");
    }
    const std::string id = requireNonEmptyString(spec->id(), base + ".id");
    const std::string named = base + " ('" + id + "')";
    auto calendar = parsePricingCalendar(spec->pricing_calendar(), spec->pricing_holidays(), named);

    const auto* months = spec->months();
    if (!months) {
        QuantraInvalidArgumentAt(named + ".months", named + ".months is required");
    }
    std::vector<commodities::ContractMonth> parsedMonths;
    parsedMonths.reserve(months->size());
    for (flatbuffers::uoffset_t i = 0; i < months->size(); ++i) {
        const auto* m = months->Get(i);
        const std::string field = at(named + ".months", i);
        if (!m) {
            QuantraInvalidArgumentAt(field, field + " is null");
        }
        commodities::ContractMonth month;
        month.month = requireNonEmptyString(m->month(), field + ".month");
        month.lastTradeDate = requireIsoDate(m->last_trade_date(), field + ".last_trade_date");
        if (m->price().has_value()) {
            month.price = requireScalarPresent(m->price(), field + ".price");
        }
        parsedMonths.push_back(std::move(month));
    }

    std::vector<commodities::ContractFixing> fixings;
    if (const auto* list = spec->fixings()) {
        fixings.reserve(list->size());
        for (flatbuffers::uoffset_t i = 0; i < list->size(); ++i) {
            const auto* f = list->Get(i);
            const std::string field = at(named + ".fixings", i);
            if (!f) {
                QuantraInvalidArgumentAt(field, field + " is null");
            }
            commodities::ContractFixing fixing;
            fixing.month = requireNonEmptyString(f->month(), field + ".month");
            fixing.date = requireIsoDate(f->date(), field + ".date");
            fixing.value = requireScalarPresent(f->value(), field + ".value");
            fixings.push_back(std::move(fixing));
        }
    }
    return CommodityContractData{
        id, std::move(calendar), std::move(parsedMonths), std::move(fixings)};
}

commodities::SpotInterpolation DecodeCommodityInterpolation(
    quantra::enums::Interpolator value, const std::string& field) {
    switch (value) {
        case quantra::enums::Interpolator_Linear: return commodities::SpotInterpolation::Linear;
        case quantra::enums::Interpolator_BackwardFlat:
            return commodities::SpotInterpolation::BackwardFlat;
        case quantra::enums::Interpolator_ForwardFlat:
            return commodities::SpotInterpolation::ForwardFlat;
        default: break;
    }
    QUANTRA_INVALID_ARGUMENT(
        field + " must be Linear, BackwardFlat or ForwardFlat (LogLinear and LogCubic are " +
        "refused: prices can be zero or negative)");
    return commodities::SpotInterpolation::Linear; // unreachable
}

std::shared_ptr<const commodities::FuturesContract> CommodityRegistry::contract(
    const std::string& id, const std::string& field) const {
    auto it = contracts.find(id);
    if (it == contracts.end()) {
        QUANTRA_NOT_FOUND(field + ": commodity contract not found: " + id);
    }
    return it->second;
}

std::shared_ptr<const commodities::SpotPrice> CommodityRegistry::spotPrice(
    const std::string& id, const std::string& field) const {
    auto it = spotPrices.find(id);
    if (it == spotPrices.end()) {
        QUANTRA_NOT_FOUND(field + ": commodity spot price not found: " + id);
    }
    return it->second;
}

CommodityRegistry BuildCommodityRegistry(const quantra::CommodityMarketData* commodity) {
    CommodityRegistry out;
    if (!commodity) {
        return out;
    }
    if (const auto* contracts = commodity->contracts()) {
        for (flatbuffers::uoffset_t i = 0; i < contracts->size(); ++i) {
            auto contract = parseContract(contracts->Get(i), at("pricing.commodity.contracts", i));
            if (!out.contracts.emplace(contract->id(), contract).second) {
                QUANTRA_INVALID_ARGUMENT(
                    "duplicate commodity contract id in pricing.commodity.contracts: " +
                    contract->id());
            }
        }
    }
    if (const auto* spots = commodity->spot_prices()) {
        for (flatbuffers::uoffset_t i = 0; i < spots->size(); ++i) {
            auto spot = parseSpot(spots->Get(i), at("pricing.commodity.spot_prices", i));
            if (!out.spotPrices.emplace(spot->id(), spot).second) {
                QUANTRA_INVALID_ARGUMENT(
                    "duplicate commodity spot price id in pricing.commodity.spot_prices: " +
                    spot->id());
            }
        }
    }
    return out;
}

} // namespace quantra
