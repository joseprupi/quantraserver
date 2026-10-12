#include "fx_spot_registry.h"

#include "error.h"
#include "fx_spot_parser.h"

namespace quantra {

const FxSpot& FxSpotRegistry::get(const std::string& id) const {
    auto it = spots_.find(id);
    if (it == spots_.end()) {
        QUANTRA_NOT_FOUND("FX spot not found: " + id);
    }
    return it->second;
}

FxSpotRegistry FxSpotRegistryBuilder::build(
    const quantra::FxMarketData* fx, const QuoteRegistry& quotes) const {
    FxSpotRegistry out;
    if (!fx || !fx->spots()) {
        return out;
    }
    FxSpotParser parser;
    for (const auto* spec : *fx->spots()) {
        FxSpot spot = parser.parse(spec, &quotes);
        if (out.has(spot.id)) {
            QUANTRA_INVALID_ARGUMENT("duplicate FX spot id in pricing.fx.spots: " + spot.id);
        }
        out.put(spot);
    }
    return out;
}

} // namespace quantra
