#include "swaption_model_calibration.h"

#include "require_period.h"

#include <cmath>
#include <atomic>
#include <iostream>
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

#include "date_convert.h"
#include "error.h"
#include "hw_calibrate_cache.h"
#include "hw_calibrate_cache_key.h"

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
    return requirePeriod(p, "Calibration Period");
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
            QUANTRA_INVALID_ARGUMENT(
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

// Server-side ceilings on the client-controlled Levenberg-Marquardt knobs.
// Invariant: client-supplied knobs cannot make a single calibration unbounded.
// The request's values are CLAMPED (not rejected — a clamp is not an error) so a
// caller can only ever ask for LESS work than these caps. Domain defaults are
// 200 / 1000; these leave generous headroom for legitimate hard calibrations.
constexpr int kMaxCalibrationIterations = 1000;
constexpr int kMaxCalibrationFunctionEvaluations = 5000;

int clampCalibrationKnob(int requested, int ceiling) {
    if (requested > ceiling) return ceiling;
    return requested;
}

// Resolve a curve cache key by curve id, returning "" when the curve cache is
// disabled or the curve is missing from the keys map. An empty key disables the
// cross-request HW-calibration cache for this run (safe fallback — never wrong,
// only slower). Mirrors getCurveCacheKey in swaption_vol_runtime.cpp.
std::string getCurveCacheKey(const quantra::PricingRegistry& reg, const std::string& curveId) {
    auto it = reg.rates.curveKeys.find(curveId);
    return (it == reg.rates.curveKeys.end()) ? std::string() : it->second;
}

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
    if (!calibSpec || !calibSpec->swaption_vol_id() || !calibSpec->discount_curve_id() ||
        !calibSpec->forwarding_curve_id() || !calibSpec->swap_index_id()) {
        QUANTRA_INVALID_ARGUMENT(
            "SwaptionHwCalibrationSpec requires swaption_vol_id, discount_curve_id, forwarding_curve_id, and swap_index_id");
    }

    SwaptionHwCalibrationDomain domain;
    domain.swaption_vol_id = calibSpec->swaption_vol_id()->str();
    domain.discount_curve_id = calibSpec->discount_curve_id()->str();
    domain.forwarding_curve_id = calibSpec->forwarding_curve_id()->str();
    domain.swap_index_id = calibSpec->swap_index_id()->str();
    if (calibSpec->expiries() && calibSpec->expiries()->size() > 0) {
        domain.expiries.reserve(calibSpec->expiries()->size());
        for (auto it = calibSpec->expiries()->begin(); it != calibSpec->expiries()->end(); ++it) {
            domain.expiries.push_back(toQlPeriod(*it));
        }
    }
    if (calibSpec->tenors() && calibSpec->tenors()->size() > 0) {
        domain.tenors.reserve(calibSpec->tenors()->size());
        for (auto it = calibSpec->tenors()->begin(); it != calibSpec->tenors()->end(); ++it) {
            domain.tenors.push_back(toQlPeriod(*it));
        }
    }
    domain.calibrate_a = calibSpec->calibrate_a();
    domain.calibrate_sigma = calibSpec->calibrate_sigma();
    domain.a_init = calibSpec->a_init();
    domain.sigma_init = calibSpec->sigma_init();
    domain.max_iterations = calibSpec->max_iterations();
    domain.function_evaluations = calibSpec->function_evaluations();
    domain.end_criteria_eps = calibSpec->end_criteria_eps();
    return calibrateHullWhiteFromSwaptionVol(reg, domain, asOf);
}

