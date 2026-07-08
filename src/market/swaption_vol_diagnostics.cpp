#include "swaption_vol_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include <ql/math/optimization/endcriteria.hpp>
#include <ql/termstructures/volatility/sabrsmilesection.hpp>

#include "common_generated.h"
#include "enum_convert.h"

namespace quantra {

namespace {

// Thresholds for diagnostic warnings. Conservatively set: these flag genuinely
// bad fits, not just noisy market data.
constexpr double kRmseWarnThreshold = 0.005;     // 50 bps in vol units
constexpr double kMaxErrorWarnThreshold = 0.01;  // 100 bps in vol units

bool endCriteriaConverged(int endCriteria) {
    auto t = static_cast<QuantLib::EndCriteria::Type>(endCriteria);
    switch (t) {
        case QuantLib::EndCriteria::StationaryPoint:
        case QuantLib::EndCriteria::StationaryFunctionValue:
        case QuantLib::EndCriteria::StationaryFunctionAccuracy:
        case QuantLib::EndCriteria::ZeroGradientNorm:
        case QuantLib::EndCriteria::FunctionEpsilonTooSmall:
            return true;
        case QuantLib::EndCriteria::None:
        case QuantLib::EndCriteria::MaxIterations:
        case QuantLib::EndCriteria::Unknown:
        default:
            return false;
    }
}

std::string endCriteriaName(int endCriteria) {
    auto t = static_cast<QuantLib::EndCriteria::Type>(endCriteria);
    switch (t) {
        case QuantLib::EndCriteria::None: return "None";
        case QuantLib::EndCriteria::MaxIterations: return "MaxIterations";
        case QuantLib::EndCriteria::StationaryPoint: return "StationaryPoint";
        case QuantLib::EndCriteria::StationaryFunctionValue: return "StationaryFunctionValue";
        case QuantLib::EndCriteria::StationaryFunctionAccuracy: return "StationaryFunctionAccuracy";
        case QuantLib::EndCriteria::ZeroGradientNorm: return "ZeroGradientNorm";
        case QuantLib::EndCriteria::FunctionEpsilonTooSmall: return "FunctionEpsilonTooSmall";
        case QuantLib::EndCriteria::Unknown: return "Unknown";
        default: return "Type=" + std::to_string(endCriteria);
    }
}

flatbuffers::Offset<quantra::Period> writePeriod(
    flatbuffers::FlatBufferBuilder& fbb, const QuantLib::Period& p) {
    // The schema's enums::TimeUnit is alphabetical and does NOT match
    // QuantLib's enum order, so the unit must be mapped explicitly (a raw
    // cast mislabels everything but Days).
    return quantra::CreatePeriod(fbb, p.length(), TimeUnitToFb(p.units()));
}

std::vector<flatbuffers::Offset<quantra::Period>> writePeriods(
    flatbuffers::FlatBufferBuilder& fbb, const std::vector<QuantLib::Period>& periods) {
    std::vector<flatbuffers::Offset<quantra::Period>> out;
    out.reserve(periods.size());
    for (const auto& p : periods) {
        out.push_back(writePeriod(fbb, p));
    }
    return out;
}

} // namespace

flatbuffers::Offset<quantra::SwaptionVolDiagnostics> buildSwaptionVolDiagnostics(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::string& volId,
    const SwaptionVolEntry& entry,
    const std::vector<std::string>& extraWarnings) {
    const int nExp = entry.nExp;
    const int nTen = entry.nTen;
    const int nNodes = nExp * nTen;
    const bool isCalibrate = entry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate;
    const bool isSabr = isCalibrate || entry.volKind == quantra::enums::SwaptionVolKind_SabrParams;

    auto volIdOff = fbb.CreateString(volId);
    auto expiriesOff = fbb.CreateVector(writePeriods(fbb, entry.expiries));
    auto tenorsOff = fbb.CreateVector(writePeriods(fbb, entry.tenors));

    // Time-to-expiry: derive from entry's calendar/business-day-convention and
    // day counter. Matches the convention used by withSwaptionSabrParamsAtm
    // and finalize. Empty if dimensions invalid.
    std::vector<double> tte;
    if (nNodes > 0 && entry.referenceDate != QuantLib::Date()) {
        tte.assign(nNodes, 0.0);
        for (int i = 0; i < nExp; ++i) {
            QuantLib::Date exercise = entry.calendar.advance(
                entry.referenceDate, entry.expiries[i], entry.businessDayConvention);
            double t = entry.dayCounter.yearFraction(entry.referenceDate, exercise);
            for (int j = 0; j < nTen; ++j) {
                tte[i * nTen + j] = t;
            }
        }
    }

    // Per-node ATM vol via SabrSmileSection — using the calibrated/params SABR
    // coefficients at strike = F. Only for SABR surfaces; empty for non-SABR.
    std::vector<double> atmVol;
    std::vector<double> perStrikeError;
    std::vector<std::string> warnings;
    bool converged = true;

    if (isSabr && nNodes > 0 &&
        static_cast<int>(entry.atmForwardsFlat.size()) == nNodes &&
        static_cast<int>(entry.sabrAlpha.size()) == nNodes &&
        static_cast<int>(entry.sabrBeta.size()) == nNodes &&
        static_cast<int>(entry.sabrNu.size()) == nNodes &&
        static_cast<int>(entry.sabrRho.size()) == nNodes) {
        atmVol.assign(nNodes, 0.0);
        for (int i = 0; i < nExp; ++i) {
            for (int j = 0; j < nTen; ++j) {
                const int k = i * nTen + j;
                if (!(tte.empty()) && tte[k] > 0.0) {
                    // SabrSmileSection parameter order: alpha, beta, nu, rho.
                    std::vector<QuantLib::Real> params = {
                        entry.sabrAlpha[k], entry.sabrBeta[k], entry.sabrNu[k], entry.sabrRho[k]};
                    QuantLib::SabrSmileSection section(
                        tte[k], entry.atmForwardsFlat[k], params, entry.displacement, entry.qlVolType);
                    atmVol[k] = section.volatility(entry.atmForwardsFlat[k]);
                }
            }
        }

        if (isCalibrate &&
            static_cast<int>(entry.sabrStrikeSpreads.size()) == entry.nStrikes &&
            static_cast<int>(entry.sabrMarketVolsFlat.size()) == nNodes * entry.nStrikes) {
            perStrikeError.assign(nNodes * entry.nStrikes, 0.0);
            for (int i = 0; i < nExp; ++i) {
                for (int j = 0; j < nTen; ++j) {
                    const int k = i * nTen + j;
                    if (tte.empty() || tte[k] <= 0.0) continue;
                    std::vector<QuantLib::Real> params = {
                        entry.sabrAlpha[k], entry.sabrBeta[k], entry.sabrNu[k], entry.sabrRho[k]};
                    QuantLib::SabrSmileSection section(
                        tte[k], entry.atmForwardsFlat[k], params, entry.displacement, entry.qlVolType);
                    for (int s = 0; s < entry.nStrikes; ++s) {
                        const double abs = entry.atmForwardsFlat[k] + entry.sabrStrikeSpreads[s];
                        const double model = section.volatility(abs);
                        const double mkt = entry.sabrMarketVolsFlat[(i * nTen + j) * entry.nStrikes + s];
                        perStrikeError[k * entry.nStrikes + s] = model - mkt;
                    }
                }
            }
        }
    }

    // Warnings for SabrCalibrate: large fit errors, non-convergent nodes.
    flatbuffers::Offset<quantra::SabrCalibrationDiagnostics> calibOff = 0;
    if (isCalibrate &&
        static_cast<int>(entry.sabrPerNodeRmse.size()) == nNodes &&
        static_cast<int>(entry.sabrPerNodeMaxError.size()) == nNodes &&
        static_cast<int>(entry.sabrPerNodeEndCriteria.size()) == nNodes) {
        double sumSq = 0.0;
        int countedRmse = 0;
        for (int i = 0; i < nExp; ++i) {
            for (int j = 0; j < nTen; ++j) {
                const int k = i * nTen + j;
                const double rmse = entry.sabrPerNodeRmse[k];
                const double mxe = entry.sabrPerNodeMaxError[k];
                if (std::isfinite(rmse)) {
                    sumSq += rmse * rmse;
                    ++countedRmse;
                }
                if (rmse > kRmseWarnThreshold) {
                    std::ostringstream w;
                    w << "vol_id='" << volId << "' node(expiryIdx=" << i << ",tenorIdx=" << j
                      << ") rmse=" << rmse << " exceeds threshold " << kRmseWarnThreshold;
                    warnings.push_back(w.str());
                }
                if (mxe > kMaxErrorWarnThreshold) {
                    std::ostringstream w;
                    w << "vol_id='" << volId << "' node(expiryIdx=" << i << ",tenorIdx=" << j
                      << ") max_abs_error=" << mxe << " exceeds threshold " << kMaxErrorWarnThreshold;
                    warnings.push_back(w.str());
                }
                if (!endCriteriaConverged(entry.sabrPerNodeEndCriteria[k])) {
                    converged = false;
                    std::ostringstream w;
                    w << "vol_id='" << volId << "' node(expiryIdx=" << i << ",tenorIdx=" << j
                      << ") did not converge: end_criteria=" << endCriteriaName(entry.sabrPerNodeEndCriteria[k]);
                    warnings.push_back(w.str());
                }
            }
        }
        const double overallRmse = countedRmse > 0
            ? std::sqrt(sumSq / static_cast<double>(countedRmse))
            : std::numeric_limits<double>::quiet_NaN();

        // QuantLib 1.41's SABRInterpolation does not expose iteration count
        // publicly; emit -1 as the documented sentinel.
        std::vector<int> iters(nNodes, -1);

        auto rmseOff = fbb.CreateVector(entry.sabrPerNodeRmse);
        auto maxErrOff = fbb.CreateVector(entry.sabrPerNodeMaxError);
        auto itersOff = fbb.CreateVector(iters);
        auto strikesOff = fbb.CreateVector(entry.sabrStrikeSpreads);
        auto perStrikeOff = perStrikeError.empty()
            ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
            : fbb.CreateVector(perStrikeError);

        quantra::SabrCalibrationDiagnosticsBuilder cb(fbb);
        cb.add_per_node_rmse(rmseOff);
        cb.add_per_node_max_abs_error(maxErrOff);
        cb.add_overall_rmse(overallRmse);
        cb.add_converged(converged);
        cb.add_iterations_per_node(itersOff);
        cb.add_strikes(strikesOff);
        if (!perStrikeError.empty()) {
            cb.add_per_strike_fit_error(perStrikeOff);
        }
        calibOff = cb.Finish();
    }

    for (const auto& w : extraWarnings) {
        warnings.push_back(w);
    }

    auto fwdOff = entry.atmForwardsFlat.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(entry.atmForwardsFlat);
    auto atmVolOff = atmVol.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(atmVol);
    auto tteOff = tte.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(tte);
    auto alphaOff = entry.sabrAlpha.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(entry.sabrAlpha);
    auto betaOff = entry.sabrBeta.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(entry.sabrBeta);
    auto rhoOff = entry.sabrRho.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(entry.sabrRho);
    auto nuOff = entry.sabrNu.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<double>>(0)
        : fbb.CreateVector(entry.sabrNu);
    std::vector<flatbuffers::Offset<flatbuffers::String>> warnOffs;
    warnOffs.reserve(warnings.size());
    for (const auto& w : warnings) {
        warnOffs.push_back(fbb.CreateString(w));
    }
    auto warningsOff = warnOffs.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>>(0)
        : fbb.CreateVector(warnOffs);

    quantra::SwaptionVolDiagnosticsBuilder b(fbb);
    b.add_vol_id(volIdOff);
    b.add_kind(entry.volKind);
    if (!entry.expiries.empty()) b.add_expiries(expiriesOff);
    if (!entry.tenors.empty()) b.add_tenors(tenorsOff);
    b.add_n_expiries(nExp);
    b.add_n_tenors(nTen);
    if (!entry.atmForwardsFlat.empty()) b.add_forward_per_node(fwdOff);
    if (!atmVol.empty()) b.add_atm_vol_per_node(atmVolOff);
    if (!tte.empty()) b.add_time_to_expiry_per_node(tteOff);
    if (!entry.sabrAlpha.empty()) b.add_alpha_per_node(alphaOff);
    if (!entry.sabrBeta.empty()) b.add_beta_per_node(betaOff);
    if (!entry.sabrRho.empty()) b.add_rho_per_node(rhoOff);
    if (!entry.sabrNu.empty()) b.add_nu_per_node(nuOff);
    if (calibOff.o != 0) b.add_calibration(calibOff);
    if (!warnOffs.empty()) b.add_warnings(warningsOff);
    return b.Finish();
}

flatbuffers::Offset<quantra::SwaptionVolDiagnostics> buildPartialSwaptionVolDiagnostics(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::string& volId,
    quantra::enums::SwaptionVolKind kind,
    const std::vector<std::string>& warnings) {
    auto volIdOff = fbb.CreateString(volId);
    std::vector<flatbuffers::Offset<flatbuffers::String>> warnOffs;
    warnOffs.reserve(warnings.size());
    for (const auto& w : warnings) {
        warnOffs.push_back(fbb.CreateString(w));
    }
    auto warningsOff = warnOffs.empty()
        ? flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>>(0)
        : fbb.CreateVector(warnOffs);
    quantra::SwaptionVolDiagnosticsBuilder b(fbb);
    b.add_vol_id(volIdOff);
    b.add_kind(kind);
    if (!warnOffs.empty()) b.add_warnings(warningsOff);
    return b.Finish();
}

} // namespace quantra
