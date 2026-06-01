#include "bootstrap_curves_evaluator.h"

#include <memory>

#include <ql/interestrate.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "error.h"

namespace quantra {

namespace {

double sampleDiscount(const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
                      const QuantLib::Date& d) {
    return curve->discount(d);
}

double sampleZero(const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
                  const QuantLib::Date& d,
                  const ZeroSampleSpec& spec) {
    QuantLib::DayCounter dc =
        spec.useCurveDayCounter ? curve->dayCounter() : spec.dayCounter;
    QuantLib::Date refDate = curve->referenceDate();
    QuantLib::Date dd = (d <= refDate) ? refDate + 1 : d;
    return curve->zeroRate(dd, dc, spec.compounding, spec.frequency).rate();
}

double sampleForward(const std::shared_ptr<QuantLib::YieldTermStructure>& curve,
                     const QuantLib::Date& d,
                     const ForwardSampleSpec& spec) {
    QuantLib::DayCounter dc =
        spec.useCurveDayCounter ? curve->dayCounter() : spec.dayCounter;
    QuantLib::Date endDate;
    if (spec.instantaneous) {
        endDate = (spec.epsUnit == QuantLib::Days)
            ? d + spec.epsNumber
            : spec.calendar.advance(
                  d, QuantLib::Period(spec.epsNumber, spec.epsUnit), spec.bdc);
    } else {
        endDate = spec.calendar.advance(d, spec.tenor, spec.bdc);
    }
    if (endDate <= d) endDate = d + 1;
    return curve->forwardRate(d, endDate, dc, spec.compounding, spec.frequency).rate();
}

BootstrapCurvesPerCurve sampleQuery(const BootstrapCurvesQuery& query,
                                    const PricingRegistry& reg) {
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

    if (query.allowExtrapolation) {
        curve->enableExtrapolation();
    } else {
        curve->disableExtrapolation();
    }

    out.referenceDate = curve->referenceDate();

    out.series.reserve(query.measures.size());
    for (auto measure : query.measures) {
        BootstrapCurvesSeries series;
        series.measure = measure;
        series.values.reserve(query.gridDates.size());

        switch (measure) {
        case CurveSampleMeasure::DiscountFactor:
            for (const auto& d : query.gridDates) {
                series.values.push_back(sampleDiscount(curve, d));
            }
            break;
        case CurveSampleMeasure::ZeroRate:
            for (const auto& d : query.gridDates) {
                series.values.push_back(sampleZero(curve, d, query.zero));
            }
            break;
        case CurveSampleMeasure::ForwardRate:
            for (const auto& d : query.gridDates) {
                series.values.push_back(sampleForward(curve, d, query.fwd));
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
