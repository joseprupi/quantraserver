#include "calibrate_equity_vol_mapper.h"

#include "date_convert.h"
#include "error.h"

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace quantra {

namespace {

using FbBuilder = flatbuffers::grpc::MessageBuilder;

flatbuffers::Optional<double> finiteOrNull(double v) {
    if (std::isfinite(v)) return v;
    return flatbuffers::nullopt;
}

std::string describe(const ssvi::Violation& v) {
    std::ostringstream os;
    os << v.condition << " at slice " << v.slice;
    if (v.prevSlice != v.slice) os << " vs slice " << v.prevSlice;
    os << " (violated by " << v.margin << ")";
    return os.str();
}

flatbuffers::Offset<quantra::SsviSlice> buildSlice(
    FbBuilder& b, const SsviVolData& d, std::size_t i) {
    return quantra::CreateSsviSlice(
        b, b.CreateString(DateToIso(d.expiries[i])), d.params[i].theta, d.params[i].rho,
        d.params[i].psi, d.forwards[i]);
}

flatbuffers::Offset<quantra::EquityVolSliceDiagnostics> buildSliceDiagnostics(
    FbBuilder& b, const SsviVolData& d, std::size_t i) {
    const ssvi::CalibrationResult& fit = *d.fit;
    const ssvi::SliceDiagnostics& s = fit.slices[i];
    int excluded = 0;
    for (const ssvi::QuoteRow& row : fit.quotes) {
        if (row.expiry == fit.expiries[i] && !row.used) ++excluded;
    }
    const bool first = i == 0;
    return quantra::CreateEquityVolSliceDiagnostics(
        b, b.CreateString(DateToIso(fit.expiries[i])), fit.times[i], fit.fitForwards[i],
        finiteOrNull(fit.curveForwards[i]), fit.impliedDividendYields[i], s.anchorK, s.anchorTheta,
        s.anchorClamped, static_cast<int32_t>(s.usedQuotes), excluded, s.rmseVolPts / 100.0,
        s.maxAbsVolPts / 100.0, s.marginButterflyPsi, s.marginButterflyPsi2,
        first ? flatbuffers::nullopt : flatbuffers::Optional<double>(s.marginCalendarTheta),
        first ? flatbuffers::nullopt : flatbuffers::Optional<double>(s.marginCalendarPsi),
        first ? flatbuffers::nullopt : flatbuffers::Optional<double>(s.marginCalendarA),
        static_cast<uint32_t>(s.parityPairsUsed), static_cast<uint32_t>(s.parityPairsRejected));
}

flatbuffers::Offset<quantra::EquityVolQuoteDiagnostics> buildQuoteDiagnostics(
    FbBuilder& b, const SsviVolData& d, std::size_t i) {
    const ssvi::QuoteRow& row = d.fit->quotes[i];
    const ssvi::OptionQuote& q = d.quotes[i];
    const bool solved = std::isfinite(row.marketVol);
    const std::string& reason = row.exclusionReason;
    return quantra::CreateEquityVolQuoteDiagnostics(
        b, b.CreateString(DateToIso(row.expiry)), row.strike,
        q.type == QuantLib::Option::Put ? quantra::enums::EquityOptionType_Put
                                        : quantra::enums::EquityOptionType_Call,
        finiteOrNull(row.marketVol), row.modelVol,
        solved ? flatbuffers::Optional<double>(row.residualVolPts / 100.0) : flatbuffers::nullopt,
        q.weight, row.used, b.CreateString(reason));
}

flatbuffers::Offset<quantra::EquityVolDiagnostics> buildDiagnostics(
    FbBuilder& b, const SsviVolData& d) {
    const ssvi::CalibrationResult& fit = *d.fit;
    std::vector<flatbuffers::Offset<quantra::EquityVolSliceDiagnostics>> slices;
    for (std::size_t i = 0; i < fit.slices.size(); ++i) {
        slices.push_back(buildSliceDiagnostics(b, d, i));
    }
    std::vector<flatbuffers::Offset<quantra::EquityVolQuoteDiagnostics>> quotes;
    for (std::size_t i = 0; i < fit.quotes.size(); ++i) {
        quotes.push_back(buildQuoteDiagnostics(b, d, i));
    }
    std::vector<flatbuffers::Offset<flatbuffers::String>> violations;
    for (const ssvi::Violation& v : fit.violations) {
        violations.push_back(b.CreateString(describe(v)));
    }
    const ssvi::ScanResult& sc = fit.scan;
    return quantra::CreateEquityVolDiagnostics(
        b, b.CreateVector(slices), b.CreateVector(quotes),
        static_cast<int32_t>(fit.excludedByMoneyness), fit.rmseVolPts / 100.0,
        fit.maxAbsVolPts / 100.0, b.CreateVector(violations), sc.minG, sc.minGt, sc.minGk, sc.minWt,
        sc.minWtT, sc.minWtK);
}

} // namespace

CalibrateEquityVolInputs CalibrateEquityVolMapper::toInputs(
    const quantra::CalibrateEquityVolRequest* req) const {
    if (!req || !req->pricing() || !req->vol_id()) {
        QUANTRA_INVALID_ARGUMENT("CalibrateEquityVolRequest requires pricing and vol_id");
    }
    CalibrateEquityVolInputs inputs;
    inputs.volId = req->vol_id()->str();
    if (inputs.volId.empty()) {
        QUANTRA_INVALID_ARGUMENT("CalibrateEquityVolRequest field vol_id must be non-empty");
    }
    return inputs;
}

flatbuffers::Offset<quantra::CalibrateEquityVolResponse> CalibrateEquityVolMapper::toResponse(
    FbBuilder& builder, const CalibrateEquityVolResult& result) const {
    const SsviVolData& d = *result.data;
    std::vector<flatbuffers::Offset<quantra::SsviSlice>> slices;
    for (std::size_t i = 0; i < d.expiries.size(); ++i) {
        slices.push_back(buildSlice(builder, d, i));
    }
    const auto slicesOff = builder.CreateVector(slices);
    const auto diagnosticsOff = buildDiagnostics(builder, d);
    const auto volIdOff = builder.CreateString(result.volId);
    quantra::CalibrateEquityVolResponseBuilder rb(builder);
    rb.add_vol_id(volIdOff);
    rb.add_slices(slicesOff);
    rb.add_diagnostics(diagnosticsOff);
    return rb.Finish();
}

} // namespace quantra
