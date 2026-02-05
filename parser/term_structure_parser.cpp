#include "term_structure_parser.h"

#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>

using namespace QuantLib;

namespace quantra {

// =============================================================================
// Legacy parse (backward compatible, no registries)
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::parse(
    const quantra::TermStructure* ts)
{
    return parse(ts, nullptr, nullptr, nullptr);
}

// =============================================================================
// Full parse with registries
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::parse(
    const quantra::TermStructure* ts,
    const QuoteRegistry* quotes,
    const CurveRegistry* curves,
    const IndexFactory* indexFactory)
{
    if (ts == nullptr)
        QUANTRA_ERROR("TermStructure not found");

    auto points = ts->points();
    if (!points || points->size() == 0)
        QUANTRA_ERROR("Empty list of points for term structure");

    TermStructurePointParser pointParser;
    std::vector<std::shared_ptr<RateHelper>> instruments;
    instruments.reserve(points->size());

    for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
        auto point = points->Get(i)->point();
        auto type  = points->Get(i)->point_type();
        auto helper = pointParser.parse(type, point, quotes, curves, indexFactory);
        if (!helper)
            QUANTRA_ERROR("Failed to parse term structure point at index " + std::to_string(i));
        instruments.push_back(helper);
    }

    return buildCurve(ts, instruments);
}

// =============================================================================
// Build PiecewiseYieldCurve from helpers (shared by both parse overloads)
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::buildCurve(
    const quantra::TermStructure* ts,
    std::vector<std::shared_ptr<RateHelper>>& instruments)
{
    double tolerance = 1.0e-15;
    
    // FIX: Handle null reference_date by using evaluation date as fallback
    Date ref;
    if (ts->reference_date()) {
        ref = DateToQL(ts->reference_date()->str());
    } else {
        // Use the global evaluation date as fallback
        ref = Settings::instance().evaluationDate();
    }
    
    DayCounter dc = DayCounterToQL(ts->day_counter());

    switch (ts->interpolator()) {
    case enums::Interpolator_BackwardFlat:
        switch (ts->bootstrap_trait()) {
        case enums::BootstrapTrait_Discount:
            return std::make_shared<PiecewiseYieldCurve<Discount, BackwardFlat>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<Discount, BackwardFlat>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_ZeroRate:
            return std::make_shared<PiecewiseYieldCurve<ZeroYield, BackwardFlat>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ZeroYield, BackwardFlat>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_FwdRate:
            return std::make_shared<PiecewiseYieldCurve<ForwardRate, BackwardFlat>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ForwardRate, BackwardFlat>::bootstrap_type(tolerance));
        default:
            QUANTRA_ERROR("Unsupported BootstrapTrait for BackwardFlat");
        }
        break;

    case enums::Interpolator_ForwardFlat:
        switch (ts->bootstrap_trait()) {
        case enums::BootstrapTrait_Discount:
            return std::make_shared<PiecewiseYieldCurve<Discount, ForwardFlat>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<Discount, ForwardFlat>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_ZeroRate:
            return std::make_shared<PiecewiseYieldCurve<ZeroYield, ForwardFlat>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ZeroYield, ForwardFlat>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_FwdRate:
            return std::make_shared<PiecewiseYieldCurve<ForwardRate, ForwardFlat>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ForwardRate, ForwardFlat>::bootstrap_type(tolerance));
        default:
            QUANTRA_ERROR("Unsupported BootstrapTrait for ForwardFlat");
        }
        break;

    case enums::Interpolator_Linear:
        switch (ts->bootstrap_trait()) {
        case enums::BootstrapTrait_Discount:
            return std::make_shared<PiecewiseYieldCurve<Discount, Linear>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<Discount, Linear>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_ZeroRate:
            return std::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ZeroYield, Linear>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_FwdRate:
            return std::make_shared<PiecewiseYieldCurve<ForwardRate, Linear>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ForwardRate, Linear>::bootstrap_type(tolerance));
        default:
            QUANTRA_ERROR("Unsupported BootstrapTrait for Linear");
        }
        break;

    case enums::Interpolator_LogLinear:
        switch (ts->bootstrap_trait()) {
        case enums::BootstrapTrait_Discount:
            return std::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<Discount, LogLinear>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_ZeroRate:
            return std::make_shared<PiecewiseYieldCurve<ZeroYield, LogLinear>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ZeroYield, LogLinear>::bootstrap_type(tolerance));
        case enums::BootstrapTrait_FwdRate:
            return std::make_shared<PiecewiseYieldCurve<ForwardRate, LogLinear>>(
                ref, instruments, dc,
                PiecewiseYieldCurve<ForwardRate, LogLinear>::bootstrap_type(tolerance));
        default:
            QUANTRA_ERROR("Unsupported BootstrapTrait for LogLinear");
        }
        break;

    case enums::Interpolator_LogCubic:
        switch (ts->bootstrap_trait()) {
        case enums::BootstrapTrait_Discount:
            return std::make_shared<PiecewiseYieldCurve<Discount, LogCubic>>(
                ref, instruments, dc, MonotonicLogCubic());
        case enums::BootstrapTrait_ZeroRate:
            return std::make_shared<PiecewiseYieldCurve<ZeroYield, LogCubic>>(
                ref, instruments, dc, MonotonicLogCubic());
        case enums::BootstrapTrait_FwdRate:
            return std::make_shared<PiecewiseYieldCurve<ForwardRate, LogCubic>>(
                ref, instruments, dc, MonotonicLogCubic());
        default:
            QUANTRA_ERROR("Unsupported BootstrapTrait for LogCubic");
        }
        break;

    default:
        QUANTRA_ERROR("Unsupported Interpolator");
    }

    return nullptr;
}

} // namespace quantra