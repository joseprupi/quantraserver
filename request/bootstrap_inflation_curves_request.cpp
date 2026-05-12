#include "bootstrap_inflation_curves_request.h"

#include <set>

#include "common_parser.h"
#include "error.h"
#include "grid_utils.h"

using namespace QuantLib;
using namespace quantra;

namespace {

} // namespace

flatbuffers::Offset<BootstrapInflationCurvesResponse> BootstrapInflationCurvesRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const BootstrapInflationCurvesRequest* request) const {
    if (!request || !request->pricing()) {
        QUANTRA_INVALID_ARGUMENT("BootstrapInflationCurvesRequest.pricing is required");
    }
    if (!request->queries()) {
        QUANTRA_INVALID_ARGUMENT("BootstrapInflationCurvesRequest.queries is required");
    }

    PricingRegistry reg;
    std::string pricingBuildError;
    try {
        PricingRegistryBuilder regBuilder;
        reg = regBuilder.build(request->pricing());
    } catch (const std::exception& e) {
        pricingBuildError = e.what();
    }
    const Date asOfDate = DateToQL(request->pricing()->as_of_date()->str());

    std::vector<flatbuffers::Offset<BootstrapInflationCurveResult>> results;
    auto querySpecs = request->queries();
    for (flatbuffers::uoffset_t i = 0; i < querySpecs->size(); ++i) {
        const auto* query = querySpecs->Get(i);
        std::string curveId = (query && query->curve_id()) ? query->curve_id()->str() : "";

        try {
            if (!pricingBuildError.empty()) {
                QUANTRA_INVALID_ARGUMENT(pricingBuildError);
            }
            if (curveId.empty()) {
                QUANTRA_INVALID_ARGUMENT("InflationCurveQuerySpec.curve_id is required");
            }
            if (!query->grid()) {
                QUANTRA_INVALID_ARGUMENT("InflationCurveQuerySpec.grid is required for curve_id: " + curveId);
            }
            if (!query->measures() || query->measures()->size() == 0) {
                QUANTRA_INVALID_ARGUMENT("InflationCurveQuerySpec.measures is required for curve_id: " + curveId);
            }

            // Find curve handle in either map.
            auto zIt = reg.inflation.zeroInflationCurves.find(curveId);
            auto yIt = reg.inflation.yoyInflationCurves.find(curveId);
            const bool hasZ = (zIt != reg.inflation.zeroInflationCurves.end());
            const bool hasY = (yIt != reg.inflation.yoyInflationCurves.end());
            if (!hasZ && !hasY) {
                QUANTRA_NOT_FOUND("Inflation curve id not found in PricingRegistry: " + curveId);
            }
            if (hasZ && hasY) {
                QUANTRA_INVALID_ARGUMENT("Inflation curve id exists as both zero and YoY: " + curveId);
            }

            std::shared_ptr<QuantLib::ZeroInflationTermStructure> zc;
            std::shared_ptr<QuantLib::YoYInflationTermStructure> yy;
            if (hasZ) {
                if (!zIt->second || zIt->second->empty()) {
                    QUANTRA_INVALID_ARGUMENT("Zero inflation curve handle has no linked curve for id: " + curveId);
                }
                zc = zIt->second->currentLink();
            } else {
                if (!yIt->second || yIt->second->empty()) {
                    QUANTRA_INVALID_ARGUMENT("YoY inflation curve handle has no linked curve for id: " + curveId);
                }
                yy = yIt->second->currentLink();
            }

            const auto* options = query->options();
            const bool allowExtrapolation = !options || options->allow_extrapolation();
            const bool strictMode = !options || options->strict();

            auto metaIt = reg.inflation.curveMetadata.find(curveId);
            if (metaIt == reg.inflation.curveMetadata.end()) {
                QUANTRA_INVALID_ARGUMENT("Inflation curve metadata missing for id: " + curveId);
            }
            const auto& meta = metaIt->second;

            Date referenceDate = hasZ ? zc->referenceDate() : yy->referenceDate();
            if (strictMode && referenceDate != asOfDate) {
                std::ostringstream err;
                err << "Strict mode: pricing.as_of_date (" << io::iso_date(asOfDate)
                    << ") must equal inflation curve referenceDate ("
                    << io::iso_date(referenceDate) << ") for curve '" << curveId << "'";
                QUANTRA_INVALID_ARGUMENT(err.str());
            }

            if (allowExtrapolation) {
                if (hasZ) zc->enableExtrapolation(); else yy->enableExtrapolation();
            } else {
                if (hasZ) zc->disableExtrapolation(); else yy->disableExtrapolation();
            }

            Calendar fallbackCal = meta.calendar;
            BusinessDayConvention fallbackBdc = meta.businessDayConvention;

            const Calendar gridCal = grid_utils::ResolveCalendar(query->grid(), options, fallbackCal);
            const BusinessDayConvention gridBdc =
                grid_utils::ResolveBusinessDayConvention(query->grid(), options, fallbackBdc);

            std::vector<Date> gridDates;
            if (query->grid()->grid_type() == DateGrid_TenorGrid) {
                auto* tenorGrid = query->grid()->grid_as_TenorGrid();
                if (tenorGrid && tenorGrid->calendar() == enums::Calendar_NullCalendar) {
                    // Ensure inflation queries consistently use calendar-based advance.
                    gridDates = grid_utils::BuildTenorGrid(tenorGrid, referenceDate, gridCal, true);
                } else {
                    gridDates = grid_utils::BuildTenorGrid(tenorGrid, referenceDate, fallbackCal, true);
                }
            } else if (query->grid()->grid_type() == DateGrid_RangeGrid) {
                const int maxPoints = (options && options->max_points() > 0) ? options->max_points() : 50000;
                gridDates = grid_utils::BuildRangeGrid(query->grid()->grid_as_RangeGrid(), asOfDate, maxPoints);
            } else {
                QUANTRA_INVALID_ARGUMENT("DateGridSpec.grid is required for curve_id: " + curveId);
            }

            std::vector<flatbuffers::Offset<flatbuffers::String>> gridDateStrings;
            gridDateStrings.reserve(gridDates.size());
            for (const auto& d : gridDates) {
                gridDateStrings.push_back(builder->CreateString(grid_utils::ToIsoDate(d)));
            }

            std::vector<flatbuffers::Offset<InflationCurveSeries>> seriesVector;
            for (flatbuffers::uoffset_t m = 0; m < query->measures()->size(); ++m) {
                enums::InflationCurveMeasure measure =
                    static_cast<enums::InflationCurveMeasure>(query->measures()->Get(m));
                std::vector<double> values;
                values.reserve(gridDates.size());

                for (const auto& d : gridDates) {
                    if (!allowExtrapolation) {
                        Date maxDate = hasZ ? zc->maxDate() : yy->maxDate();
                        if (d > maxDate) {
                            QUANTRA_INVALID_ARGUMENT("Date is outside inflation curve support");
                        }
                    }
                    if (measure == enums::InflationCurveMeasure_ZeroRate) {
                        if (!hasZ) QUANTRA_INVALID_ARGUMENT("ZeroRate measure requires a zero inflation curve");
                        values.push_back(zc->zeroRate(d));
                    } else if (measure == enums::InflationCurveMeasure_YoYRate) {
                        if (!hasY) QUANTRA_INVALID_ARGUMENT("YoYRate measure requires a YoY inflation curve");
                        values.push_back(yy->yoyRate(d));
                    } else {
                        QUANTRA_INVALID_ARGUMENT("Unsupported InflationCurveMeasure for curve_id: " + curveId);
                    }
                }

                auto valuesVec = builder->CreateVector(values);
                InflationCurveSeriesBuilder seriesBuilder(*builder);
                seriesBuilder.add_measure(measure);
                seriesBuilder.add_values(valuesVec);
                seriesVector.push_back(seriesBuilder.Finish());
            }

            std::vector<Date> pillarDates = meta.pillarDates;
            std::vector<flatbuffers::Offset<flatbuffers::String>> pillarDateStrings;
            pillarDateStrings.reserve(pillarDates.size());
            for (const auto& d : pillarDates) {
                pillarDateStrings.push_back(builder->CreateString(grid_utils::ToIsoDate(d)));
            }

            auto idStr = builder->CreateString(curveId);
            auto refDateStr = builder->CreateString(grid_utils::ToIsoDate(referenceDate));
            auto gridDatesVec = builder->CreateVector(gridDateStrings);
            auto seriesVec = builder->CreateVector(seriesVector);
            auto pillarDatesVec = builder->CreateVector(pillarDateStrings);

            BootstrapInflationCurveResultBuilder resultBuilder(*builder);
            resultBuilder.add_id(idStr);
            resultBuilder.add_reference_date(refDateStr);
            resultBuilder.add_grid_dates(gridDatesVec);
            resultBuilder.add_series(seriesVec);
            resultBuilder.add_pillar_dates(pillarDatesVec);
            results.push_back(resultBuilder.Finish());
        } catch (const std::exception& e) {
            auto idStr = builder->CreateString(curveId);
            auto errorMsg = builder->CreateString(e.what());
            ErrorBuilder errorBuilder(*builder);
            errorBuilder.add_error_message(errorMsg);
            auto error = errorBuilder.Finish();

            BootstrapInflationCurveResultBuilder resultBuilder(*builder);
            resultBuilder.add_id(idStr);
            resultBuilder.add_error(error);
            results.push_back(resultBuilder.Finish());
        }
    }

    auto resultsVec = builder->CreateVector(results);
    BootstrapInflationCurvesResponseBuilder responseBuilder(*builder);
    responseBuilder.add_results(resultsVec);
    return responseBuilder.Finish();
}

