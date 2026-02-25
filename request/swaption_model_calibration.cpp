#include "swaption_model_calibration.h"

#include <cmath>
#include <atomic>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/math/optimization/endcriteria.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/calibrationhelper.hpp>
#include <ql/models/shortrate/calibrationhelpers/swaptionhelper.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/pricingengines/swaption/jamshidianswaptionengine.hpp>
#include <ql/settings.hpp>

#include "common.h"
#include "error.h"

namespace {

std::atomic<int> g_hwCalibrationCallCount{0};

struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
};

QuantLib::Period frequencyToPeriod(QuantLib::Frequency f) {
    switch (f) {
        case QuantLib::Annual: return QuantLib::Period(1, QuantLib::Years);
        case QuantLib::Semiannual: return QuantLib::Period(6, QuantLib::Months);
        case QuantLib::Quarterly: return QuantLib::Period(3, QuantLib::Months);
        case QuantLib::Monthly: return QuantLib::Period(1, QuantLib::Months);
        case QuantLib::Bimonthly: return QuantLib::Period(2, QuantLib::Months);
        case QuantLib::EveryFourthMonth: return QuantLib::Period(4, QuantLib::Months);
        default: return QuantLib::Period(1, QuantLib::Years);
    }
}

QuantLib::Period toQlPeriod(const quantra::Period* p) {
    if (!p) {
        QUANTRA_ERROR("Calibration Period is null");
    }
    return QuantLib::Period(p->n(), TimeUnitToQL(p->unit()));
}

std::vector<QuantLib::Period> selectPeriods(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::Period>>* fromSpec,
    const std::vector<QuantLib::Period>& fallback,
    const std::string& label) {
    if (fromSpec && fromSpec->size() > 0) {
        std::vector<QuantLib::Period> out;
        out.reserve(fromSpec->size());
        for (auto it = fromSpec->begin(); it != fromSpec->end(); ++it) {
            out.push_back(toQlPeriod(*it));
        }
        return out;
    }
    if (!fallback.empty()) {
        return fallback;
    }
    QUANTRA_ERROR("Hull-White calibration requires non-empty " + label + " grid");
    return {};
}

double marketVolAtNode(
    const quantra::SwaptionVolEntry& volEntry,
    const QuantLib::Period& expiry,
    const QuantLib::Period& tenor) {
    double v = 0.0;
    switch (volEntry.volKind) {
        case quantra::enums::SwaptionVolKind_Constant:
            v = volEntry.constantVol;
            break;
        case quantra::enums::SwaptionVolKind_AtmMatrix2D:
            v = volEntry.handle->volatility(expiry, tenor, 0.0, true);
            break;
        default:
            QUANTRA_ERROR(
                "Hull-White calibration supports only SwaptionVolKind Constant and AtmMatrix2D");
    }
    if (!(v > 0.0) || !std::isfinite(v)) {
        QUANTRA_ERROR("Invalid market swaption vol extracted for calibration node");
    }
    return v;
}

constexpr int kMaxCalibrationGridRows = 20;
constexpr int kMaxCalibrationGridCols = 20;
constexpr int kMaxCalibrationGridPoints = 400;

} // namespace

