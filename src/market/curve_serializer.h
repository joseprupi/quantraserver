#ifndef QUANTRASERVER_CURVE_SERIALIZER_H
#define QUANTRASERVER_CURVE_SERIALIZER_H

#include "curve_cache.h"
#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

#include "term_structure_generated.h"

#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/forwardflatinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/termstructures/yield/forwardcurve.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace quantra {

/**
 * A bootstrapped curve's nodes on the interpolated curve the bootstrap fills
 * (InterpolatedDiscountCurve, InterpolatedZeroCurve or InterpolatedForwardCurve
 * for the Discount, ZeroYield and ForwardRate traits). The bootstrap takes the
 * max date from the helpers, so it is carried over as well.
 */
template <class Base> class FrozenYieldCurve : public Base {
public:
    template <class... Args>
    explicit FrozenYieldCurve(const QuantLib::Date& maxDate, Args&&... args)
        : Base(std::forward<Args>(args)...) {
        this->maxDate_ = maxDate;
    }
};

/**
 * CurveSerializer - Converts between live QuantLib curves and CachedCurveData.
 *
 * Serialization: extracts ACTUAL pillar dates + discount factors from a
 * bootstrapped PiecewiseYieldCurve. This is critical for exact reconstruction —
 * at pillar nodes the values match exactly, and between pillars the same
 * interpolator reproduces identical values.
 *
 * Reconstruction: builds an InterpolatedDiscountCurve from cached DFs.
 *
 * Freezing: copies a bootstrapped curve, without its rate helpers, for the
 * in-process (L1) cache.
 */
