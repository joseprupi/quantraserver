#ifndef QUANTRASERVER_INDEX_REGISTRY_H
#define QUANTRASERVER_INDEX_REGISTRY_H

#include <string>
#include <unordered_map>
#include <memory>

#include <ql/indexes/iborindex.hpp>
#include <ql/indexes/ibor/all.hpp>
#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "error.h"

namespace quantra {

/**
 * IndexRegistry - Stores QuantLib InterestRateIndex objects by id.
 *
 * Built from the request's indices:[IndexDef] array.
 * Used by curve helpers (SwapHelper, OISHelper) and instrument parsers
 * (FloatingRateBond, VanillaSwap, CapFloor, FRA) to resolve IndexRef.id
 * to a live QuantLib index object.
 *
 * Design:
 *   - Stores both IborIndex and OvernightIndex as InterestRateIndex base.
 *   - Callers downcast when needed (e.g., OISHelper needs OvernightIndex).
 *   - Forwarding curves are linked after construction via clone().
 */
class IndexRegistry {
public:
    void put(const std::string& id, std::shared_ptr<QuantLib::InterestRateIndex> idx) {
        indices_[id] = idx;
    }

    bool has(const std::string& id) const {
        return indices_.find(id) != indices_.end();
    }

    /// Get as base InterestRateIndex
    std::shared_ptr<QuantLib::InterestRateIndex> get(const std::string& id) const {
        auto it = indices_.find(id);
        if (it == indices_.end()) {
            QUANTRA_ERROR("Unknown index id: " + id);
        }
        return it->second;
    }

    /// Get as IborIndex (throws if not ibor)
    std::shared_ptr<QuantLib::IborIndex> getIbor(const std::string& id) const {
        auto base = get(id);
        auto ibor = std::dynamic_pointer_cast<QuantLib::IborIndex>(base);
        if (!ibor) {
            QUANTRA_ERROR("Index '" + id + "' is not an IborIndex");
        }
        return ibor;
    }

    /// Get as OvernightIndex (throws if not overnight)
    std::shared_ptr<QuantLib::OvernightIndex> getOvernight(const std::string& id) const {
        auto base = get(id);
        auto on = std::dynamic_pointer_cast<QuantLib::OvernightIndex>(base);
        if (!on) {
            QUANTRA_ERROR("Index '" + id + "' is not an OvernightIndex");
        }
        return on;
    }

    /// Get IborIndex cloned with a specific forwarding curve
    std::shared_ptr<QuantLib::IborIndex> getIborWithCurve(
        const std::string& id,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding
    ) const {
        auto ibor = getIbor(id);
        if (forwarding.empty()) return ibor;
        return ibor->clone(forwarding);
    }

    /// Get OvernightIndex cloned with a specific forwarding curve
    std::shared_ptr<QuantLib::OvernightIndex> getOvernightWithCurve(
        const std::string& id,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding
    ) const {
        auto on = getOvernight(id);
        if (forwarding.empty()) return on;
        // OvernightIndex::clone returns IborIndex, need to cast
        auto cloned = on->clone(forwarding);
        auto result = std::dynamic_pointer_cast<QuantLib::OvernightIndex>(cloned);
        if (!result) {
            // Fallback: reconstruct manually
            result = std::make_shared<QuantLib::OvernightIndex>(
                on->familyName(),
                on->fixingDays(),
                on->currency(),
                on->fixingCalendar(),
                on->dayCounter(),
                forwarding
            );
        }
        return result;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<QuantLib::InterestRateIndex>> indices_;
};

} // namespace quantra

#endif // QUANTRASERVER_INDEX_REGISTRY_H