HwCalibResult calibrateHullWhiteFromSwaptionVol(
    const PricingRegistry& reg,
    const SwaptionHwCalibrationDomain& calibSpec,
    const QuantLib::Date asOf) {
    // NOTE: g_hwCalibrationCallCount is incremented only on a real calibration
    // (cache miss) below, so the test hooks measure genuine calibration work
    // and a cross-request cache hit does not inflate the count.
    EvalDateGuard evalGuard;
    QuantLib::Settings::instance().evaluationDate() = asOf;

    if (calibSpec.swaption_vol_id.empty() || calibSpec.discount_curve_id.empty() ||
        calibSpec.forwarding_curve_id.empty() || calibSpec.swap_index_id.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "SwaptionHwCalibrationSpec requires swaption_vol_id, discount_curve_id, forwarding_curve_id, and swap_index_id");
    }

    const std::string& volId = calibSpec.swaption_vol_id;
    const std::string& discountCurveId = calibSpec.discount_curve_id;
    const std::string& forwardingCurveId = calibSpec.forwarding_curve_id;
    const std::string& swapIndexId = calibSpec.swap_index_id;

    auto vIt = reg.volatility.swaptionVols.find(volId);
    if (vIt == reg.volatility.swaptionVols.end()) {
        QUANTRA_NOT_FOUND("Calibration swaption vol not found: " + volId);
    }
    const auto& volEntry = vIt->second;
    if (volEntry.handle.empty()) {
        QUANTRA_ERROR("Calibration swaption vol handle is empty: " + volId);
    }

    auto cIt = reg.rates.curves.find(discountCurveId);
    if (cIt == reg.rates.curves.end() || !cIt->second || cIt->second->empty()) {
        QUANTRA_NOT_FOUND("Calibration discount curve not found: " + discountCurveId);
    }
    QuantLib::Handle<QuantLib::YieldTermStructure> discountCurve(cIt->second->currentLink());
    auto fIt = reg.rates.curves.find(forwardingCurveId);
    if (fIt == reg.rates.curves.end() || !fIt->second || fIt->second->empty()) {
        QUANTRA_NOT_FOUND("Calibration forwarding curve not found: " + forwardingCurveId);
    }
    QuantLib::Handle<QuantLib::YieldTermStructure> forwardingCurve(fIt->second->currentLink());

    if (!reg.rates.swapIndices.has(swapIndexId)) {
        QUANTRA_NOT_FOUND("Calibration swap index not found: " + swapIndexId);
    }
    const auto& sidx = reg.rates.swapIndices.get(swapIndexId);
    if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
        QUANTRA_INVALID_ARGUMENT("Hull-White calibration currently supports Ibor swap_index_id only");
    }
    // Guard against convention drift: helper constructor cannot fully encode non-standard swap conventions.
    if (sidx.fixedDateRule != QuantLib::DateGeneration::Forward ||
        sidx.fixedBdc != QuantLib::ModifiedFollowing ||
        sidx.fixedTermBdc != QuantLib::ModifiedFollowing ||
        sidx.fixedEom) {
        QUANTRA_INVALID_ARGUMENT(
            "Hull-White calibration currently supports swap indices with Forward generation, "
            "ModifiedFollowing conventions, and fixed_eom=false");
    }

    std::vector<QuantLib::Period> fallbackExpiries;
    std::vector<QuantLib::Period> fallbackTenors;
    if (volEntry.volKind == quantra::enums::SwaptionVolKind_AtmMatrix2D) {
        fallbackExpiries = volEntry.expiries;
        fallbackTenors = volEntry.tenors;
    }
    auto pickGrid = [&](const std::vector<QuantLib::Period>& fromSpec,
                        const std::vector<QuantLib::Period>& fallback,
                        const std::string& label) {
        if (!fromSpec.empty()) return fromSpec;
        if (!fallback.empty()) return fallback;
        QUANTRA_INVALID_ARGUMENT("Hull-White calibration requires non-empty " + label + " grid");
        return std::vector<QuantLib::Period>{};
    };
    auto expiries = pickGrid(calibSpec.expiries, fallbackExpiries, "expiries");
    auto tenors = pickGrid(calibSpec.tenors, fallbackTenors, "tenors");
    if (expiries.empty() || tenors.empty()) {
        QUANTRA_INVALID_ARGUMENT("Calibration grid is empty");
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
        QUANTRA_INVALID_ARGUMENT(os.str());
    }

    auto ibor = reg.rates.indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);
    if (sidx.fixedCalendar != ibor->fixingCalendar()) {
        QUANTRA_INVALID_ARGUMENT(
            "Hull-White calibration requires swap index fixed_calendar to match IBOR fixing calendar");
    }
    if (sidx.floatCalendar != ibor->fixingCalendar()) {
        QUANTRA_INVALID_ARGUMENT(
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

    // Clamp client knobs so no single calibration can be driven unbounded. These
    // CLAMPED values (not the raw request values) feed both the cache key and the
    // optimizer below, so a caller cannot vary a cache entry by asking for more
    // work than the server ceiling allows.
    const int clampedFunctionEvaluations =
        clampCalibrationKnob(calibSpec.function_evaluations, kMaxCalibrationFunctionEvaluations);
    const int clampedMaxIterations =
        clampCalibrationKnob(calibSpec.max_iterations, kMaxCalibrationIterations);

    // ------------------------------------------------------------------
    // Cross-request calibration cache.
    //
    // The calibrated (a, sigma) is a deterministic function of the consumed
    // market vols, the resolved grid, the discount/forwarding curves, the
    // swap-index conventions and the (clamped) calibration spec. Build a content
    // key from exactly those inputs and, when the cache is enabled and the key is
    // buildable, serve a prior result instead of re-running the whole
    // Levenberg-Marquardt fit.
    //
    // Fail-closed: if either curve cache key is unavailable (curve cache off, or
    // a bumped run) or key assembly throws, the key is left empty and we
    // calibrate live — never serve parameters under a partial key.
    // ------------------------------------------------------------------
    std::string cacheKey;
    try {
        const std::string discCurveKey = getCurveCacheKey(reg, discountCurveId);
        const std::string fwdCurveKey = getCurveCacheKey(reg, forwardingCurveId);
        if (!discCurveKey.empty() && !fwdCurveKey.empty()) {
            HwCalibrateKeyInputs ki;
            ki.consumedVols.reserve(expiries.size() * tenors.size());
            for (const auto& exp : expiries) {
                for (const auto& ten : tenors) {
                    ki.consumedVols.push_back(marketVolAtNode(volEntry, exp, ten));
                }
            }
            ki.expiries = expiries;
            ki.tenors = tenors;
            ki.volType = volEntry.qlVolType;
            ki.displacement = volEntry.displacement;
            ki.volReferenceDate = volEntry.referenceDate;
            ki.discountCurveKey = discCurveKey;
            ki.forwardingCurveKey = fwdCurveKey;
            ki.swapIndexId = swapIndexId;
            ki.floatIndexId = sidx.floatIndexId;
            ki.fixedFrequency = sidx.fixedFrequency;
            ki.fixedDayCounter = sidx.fixedDayCounter;
            ki.spotDays = sidx.spotDays;
            ki.fixedCalendar = sidx.fixedCalendar;
            ki.floatCalendar = sidx.floatCalendar;
            ki.iborTenor = ibor->tenor();
            ki.iborDayCounter = ibor->dayCounter();
            ki.iborConvention = ibor->businessDayConvention();
            ki.iborEndOfMonth = ibor->endOfMonth();
            ki.iborFixingCalendar = ibor->fixingCalendar();
            ki.calibrateA = calibSpec.calibrate_a;
            ki.calibrateSigma = calibSpec.calibrate_sigma;
            ki.aInit = calibSpec.a_init;
            ki.sigmaInit = calibSpec.sigma_init;
            ki.maxIterations = clampedMaxIterations;
            ki.functionEvaluations = clampedFunctionEvaluations;
            ki.endCriteriaEps = calibSpec.end_criteria_eps;
            ki.asOf = asOf;
            cacheKey = buildHwCalibrateCacheKey(ki);
        }
    } catch (const std::exception& e) {
        // Key-build failure must never fail calibration — it only loses caching
        // for this request. Warn once per process so the degradation is visible
        // without flooding logs.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::cerr << "[HwCalibCache] WARNING: cache key build failed: "
                      << e.what() << " — caching skipped (logged once per process)"
                      << std::endl;
        }
        cacheKey.clear();
    }

    auto& hwCache = HwCalibrateCache::instance();
    const bool cacheEnabled = HwCalibrateCache::enabled() && !cacheKey.empty();
    if (cacheEnabled) {
        if (auto cached = hwCache.tryGet(cacheKey)) {
            return *cached;
        }
    }

    // Cache miss (or caching disabled): a real calibration is about to run.
    g_hwCalibrationCallCount.fetch_add(1);

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

    const bool calibrateA = calibSpec.calibrate_a;
    const bool calibrateSigma = calibSpec.calibrate_sigma;
    if (!calibrateA && !calibrateSigma) {
        QUANTRA_INVALID_ARGUMENT("At least one of calibrate_a or calibrate_sigma must be true");
    }

    auto hwModel = QuantLib::ext::make_shared<QuantLib::HullWhite>(
        discountCurve, calibSpec.a_init, calibSpec.sigma_init);
    auto engine = QuantLib::ext::make_shared<QuantLib::JamshidianSwaptionEngine>(hwModel);
    for (auto& h : helpers) {
        auto blackHelper = QuantLib::ext::dynamic_pointer_cast<QuantLib::BlackCalibrationHelper>(h);
        if (!blackHelper) {
            QUANTRA_ERROR("Unexpected helper type during Hull-White calibration");
        }
        blackHelper->setPricingEngine(engine);
    }

    QuantLib::LevenbergMarquardt lm;
    // clampedFunctionEvaluations / clampedMaxIterations computed above (and fed
    // into the cache key) so the entry reflects the work actually performed.
    QuantLib::EndCriteria endCriteria(
        clampedFunctionEvaluations,
        clampedMaxIterations,
        calibSpec.end_criteria_eps,
        calibSpec.end_criteria_eps,
        calibSpec.end_criteria_eps);
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

    if (cacheEnabled) {
        hwCache.put(cacheKey, std::make_shared<const HwCalibResult>(out));
    }
    return out;
}

} // namespace quantra
