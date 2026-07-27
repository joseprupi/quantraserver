#include "term_structure_parser.h"

#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/termstructures/yield/forwardcurve.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>

#include <cmath>

using namespace QuantLib;

namespace quantra {

namespace {

// Human-readable name for a bootstrap trait, used in diagnostics.
std::string bootstrapTraitName(enums::BootstrapTrait trait) {
    switch (trait) {
    case enums::BootstrapTrait_Discount:             return "Discount";
    case enums::BootstrapTrait_FwdRate:              return "FwdRate";
    case enums::BootstrapTrait_InterpolatedDiscount: return "InterpolatedDiscount";
    case enums::BootstrapTrait_InterpolatedFwd:      return "InterpolatedFwd";
    case enums::BootstrapTrait_InterpolatedZero:     return "InterpolatedZero";
    case enums::BootstrapTrait_ZeroRate:             return "ZeroRate";
    default:                                         return "Unknown";
    }
}

// Human-readable name for a curve point union type, used in diagnostics.
std::string pointTypeName(quantra::Point type) {
    switch (type) {
    case quantra::Point_DepositHelper:        return "DepositHelper";
    case quantra::Point_FRAHelper:            return "FRAHelper";
    case quantra::Point_FutureHelper:         return "FutureHelper";
    case quantra::Point_SwapHelper:           return "SwapHelper";
    case quantra::Point_BondHelper:           return "BondHelper";
    case quantra::Point_OISHelper:            return "OISHelper";
    case quantra::Point_DatedOISHelper:       return "DatedOISHelper";
    case quantra::Point_ZeroRatePoint:        return "ZeroRatePoint";
    case quantra::Point_DiscountFactorPoint:  return "DiscountFactorPoint";
    case quantra::Point_ForwardRatePoint:     return "ForwardRatePoint";
    case quantra::Point_TenorBasisSwapHelper: return "TenorBasisSwapHelper";
    case quantra::Point_FxSwapHelper:         return "FxSwapHelper";
    case quantra::Point_CrossCcyBasisHelper:  return "CrossCcyBasisHelper";
    default:                                  return "unknown point type";
    }
}

} // namespace

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
        QUANTRA_INVALID_ARGUMENT("TermStructure not found");

    auto points = ts->points();
    if (!points || points->size() == 0)
        QUANTRA_INVALID_ARGUMENT("Empty list of points for term structure");