class CurveSerializer {
public:
    /**
     * Helper-free copy of a curve TermStructureParser bootstrapped. A
     * PiecewiseYieldCurve observes its rate helpers, and QuantLib's tenor
     * helpers re-date themselves on every evaluation-date change, with the
     * calendars in force at that moment: a cached bootstrapped curve would be
     * re-dated and re-bootstrapped by other requests. The copy is the curve's
     * own interpolated base built from its nodes (dates and data), with the
     * same interpolator, day counter, calendar, max date and extrapolation
     * setting, and observes nothing.
     *
     * Returns nullptr, and the curve is then not to be cached, when it is not
     * a PiecewiseYieldCurve the parser builds, when it fails to bootstrap (the
     * request meets the error where it uses the curve, as without the cache),
     * or when the copy does not return exactly the curve's times and discount
     * factors at every node, halfway between nodes and a year past the last.
     */
    static std::shared_ptr<QuantLib::YieldTermStructure> freeze(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve) {
        try {
            if (auto frozen = freezeTrait<QuantLib::Discount>(curve)) return frozen;
            if (auto frozen = freezeTrait<QuantLib::ZeroYield>(curve)) return frozen;
            return freezeTrait<QuantLib::ForwardRate>(curve);
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    /**
     * True iff serialize() can extract the curve's ACTUAL bootstrap pillars,
     * i.e. the curve is a PiecewiseYieldCurve matching the TermStructure's
     * (trait, interpolator) so reconstruct(serialize(...)) reproduces it
     * value-exactly at and between pillars.
     *
     * Curves built any other way — e.g. a ZeroRatePoint curve (an
     * InterpolatedZeroCurve), a DiscountFactorPoint curve (an
     * InterpolatedDiscountCurve) or a ForwardRatePoint curve (an
     * InterpolatedForwardCurve) — are not PiecewiseYieldCurves, so
     * tryExtractPillars() below fails and serialize() would WEEKLY-RESAMPLE
     * them, an approximation that must never be served on a cache hit.
     * Callers must check this before storing serialized data.
     */
    static bool canSerializeExactly(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const quantra::TermStructure* ts) {
        std::vector<QuantLib::Date> pillars;
        return tryExtractPillars(curve, ts, pillars);
    }

    /**
     * Extract pillar dates + DFs from a bootstrapped curve.
     *
     * Attempts to extract actual pillar nodes from PiecewiseYieldCurve by
     * trying all supported (Trait, Interpolator) combinations. Falls back to
     * dense weekly sampling if the downcast fails — an APPROXIMATION of the
     * live curve. Callers that cache the result must gate on
     * canSerializeExactly() first so the fallback is never served on a
     * cache hit.
     */
    static CachedCurveData serialize(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const quantra::TermStructure* ts) {
        CachedCurveData data;

        // Metadata
        auto refDate = curve->referenceDate();
        data.reference_date = DateToIso(refDate);
        // Reached only after a successful bootstrap, where TermStructureParser
        // has already required both fields to be present.
        data.day_counter = static_cast<uint8_t>(ts->day_counter().value());
        data.interpolator = static_cast<uint8_t>(ts->interpolator().value());

        // Try to extract actual pillar nodes from PiecewiseYieldCurve
        std::vector<QuantLib::Date> pillarDates;
        bool extracted = tryExtractPillars(curve, ts, pillarDates);

        if (!extracted) {
            // Fallback: dense sampling (shouldn't normally happen)
            pillarDates = generateFallbackDates(curve);
        }

        // Store dates and discount factors at pillars
        data.dates.reserve(pillarDates.size());
        data.discount_factors.reserve(pillarDates.size());

        for (const auto& dt : pillarDates) {
            data.dates.push_back(DateToIso(dt));
            data.discount_factors.push_back(curve->discount(dt));
        }

        return data;
    }

    /**
     * Reconstruct a QuantLib YieldTermStructure from cached data.
     *
     * Builds an InterpolatedDiscountCurve using the stored pillar DFs.
     * Always stores/reconstructs DFs regardless of original bootstrap trait.
     */
    static std::shared_ptr<QuantLib::YieldTermStructure> reconstruct(const CachedCurveData& data) {
        std::vector<QuantLib::Date> dates;
        dates.reserve(data.dates.size());
        for (const auto& ds : data.dates) {
            dates.push_back(DateToQL(ds));
        }

        QuantLib::DayCounter dc =
            DayCounterToQL(static_cast<quantra::enums::DayCounter>(data.day_counter));

        auto interpolator = static_cast<quantra::enums::Interpolator>(data.interpolator);

        std::shared_ptr<QuantLib::YieldTermStructure> curve;

        switch (interpolator) {
            case quantra::enums::Interpolator_LogLinear:
                curve = std::make_shared<QuantLib::InterpolatedDiscountCurve<QuantLib::LogLinear>>(
                    dates, data.discount_factors, dc);
                break;

            case quantra::enums::Interpolator_Linear:
                curve = std::make_shared<QuantLib::InterpolatedDiscountCurve<QuantLib::Linear>>(
                    dates, data.discount_factors, dc);
                break;

            case quantra::enums::Interpolator_BackwardFlat:
                curve =
                    std::make_shared<QuantLib::InterpolatedDiscountCurve<QuantLib::BackwardFlat>>(
                        dates, data.discount_factors, dc);
                break;

            case quantra::enums::Interpolator_ForwardFlat:
                curve =
                    std::make_shared<QuantLib::InterpolatedDiscountCurve<QuantLib::ForwardFlat>>(
                        dates, data.discount_factors, dc);
                break;

            case quantra::enums::Interpolator_LogCubic:
                curve = std::make_shared<QuantLib::InterpolatedDiscountCurve<QuantLib::LogCubic>>(
                    dates, data.discount_factors, dc, QuantLib::MonotonicLogCubic());
                break;

            default:
                // Fail closed: substituting a different interpolator would make a
                // cache hit price differently than a miss between pillars. Callers
                // catch this and treat the entry as a miss.
                QUANTRA_ERROR(
                    "CurveSerializer::reconstruct: unsupported interpolator enum " +
                    std::to_string(static_cast<int>(data.interpolator)));
        }

        // No enableExtrapolation(): live curves built by TermStructureParser
        // never enable it, so a request past the last pillar throws there. The
        // frozen reconstruction must fail identically, or a cache hit would
        // silently turn that error into an extrapolated number.
        return curve;
    }

private:
    /// The interpolators TermStructureParser::buildCurve builds with.
    template <class Trait>
    static std::shared_ptr<QuantLib::YieldTermStructure> freezeTrait(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve) {
        if (auto frozen = freezeAs<Trait>(curve, QuantLib::Linear())) return frozen;
        if (auto frozen = freezeAs<Trait>(curve, QuantLib::LogLinear())) return frozen;
        if (auto frozen = freezeAs<Trait>(curve, QuantLib::BackwardFlat())) return frozen;
        if (auto frozen = freezeAs<Trait>(curve, QuantLib::ForwardFlat())) return frozen;
        return freezeAs<Trait, QuantLib::LogCubic>(curve, QuantLib::MonotonicLogCubic());
    }

    template <class Trait, class Interpolator>
    static std::shared_ptr<QuantLib::YieldTermStructure> freezeAs(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const Interpolator& interpolator) {
        auto live =
            std::dynamic_pointer_cast<QuantLib::PiecewiseYieldCurve<Trait, Interpolator>>(curve);
        // The parser builds no jumps; a curve with jumps is not copied.
        if (!live || !live->jumpDates().empty()) return nullptr;
        using Base = typename Trait::template curve<Interpolator>::type;
        auto frozen = std::make_shared<FrozenYieldCurve<Base>>(
            live->maxDate(), live->dates(), live->data(), live->dayCounter(), live->calendar(),
            interpolator);
        if (live->allowsExtrapolation()) frozen->enableExtrapolation();
        if (frozen->times() != live->times() || frozen->referenceDate() != live->referenceDate() ||
            frozen->maxDate() != live->maxDate() || !sameDiscounts(*live, *frozen, live->times())) {
            return nullptr;
        }
        return frozen;
    }

    /// Exact equality of the discount factors at every node, halfway between
    /// nodes and a year past the last node.
    static bool sameDiscounts(
        const QuantLib::YieldTermStructure& a,
        const QuantLib::YieldTermStructure& b,
        const std::vector<QuantLib::Time>& times) {
        if (times.empty()) return false;
        for (std::size_t i = 0; i < times.size(); ++i) {
            if (a.discount(times[i], true) != b.discount(times[i], true)) return false;
            if (i + 1 < times.size()) {
                const QuantLib::Time mid = 0.5 * (times[i] + times[i + 1]);
                if (a.discount(mid, true) != b.discount(mid, true)) return false;
            }
        }
        const QuantLib::Time past = times.back() + 1.0;
        return a.discount(past, true) == b.discount(past, true);
    }

    /**
     * Try to extract actual pillar dates from a PiecewiseYieldCurve.
     *
     * PiecewiseYieldCurve<Trait, Interp>::dates() returns the bootstrap nodes.
     * We try the (trait, interpolator) combo specified in the TermStructure.
     */
    static bool tryExtractPillars(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
        const quantra::TermStructure* ts,
        std::vector<QuantLib::Date>& out) {
#define TRY_EXTRACT(TraitT, InterpT)                                                               \
    {                                                                                              \
        auto ptr = std::dynamic_pointer_cast<                                                      \
            QuantLib::PiecewiseYieldCurve<QuantLib::TraitT, QuantLib::InterpT>>(curve);            \
        if (ptr) {                                                                                 \
            out = ptr->dates();                                                                    \
            return true;                                                                           \
        }                                                                                          \
    }

        auto interp = ts->interpolator().value();
        // bootstrap_trait is presence-required and always set by the time a curve
        // has bootstrapped; an absent trait cannot produce an extractable
        // PiecewiseYieldCurve, so treat it as non-exact.
        if (!ts->bootstrap_trait().has_value()) return false;
        auto trait = ts->bootstrap_trait().value();

        // Try the expected combination first (most likely hit)
        switch (interp) {
            case enums::Interpolator_LogLinear:
                if (trait == enums::BootstrapTrait_Discount) TRY_EXTRACT(Discount, LogLinear);
                if (trait == enums::BootstrapTrait_ZeroRate) TRY_EXTRACT(ZeroYield, LogLinear);
                if (trait == enums::BootstrapTrait_FwdRate) TRY_EXTRACT(ForwardRate, LogLinear);
                break;
            case enums::Interpolator_Linear:
                if (trait == enums::BootstrapTrait_Discount) TRY_EXTRACT(Discount, Linear);
                if (trait == enums::BootstrapTrait_ZeroRate) TRY_EXTRACT(ZeroYield, Linear);
                if (trait == enums::BootstrapTrait_FwdRate) TRY_EXTRACT(ForwardRate, Linear);
                break;
            case enums::Interpolator_BackwardFlat:
                if (trait == enums::BootstrapTrait_Discount) TRY_EXTRACT(Discount, BackwardFlat);
                if (trait == enums::BootstrapTrait_ZeroRate) TRY_EXTRACT(ZeroYield, BackwardFlat);
                if (trait == enums::BootstrapTrait_FwdRate) TRY_EXTRACT(ForwardRate, BackwardFlat);
                break;
            case enums::Interpolator_ForwardFlat:
                if (trait == enums::BootstrapTrait_Discount) TRY_EXTRACT(Discount, ForwardFlat);
                if (trait == enums::BootstrapTrait_ZeroRate) TRY_EXTRACT(ZeroYield, ForwardFlat);
                if (trait == enums::BootstrapTrait_FwdRate) TRY_EXTRACT(ForwardRate, ForwardFlat);
                break;
            case enums::Interpolator_LogCubic:
                if (trait == enums::BootstrapTrait_Discount) TRY_EXTRACT(Discount, LogCubic);
                if (trait == enums::BootstrapTrait_ZeroRate) TRY_EXTRACT(ZeroYield, LogCubic);
                if (trait == enums::BootstrapTrait_FwdRate) TRY_EXTRACT(ForwardRate, LogCubic);
                break;
            default: break;
        }

#undef TRY_EXTRACT

        return false;
    }

    /**
     * Fallback: generate dense sample dates if we can't extract pillars.
     * Weekly sampling gives reasonable accuracy.
     */
    static std::vector<QuantLib::Date> generateFallbackDates(
        const std::shared_ptr<QuantLib::YieldTermStructure>& curve) {
        std::vector<QuantLib::Date> dates;
        auto refDate = curve->referenceDate();
        auto maxDate = curve->maxDate();

        dates.push_back(refDate);

        QuantLib::Date d = refDate;
        while (d < maxDate) {
            d += QuantLib::Period(1, QuantLib::Weeks);
            if (d > maxDate) d = maxDate;
            dates.push_back(d);
        }

        std::sort(dates.begin(), dates.end());
        dates.erase(std::unique(dates.begin(), dates.end()), dates.end());

        return dates;
    }
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_SERIALIZER_H
