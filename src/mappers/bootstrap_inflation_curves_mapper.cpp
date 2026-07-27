#include "bootstrap_inflation_curves_mapper.h"

#include <utility>

#include <ql/time/calendars/target.hpp>

#include "date_convert.h"
#include "enum_convert.h"
#include "request_validation.h"
#include "error.h"
#include "grid_utils.h"

namespace quantra {

namespace {

const quantra::InflationCurveSpec* findInflationCurveSpecById(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::InflationCurveSpec>>* curves,
    const std::string& curveId) {
    if (!curves) return nullptr;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); ++i) {
        const auto* spec = curves->Get(i);
        if (spec && spec->id() && spec->id()->str() == curveId) return spec;
    }
    return nullptr;
}

quantra::enums::InflationCurveMeasure toFbMeasure(InflationCurveSampleMeasure m) {
    switch (m) {
    case InflationCurveSampleMeasure::ZeroRate:
        return quantra::enums::InflationCurveMeasure_ZeroRate;
    case InflationCurveSampleMeasure::YoYRate:
        return quantra::enums::InflationCurveMeasure_YoYRate;
    }
    return quantra::enums::InflationCurveMeasure_ZeroRate;
}

InflationCurveSampleMeasure fromFbMeasure(quantra::enums::InflationCurveMeasure m,
                                          const std::string& curveId) {
    switch (m) {
    case quantra::enums::InflationCurveMeasure_ZeroRate:
        return InflationCurveSampleMeasure::ZeroRate;
    case quantra::enums::InflationCurveMeasure_YoYRate:
        return InflationCurveSampleMeasure::YoYRate;
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported InflationCurveMeasure for curve_id: " + curveId);
    return InflationCurveSampleMeasure::ZeroRate; // unreachable
}

BootstrapInflationCurvesQuery extractQuery(
    const quantra::InflationCurveQuerySpec* query,
    const quantra::InflationCurveSpec* curveSpec,
    const QuantLib::Date& asOfDate) {
    if (query == nullptr) {
        QUANTRA_INVALID_ARGUMENT("InflationCurveQuerySpec is null");
    }
    if (!query->curve_id() || query->curve_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("InflationCurveQuerySpec.curve_id is required");
    }
    const std::string curveId = query->curve_id()->str();
    if (!query->grid()) {
        QUANTRA_INVALID_ARGUMENT(
            "InflationCurveQuerySpec.grid is required for curve_id: " + curveId);
    }
    if (!query->measures() || query->measures()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "InflationCurveQuerySpec.measures is required for curve_id: " + curveId);
    }
    if (curveSpec == nullptr) {
        QUANTRA_NOT_FOUND(
            "Inflation curve id not found in PricingRegistry: " + curveId);
    }
    if (!curveSpec->reference_date()) {
        QUANTRA_INVALID_ARGUMENT(
            "InflationCurveSpec.reference_date is required for curve id: " + curveId);
    }

    BootstrapInflationCurvesQuery out;
    out.curveId = curveId;
    out.asOfDate = asOfDate;

    const auto* options = query->options();
    out.allowExtrapolation = !options || options->allow_extrapolation();
    out.strict = !options || options->strict();

    const QuantLib::Calendar fallbackCal = CalendarToQL(
        requireEnum(curveSpec->calendar(), "InflationCurveSpec.calendar"));
    const QuantLib::BusinessDayConvention fallbackBdc = ConventionToQL(requireEnum(
        curveSpec->business_day_convention(), "InflationCurveSpec.business_day_convention"));
    const QuantLib::Date referenceDate = DateToQL(curveSpec->reference_date()->str());

    const QuantLib::Calendar gridCal =
        grid_utils::ResolveCalendar(query->grid(), options, fallbackCal);
    // Resolve grid BDC up-front so future grid_utils consumers can use it;
    // BuildTenorGrid currently only honours grid->business_day_convention()
    // when grid->calendar() is set, matching the legacy behaviour.
    (void)grid_utils::ResolveBusinessDayConvention(query->grid(), options, fallbackBdc);

    if (query->grid()->grid_type() == quantra::DateGrid_TenorGrid) {
        const auto* tenorGrid = query->grid()->grid_as_TenorGrid();
        // Inflation queries consistently use calendar-based advance
        // (forceCalendarAdvance=true); when the grid carries no calendar,
        // fall back to the options-or-curve calendar.
        if (tenorGrid && tenorGrid->calendar() == quantra::enums::Calendar_NullCalendar) {
            out.gridDates = grid_utils::BuildTenorGrid(
                tenorGrid, referenceDate, gridCal, /*forceCalendarAdvance=*/true);
        } else {
            out.gridDates = grid_utils::BuildTenorGrid(
                tenorGrid, referenceDate, fallbackCal, /*forceCalendarAdvance=*/true);
        }
    } else if (query->grid()->grid_type() == quantra::DateGrid_RangeGrid) {
        const int maxPoints =
            (options && options->max_points() > 0) ? options->max_points() : 50000;
        out.gridDates = grid_utils::BuildRangeGrid(
            query->grid()->grid_as_RangeGrid(), asOfDate, maxPoints);
    } else {
        QUANTRA_INVALID_ARGUMENT(
            "DateGridSpec.grid is required for curve_id: " + curveId);
    }

    out.measures.reserve(query->measures()->size());
    for (flatbuffers::uoffset_t m = 0; m < query->measures()->size(); ++m) {
        auto fbMeasure = static_cast<quantra::enums::InflationCurveMeasure>(
            query->measures()->Get(m));
        out.measures.push_back(fromFbMeasure(fbMeasure, curveId));
    }
    return out;
}

} // namespace