    if (!ts->day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("TermStructure.day_counter is required");
    if (!ts->interpolator().has_value())
        QUANTRA_INVALID_ARGUMENT("TermStructure.interpolator is required");
    if (!ts->bootstrap_trait().has_value())
        QUANTRA_INVALID_ARGUMENT("TermStructure.bootstrap_trait is required");

    // The bootstrap trait is the EXPLICIT curve-family selector. It decides how
    // the curve is built from its points; the point types are validated against
    // it (no auto-dispatch, no silent mixing).
    auto trait = ts->bootstrap_trait().value();

    switch (trait) {

    case enums::BootstrapTrait_InterpolatedDiscount: {
        // InterpolatedDiscount interpolates explicit discount-factor points
        // directly (no bootstrap), feeding a QuantLib InterpolatedDiscountCurve.
        // Every point MUST be a DiscountFactorPoint; a rate helper or any other
        // point type is a request error. This interpolates a DIFFERENT quantity
        // than InterpolatedZero (discount factors vs zero rates), so off-node
        // values differ for the same market data.
        std::vector<Date> dates;
        std::vector<DiscountFactor> dfs;
        dates.reserve(points->size());
        dfs.reserve(points->size());

        Date ref;
        if (ts->reference_date()) {
            ref = DateToQL(ts->reference_date()->str());
        } else {
            ref = Settings::instance().evaluationDate();
        }

        for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
            auto pw = points->Get(i);
            if (pw->point_type() != quantra::Point_DiscountFactorPoint) {
                QUANTRA_INVALID_ARGUMENT(
                    "bootstrap_trait InterpolatedDiscount builds from "
                    "discount-factor points but received a " +
                    pointTypeName(pw->point_type()));
            }
            auto p = pw->point_as_DiscountFactorPoint();
            if (!p) {
                QUANTRA_INVALID_ARGUMENT("DiscountFactorPoint is null");
            }

            Date d;
            if (p->date()) {
                d = DateToQL(p->date()->str());
            } else {
                if (!p->tenor()) {
                    QUANTRA_INVALID_ARGUMENT(
                        "DiscountFactorPoint.tenor is required when date is not provided");
                }
                int tenorN = p->tenor()->n();
                auto tenorUnit = p->tenor()->unit();
                if (tenorN <= 0) {
                    QUANTRA_INVALID_ARGUMENT("DiscountFactorPoint requires date or tenor");
                }
                auto cal = CalendarToQL(p->calendar());
                auto bdc = ConventionToQL(p->business_day_convention());
                d = cal.advance(ref, tenorN * TimeUnitToQL(tenorUnit), bdc);
            }

            if (!p->discount_factor().has_value()) {
                QUANTRA_INVALID_ARGUMENT("DiscountFactorPoint.discount_factor is required");
            }
            double df = p->discount_factor().value();
            if (!std::isfinite(df)) {
                QUANTRA_INVALID_ARGUMENT("DiscountFactorPoint.discount_factor must be finite");
            }
            // A discount factor must be positive and no greater than 1 for a sane
            // discount curve (DF(0)=1, monotonically decaying with a positive rate).
            if (df <= 0.0 || df > 1.0) {
                QUANTRA_INVALID_ARGUMENT(
                    "DiscountFactorPoint.discount_factor must be in (0, 1]");
            }
            // QuantLib's InterpolatedDiscountCurve uses the first pillar as the
            // reference date and requires its discount factor to be exactly 1.0.
            // Enforce it here so a violation is a named 400, not a QL 500.
            if (i == 0 && df != 1.0) {
                QUANTRA_INVALID_ARGUMENT(
                    "DiscountFactorPoint.discount_factor of the first (reference-date) "
                    "point must be exactly 1.0");
            }

            dates.push_back(d);
            dfs.push_back(df);
        }

        return buildDiscountCurve(ts, dates, dfs);
    }

    case enums::BootstrapTrait_InterpolatedFwd: {
        // InterpolatedFwd interpolates explicit instantaneous, continuously
        // compounded forward-rate points directly (no bootstrap), feeding a
        // QuantLib InterpolatedForwardCurve. Every point MUST be a
        // ForwardRatePoint; a rate helper or any other point type is a request
        // error. This interpolates the forward rate — a DIFFERENT quantity than
        // InterpolatedZero (zero rates) or InterpolatedDiscount (discount
        // factors) — so off-node values differ for the same market data.
        std::vector<Date> dates;
        std::vector<Rate> forwards;
        dates.reserve(points->size());
        forwards.reserve(points->size());

        Date ref;
        if (ts->reference_date()) {
            ref = DateToQL(ts->reference_date()->str());
        } else {
            ref = Settings::instance().evaluationDate();
        }

        for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
            auto pw = points->Get(i);
            if (pw->point_type() != quantra::Point_ForwardRatePoint) {
                QUANTRA_INVALID_ARGUMENT(
                    "bootstrap_trait InterpolatedFwd builds from forward-rate "
                    "points but received a " + pointTypeName(pw->point_type()));
            }
            auto p = pw->point_as_ForwardRatePoint();
            if (!p) {
                QUANTRA_INVALID_ARGUMENT("ForwardRatePoint is null");
            }

            Date d;
            if (p->date()) {
                d = DateToQL(p->date()->str());
            } else {
                if (!p->tenor()) {
                    QUANTRA_INVALID_ARGUMENT(
                        "ForwardRatePoint.tenor is required when date is not provided");
                }
                int tenorN = p->tenor()->n();
                auto tenorUnit = p->tenor()->unit();
                if (tenorN <= 0) {
                    QUANTRA_INVALID_ARGUMENT("ForwardRatePoint requires date or tenor");
                }
                auto cal = CalendarToQL(p->calendar());
                auto bdc = ConventionToQL(p->business_day_convention());
                d = cal.advance(ref, tenorN * TimeUnitToQL(tenorUnit), bdc);
            }

            if (!p->forward_rate().has_value()) {
                QUANTRA_INVALID_ARGUMENT("ForwardRatePoint.forward_rate is required");
            }
            double fwd = p->forward_rate().value();
            if (!std::isfinite(fwd)) {
                QUANTRA_INVALID_ARGUMENT("ForwardRatePoint.forward_rate must be finite");
            }
            // Forward rates may legitimately be negative in some markets, so no
            // positivity bound is imposed — only non-finite is rejected above.

            dates.push_back(d);
            forwards.push_back(fwd + bump);
        }

        return buildForwardCurve(ts, dates, forwards);
    }

