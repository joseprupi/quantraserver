#include "bootstrap_inflation_curves_evaluator.h"

#include <memory>
#include <sstream>

#include <ql/termstructures/inflationtermstructure.hpp>

#include "common.h"
#include "error.h"

namespace quantra {

namespace {

BootstrapInflationCurvesPerCurve sampleQuery(
    const BootstrapInflationCurvesQuery& query,
    const PricingRegistry& reg) {
    BootstrapInflationCurvesPerCurve out;
    out.id = query.curveId;
    out.gridDates = query.gridDates;

    auto zIt = reg.inflation.zeroInflationCurves.find(query.curveId);
    auto yIt = reg.inflation.yoyInflationCurves.find(query.curveId);
    const bool hasZ = (zIt != reg.inflation.zeroInflationCurves.end());
    const bool hasY = (yIt != reg.inflation.yoyInflationCurves.end());
    if (!hasZ && !hasY) {
        QUANTRA_NOT_FOUND("Inflation curve id not found in PricingRegistry: " + query.curveId);
    }
    if (hasZ && hasY) {
        QUANTRA_ERROR("Inflation curve id exists as both zero and YoY: " + query.curveId);
    }

    std::shared_ptr<QuantLib::ZeroInflationTermStructure> zc;
    std::shared_ptr<QuantLib::YoYInflationTermStructure> yy;
    if (hasZ) {
        if (!zIt->second || zIt->second->empty()) {
            QUANTRA_ERROR("Zero inflation curve handle has no linked curve for id: " + query.curveId);
        }
        zc = zIt->second->currentLink();
    } else {
        if (!yIt->second || yIt->second->empty()) {
            QUANTRA_ERROR("YoY inflation curve handle has no linked curve for id: " + query.curveId);
        }
        yy = yIt->second->currentLink();
    }

    auto metaIt = reg.inflation.curveMetadata.find(query.curveId);
    if (metaIt == reg.inflation.curveMetadata.end()) {
        QUANTRA_ERROR("Inflation curve metadata missing for id: " + query.curveId);
    }
    const auto& meta = metaIt->second;

    QuantLib::Date referenceDate = hasZ ? zc->referenceDate() : yy->referenceDate();
    if (query.strict && referenceDate != query.asOfDate) {
        std::ostringstream err;
        err << "Strict mode: pricing.as_of_date (" << DateToIso(query.asOfDate)
            << ") must equal inflation curve referenceDate ("
            << DateToIso(referenceDate) << ") for curve '" << query.curveId << "'";
        QUANTRA_INVALID_ARGUMENT(err.str());
    }

    if (query.allowExtrapolation) {
        if (hasZ) zc->enableExtrapolation(); else yy->enableExtrapolation();
    } else {
        if (hasZ) zc->disableExtrapolation(); else yy->disableExtrapolation();
    }

    out.referenceDate = referenceDate;
    out.pillarDates = meta.pillarDates;

    out.series.reserve(query.measures.size());
    for (auto measure : query.measures) {
        BootstrapInflationCurvesSeries series;
        series.measure = measure;
        series.values.reserve(query.gridDates.size());

        for (const auto& d : query.gridDates) {
            if (!query.allowExtrapolation) {
                QuantLib::Date maxDate = hasZ ? zc->maxDate() : yy->maxDate();
                if (d > maxDate) {
                    QUANTRA_INVALID_ARGUMENT("Date is outside inflation curve support");
                }
            }
            if (measure == InflationCurveSampleMeasure::ZeroRate) {
                if (!hasZ) QUANTRA_INVALID_ARGUMENT("ZeroRate measure requires a zero inflation curve");
                series.values.push_back(zc->zeroRate(d));
            } else if (measure == InflationCurveSampleMeasure::YoYRate) {
                if (!hasY) QUANTRA_INVALID_ARGUMENT("YoYRate measure requires a YoY inflation curve");
                series.values.push_back(yy->yoyRate(d));
            } else {
                QUANTRA_INVALID_ARGUMENT("Unsupported InflationCurveMeasure for curve_id: " + query.curveId);
            }
        }
        out.series.push_back(std::move(series));
    }
    return out;
}

} // namespace

BootstrapInflationCurvesResult BootstrapInflationCurvesEvaluator::evaluate(
    const BootstrapInflationCurvesInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)ctx;

    BootstrapInflationCurvesResult result;
    result.curves.reserve(inputs.queries.size());
    for (const auto& query : inputs.queries) {
        if (!query.prebuiltError.empty()) {
            BootstrapInflationCurvesPerCurve errEntry;
            errEntry.id = query.curveId;
            errEntry.error = query.prebuiltError;
            result.curves.push_back(std::move(errEntry));
            continue;
        }
        try {
            result.curves.push_back(sampleQuery(query, reg));
        } catch (const std::exception& e) {
            BootstrapInflationCurvesPerCurve errEntry;
            errEntry.id = query.curveId;
            errEntry.error = e.what();
            result.curves.push_back(std::move(errEntry));
        }
    }
    return result;
}

} // namespace quantra
