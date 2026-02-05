#ifndef QUANTRASERVER_INDEX_FACTORY_H
#define QUANTRASERVER_INDEX_FACTORY_H

#include <memory>

#include <ql/indexes/iborindex.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/handle.hpp>

#include "enums_generated.h"
#include "term_structure_generated.h"
#include "enums.h"

namespace quantra {

/**
 * IndexFactory - Central index construction.
 *
 * Builds QuantLib IborIndex / OvernightIndex objects from:
 *   1. Legacy enums (enums::Ibor, enums::OvernightIndex)
 *   2. Data-driven specs (IborIndexSpec, OvernightIndexSpec)
 *
 * Accepts an optional forwarding curve handle for bootstrapping.
 */
class IndexFactory {
public:
    // --- From legacy enums ---

    std::shared_ptr<QuantLib::IborIndex> makeIborIndex(
        quantra::enums::Ibor ibor,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding =
            QuantLib::Handle<QuantLib::YieldTermStructure>()
    ) const;

    std::shared_ptr<QuantLib::OvernightIndex> makeOvernightIndex(
        quantra::enums::OvernightIndex on,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding =
            QuantLib::Handle<QuantLib::YieldTermStructure>()
    ) const;

    // --- From data-driven IndexSpec ---

    std::shared_ptr<QuantLib::IborIndex> makeIborIndexFromSpec(
        const quantra::IborIndexSpec* spec,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding =
            QuantLib::Handle<QuantLib::YieldTermStructure>()
    ) const;

    std::shared_ptr<QuantLib::OvernightIndex> makeOvernightIndexFromSpec(
        const quantra::OvernightIndexSpec* spec,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding =
            QuantLib::Handle<QuantLib::YieldTermStructure>()
    ) const;
};

} // namespace quantra

#endif // QUANTRASERVER_INDEX_FACTORY_H
