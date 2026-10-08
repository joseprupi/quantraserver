#ifndef QUANTRASERVER_TERM_STRUCTURE_POINT_PARSER_H
#define QUANTRASERVER_TERM_STRUCTURE_POINT_PARSER_H

#include "curve_registry.h"
#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "index_registry.h"
#include "quote_registry.h"
#include "schedule_parser.h"

#include "term_structure_generated.h"

#include <ql/handle.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>

#include <memory>

namespace quantra {

/**
 * TermStructurePointParser - Builds QuantLib RateHelper objects from FlatBuffers.
 *
 * Dependencies resolved via registries:
 * - QuoteRegistry:  shared quotes (quote_id references)
 * - CurveRegistry:  exogenous curves (deps.discount_curve references)
 * - IndexRegistry:  index definitions (float_index.id, overnight_index.id references)
 *
 * All index resolution goes through IndexRegistry. No enum-based fallbacks.
 */
class TermStructurePointParser {
public:
    /**
     * Parse a single curve point into a QuantLib RateHelper.
     *
     * @param point_type  FlatBuffers union discriminator (Point_DepositHelper, etc.)
     * @param data        Raw pointer to the helper data
     * @param quotes      Optional quote registry for quote_id resolution
     * @param curves      Optional curve registry for exogenous curve deps
     * @param indices     Index registry for index_id resolution (REQUIRED for
     *                    SwapHelper, OISHelper, DatedOISHelper, basis helpers)
     * @return Built QuantLib RateHelper
     */
    std::shared_ptr<QuantLib::RateHelper> parse(
        uint8_t point_type,
        const void* data,
        const QuoteRegistry* quotes = nullptr,
        const CurveRegistry* curves = nullptr,
        const IndexRegistry* indices = nullptr,
        double bump = 0.0) const;

private:
    QuantLib::Handle<QuantLib::Quote> resolveQuote(
        double inlineValue,
        const flatbuffers::String* quoteId,
        const QuoteRegistry* quotes,
        quantra::QuoteType expectedType,
        double bump) const;

    QuantLib::Handle<QuantLib::YieldTermStructure> resolveCurve(
        const quantra::CurveRef* ref, const CurveRegistry* curves) const;
};

} // namespace quantra

#endif // QUANTRASERVER_TERM_STRUCTURE_POINT_PARSER_H