namespace quantra {

void resetHwCalibrationCallCount() {
    g_hwCalibrationCallCount.store(0);
}

int getHwCalibrationCallCount() {
    return g_hwCalibrationCallCount.load();
}

HwCalibResult calibrateHullWhiteFromSwaptionVol(
    const PricingRegistry& reg,
    const quantra::SwaptionHwCalibrationSpec* calibSpec,
    const QuantLib::Date asOf) {
    g_hwCalibrationCallCount.fetch_add(1);
    EvalDateGuard evalGuard;
    QuantLib::Settings::instance().evaluationDate() = asOf;

    if (!calibSpec || !calibSpec->swaption_vol_id() || !calibSpec->discount_curve_id() ||
        !calibSpec->forwarding_curve_id() || !calibSpec->swap_index_id()) {
        QUANTRA_ERROR(
            "SwaptionHwCalibrationSpec requires swaption_vol_id, discount_curve_id, forwarding_curve_id, and swap_index_id");
    }

    const std::string volId = calibSpec->swaption_vol_id()->str();
    const std::string discountCurveId = calibSpec->discount_curve_id()->str();
    const std::string forwardingCurveId = calibSpec->forwarding_curve_id()->str();
    const std::string swapIndexId = calibSpec->swap_index_id()->str();

    auto vIt = reg.swaptionVols.find(volId);
    if (vIt == reg.swaptionVols.end()) {
        QUANTRA_ERROR("Calibration swaption vol not found: " + volId);
    }
    const auto& volEntry = vIt->second;
    if (volEntry.handle.empty()) {
        QUANTRA_ERROR("Calibration swaption vol handle is empty: " + volId);
    }

    auto cIt = reg.curves.find(discountCurveId);
    if (cIt == reg.curves.end() || !cIt->second || cIt->second->empty()) {
        QUANTRA_ERROR("Calibration discount curve not found: " + discountCurveId);
    }
    QuantLib::Handle<QuantLib::YieldTermStructure> discountCurve(cIt->second->currentLink());
    auto fIt = reg.curves.find(forwardingCurveId);
    if (fIt == reg.curves.end() || !fIt->second || fIt->second->empty()) {
        QUANTRA_ERROR("Calibration forwarding curve not found: " + forwardingCurveId);
    }
    QuantLib::Handle<QuantLib::YieldTermStructure> forwardingCurve(fIt->second->currentLink());

    if (!reg.swapIndices.has(swapIndexId)) {
        QUANTRA_ERROR("Calibration swap index not found: " + swapIndexId);
    }
    const auto& sidx = reg.swapIndices.get(swapIndexId);
    if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
        QUANTRA_ERROR("Hull-White calibration currently supports Ibor swap_index_id only");
    }
    // Guard against convention drift: helper constructor cannot fully encode non-standard swap conventions.
    if (sidx.fixedDateRule != QuantLib::DateGeneration::Forward ||
        sidx.fixedBdc != QuantLib::ModifiedFollowing ||
        sidx.fixedTermBdc != QuantLib::ModifiedFollowing ||
        sidx.fixedEom) {
        QUANTRA_ERROR(
            "Hull-White calibration currently supports swap indices with Forward generation, "
            "ModifiedFollowing conventions, and fixed_eom=false");
    }

    std::vector<QuantLib::Period> fallbackExpiries;
    std::vector<QuantLib::Period> fallbackTenors;
    if (volEntry.volKind == quantra::enums::SwaptionVolKind_AtmMatrix2D) {
        fallbackExpiries = volEntry.expiries;
        fallbackTenors = volEntry.tenors;
    }
    auto expiries = selectPeriods(calibSpec->expiries(), fallbackExpiries, "expiries");
    auto tenors = selectPeriods(calibSpec->tenors(), fallbackTenors, "tenors");
    if (expiries.empty() || tenors.empty()) {
        QUANTRA_ERROR("Calibration grid is empty");
    }
    const int gridRows = static_cast<int>(expiries.size());
    const int gridCols = static_cast<int>(tenors.size());
    const int gridPoints = gridRows * gridCols;
    if (gridRows > kMaxCalibrationGridRows || gridCols > kMaxCalibrationGridCols ||
        gridPoints > kMaxCalibrationGridPoints) {
        std::ostringstream os;
        os << "Hull-White calibration grid too large: "
           << "rows=" << gridRows << ", cols=" << gridCols << ", points=" << gridPoints
           << " (max rows=" << kMaxCalibrationGridRows
           << ", max cols=" << kMaxCalibrationGridCols
           << ", max points=" << kMaxCalibrationGridPoints << ")";
        QUANTRA_ERROR(os.str());
    }

