#include "term_structure_parser.h"

#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
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
    const IndexRegistry* indices,
    double bump)
{
    if (ts == nullptr)
        QUANTRA_ERROR("TermStructure not found");

    auto points = ts->points();
    if (!points || points->size() == 0)
        QUANTRA_ERROR("Empty list of points for term structure");

    bool hasZeroPoints = false;
    for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
        if (points->Get(i)->point_type() == quantra::Point_ZeroRatePoint) {
            hasZeroPoints = true;
            break;
        }
    }

    if (hasZeroPoints) {
        std::vector<Date> dates;
        std::vector<Rate> zeroRates;
        dates.reserve(points->size());
        zeroRates.reserve(points->size());

        Compounding comp = QuantLib::Continuous;
        Frequency freq = QuantLib::Annual;
        bool compSet = false;

        Date ref;
        if (ts->reference_date()) {
            ref = DateToQL(ts->reference_date()->str());
        } else {
            ref = Settings::instance().evaluationDate();
        }

        for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
            auto pw = points->Get(i);
            if (pw->point_type() != quantra::Point_ZeroRatePoint) {
                QUANTRA_ERROR("ZeroRatePoint cannot be mixed with bootstrap helpers");
            }
            auto p = pw->point_as_ZeroRatePoint();
            if (!p) {
                QUANTRA_ERROR("ZeroRatePoint is null");
            }

            Date d;
            if (p->date()) {
                d = DateToQL(p->date()->str());
            } else {
                if (p->tenor_number() <= 0) {
                    QUANTRA_ERROR("ZeroRatePoint requires date or tenor");
                }
                auto cal = CalendarToQL(p->calendar());
                auto bdc = ConventionToQL(p->business_day_convention());
                d = cal.advance(ref, p->tenor_number() * TimeUnitToQL(p->tenor_time_unit()), bdc);
            }
            dates.push_back(d);
            zeroRates.push_back(p->zero_rate() + bump);

            if (!compSet) {
                comp = CompoundingToQL(p->compounding());
                freq = FrequencyToQL(p->frequency());
                compSet = true;
            }
        }

        return buildZeroCurve(ts, dates, zeroRates, comp, freq);
    }

    TermStructurePointParser pointParser;
    std::vector<std::shared_ptr<RateHelper>> instruments;
    instruments.reserve(points->size());

    for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
        auto point = points->Get(i)->point();
        auto type  = points->Get(i)->point_type();
        auto helper = pointParser.parse(type, point, quotes, curves, indices, bump);
        if (!helper)
            QUANTRA_ERROR("Failed to parse term structure point at index " + std::to_string(i));
        instruments.push_back(helper);
    }

    return buildCurve(ts, instruments);
}

// =============================================================================
// Build PiecewiseYieldCurve from helpers
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::buildCurve(
    const quantra::TermStructure* ts,
    std::vector<std::shared_ptr<RateHelper>>& instruments)
{
    double tolerance = 1.0e-15;
    
    Date ref;
    if (ts->reference_date()) {
        ref = DateToQL(ts->reference_date()->str());
    } else {
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

// =============================================================================
// Build zero curve from explicit zero rates
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::buildZeroCurve(
    const quantra::TermStructure* ts,
    const std::vector<Date>& dates,
    const std::vector<Rate>& zeroRates,
    Compounding compounding,
    Frequency frequency)
{
    if (dates.size() != zeroRates.size() || dates.empty()) {
        QUANTRA_ERROR("Zero curve requires matching non-empty date/rate vectors");
    }

    DayCounter dc = DayCounterToQL(ts->day_counter());

    switch (ts->interpolator()) {
    case enums::Interpolator_Linear:
        return std::make_shared<InterpolatedZeroCurve<Linear>>(
            dates, zeroRates, dc, Calendar(), Linear(), compounding, frequency);
    case enums::Interpolator_LogLinear:
        return std::make_shared<InterpolatedZeroCurve<LogLinear>>(
            dates, zeroRates, dc, Calendar(), LogLinear(), compounding, frequency);
    case enums::Interpolator_LogCubic:
        return std::make_shared<InterpolatedZeroCurve<LogCubic>>(
            dates, zeroRates, dc, Calendar(), MonotonicLogCubic(), compounding, frequency);
    case enums::Interpolator_BackwardFlat:
        return std::make_shared<InterpolatedZeroCurve<BackwardFlat>>(
            dates, zeroRates, dc, Calendar(), BackwardFlat(), compounding, frequency);
    case enums::Interpolator_ForwardFlat:
        return std::make_shared<InterpolatedZeroCurve<ForwardFlat>>(
            dates, zeroRates, dc, Calendar(), ForwardFlat(), compounding, frequency);
    default:
        QUANTRA_ERROR("Unsupported Interpolator for zero curve");
    }

    return nullptr;
}

} // namespace quantra
