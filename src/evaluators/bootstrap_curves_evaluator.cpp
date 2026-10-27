#include "bootstrap_curves_evaluator.h"

#include "error.h"

#include <ql/interestrate.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include <memory>

namespace quantra {

namespace {

double sampleDiscount(
    const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
    const QuantLib::Date& d,
    bool extrapolate) {
    return curve->discount(d, extrapolate);
}

double sampleZero(
    const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
    const QuantLib::Date& d,
    const ZeroSampleSpec& spec,
    bool extrapolate) {
    QuantLib::DayCounter dc = spec.useCurveDayCounter ? curve->dayCounter() : spec.dayCounter;
    // A sample on or before the reference date is the t -> 0 limit of the zero
    // rate. QuantLib's zeroRate(Date) computes that limit itself when the date
    // IS the reference date (yieldtermstructure.cpp: t == 0 -> the rate over
    // dt = 1e-4 years, which is the first pillar's zero for a flat curve and
    // for the log-linear discount curves the bootstrap builds, and within
    // 1e-4 x slope of it for a linearly interpolated zero curve), so the clamp
    // is to the reference date itself. Clamping one day past it (the previous
    // behaviour) returned the one-day zero, which differs from the limit on a
    // curve whose zero is interpolated from t = 0.
    const QuantLib::Date refDate = curve->referenceDate();
    const QuantLib::Date dd = (d < refDate) ? refDate : d;
    return curve->zeroRate(dd, dc, spec.compounding, spec.frequency, extrapolate).rate();
}

double sampleForward(
    const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
    const QuantLib::Date& d,
    const ForwardSampleSpec& spec,
    bool extrapolate) {
    QuantLib::DayCounter dc = spec.useCurveDayCounter ? curve->dayCounter() : spec.dayCounter;
    QuantLib::Date endDate;
    if (spec.instantaneous) {
        // The eps of an instantaneous forward is a step on the curve's time
        // axis, not a settlement period: in Days it is raw calendar days on
        // purpose (a 1-day eps over a weekend stays 1/365, not 3/365), while
        // Weeks / Months / Years go through the calendar like any tenor. Kept
        // as is (documented, not changed) in 0.13.0.
        endDate = (spec.epsUnit == QuantLib::Days)
                      ? d + spec.epsNumber
                      : spec.calendar.advance(
                            d, QuantLib::Period(spec.epsNumber, spec.epsUnit), spec.bdc);
    } else {
        endDate = spec.calendar.advance(d, spec.tenor, spec.bdc);
    }
    // A calendar advance with a Preceding convention (or a zero tenor) can land
    // on or before `d`; forwardRate needs a positive period, so fall back to
    // the next calendar day.
    if (endDate <= d) endDate = d + 1;
    return curve->forwardRate(d, endDate, dc, spec.compounding, spec.frequency, extrapolate).rate();
}

BootstrapCurvesPerCurve sampleQuery(const BootstrapCurvesQuery& query, const PricingRegistry& reg) {
    BootstrapCurvesPerCurve out;
    out.id = query.curveId;
    out.gridDates = query.gridDates;
    out.pillarDates = query.pillarDates;

    auto it = reg.rates.curves.find(query.curveId);
    if (it == reg.rates.curves.end() || !it->second || it->second->empty()) {
        QUANTRA_NOT_FOUND("Curve id not found in PricingRegistry: " + query.curveId);
    }
    auto curve = it->second->currentLink();
    if (!curve) {
        QUANTRA_NOT_FOUND("Curve handle has no linked curve for id: " + query.curveId);
    }

    // The query's option goes to each read, never onto the curve: with the
    // curve cache on, the curve object is shared with later requests. Curves
    // are built without extrapolation, so `false` keeps QuantLib's range check.
    const bool extrapolate = query.allowExtrapolation;

    out.referenceDate = curve->referenceDate();

    out.series.reserve(query.measures.size());
    for (auto measure : query.measures) {
        BootstrapCurvesSeries series;
        series.measure = measure;
        series.values.reserve(query.gridDates.size());

        switch (measure) {
            case CurveSampleMeasure::DiscountFactor:
                for (const auto& d : query.gridDates) {
                    series.values.push_back(sampleDiscount(curve, d, extrapolate));
                }
                break;
            case CurveSampleMeasure::ZeroRate:
                for (const auto& d : query.gridDates) {
                    series.values.push_back(sampleZero(curve, d, query.zero, extrapolate));
                }
                break;
            case CurveSampleMeasure::ForwardRate:
                for (const auto& d : query.gridDates) {
                    series.values.push_back(sampleForward(curve, d, query.fwd, extrapolate));
                }
                break;
        }
        out.series.push_back(std::move(series));
    }
    return out;
}

} // namespace

BootstrapCurvesResult BootstrapCurvesEvaluator::evaluate(
    const BootstrapCurvesInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)ctx;

    BootstrapCurvesResult result;
    result.curves.reserve(inputs.queries.size());
    for (const auto& query : inputs.queries) {
        if (!query.prebuiltError.empty()) {
            BootstrapCurvesPerCurve errEntry;
            errEntry.id = query.curveId;
            errEntry.error = query.prebuiltError;
            result.curves.push_back(std::move(errEntry));
            continue;
        }
        try {
            result.curves.push_back(sampleQuery(query, reg));
        } catch (const std::exception& e) {
            BootstrapCurvesPerCurve errEntry;
            errEntry.id = query.curveId;
            errEntry.error = e.what();
            result.curves.push_back(std::move(errEntry));
        }
    }
    return result;
}

} // namespace quantra
