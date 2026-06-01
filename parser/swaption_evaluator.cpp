#include "swaption_evaluator.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <variant>

#include <ql/exercise.hpp>
#include <ql/instruments/fixedvsfloatingswap.hpp>
#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/pricingengines/bacheliercalculator.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/swaption/blackswaptionengine.hpp>
#include <ql/pricingengines/swaption/treeswaptionengine.hpp>
#include <ql/settings.hpp>
#include <ql/utilities/dataformatters.hpp>
#include <ql/utilities/null.hpp>
#include <ql/version.hpp>

#include "common.h"
#include "enums.h"
#include "error.h"
#include "eval_date_guard.h"
#include "model_domain.h"
#include "sabr_calibrate_cache_key.h"
#include "swaption_model_calibration.h"
#include "swaption_vol_runtime.h"

// BachelierSwaptionEngine ships in blackswaptionengine.hpp from QuantLib 1.20 on.
#if QL_HEX_VERSION >= 0x012000f0
    #define QL_HAS_BACHELIER_SWAPTION_ENGINE 1
#else
    #define QL_HAS_BACHELIER_SWAPTION_ENGINE 0
#endif

namespace quantra {

namespace {

/// Build the underlying QL swap from the plain instrument. Reproduces
/// VanillaSwapParser::parse / OisSwapParser::parse construction exactly — same
/// argument order, same index cloning against the forwarding handle, same
/// notional-mismatch warning — so the swaption prices byte-identically.
std::shared_ptr<QuantLib::FixedVsFloatingSwap> buildUnderlyingSwap(
    const SwaptionInstrument& inst,
    const IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding) {
    if (inst.underlyingIsVanilla) {
        const auto& v = inst.vanillaUnderlying;
        auto iborIndex = indices.getIborWithCurve(v.ibor.indexId, forwarding);
        if (v.fixed.notional != v.ibor.notional) {
            std::cout << "Warning: Fixed and floating notionals differ. Using fixed notional."
                      << std::endl;
        }
        return std::make_shared<QuantLib::VanillaSwap>(
            v.swapType,
            v.fixed.notional,
            v.fixed.schedule,
            v.fixed.rate,
            v.fixed.dayCounter,
            v.ibor.schedule,
            iborIndex,
            v.ibor.spread,
            v.ibor.dayCounter);
    }

    const auto& o = inst.oisUnderlying;
    auto overnightIndex = indices.getOvernightWithCurve(o.overnight.indexId, forwarding);
    QuantLib::Natural lookbackDays = o.overnight.lookbackDays < 0
        ? QuantLib::Null<QuantLib::Natural>()
        : static_cast<QuantLib::Natural>(o.overnight.lookbackDays);
    QuantLib::Natural lockoutDays =
        static_cast<QuantLib::Natural>(o.overnight.lockoutDays);
    if (o.fixed.notional != o.overnight.notional) {
        std::cout << "Warning: Fixed and overnight notionals differ. Using fixed notional."
                  << std::endl;
    }
    return std::make_shared<QuantLib::OvernightIndexedSwap>(
        o.swapType,
        o.fixed.notional,
        o.fixed.schedule,
        o.fixed.rate,
        o.fixed.dayCounter,
        o.overnight.schedule,
        overnightIndex,
        o.overnight.spread,
        o.overnight.paymentLag,
        o.overnight.paymentConvention,
        o.overnight.paymentCalendar,
        o.overnight.telescopicValueDates,
        o.overnight.averagingMethod,
        lookbackDays,
        lockoutDays,
        o.overnight.applyObservationShift);
}

/// FB-free reconstruction of the QuantLib::Swaption from the plain instrument.
/// Reproduces the legacy SwaptionParser::parse step-for-step: exercise-date
/// validation, underlying-swap construction, exercise object, settlement type,
/// and final assembly. The evaluation-date-dependent pieces (American exercise
/// reference, Bermudan date checks) read Settings::evaluationDate() live, so the
/// theta-leg rebuild under a rolled eval date matches parse() exactly. Called at
/// both the base and rebump valuation sites in place of the old closure.
std::shared_ptr<QuantLib::Swaption> buildSwaptionInstrument(
    const SwaptionInstrument& inst,
    const IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding) {
    const auto exerciseType = inst.exerciseType;
    const bool needsSingleExerciseDate =
        (exerciseType == quantra::enums::ExerciseType_European ||
         exerciseType == quantra::enums::ExerciseType_American);
    const bool needsExerciseDateSet =
        (exerciseType == quantra::enums::ExerciseType_Bermudan);

    if (needsSingleExerciseDate && !inst.hasExerciseDate)
        QUANTRA_INVALID_ARGUMENT("Swaption exercise_date not found");

    if (needsExerciseDateSet) {
        if (inst.bermudanExerciseDates.size() < 2) {
            QUANTRA_INVALID_ARGUMENT("Swaption Bermudan requires exercise_dates with at least 2 dates");
        }
        const QuantLib::Date evalDate = QuantLib::Settings::instance().evaluationDate();
        for (size_t i = 1; i < inst.bermudanExerciseDates.size(); ++i) {
            if (inst.bermudanExerciseDates[i] <= inst.bermudanExerciseDates[i - 1]) {
                QUANTRA_INVALID_ARGUMENT("Swaption Bermudan exercise_dates must be strictly increasing");
            }
        }
        for (const auto& d : inst.bermudanExerciseDates) {
            if (d < evalDate) {
                QUANTRA_INVALID_ARGUMENT("Swaption Bermudan exercise_dates must be on/after evaluation date");
            }
        }
    }

    auto underlyingSwap = buildUnderlyingSwap(inst, indices, forwarding);

    std::shared_ptr<QuantLib::Exercise> exercise;
    switch (exerciseType) {
        case quantra::enums::ExerciseType_European:
            exercise = std::make_shared<QuantLib::EuropeanExercise>(inst.exerciseDate);
            break;
        case quantra::enums::ExerciseType_Bermudan:
            exercise = std::make_shared<QuantLib::BermudanExercise>(inst.bermudanExerciseDates);
            break;
        case quantra::enums::ExerciseType_American:
            exercise = std::make_shared<QuantLib::AmericanExercise>(
                QuantLib::Settings::instance().evaluationDate(),
                inst.exerciseDate);
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid exercise type");
    }

    QuantLib::Settlement::Type settlementType;
    switch (inst.settlementType) {
        case quantra::enums::SettlementType_Physical:
            settlementType = QuantLib::Settlement::Physical;
            break;
        case quantra::enums::SettlementType_Cash:
            settlementType = QuantLib::Settlement::Cash;
            break;
        default:
            settlementType = QuantLib::Settlement::Physical;
    }

    return std::make_shared<QuantLib::Swaption>(
        underlyingSwap,
        exercise,
        settlementType,
        inst.settlementMethod);
}

/**
 * Plain-domain finalize for a SwaptionVolEntry. Mirrors
 * finalizeSwaptionVolEntryForPricing in swaption_vol_runtime, but takes the
 * trade-derived validation context as plain values (float-index id, exercise
 * date, swap start date) so the evaluator never sees the FB request. Reuses the
 * shared computeServerAtmForwards helper for the actual ATM matrix.
 */
SwaptionVolEntry finalizeVolEntry(
    const SwaptionVolEntry& raw,
    const PricingRegistry& reg,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    bool forceRecomputeAtm,
    const SwaptionTrade& trade) {
    const bool isSmileCubeSpread =
        raw.volKind == quantra::enums::SwaptionVolKind_SmileCube3D &&
        raw.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM;
    const bool isSabrParams = raw.volKind == quantra::enums::SwaptionVolKind_SabrParams;
    const bool isSabrCalibrate = raw.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate;

    if (!isSmileCubeSpread && !isSabrParams && !isSabrCalibrate) {
        return raw;
    }
    if (raw.referenceDate == QuantLib::Date()) {
        QUANTRA_INVALID_ARGUMENT("Swaption vol surface requires a valid referenceDate");
    }
    if (raw.swapIndexId.empty()) {
        QUANTRA_INVALID_ARGUMENT("Swaption vol surface requires swap_index_id for forward resolution");
    }

    if (isSmileCubeSpread) {
        if (raw.allowExternalAtm && !raw.atmForwardsFlat.empty()) {
            return raw;
        }
        if (!forceRecomputeAtm && !raw.atmForwardsFlat.empty()) {
            return raw;
        }
    } else {
        if (!forceRecomputeAtm && !raw.handle.empty() && !raw.atmForwardsFlat.empty()) {
            return raw;
        }
    }

    if (!reg.rates.swapIndices.has(raw.swapIndexId)) {
        QUANTRA_NOT_FOUND("Missing swap index definition for id: " + raw.swapIndexId);
    }
    const auto& sidx = reg.rates.swapIndices.get(raw.swapIndexId);

    // Trade-context validation: float-index match and spot-days alignment.
    if (!trade.tradeFloatIndexId.empty() && trade.tradeFloatIndexId != sidx.floatIndexId) {
        QUANTRA_INVALID_ARGUMENT(
            "Swap index '" + raw.swapIndexId + "' float_index_id '" + sidx.floatIndexId +
            "' does not match swaption floating index '" + trade.tradeFloatIndexId + "'");
    }
    if (trade.exerciseDate != QuantLib::Date() && trade.underlyingStartDate != QuantLib::Date()) {
        QuantLib::Date tradeExerciseAdjusted =
            sidx.fixedCalendar.adjust(trade.exerciseDate, sidx.fixedBdc);
        QuantLib::Date expectedStart = sidx.fixedCalendar.advance(
            tradeExerciseAdjusted, sidx.spotDays, QuantLib::Days, sidx.fixedBdc);
        QuantLib::Date tradeStartAdjusted =
            sidx.fixedCalendar.adjust(trade.underlyingStartDate, sidx.fixedBdc);
        if (expectedStart != tradeStartAdjusted) {
            std::ostringstream err;
            err << "Swap index '" << raw.swapIndexId
                << "' spot_days mismatch against trade start convention: expected start "
                << DateToIso(expectedStart) << " from exercise "
                << DateToIso(trade.exerciseDate)
                << " (adjusted: " << DateToIso(tradeExerciseAdjusted) << ")"
                << " with spot_days=" << sidx.spotDays
                << ", but trade start is " << DateToIso(trade.underlyingStartDate)
                << " (adjusted: " << DateToIso(tradeStartAdjusted) << ")";
            QUANTRA_INVALID_ARGUMENT(err.str());
        }
    }

    auto atms = computeServerAtmForwards(
        raw, sidx, reg.rates.indices, discountCurve, forwardingCurve);

    if (isSabrParams) {
        return withSwaptionSabrParamsAtm(raw, atms);
    }
    if (isSabrCalibrate) {
        if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
            QUANTRA_NOT_IMPLEMENTED(
                "SABR calibrate finalize: swap index '" + raw.swapIndexId +
                "' is not an Ibor swap index (OIS-shaped SABR calibrate not "
                "supported in v1)");
        }
        const QuantLib::Period swapIdxTenor = raw.tenors.front();
        auto swapIndexBase = reg.rates.swapIndices.getIborSwapIndexWithCurves(
            raw.swapIndexId, swapIdxTenor, reg.rates.indices, forwardingCurve, discountCurve);

        auto discKeyIt = reg.rates.curveKeys.find(trade.discountingCurveId);
        auto fwdKeyIt = reg.rates.curveKeys.find(trade.forwardingCurveId);
        const std::string discCurveKey =
            (discKeyIt == reg.rates.curveKeys.end()) ? std::string() : discKeyIt->second;
        const std::string fwdCurveKey =
            (fwdKeyIt == reg.rates.curveKeys.end()) ? std::string() : fwdKeyIt->second;
        std::string cubeKey;
        if (!discCurveKey.empty() && !fwdCurveKey.empty()) {
            cubeKey = buildSabrCalibrateCacheKey(raw, atms, discCurveKey, fwdCurveKey);
        }
        return withSwaptionSabrCalibrateAtm(raw, atms, swapIndexBase, cubeKey);
    }
    return withSwaptionSmileCubeAtm(raw, atms);
}

/// Inline construction of the swaption pricing engine from a SwaptionModelDomain.
/// Mirrors the legacy EngineFactory::makeSwaptionEngine branching exactly, plus
/// the HullWhiteLattice path (Explicit) and the Calibrate path (consumes the
/// already-computed HwCalibResult). Validation messages match legacy verbatim.
std::shared_ptr<QuantLib::PricingEngine> buildEngine(
    const std::string& modelId,
    const SwaptionModelDomain& model,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const SwaptionVolEntry& volEntry,
    const HwCalibResult* calibrated) {
    if (model.model_type == IrModelTypeKind::Bachelier &&
        (volEntry.volKind == quantra::enums::SwaptionVolKind_SabrParams ||
         volEntry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate)) {
        QUANTRA_INVALID_ARGUMENT(
            "Model '" + modelId + "': Bachelier engine cannot be paired with SABR vol surface "
            "(SABR via Hagan returns lognormal Black vol). Use Black or ShiftedBlack instead.");
    }

    switch (model.model_type) {
        case IrModelTypeKind::Bachelier:
#if QL_HAS_BACHELIER_SWAPTION_ENGINE
            if (volEntry.qlVolType != QuantLib::Normal) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Bachelier requires Normal vols");
            }
            return std::make_shared<QuantLib::BachelierSwaptionEngine>(
                discountCurve, volEntry.handle);
#else
            QUANTRA_NOT_IMPLEMENTED("Model '" + modelId + "': BachelierSwaptionEngine not available "
                          "in this QuantLib version (requires QuantLib 1.20+)");
#endif

        case IrModelTypeKind::Black:
            if (volEntry.displacement != 0.0) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Black requires displacement=0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackSwaptionEngine>(
                discountCurve, volEntry.handle);

        case IrModelTypeKind::ShiftedBlack:
            if (volEntry.displacement <= 0.0) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': ShiftedBlack requires displacement>0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackSwaptionEngine>(
                discountCurve, volEntry.handle);

