#include "index_factory.h"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/ibor/eonia.hpp>

// QuantLib 1.22+ should have these. If your build fails on any,
// comment out the missing one and add a case to the switch that
// throws QUANTRA_ERROR("... not available in this QuantLib build").
#if QL_HEX_VERSION >= 0x011d00   // QL >= 1.29
#include <ql/indexes/ibor/sofr.hpp>
#include <ql/indexes/ibor/estr.hpp>
#include <ql/indexes/ibor/sonia.hpp>
#include <ql/indexes/ibor/fedfunds.hpp>
#endif

// USD LIBOR if available
#if __has_include(<ql/indexes/ibor/usdlibor.hpp>)
#include <ql/indexes/ibor/usdlibor.hpp>
#define HAS_USD_LIBOR 1
#endif

#if __has_include(<ql/indexes/ibor/gbplibor.hpp>)
#include <ql/indexes/ibor/gbplibor.hpp>
#define HAS_GBP_LIBOR 1
#endif

#if __has_include(<ql/indexes/ibor/jpylibor.hpp>)
#include <ql/indexes/ibor/jpylibor.hpp>
#define HAS_JPY_LIBOR 1
#endif

#if __has_include(<ql/indexes/ibor/tibor.hpp>)
#include <ql/indexes/ibor/tibor.hpp>
#define HAS_TIBOR 1
#endif

#include "common.h"

namespace quantra {

// =============================================================================
// From legacy enums::Ibor (Euribor-only, backward compatible)
// =============================================================================
std::shared_ptr<QuantLib::IborIndex> IndexFactory::makeIborIndex(
    quantra::enums::Ibor ibor,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding
) const {
    // Delegate to existing IborToQL for backward compatibility,
    // but wrap with forwarding curve if provided.
    // IborToQL creates without forwarding, so we clone with forwarding.
    auto base = IborToQL(ibor);
    if (forwarding.empty()) return base;

    // Clone the index with a forwarding curve
    return base->clone(forwarding);
}

// =============================================================================
// From legacy enums::OvernightIndex
// =============================================================================
std::shared_ptr<QuantLib::OvernightIndex> IndexFactory::makeOvernightIndex(
    quantra::enums::OvernightIndex on,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding
) const {
    switch (on) {
#if QL_HEX_VERSION >= 0x011d00
        case quantra::enums::OvernightIndex_SOFR:
            return std::make_shared<QuantLib::Sofr>(forwarding);
        case quantra::enums::OvernightIndex_ESTR:
            return std::make_shared<QuantLib::Estr>(forwarding);
        case quantra::enums::OvernightIndex_SONIA:
            return std::make_shared<QuantLib::Sonia>(forwarding);
        case quantra::enums::OvernightIndex_FedFunds:
            return std::make_shared<QuantLib::FedFunds>(forwarding);
#else
        // Fallback for older QuantLib: construct generic OvernightIndex
        case quantra::enums::OvernightIndex_SOFR:
            return std::make_shared<QuantLib::OvernightIndex>(
                "SOFR", 0,
                QuantLib::USDCurrency(),
                QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
                QuantLib::Actual360(),
                forwarding);
        case quantra::enums::OvernightIndex_ESTR:
            return std::make_shared<QuantLib::OvernightIndex>(
                "ESTR", 0,
                QuantLib::EURCurrency(),
                QuantLib::TARGET(),
                QuantLib::Actual360(),
                forwarding);
        case quantra::enums::OvernightIndex_SONIA:
            return std::make_shared<QuantLib::OvernightIndex>(
                "SONIA", 0,
                QuantLib::GBPCurrency(),
                QuantLib::UnitedKingdom(),
                QuantLib::Actual365Fixed(),
                forwarding);
        case quantra::enums::OvernightIndex_FedFunds:
            return std::make_shared<QuantLib::OvernightIndex>(
                "FedFunds", 0,
                QuantLib::USDCurrency(),
                QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
                QuantLib::Actual360(),
                forwarding);
#endif
        case quantra::enums::OvernightIndex_EONIA:
            return std::make_shared<QuantLib::Eonia>(forwarding);

        default:
            QUANTRA_ERROR("Unsupported OvernightIndex enum value");
    }
}

// =============================================================================
// From data-driven IborIndexSpec
// =============================================================================
std::shared_ptr<QuantLib::IborIndex> IndexFactory::makeIborIndexFromSpec(
    const quantra::IborIndexSpec* spec,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding
) const {
    if (!spec) QUANTRA_ERROR("IborIndexSpec is null");

    QuantLib::Period tenor(spec->tenor_number(), TimeUnitToQL(spec->tenor_time_unit()));

    switch (spec->family()) {
        case quantra::enums::IborFamily_Euribor:
        {
            // Use built-in Euribor which has correct conventions
            // Then clone with forwarding curve
            // Euribor constructor accepts tenor + forwarding handle
            return std::make_shared<QuantLib::Euribor>(tenor, forwarding);
        }

#ifdef HAS_USD_LIBOR
        case quantra::enums::IborFamily_USDLibor:
            return std::make_shared<QuantLib::USDLibor>(tenor, forwarding);
#endif

#ifdef HAS_GBP_LIBOR
        case quantra::enums::IborFamily_GBPLibor:
            return std::make_shared<QuantLib::GBPLibor>(tenor, forwarding);
#endif

#ifdef HAS_JPY_LIBOR
        case quantra::enums::IborFamily_JPYLibor:
            return std::make_shared<QuantLib::JPYLibor>(tenor, forwarding);
#endif

#ifdef HAS_TIBOR
        case quantra::enums::IborFamily_Tibor:
            return std::make_shared<QuantLib::Tibor>(tenor, forwarding);
#endif

        default:
        {
            // Generic fallback: construct IborIndex from individual fields
            // This works for any currency/conventions combination
            std::string familyName;
            switch (spec->family()) {
                case quantra::enums::IborFamily_USDLibor: familyName = "USDLibor"; break;
                case quantra::enums::IborFamily_GBPLibor: familyName = "GBPLibor"; break;
                case quantra::enums::IborFamily_JPYLibor: familyName = "JPYLibor"; break;
                case quantra::enums::IborFamily_Tibor:    familyName = "Tibor";    break;
                default: familyName = "CustomIbor"; break;
            }

            return std::make_shared<QuantLib::IborIndex>(
                familyName,
                tenor,
                spec->fixing_days(),
                QuantLib::Currency(), // generic
                CalendarToQL(spec->calendar()),
                ConventionToQL(spec->business_day_convention()),
                spec->end_of_month(),
                DayCounterToQL(spec->day_counter()),
                forwarding
            );
        }
    }
}

// =============================================================================
// From data-driven OvernightIndexSpec
// =============================================================================
std::shared_ptr<QuantLib::OvernightIndex> IndexFactory::makeOvernightIndexFromSpec(
    const quantra::OvernightIndexSpec* spec,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding
) const {
    if (!spec) QUANTRA_ERROR("OvernightIndexSpec is null");

    // Delegate to enum-based factory since the spec carries the enum
    return makeOvernightIndex(spec->name(), forwarding);
}

} // namespace quantra
