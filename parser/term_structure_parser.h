#ifndef QUANTRASERVER_TERMSTRUCTUREPARSER_H
#define QUANTRASERVER_TERMSTRUCTUREPARSER_H

#include <memory>

#include <ql/qldefines.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>

#include "term_structure_generated.h"
#include "term_structure_point_parser.h"
#include "quote_registry.h"
#include "curve_registry.h"
#include "index_factory.h"
#include "enums.h"
#include "common.h"

namespace quantra {

/**
 * TermStructureParser
 *
 * Bootstraps a PiecewiseYieldCurve from a FlatBuffers TermStructure spec.
 *
 * Two signatures:
 *   1. parse(ts) - legacy, inline values only (backward compatible)
 *   2. parse(ts, quotes, curves, indexFactory) - full multi-curve support
 */
class TermStructureParser {
public:
    /// Legacy: parse with inline values only (backward compatible)
    std::shared_ptr<QuantLib::YieldTermStructure> parse(
        const quantra::TermStructure* ts);

    /// Full: parse with registries for quote resolution, exogenous curves, etc.
    std::shared_ptr<QuantLib::YieldTermStructure> parse(
        const quantra::TermStructure* ts,
        const QuoteRegistry* quotes,
        const CurveRegistry* curves,
        const IndexFactory* indexFactory);

private:
    /// Internal: build piecewise curve from helpers
    std::shared_ptr<QuantLib::YieldTermStructure> buildCurve(
        const quantra::TermStructure* ts,
        std::vector<std::shared_ptr<QuantLib::RateHelper>>& instruments);
};

} // namespace quantra

#endif // QUANTRASERVER_TERMSTRUCTUREPARSER_H