    auto ibor = reg.indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);
    if (sidx.fixedCalendar != ibor->fixingCalendar()) {
        QUANTRA_ERROR(
            "Hull-White calibration requires swap index fixed_calendar to match IBOR fixing calendar");
    }
    if (sidx.floatCalendar != ibor->fixingCalendar()) {
        QUANTRA_ERROR(
            "Hull-White calibration requires swap index float_calendar to match IBOR fixing calendar");
    }
    QuantLib::Period fixedLegTenor = frequencyToPeriod(sidx.fixedFrequency);
    QuantLib::DayCounter floatingDc = ibor->dayCounter();
    // RelativePriceError can overweight low-price instruments, especially under Normal vols.
    // Use PriceError for Normal vols and ImpliedVolError for Black/ShiftedBlack stability.
    const auto calibErrorType =
        (volEntry.qlVolType == QuantLib::Normal)
            ? QuantLib::BlackCalibrationHelper::PriceError
            : QuantLib::BlackCalibrationHelper::ImpliedVolError;

    std::vector<QuantLib::ext::shared_ptr<QuantLib::CalibrationHelper>> helpers;
    helpers.reserve(expiries.size() * tenors.size());
    for (const auto& exp : expiries) {
        for (const auto& ten : tenors) {
            const double marketVol = marketVolAtNode(volEntry, exp, ten);
            auto volQuote = QuantLib::Handle<QuantLib::Quote>(
                QuantLib::ext::make_shared<QuantLib::SimpleQuote>(marketVol));
            auto helper = QuantLib::ext::make_shared<QuantLib::SwaptionHelper>(
                exp,
                ten,
                volQuote,
                ibor,
                fixedLegTenor,
                sidx.fixedDayCounter,
                floatingDc,
                discountCurve,
                calibErrorType,
                QuantLib::Null<QuantLib::Real>(),
                1.0,
                volEntry.qlVolType,
                volEntry.displacement,
                static_cast<QuantLib::Natural>(sidx.spotDays));
            helpers.push_back(helper);
        }
    }
    if (helpers.empty()) {
        QUANTRA_ERROR("No calibration helpers were built");
    }

    const bool calibrateA = calibSpec->calibrate_a();
    const bool calibrateSigma = calibSpec->calibrate_sigma();
    if (!calibrateA && !calibrateSigma) {
        QUANTRA_ERROR("At least one of calibrate_a or calibrate_sigma must be true");
    }

    auto hwModel = QuantLib::ext::make_shared<QuantLib::HullWhite>(
        discountCurve, calibSpec->a_init(), calibSpec->sigma_init());
    auto engine = QuantLib::ext::make_shared<QuantLib::JamshidianSwaptionEngine>(hwModel);
    for (auto& h : helpers) {
        auto blackHelper = QuantLib::ext::dynamic_pointer_cast<QuantLib::BlackCalibrationHelper>(h);
        if (!blackHelper) {
            QUANTRA_ERROR("Unexpected helper type during Hull-White calibration");
        }
        blackHelper->setPricingEngine(engine);
    }

    QuantLib::LevenbergMarquardt lm;
    QuantLib::EndCriteria endCriteria(
        calibSpec->function_evaluations(),
        calibSpec->max_iterations(),
        calibSpec->end_criteria_eps(),
        calibSpec->end_criteria_eps(),
        calibSpec->end_criteria_eps());
    std::vector<bool> fixParams = { !calibrateA, !calibrateSigma };
    hwModel->calibrate(
        helpers,
        lm,
        endCriteria,
        QuantLib::NoConstraint(),
        std::vector<double>(),
        fixParams);

    const auto params = hwModel->params();
    if (params.size() < 2) {
        QUANTRA_ERROR("HullWhite calibration returned invalid parameter vector");
    }

    double err2 = 0.0;
    for (const auto& h : helpers) {
        const double e = h->calibrationError();
        err2 += e * e;
    }
    // RMSE is expressed in the chosen helper error metric (price or implied-vol error).
    const double rmse = std::sqrt(err2 / static_cast<double>(helpers.size()));

    HwCalibResult out;
    out.a = params[0];
    out.sigma = params[1];
    out.rmse = rmse;
    out.numHelpers = static_cast<int>(helpers.size());
    out.gridRows = gridRows;
    out.gridCols = gridCols;
    out.gridPoints = gridPoints;
    return out;
}

} // namespace quantra