        case IrModelTypeKind::HullWhiteLattice: {
            double a = model.hw_a;
            double sigma = model.hw_sigma;
            if (model.param_mode == ModelParamModeKind::Calibrate) {
                if (calibrated == nullptr) {
                    QUANTRA_ERROR("Model '" + modelId + "' Calibrate path missing HW result");
                }
                a = calibrated->a;
                sigma = calibrated->sigma;
            }
            if (model.lattice_steps <= 0) {
                QUANTRA_INVALID_ARGUMENT("HullWhiteLattice requires lattice_steps > 0");
            }
            auto hwModel = std::make_shared<QuantLib::HullWhite>(discountCurve, a, sigma);
            return std::make_shared<QuantLib::TreeSwaptionEngine>(hwModel, model.lattice_steps);
        }
    }
    QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Unknown IrModelType value " +
                  std::to_string(static_cast<int>(model.model_type)));
    return nullptr;
}

/// Up-front consistency checks lifted verbatim from the legacy request body.
/// Run before bootstrap of the QL Swaption so error messages match exactly.
void validateTradeAgainstModel(
    const SwaptionTrade& trade,
    const SwaptionModelDomain& model,
    const PricingRegistry& reg) {
    // Exercise type vs model: Bermudan/American is HullWhiteLattice only.
    // The evaluator carries the raw FB enum (see swaption.fbs) plumbed through
    // the plain SwaptionInstrument; the validation only needs the model side.
    if (model.model_type != IrModelTypeKind::HullWhiteLattice &&
        model.param_mode == ModelParamModeKind::Calibrate) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' param_mode=Calibrate is only supported for HullWhiteLattice");
    }
    if (model.model_type != IrModelTypeKind::HullWhiteLattice) {
        return;
    }
    if (model.param_mode != ModelParamModeKind::Calibrate) {
        return;
    }
    if (!model.hw_calibration) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' param_mode=Calibrate requires hw_calibration");
    }
    const auto& calib = *model.hw_calibration;
    if (calib.discount_curve_id.empty() || calib.forwarding_curve_id.empty() ||
        calib.swaption_vol_id.empty() || calib.swap_index_id.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "Model '" + trade.modelId + "' hw_calibration must include discount_curve_id, "
            "forwarding_curve_id, swaption_vol_id, and swap_index_id");
    }
    if (calib.discount_curve_id != trade.discountingCurveId) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' hw_calibration.discount_curve_id must match trade discounting_curve");
    }
    if (calib.forwarding_curve_id != trade.forwardingCurveId) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' hw_calibration.forwarding_curve_id must match trade forwarding_curve");
    }
    if (calib.swaption_vol_id != trade.volatilityId) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' hw_calibration.swaption_vol_id must match trade volatility");
    }
    if (!reg.rates.swapIndices.has(calib.swap_index_id)) {
        QUANTRA_NOT_FOUND("Model '" + trade.modelId +
                      "' hw_calibration.swap_index_id not found in pricing.swap_indices");
    }
    if (!trade.underlyingIsVanillaSwap) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' param_mode=Calibrate currently supports VanillaSwap underlyings only");
    }
    if (trade.tradeFloatIndexId.empty()) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' param_mode=Calibrate requires trade floating index id for compatibility checks");
    }
    const auto& sidx = reg.rates.swapIndices.get(calib.swap_index_id);
    if (sidx.floatIndexId != trade.tradeFloatIndexId) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId +
                      "' hw_calibration.swap_index_id float_index_id does not match "
                      "trade floating index");
    }
}

