#ifndef QUANTRASERVER_INDEX_REGISTRY_BUILDER_H
#define QUANTRASERVER_INDEX_REGISTRY_BUILDER_H

#include "index_registry.h"
#include "index_generated.h"
#include "enums.h"
#include "common.h"
#include "error.h"

#include <ql/indexes/iborindex.hpp>
#include <ql/indexes/ibor/all.hpp>
#include <ql/currencies/europe.hpp>
#include <ql/currencies/america.hpp>
#include <ql/currencies/asia.hpp>
#include <ql/currencies/all.hpp>

namespace quantra {

/**
 * Map ISO currency string to QuantLib Currency.
 */
inline QuantLib::Currency CurrencyFromString(const std::string& ccy) {
    if (ccy == "EUR") return QuantLib::EURCurrency();
    if (ccy == "USD") return QuantLib::USDCurrency();
    if (ccy == "GBP") return QuantLib::GBPCurrency();
    if (ccy == "JPY") return QuantLib::JPYCurrency();
    if (ccy == "CHF") return QuantLib::CHFCurrency();
    if (ccy == "AUD") return QuantLib::AUDCurrency();
    if (ccy == "CAD") return QuantLib::CADCurrency();
    if (ccy == "SEK") return QuantLib::SEKCurrency();
    if (ccy == "NOK") return QuantLib::NOKCurrency();
    if (ccy == "DKK") return QuantLib::DKKCurrency();
    if (ccy == "NZD") return QuantLib::NZDCurrency();
    if (ccy == "SGD") return QuantLib::SGDCurrency();
    if (ccy == "HKD") return QuantLib::HKDCurrency();
    if (ccy == "CNY") return QuantLib::CNYCurrency();
    if (ccy == "INR") return QuantLib::INRCurrency();
    if (ccy == "BRL") return QuantLib::BRLCurrency();
    if (ccy == "MXN") return QuantLib::MXNCurrency();
    if (ccy == "ZAR") return QuantLib::ZARCurrency();
    if (ccy == "PLN") return QuantLib::PLNCurrency();
    if (ccy == "CZK") return QuantLib::CZKCurrency();
    if (ccy == "HUF") return QuantLib::HUFCurrency();
    if (ccy == "TRY") return QuantLib::TRYCurrency();
    if (ccy == "KRW") return QuantLib::KRWCurrency();
    if (ccy == "TWD") return QuantLib::TWDCurrency();
    // Fallback: treat as EUR (you could also throw here)
    QUANTRA_ERROR("Unsupported currency: " + ccy);
    return QuantLib::EURCurrency(); // unreachable
}

/**
 * Build an IndexRegistry from a FlatBuffers indices array.
 *
 * For each IndexDef:
 *   - If index_type == Overnight: creates QuantLib::OvernightIndex
 *   - If index_type == Ibor: creates QuantLib::IborIndex
 *   - Applies historical fixings if present
 */
class IndexRegistryBuilder {
public:
    IndexRegistry build(
        const flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>* indices
    ) const {
        IndexRegistry registry;

        if (!indices) return registry;

        for (auto it = indices->begin(); it != indices->end(); ++it) {
            const auto* def = *it;
            if (!def->id()) {
                QUANTRA_ERROR("IndexDef.id is required");
            }
            if (!def->name()) {
                QUANTRA_ERROR("IndexDef.name is required");
            }

            std::string id = def->id()->str();
            std::string name = def->name()->str();

            // Parse currency (required field, default to EUR if somehow empty)
            std::string ccyStr = (def->currency() && def->currency()->size() > 0)
                ? def->currency()->str() : "EUR";
            QuantLib::Currency currency = CurrencyFromString(ccyStr);

            // Parse conventions
            if (!def->tenor()) {
                QUANTRA_ERROR("IndexDef.tenor is required for id: " + id);
            }
            int tenorN = def->tenor()->n();
            QuantLib::TimeUnit tenorUnit = TimeUnitToQL(def->tenor()->unit());
            QuantLib::Period tenor(tenorN, tenorUnit);
            int fixingDays = def->fixing_days();
            QuantLib::Calendar calendar = CalendarToQL(def->calendar());
            QuantLib::BusinessDayConvention bdc = ConventionToQL(def->business_day_convention());
            QuantLib::DayCounter dayCounter = DayCounterToQL(def->day_counter());
            bool eom = def->end_of_month();

            std::shared_ptr<QuantLib::InterestRateIndex> index;

            if (def->index_type() == quantra::IndexType_Overnight) {
                // Build OvernightIndex
                index = std::make_shared<QuantLib::OvernightIndex>(
                    name,
                    fixingDays,
                    currency,
                    calendar,
                    dayCounter
                );
            } else {
                // Build IborIndex
                index = std::make_shared<QuantLib::IborIndex>(
                    name,
                    tenor,
                    fixingDays,
                    currency,
                    calendar,
                    bdc,
                    eom,
                    dayCounter
                );
            }

            // Apply historical fixings
            if (def->fixings()) {
                // Clear any stale fixings for this index
                index->clearFixings();

                for (auto fIt = def->fixings()->begin(); fIt != def->fixings()->end(); ++fIt) {
                    if (!fIt->date()) continue;
                    QuantLib::Date d = DateToQL(fIt->date()->str());
                    index->addFixing(d, fIt->value());
                }
            }

            registry.put(id, index);
        }

        return registry;
    }
};

} // namespace quantra

#endif // QUANTRASERVER_INDEX_REGISTRY_BUILDER_H
