#ifndef QUANTRA_FX_SPOT_PARSER_H
#define QUANTRA_FX_SPOT_PARSER_H

#include "fx_spot_domain.h"
#include "quote_registry.h"

#include "fx_generated.h"

namespace quantra {

/**
 * FxSpotParser - FxSpotSpec (wire) -> FxSpot (domain).
 *
 * Every field is validated here and an invalid one is a 400 naming the
 * field; a `quote_id` resolves through the QuoteRegistry and must carry the
 * Fx quote type (a Curve quote would be shifted by the rate bump). The parser
 * does not check uniqueness across spots — FxSpotRegistryBuilder does.
 */
class FxSpotParser {
public:
    FxSpot parse(const quantra::FxSpotSpec* spec, const QuoteRegistry* quotes) const;
};

} // namespace quantra

#endif // QUANTRA_FX_SPOT_PARSER_H