    case enums::BootstrapTrait_InterpolatedZero: {
        // InterpolatedZero interpolates explicit zero-rate points directly (no
        // bootstrap). Every point MUST be a ZeroRatePoint; a rate helper is a
        // request error.
        std::vector<Date> dates;
        std::vector<Rate> zeroRates;
        dates.reserve(points->size());
        zeroRates.reserve(points->size());

        Compounding comp = QuantLib::Continuous;
        Frequency freq = QuantLib::Annual;
        bool convSet = false;

        Date ref;
        if (ts->reference_date()) {
            ref = DateToQL(ts->reference_date()->str());
        } else {
            ref = Settings::instance().evaluationDate();
        }

        for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
            auto pw = points->Get(i);
            if (pw->point_type() != quantra::Point_ZeroRatePoint) {
                QUANTRA_INVALID_ARGUMENT(
                    "bootstrap_trait InterpolatedZero builds from zero-rate "
                    "points but received a " + pointTypeName(pw->point_type()));
            }
            auto p = pw->point_as_ZeroRatePoint();
            if (!p) {
                QUANTRA_INVALID_ARGUMENT("ZeroRatePoint is null");
            }

            Date d;
            if (p->date()) {
                d = DateToQL(p->date()->str());
            } else {
                if (!p->tenor()) {
                    QUANTRA_INVALID_ARGUMENT("ZeroRatePoint.tenor is required when date is not provided");
                }
                int tenorN = p->tenor()->n();
                auto tenorUnit = p->tenor()->unit();
                if (tenorN <= 0) {
                    QUANTRA_INVALID_ARGUMENT("ZeroRatePoint requires date or tenor");
                }
                auto cal = CalendarToQL(p->calendar());
                auto bdc = ConventionToQL(p->business_day_convention());
                d = cal.advance(ref, tenorN * TimeUnitToQL(tenorUnit), bdc);
            }
            dates.push_back(d);
            if (!p->zero_rate().has_value()) {
                QUANTRA_INVALID_ARGUMENT("ZeroRatePoint.zero_rate is required");
            }
            zeroRates.push_back(p->zero_rate().value() + bump);

            // A single InterpolatedZeroCurve carries ONE compounding/frequency
            // convention. Applying the first point's pair to every rate would
            // silently mis-build the curve if the points disagree, so require
            // them identical across all points.
            Compounding pointComp = CompoundingToQL(p->compounding());
            Frequency pointFreq = FrequencyToQL(p->frequency());
            if (!convSet) {
                comp = pointComp;
                freq = pointFreq;
                convSet = true;
            } else {
                if (pointComp != comp) {
                    QUANTRA_INVALID_ARGUMENT(
                        "ZeroRatePoint.compounding must be identical across all points");
                }
                if (pointFreq != freq) {
                    QUANTRA_INVALID_ARGUMENT(
                        "ZeroRatePoint.frequency must be identical across all points");
                }
            }
        }

        return buildZeroCurve(ts, dates, zeroRates, comp, freq);
    }

    case enums::BootstrapTrait_Discount:
    case enums::BootstrapTrait_ZeroRate:
    case enums::BootstrapTrait_FwdRate: {
        // Bootstrap traits build a PiecewiseYieldCurve from rate helpers. A
        // ZeroRatePoint or DiscountFactorPoint is direct data, not a helper, so
        // it is a request error.
        TermStructurePointParser pointParser;
        std::vector<std::shared_ptr<RateHelper>> instruments;
        instruments.reserve(points->size());

        for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
            auto pw = points->Get(i);
            if (pw->point_type() == quantra::Point_ZeroRatePoint ||
                pw->point_type() == quantra::Point_DiscountFactorPoint ||
                pw->point_type() == quantra::Point_ForwardRatePoint) {
                QUANTRA_INVALID_ARGUMENT(
                    "bootstrap_trait " + bootstrapTraitName(trait) +
                    " builds from rate helpers but received a " +
                    pointTypeName(pw->point_type()));
            }
            auto point = pw->point();
            auto type  = pw->point_type();
            auto helper = pointParser.parse(type, point, quotes, curves, indices, bump);
            if (!helper)
                QUANTRA_INVALID_ARGUMENT("Failed to parse term structure point at index " + std::to_string(i));
            instruments.push_back(helper);
        }

        return buildCurve(ts, instruments);
    }

    default:
        QUANTRA_INVALID_ARGUMENT("Unsupported BootstrapTrait");
    }

    return nullptr;
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
    
    DayCounter dc = DayCounterToQL(ts->day_counter().value());

    switch (ts->interpolator().value()) {
    case enums::Interpolator_BackwardFlat:
        switch (ts->bootstrap_trait().value()) {
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
            QUANTRA_INVALID_ARGUMENT("Unsupported BootstrapTrait for BackwardFlat");
        }
        break;

    case enums::Interpolator_ForwardFlat:
        switch (ts->bootstrap_trait().value()) {
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
            QUANTRA_INVALID_ARGUMENT("Unsupported BootstrapTrait for ForwardFlat");
        }
        break;

    case enums::Interpolator_Linear:
        switch (ts->bootstrap_trait().value()) {
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
            QUANTRA_INVALID_ARGUMENT("Unsupported BootstrapTrait for Linear");
        }
        break;

    case enums::Interpolator_LogLinear:
        switch (ts->bootstrap_trait().value()) {
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
            QUANTRA_INVALID_ARGUMENT("Unsupported BootstrapTrait for LogLinear");
        }
        break;

    case enums::Interpolator_LogCubic:
        switch (ts->bootstrap_trait().value()) {
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
            QUANTRA_INVALID_ARGUMENT("Unsupported BootstrapTrait for LogCubic");
        }
        break;

    default:
        QUANTRA_INVALID_ARGUMENT("Unsupported Interpolator");
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
        QUANTRA_INVALID_ARGUMENT("Zero curve requires matching non-empty date/rate vectors");
    }

    DayCounter dc = DayCounterToQL(ts->day_counter().value());

    switch (ts->interpolator().value()) {
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
        QUANTRA_INVALID_ARGUMENT("Unsupported Interpolator for zero curve");
    }

    return nullptr;
}