double resultOrDefault(const std::shared_ptr<QuantLib::Swaption>& swap,
                       const std::string& key, double fallback) {
    try {
        return swap->result<double>(key);
    } catch (...) {
        return fallback;
    }
}

} // namespace

SwaptionResult SwaptionEvaluator::evaluate(const SwaptionInputs& inputs,
                                     const PricingRegistry& reg,
                                     const PricingContext& ctx) const {
    SwaptionResult result;
    result.values.reserve(inputs.trades.size());
    result.includeDiagnostics = inputs.includeDiagnostics;

    // HW calibrations are deterministic for a given (model_id, asOf, market), so
    // we calibrate at most once per model_id and reuse the result for rebump
    // and theta legs — mirrors the legacy per-request cache.
    std::unordered_map<std::string, HwCalibResult> hwCalibrationCache;

    for (const auto& trade : inputs.trades) {
        auto dIt = reg.rates.curves.find(trade.discountingCurveId);
        if (dIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
        }
        auto fIt = reg.rates.curves.find(trade.forwardingCurveId);
        if (fIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Forwarding curve not found: " + trade.forwardingCurveId);
        }
        auto vIt = reg.volatility.swaptionVols.find(trade.volatilityId);
        if (vIt == reg.volatility.swaptionVols.end()) {
            QUANTRA_NOT_FOUND("Swaption vol not found: " + trade.volatilityId);
        }
        if (vIt->second.referenceDate == QuantLib::Date()) {
            QUANTRA_INVALID_ARGUMENT("Swaption vol has invalid referenceDate: " + trade.volatilityId);
        }
        if (vIt->second.referenceDate != ctx.asOf) {
            std::ostringstream err;
            err << "Strict mode: pricing.as_of_date (" << DateToIso(ctx.asOf)
                << ") must equal swaption vol referenceDate ("
                << DateToIso(vIt->second.referenceDate)
                << ") for vol '" << trade.volatilityId << "'";
            QUANTRA_INVALID_ARGUMENT(err.str());
        }
        auto mIt = reg.volatility.modelDomains.find(trade.modelId);
        if (mIt == reg.volatility.modelDomains.end()) {
            QUANTRA_NOT_FOUND("Model not found: " + trade.modelId);
        }
        const auto* modelDomain = std::get_if<SwaptionModelDomain>(&mIt->second.payload);
        if (modelDomain == nullptr) {
            QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId + "' is not a swaption model");
        }

        validateTradeAgainstModel(trade, *modelDomain, reg);

        // Build the base QL Swaption against the (un-bumped) forwarding curve.
        QuantLib::Handle<QuantLib::YieldTermStructure> forwardingHandle(fIt->second->currentLink());
        QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle(dIt->second->currentLink());
        auto swaption = buildSwaptionInstrument(trade.instrument, reg.rates.indices, forwardingHandle);

        // Resolve the vol entry (finalize SABR/spread-from-ATM surfaces with
        // server-computed ATM forwards). Run once per trade for the base path.
        SwaptionVolEntry volEntry = finalizeVolEntry(
            vIt->second, reg, discountHandle, forwardingHandle,
            /*forceRecomputeAtm=*/false, trade);

        if (inputs.includeDiagnostics &&
            (volEntry.volKind == enums::SwaptionVolKind_SabrCalibrate ||
             volEntry.volKind == enums::SwaptionVolKind_SabrParams)) {
            if (!result.finalizedSabrEntries.count(trade.volatilityId)) {
                result.sabrVolIdsInOrder.push_back(trade.volatilityId);
            }
            result.finalizedSabrEntries[trade.volatilityId] = volEntry;
        }

        // Resolve calibrated HW parameters once per model_id; reused below in
        // engine construction and for the used_hw_* diagnostics.
        const HwCalibResult* calibratedHw = nullptr;
        if (modelDomain->model_type == IrModelTypeKind::HullWhiteLattice &&
            modelDomain->param_mode == ModelParamModeKind::Calibrate) {
            auto cacheIt = hwCalibrationCache.find(trade.modelId);
            if (cacheIt == hwCalibrationCache.end()) {
                cacheIt = hwCalibrationCache.emplace(
                    trade.modelId,
                    calibrateHullWhiteFromSwaptionVol(reg, *modelDomain->hw_calibration, ctx.asOf))
                    .first;
            }
            calibratedHw = &cacheIt->second;
        }

        auto engine = buildEngine(trade.modelId, *modelDomain, discountHandle, volEntry, calibratedHw);
        swaption->setPricingEngine(engine);

        const double npv = swaption->NPV();
        std::cout << "Swaption NPV: " << npv << std::endl;

        SwaptionPerTrade row;
        row.npv = npv;
        row.impliedVolatility =
            (volEntry.volKind == quantra::enums::SwaptionVolKind_Constant)
                ? volEntry.constantVol
                : std::numeric_limits<double>::quiet_NaN();
        row.volKind = volEntry.volKind;
        row.usedStrikeKind = volEntry.strikeKind;
        row.usedModelParamMode =
            modelDomain->param_mode == ModelParamModeKind::Calibrate
                ? quantra::enums::ModelParamMode_Calibrate
                : quantra::enums::ModelParamMode_Explicit;

        if (modelDomain->model_type == IrModelTypeKind::HullWhiteLattice) {
            if (calibratedHw != nullptr) {
                row.usedHwA = calibratedHw->a;
                row.usedHwSigma = calibratedHw->sigma;
                row.usedHwRmse = calibratedHw->rmse;
                row.usedHwNumHelpers = calibratedHw->numHelpers;
                row.usedHwGridRows = calibratedHw->gridRows;
                row.usedHwGridCols = calibratedHw->gridCols;
                row.usedHwGridPoints = calibratedHw->gridPoints;
            } else {
                row.usedHwA = modelDomain->hw_a;
                row.usedHwSigma = modelDomain->hw_sigma;
            }
        }

        // Re-prices the swaption with optional curve/vol/eval-date
        // perturbations. Reuses cached HW calibration and the mapper-supplied
        // pre-built market snapshots to keep the rebump path entirely FB-free.
        // Scenario -> snapshot selection (the whole numerical ballgame):
        //   curveBump>0                 -> curveUp   (+bump @ asOf)
        //   curveBump<0                 -> curveDown (-bump @ asOf)
        //   rollDays>0                  -> roll      (bump 0 @ asOf+rollDays)
        //   curveBump==0 && rollDays==0 -> base registry curves @ asOf (vol bumps)
        auto priceWithRebump = [&](double curveBump, double volBump, int rollDays) {
            EvalDateGuard evalGuard;
            QuantLib::Date eval = ctx.asOf + rollDays;
            QuantLib::Settings::instance().evaluationDate() = eval;

            if (!inputs.rebumpMarkets.present) {
                QUANTRA_ERROR("Rebump greeks requested but rebump market snapshots are unset");
            }

            QuantLib::Handle<QuantLib::YieldTermStructure> bDiscount;
            QuantLib::Handle<QuantLib::YieldTermStructure> bForwarding;
            const IndexRegistry* bumpIndices = nullptr;

            if (curveBump != 0.0 || rollDays != 0) {
                const SwaptionRebumpedMarket& market =
                    (curveBump > 0.0)   ? inputs.rebumpMarkets.curveUp
                    : (curveBump < 0.0) ? inputs.rebumpMarkets.curveDown
                                        : inputs.rebumpMarkets.roll;
                auto dItB = market.curves.handles.find(trade.discountingCurveId);
                if (dItB == market.curves.handles.end()) {
                    QUANTRA_ERROR("Discounting curve not found (rebump): " + trade.discountingCurveId);
                }
                auto fItB = market.curves.handles.find(trade.forwardingCurveId);
                if (fItB == market.curves.handles.end()) {
                    QUANTRA_ERROR("Forwarding curve not found (rebump): " + trade.forwardingCurveId);
                }
                bDiscount = QuantLib::Handle<QuantLib::YieldTermStructure>(dItB->second->currentLink());
                bForwarding = QuantLib::Handle<QuantLib::YieldTermStructure>(fItB->second->currentLink());
                bumpIndices = &market.indices;
            } else {
                // Vol-bump legs: base registry curves at asOf — bootstrapped
                // identically to a fresh bump-0 @ asOf snapshot.
                auto dItB = reg.rates.curves.find(trade.discountingCurveId);
                if (dItB == reg.rates.curves.end()) {
                    QUANTRA_ERROR("Discounting curve not found (rebump): " + trade.discountingCurveId);
                }
                auto fItB = reg.rates.curves.find(trade.forwardingCurveId);
                if (fItB == reg.rates.curves.end()) {
                    QUANTRA_ERROR("Forwarding curve not found (rebump): " + trade.forwardingCurveId);
                }
                bDiscount = QuantLib::Handle<QuantLib::YieldTermStructure>(dItB->second->currentLink());
                bForwarding = QuantLib::Handle<QuantLib::YieldTermStructure>(fItB->second->currentLink());
                bumpIndices = &reg.rates.indices;
            }

            auto bumpSwap = buildSwaptionInstrument(trade.instrument, *bumpIndices, bForwarding);

            SwaptionVolEntry volEntryBumped = bumpSwaptionVolEntry(volEntry, volBump);
            const bool forceAtmRecompute = (curveBump != 0.0) || (rollDays != 0);
            volEntryBumped = finalizeVolEntry(
                volEntryBumped, reg, bDiscount, bForwarding, forceAtmRecompute, trade);

            auto bumpEngine = buildEngine(
                trade.modelId, *modelDomain, bDiscount, volEntryBumped, calibratedHw);
            bumpSwap->setPricingEngine(bumpEngine);
            return bumpSwap->NPV();
        };

        if (ctx.options.swaptionPricingDetails) {
            row.impliedVolatility = resultOrDefault(swaption, "impliedVolatility", row.impliedVolatility);
            row.atmForward = resultOrDefault(swaption, "atmForward", 0.0);
            row.annuity = resultOrDefault(swaption, "annuity", 0.0);

            const double strike = resultOrDefault(swaption, "strike", 0.0);
            const double stdDev = resultOrDefault(swaption, "stdDev", 0.0);
            const double timeToExpiry = resultOrDefault(swaption, "timeToExpiry", 0.0);

            if (row.annuity != 0.0 && stdDev > 0.0 && timeToExpiry > 0.0) {
                QuantLib::Option::Type optType =
                    (swaption->underlying()->type() == QuantLib::Swap::Payer)
                        ? QuantLib::Option::Call
                        : QuantLib::Option::Put;
                if (volEntry.qlVolType == QuantLib::Normal) {
                    QuantLib::BachelierCalculator calc(
                        optType, strike, row.atmForward, stdDev, row.annuity);
                    row.delta = calc.deltaForward();
                    row.vega = calc.vega(timeToExpiry);
                    row.gamma = calc.gammaForward();
                    row.theta = calc.theta(row.atmForward, timeToExpiry);
                } else {
                    const double displacement = volEntry.displacement;
                    QuantLib::BlackCalculator calc(
                        optType, strike + displacement, row.atmForward + displacement,
                        stdDev, row.annuity);
                    row.delta = calc.deltaForward();
                    row.vega = calc.vega(timeToExpiry);
                    row.gamma = calc.gammaForward();
                    row.theta = calc.theta(row.atmForward + displacement, timeToExpiry);
                }
            }
            row.dv01 = row.delta * 1.0e-4;
        }
        if (!std::isfinite(row.impliedVolatility)) {
            row.impliedVolatility = -1.0;
        }

        if (ctx.options.swaptionPricingRebump) {
            const double bump = 1.0e-4; // 1bp
            const double npvUp = priceWithRebump(bump, 0.0, 0);
            const double npvDown = priceWithRebump(-bump, 0.0, 0);
            row.dv01 = (npvUp - npvDown) / 2.0;
            row.gamma = (npvUp - 2.0 * npv + npvDown);

            const double volUp = priceWithRebump(0.0, bump, 0);
            const double volDown = priceWithRebump(0.0, -bump, 0);
            row.vega = (volUp - volDown) / 2.0;

            const double npvTomorrow = priceWithRebump(0.0, 0.0, 1);
            row.theta = npvTomorrow - npv;
        }

        // used_* diagnostics: best-effort. Failures fall back to sentinels.
        try {
            if (volEntry.handle.empty()) {
                QUANTRA_ERROR("Swaption vol handle is empty (ATM not injected?)");
            }
            QuantLib::Date evalDate = QuantLib::Settings::instance().evaluationDate();
            QuantLib::Date exerciseDate = swaption->exercise()->dates()[0];
            const auto& volDc = volEntry.dayCounter;
            double optionTime = volDc.yearFraction(evalDate, exerciseDate);
            QuantLib::Date startDate = swaption->underlying()->startDate();
            QuantLib::Date endDate = swaption->underlying()->maturityDate();
            double swapLength = volDc.yearFraction(startDate, endDate);

            row.usedStrike = resultOrDefault(swaption, "strike", 0.0);
            if (row.usedStrike == 0.0) {
                if (auto vanilla = QuantLib::ext::dynamic_pointer_cast<QuantLib::VanillaSwap>(
                        swaption->underlying())) {
                    row.usedStrike = vanilla->fixedRate();
                } else if (auto ois = QuantLib::ext::dynamic_pointer_cast<QuantLib::OvernightIndexedSwap>(
                               swaption->underlying())) {
                    row.usedStrike = ois->fixedRate();
                }
            }

            row.usedVolatility =
                volEntry.handle->volatility(optionTime, swapLength, row.usedStrike);
            const bool hasRealAtm = !volEntry.atmForwardsFlat.empty();
            try {
                auto smile = volEntry.handle->smileSection(optionTime, swapLength);
                if (smile && hasRealAtm) {
                    row.usedAtmForward = smile->atmLevel();
                    row.usedCubeNodeAtm = row.usedAtmForward;
                }
            } catch (...) {
                row.usedAtmForward = -1.0;
                row.usedCubeNodeAtm = -1.0;
            }
            if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM &&
                row.usedCubeNodeAtm >= 0.0) {
                row.usedSpreadFromAtm = row.usedStrike - row.usedCubeNodeAtm;
            }

            row.usedOptionExpiry = DateToIso(exerciseDate);
            std::ostringstream tenor;
            tenor << DateToIso(startDate) << "->" << DateToIso(endDate);
            row.usedSwapTenor = tenor.str();
        } catch (...) {
            // Diagnostics are best-effort; leave defaults on failure.
        }

        result.values.push_back(std::move(row));
    }

    return result;
}

} // namespace quantra
