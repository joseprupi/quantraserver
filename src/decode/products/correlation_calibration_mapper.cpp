#include "correlation_calibration_mapper.h"

#include "date_convert.h"
#include "error.h"

namespace quantra {

namespace {

ReturnKind toReturnKind(quantra::enums::ReturnType v) {
    switch (v) {
        case quantra::enums::ReturnType_Log: return ReturnKind::Log;
        case quantra::enums::ReturnType_Simple: return ReturnKind::Simple;
        default: QUANTRA_INVALID_ARGUMENT("estimator.returns is not a known ReturnType");
    }
}

ReturnSampling toSampling(quantra::enums::ReturnFrequency v) {
    switch (v) {
        case quantra::enums::ReturnFrequency_Daily: return ReturnSampling::Daily;
        case quantra::enums::ReturnFrequency_Weekly: return ReturnSampling::Weekly;
        default: QUANTRA_INVALID_ARGUMENT("estimator.frequency is not a known ReturnFrequency");
    }
}

ShrinkageKind toShrinkage(quantra::enums::CorrelationShrinkage v) {
    switch (v) {
        case quantra::enums::CorrelationShrinkage_None: return ShrinkageKind::None;
        case quantra::enums::CorrelationShrinkage_LedoitWolf: return ShrinkageKind::LedoitWolf;
        case quantra::enums::CorrelationShrinkage_Fixed: return ShrinkageKind::Fixed;
        default:
            QUANTRA_INVALID_ARGUMENT("estimator.shrinkage is not a known CorrelationShrinkage");
    }
}

quantra::enums::ReturnType fromReturnKind(ReturnKind v) {
    return v == ReturnKind::Log ? quantra::enums::ReturnType_Log
                                : quantra::enums::ReturnType_Simple;
}

quantra::enums::ReturnFrequency fromSampling(ReturnSampling v) {
    return v == ReturnSampling::Daily ? quantra::enums::ReturnFrequency_Daily
                                      : quantra::enums::ReturnFrequency_Weekly;
}

quantra::enums::CorrelationShrinkage fromShrinkage(ShrinkageKind v) {
    switch (v) {
        case ShrinkageKind::None: return quantra::enums::CorrelationShrinkage_None;
        case ShrinkageKind::LedoitWolf: return quantra::enums::CorrelationShrinkage_LedoitWolf;
        default: return quantra::enums::CorrelationShrinkage_Fixed;
    }
}

} // namespace

CorrelationCalibrationInputs CorrelationCalibrationMapper::toInputs(
    const quantra::CalibrateCorrelationRequest* req) const {
    if (!req) {
        QUANTRA_INVALID_ARGUMENT("CalibrateCorrelationRequest is required");
    }
    if (!req->as_of_date() || req->as_of_date()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateCorrelationRequest requires as_of_date");
    }
    if (!req->correlation_id() || req->correlation_id()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateCorrelationRequest requires correlation_id");
    }
    if (!req->series() || req->series()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateCorrelationRequest requires series");
    }
    CorrelationCalibrationInputs in;
    in.asOf = DateToQL(req->as_of_date()->str());
    in.correlationId = req->correlation_id()->str();
    for (flatbuffers::uoffset_t i = 0; i < req->series()->size(); ++i) {
        const auto* s = req->series()->Get(i);
        const std::string p = "series[" + std::to_string(i) + "]";
        if (!s || !s->underlying_id() || s->underlying_id()->size() == 0) {
            QUANTRA_INVALID_ARGUMENT(p + ".underlying_id is required");
        }
        if (!s->closes() || s->closes()->size() == 0) {
            QUANTRA_INVALID_ARGUMENT(p + ".closes must contain at least one close");
        }
        PriceSeriesData d;
        d.underlyingId = s->underlying_id()->str();
        d.closes.reserve(s->closes()->size());
        for (flatbuffers::uoffset_t k = 0; k < s->closes()->size(); ++k) {
            const auto* c = s->closes()->Get(k);
            if (!c || !c->date() || c->date()->size() == 0) {
                QUANTRA_INVALID_ARGUMENT(p + ".closes[" + std::to_string(k) + "].date is required");
            }
            PriceObservation o;
            o.dateIso = c->date()->str();
            o.date = DateToQL(o.dateIso);
            o.value = c->value();
            d.closes.push_back(std::move(o));
        }
        in.series.push_back(std::move(d));
    }
    if (const auto* e = req->estimator()) {
        CorrelationEstimatorData& est = in.estimator;
        if (e->window_days().has_value()) est.windowDays = e->window_days().value();
        if (e->start_date() && e->start_date()->size() > 0) {
            est.startDate = DateToQL(e->start_date()->str());
        }
        if (e->end_date() && e->end_date()->size() > 0) {
            est.endDate = DateToQL(e->end_date()->str());
        }
        est.returns = toReturnKind(e->returns());
        est.frequency = toSampling(e->frequency());
        est.shrinkage = toShrinkage(e->shrinkage());
        if (e->shrinkage_intensity().has_value()) {
            est.shrinkageIntensity = e->shrinkage_intensity().value();
        }
        est.minOverlap = e->min_overlap();
        est.eigenvalueFloor = e->eigenvalue_floor();
    }
    if (req->labels()) {
        for (flatbuffers::uoffset_t i = 0; i < req->labels()->size(); ++i) {
            const auto* l = req->labels()->Get(i);
            in.labels.push_back(l ? l->str() : std::string());
        }
    }
    return in;
}

flatbuffers::Offset<quantra::CalibrateCorrelationResponse> CorrelationCalibrationMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& b, const CorrelationCalibrationResult& r) const {
    const auto n = static_cast<int>(r.labels.size());

    // The spec: labels, QuoteMatrix2D row-major.
    std::vector<flatbuffers::Offset<flatbuffers::String>> labels;
    for (const auto& l : r.labels)
        labels.push_back(b.CreateString(l));
    auto labelsOff = b.CreateVector(labels);
    auto valuesOff = b.CreateVector(r.correlation);
    quantra::QuoteMatrix2DBuilder mb(b);
    mb.add_n_rows(n);
    mb.add_n_cols(n);
    mb.add_values(valuesOff);
    auto matrixOff = mb.Finish();
    auto idOff = b.CreateString(r.correlationId);
    quantra::CorrelationMatrixSpecBuilder sb(b);
    sb.add_id(idOff);
    sb.add_labels(labelsOff);
    sb.add_values(matrixOff);
    auto specOff = sb.Finish();

    auto sampleOff = b.CreateVector(r.sampleCorrelation);

    std::vector<flatbuffers::Offset<quantra::CorrelationSeriesReport>> series;
    for (const auto& s : r.series) {
        auto uid = b.CreateString(s.underlyingId);
        quantra::CorrelationSeriesReportBuilder rb(b);
        rb.add_underlying_id(uid);
        rb.add_observations(s.observations);
        rb.add_dropped(s.dropped);
        rb.add_annualised_volatility(s.annualisedVolatility);
        series.push_back(rb.Finish());
    }
    auto seriesOff = b.CreateVector(series);

    const CorrelationEstimatorData& e = r.estimatorEcho;
    flatbuffers::Offset<flatbuffers::String> startOff, endOff;
    if (e.startDate) startOff = b.CreateString(DateToIso(*e.startDate));
    if (e.endDate) endOff = b.CreateString(DateToIso(*e.endDate));
    quantra::CorrelationEstimatorBuilder eb(b);
    if (e.windowDays) eb.add_window_days(*e.windowDays);
    if (e.startDate) eb.add_start_date(startOff);
    if (e.endDate) eb.add_end_date(endOff);
    eb.add_returns(fromReturnKind(e.returns));
    eb.add_frequency(fromSampling(e.frequency));
    eb.add_shrinkage(fromShrinkage(e.shrinkage));
    if (e.shrinkageIntensity) eb.add_shrinkage_intensity(*e.shrinkageIntensity);
    eb.add_min_overlap(e.minOverlap);
    eb.add_eigenvalue_floor(e.eigenvalueFloor);
    auto echoOff = eb.Finish();

    auto firstOff = b.CreateString(r.firstDateIso);
    auto lastOff = b.CreateString(r.lastDateIso);
    quantra::CalibrateCorrelationResponseBuilder rb(b);
    rb.add_correlation_spec(specOff);
    rb.add_sample_correlation(sampleOff);
    rb.add_shrinkage_target(r.shrinkageTarget);
    rb.add_shrinkage_intensity_used(r.shrinkageIntensityUsed);
    rb.add_eigenvalue_floor_applied(r.eigenvalueFloorApplied);
    rb.add_smallest_eigenvalue_before(r.smallestEigenvalueBefore);
    rb.add_smallest_eigenvalue_after(r.smallestEigenvalueAfter);
    rb.add_observations_used(r.observationsUsed);
    rb.add_returns_used(r.returnsUsed);
    rb.add_first_date(firstOff);
    rb.add_last_date(lastOff);
    rb.add_series(seriesOff);
    rb.add_estimator_echo(echoOff);
    return rb.Finish();
}

} // namespace quantra