// =============================================================================
// Build discount curve from explicit discount factors
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::buildDiscountCurve(
    const quantra::TermStructure* ts,
    const std::vector<Date>& dates,
    const std::vector<DiscountFactor>& discountFactors)
{
    if (dates.size() != discountFactors.size() || dates.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "Discount curve requires matching non-empty date/discount-factor vectors");
    }

    DayCounter dc = DayCounterToQL(ts->day_counter().value());

    // Mirrors buildZeroCurve's interpolator switch, but interpolates the
    // discount factors directly (InterpolatedDiscountCurve) instead of zero
    // rates. LogCubic uses MonotonicLogCubic, matching the zero-curve and
    // bootstrap paths.
    switch (ts->interpolator().value()) {
    case enums::Interpolator_Linear:
        return std::make_shared<InterpolatedDiscountCurve<Linear>>(
            dates, discountFactors, dc, Calendar(), Linear());
    case enums::Interpolator_LogLinear:
        return std::make_shared<InterpolatedDiscountCurve<LogLinear>>(
            dates, discountFactors, dc, Calendar(), LogLinear());
    case enums::Interpolator_LogCubic:
        return std::make_shared<InterpolatedDiscountCurve<LogCubic>>(
            dates, discountFactors, dc, Calendar(), MonotonicLogCubic());
    case enums::Interpolator_BackwardFlat:
        return std::make_shared<InterpolatedDiscountCurve<BackwardFlat>>(
            dates, discountFactors, dc, Calendar(), BackwardFlat());
    case enums::Interpolator_ForwardFlat:
        return std::make_shared<InterpolatedDiscountCurve<ForwardFlat>>(
            dates, discountFactors, dc, Calendar(), ForwardFlat());
    default:
        QUANTRA_INVALID_ARGUMENT("Unsupported Interpolator for discount curve");
    }

    return nullptr;
}

// =============================================================================
// Build forward curve from explicit instantaneous forward rates
// =============================================================================
std::shared_ptr<YieldTermStructure> TermStructureParser::buildForwardCurve(
    const quantra::TermStructure* ts,
    const std::vector<Date>& dates,
    const std::vector<Rate>& forwards)
{
    if (dates.size() != forwards.size() || dates.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "Forward curve requires matching non-empty date/forward-rate vectors");
    }

    DayCounter dc = DayCounterToQL(ts->day_counter().value());

    // Mirrors buildDiscountCurve's interpolator switch, but interpolates the
    // instantaneous forward rates directly (InterpolatedForwardCurve). QuantLib
    // derives zero rates from the interpolated forwards by integration
    // (primitive of the interpolation), so only interpolators that implement
    // that primitive are usable. Linear/BackwardFlat/ForwardFlat do;
    // LogLinear/LogCubic do NOT (QuantLib's LogInterpolation::primitive throws
    // "not implemented"), which would surface as a 500 at pricing time. Reject
    // them here as a named 400 instead.
    switch (ts->interpolator().value()) {
    case enums::Interpolator_Linear:
        return std::make_shared<InterpolatedForwardCurve<Linear>>(
            dates, forwards, dc, Calendar(), Linear());
    case enums::Interpolator_BackwardFlat:
        return std::make_shared<InterpolatedForwardCurve<BackwardFlat>>(
            dates, forwards, dc, Calendar(), BackwardFlat());
    case enums::Interpolator_ForwardFlat:
        return std::make_shared<InterpolatedForwardCurve<ForwardFlat>>(
            dates, forwards, dc, Calendar(), ForwardFlat());
    case enums::Interpolator_LogLinear:
    case enums::Interpolator_LogCubic:
        QUANTRA_INVALID_ARGUMENT(
            "InterpolatedFwd does not support log interpolators (LogLinear, "
            "LogCubic): QuantLib cannot integrate a log-interpolated forward "
            "rate. Use Linear, BackwardFlat or ForwardFlat.");
    default:
        QUANTRA_INVALID_ARGUMENT("Unsupported Interpolator for forward curve");
    }

    return nullptr;
}

} // namespace quantra
