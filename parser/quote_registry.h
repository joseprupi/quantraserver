#ifndef QUANTRASERVER_QUOTE_REGISTRY_H
#define QUANTRASERVER_QUOTE_REGISTRY_H

#include <memory>
#include <string>
#include <unordered_map>

#include <ql/quotes/simplequote.hpp>
#include <ql/handle.hpp>

#include "error.h"
#include "quotes_generated.h"

namespace quantra {

/**
 * QuoteRegistry - Shared market quote store.
 *
 * Helpers can reference quotes by id (quote_id field) instead of carrying
 * inline rate/price values. This is the foundation for scenario/bump analysis:
 * bump a SimpleQuote value and all dependent helpers update automatically.
 */
class QuoteRegistry {
public:
    void upsert(const std::string& id, double value, quantra::QuoteType type = quantra::QuoteType_Curve) {
        auto it = quotes_.find(id);
        if (it == quotes_.end()) {
            quotes_[id] = std::make_shared<QuantLib::SimpleQuote>(value);
        } else {
            it->second->setValue(value);
        }
        auto typeIt = types_.find(id);
        if (typeIt == types_.end()) {
            types_[id] = type;
        } else if (typeIt->second != type) {
            QUANTRA_ERROR("Quote id '" + id + "' has conflicting types");
        }
    }

    bool has(const std::string& id) const {
        return quotes_.find(id) != quotes_.end();
    }

    QuantLib::Handle<QuantLib::Quote> getHandle(const std::string& id) const {
        auto it = quotes_.find(id);
        if (it == quotes_.end()) {
            QUANTRA_ERROR("Unknown quote id: " + id);
        }
        return QuantLib::Handle<QuantLib::Quote>(it->second);
    }

    QuantLib::Handle<QuantLib::Quote> getHandle(const std::string& id, quantra::QuoteType expected) const {
        auto it = quotes_.find(id);
        if (it == quotes_.end()) {
            QUANTRA_ERROR("Unknown quote id: " + id);
        }
        auto typeIt = types_.find(id);
        if (typeIt == types_.end()) {
            QUANTRA_ERROR("Quote id '" + id + "' missing type");
        }
        if (typeIt->second != expected) {
            QUANTRA_ERROR("Quote id '" + id + "' has wrong type");
        }
        return QuantLib::Handle<QuantLib::Quote>(it->second);
    }

    double getValue(const std::string& id, quantra::QuoteType expected) const {
        return getHandle(id, expected)->value();
    }

private:
    std::unordered_map<std::string, std::shared_ptr<QuantLib::SimpleQuote>> quotes_;
    std::unordered_map<std::string, quantra::QuoteType> types_;
};

} // namespace quantra

#endif // QUANTRASERVER_QUOTE_REGISTRY_H
