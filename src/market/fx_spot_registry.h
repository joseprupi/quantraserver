#ifndef QUANTRA_FX_SPOT_REGISTRY_H
#define QUANTRA_FX_SPOT_REGISTRY_H

#include "fx_spot_domain.h"
#include "quote_registry.h"

#include "fx_generated.h"

#include <string>
#include <unordered_map>

namespace quantra {

/**
 * FxSpotRegistry - FX spots by id (plain domain, no FlatBuffers pointers).
 *
 * Built once per request from pricing.fx.spots. Curve helpers resolve
 * `fx_spot` references here while the curves are bootstrapped; FX products
 * resolve their spot the same way.
 */
class FxSpotRegistry {
public:
    void put(const FxSpot& spot) { spots_[spot.id] = spot; }

    bool has(const std::string& id) const { return spots_.find(id) != spots_.end(); }

    /// The spot with the given id; QuantraNotFound (404) when absent.
    const FxSpot& get(const std::string& id) const;

    std::size_t size() const { return spots_.size(); }

private:
    std::unordered_map<std::string, FxSpot> spots_;
};

/**
 * Build an FxSpotRegistry from the wire FxMarketData. Every spot goes
 * through FxSpotParser; a duplicate id across pricing.fx.spots is a 400.
 * A null FxMarketData yields an empty registry.
 */
class FxSpotRegistryBuilder {
public:
    FxSpotRegistry build(const quantra::FxMarketData* fx, const QuoteRegistry& quotes) const;
};

} // namespace quantra

#endif // QUANTRA_FX_SPOT_REGISTRY_H
