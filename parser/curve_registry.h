#ifndef QUANTRASERVER_CURVE_REGISTRY_H
#define QUANTRASERVER_CURVE_REGISTRY_H

#include <string>
#include <unordered_map>
#include <memory>

#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/handle.hpp>

#include "error.h"

namespace quantra {

/**
 * CurveRegistry - Stores yield curve handles by id.
 *
 * During multi-curve bootstrapping, curves are pre-registered as empty
 * RelinkableHandles. After bootstrapping in topological order, each handle
 * is linked to the real curve. Helpers that depend on exogenous curves
 * hold a copy of the handle, so they automatically see the linked curve.
 */
class CurveRegistry {
public:
    void put(const std::string& id, const QuantLib::Handle<QuantLib::YieldTermStructure>& h) {
        curves_[id] = h;
    }

    bool has(const std::string& id) const {
        return curves_.find(id) != curves_.end();
    }

    QuantLib::Handle<QuantLib::YieldTermStructure> get(const std::string& id) const {
        auto it = curves_.find(id);
        if (it == curves_.end()) {
            QUANTRA_ERROR("Unknown curve id: " + id);
        }
        return it->second;
    }

private:
    std::unordered_map<std::string, QuantLib::Handle<QuantLib::YieldTermStructure>> curves_;
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_REGISTRY_H