BootstrapInflationCurvesInputs BootstrapInflationCurvesMapper::toInputs(
    const quantra::BootstrapInflationCurvesRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("BootstrapInflationCurvesRequest is null");
    }
    if (!req->pricing()) {
        QUANTRA_INVALID_ARGUMENT("BootstrapInflationCurvesRequest.pricing is required");
    }
    if (!req->queries() || req->queries()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("BootstrapInflationCurvesRequest.queries is required");
    }

    const QuantLib::Date asOfDate = DateToQL(req->pricing()->as_of_date()->str());
    const auto* inflation = req->pricing()->inflation();
    const auto* inflationCurves = inflation ? inflation->inflation_curves() : nullptr;

    BootstrapInflationCurvesInputs inputs;
    inputs.queries.reserve(req->queries()->size());
    for (flatbuffers::uoffset_t i = 0; i < req->queries()->size(); ++i) {
        const auto* qspec = req->queries()->Get(i);
        std::string curveId =
            (qspec && qspec->curve_id()) ? qspec->curve_id()->str() : std::string{};
        BootstrapInflationCurvesQuery q;
        try {
            const auto* curveSpec = findInflationCurveSpecById(inflationCurves, curveId);
            q = extractQuery(qspec, curveSpec, asOfDate);
        } catch (const std::exception& e) {
            q.curveId = curveId;
            q.prebuiltError = e.what();
        }
        inputs.queries.push_back(std::move(q));
    }
    return inputs;
}

flatbuffers::Offset<quantra::BootstrapInflationCurvesResponse>
BootstrapInflationCurvesMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const BootstrapInflationCurvesResult& result) const {

    std::vector<flatbuffers::Offset<quantra::BootstrapInflationCurveResult>> results;
    results.reserve(result.curves.size());

    for (const auto& curve : result.curves) {
        auto idStr = builder.CreateString(curve.id);

        if (!curve.error.empty()) {
            auto errorMsg = builder.CreateString(curve.error);
            quantra::ErrorBuilder errorBuilder(builder);
            errorBuilder.add_error_message(errorMsg);
            auto error = errorBuilder.Finish();

            quantra::BootstrapInflationCurveResultBuilder rb(builder);
            rb.add_id(idStr);
            rb.add_error(error);
            results.push_back(rb.Finish());
            continue;
        }

        auto refDateStr = builder.CreateString(DateToIso(curve.referenceDate));

        std::vector<flatbuffers::Offset<flatbuffers::String>> gridDateStrings;
        gridDateStrings.reserve(curve.gridDates.size());
        for (const auto& d : curve.gridDates) {
            gridDateStrings.push_back(builder.CreateString(DateToIso(d)));
        }
        auto gridDatesVec = builder.CreateVector(gridDateStrings);

        std::vector<flatbuffers::Offset<quantra::InflationCurveSeries>> seriesVector;
        seriesVector.reserve(curve.series.size());
        for (const auto& s : curve.series) {
            auto valuesVec = builder.CreateVector(s.values);
            quantra::InflationCurveSeriesBuilder sb(builder);
            sb.add_measure(toFbMeasure(s.measure));
            sb.add_values(valuesVec);
            seriesVector.push_back(sb.Finish());
        }
        auto seriesVec = builder.CreateVector(seriesVector);

        std::vector<flatbuffers::Offset<flatbuffers::String>> pillarDateStrings;
        pillarDateStrings.reserve(curve.pillarDates.size());
        for (const auto& d : curve.pillarDates) {
            pillarDateStrings.push_back(builder.CreateString(DateToIso(d)));
        }
        auto pillarDatesVec = builder.CreateVector(pillarDateStrings);

        quantra::BootstrapInflationCurveResultBuilder rb(builder);
        rb.add_id(idStr);
        rb.add_reference_date(refDateStr);
        rb.add_grid_dates(gridDatesVec);
        rb.add_series(seriesVec);
        rb.add_pillar_dates(pillarDatesVec);
        results.push_back(rb.Finish());
    }

    auto resultsVec = builder.CreateVector(results);
    quantra::BootstrapInflationCurvesResponseBuilder rb(builder);
    rb.add_results(resultsVec);
    return rb.Finish();
}

void BootstrapInflationCurvesMapper::onRegistryBuildError(
    BootstrapInflationCurvesInputs& inputs, const std::string& message) const {
    for (auto& q : inputs.queries) {
        if (q.prebuiltError.empty()) {
            q.prebuiltError = message;
        }
    }
}

} // namespace quantra
