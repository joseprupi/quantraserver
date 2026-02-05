#ifndef QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H
#define QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H

#include <memory>
#include <string>

#include <ql/qldefines.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/imm.hpp>

#include "term_structure_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"

#include "quote_registry.h"
#include "curve_registry.h"
#include "index_factory.h"

namespace quantra {

/**
 * TermStructurePointParser
 *
 * Builds QuantLib::RateHelper objects from FlatBuffers helper specs.
 *
 * New signature accepts optional registries for:
 * - QuoteRegistry: resolve quote_id -> Handle<Quote>
 * - CurveRegistry: resolve deps.discount_curve -> Handle<YieldTermStructure>
 * - IndexFactory:  build IborIndex/OvernightIndex from enums or specs
 *
 * Backward compatible: passing nullptr for all registries falls back to
 * inline values and legacy IborToQL enum mapping.
 */
class TermStructurePointParser {
public:
    TermStructurePointParser() = default;

    /**
     * Parse a single helper point into a QuantLib RateHelper.
     *
     * @param point_type  FlatBuffers union discriminator (Point_DepositHelper, etc.)
     * @param data        Pointer to the FlatBuffers table
     * @param quotes      Optional quote registry (for quote_id resolution)
     * @param curves      Optional curve registry (for exogenous discount curves)
     * @param indexFactory Optional index factory (required for SwapHelper/OIS/basis)
     */
    std::shared_ptr<QuantLib::RateHelper> parse(
        uint8_t point_type,
        const void* data,
        const QuoteRegistry* quotes = nullptr,
        const CurveRegistry* curves = nullptr,
        const IndexFactory* indexFactory = nullptr
    ) const;

private:
    /**
     * Resolve a quote: if quote_id is set and registry exists, use it;
     * otherwise create an inline SimpleQuote from the given value.
     */
    QuantLib::Handle<QuantLib::Quote> resolveQuote(
        double inlineValue,
        const flatbuffers::String* quoteId,
        const QuoteRegistry* quotes
    ) const;

    /**
     * Resolve an exogenous curve reference via CurveRegistry.
     * Returns empty handle if ref is null.
     */
    QuantLib::Handle<QuantLib::YieldTermStructure> resolveCurve(
        const quantra::CurveRef* ref,
        const CurveRegistry* curves
    ) const;
};

} // namespace quantra

#endif // QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H
