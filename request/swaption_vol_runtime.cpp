#include "swaption_vol_runtime.h"

#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/utilities/dataformatters.hpp>

#include "sabr_calibrate_cache_key.h"
#include "common.h"

namespace {

std::string getTradeFloatingIndexId(const quantra::PriceSwaption* p) {
    if (!p || !p->swaption()) return "";
    auto* sw = p->swaption();
    if (sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        auto* u = sw->underlying_as_VanillaSwap();
        if (u && u->floating_leg() && u->floating_leg()->index() && u->floating_leg()->index()->id()) {
            return u->floating_leg()->index()->id()->str();
        }
    } else if (sw->underlying_type() == quantra::SwaptionUnderlying_OisSwap) {
        auto* u = sw->underlying_as_OisSwap();
        if (u && u->overnight_leg() && u->overnight_leg()->index() && u->overnight_leg()->index()->id()) {
            return u->overnight_leg()->index()->id()->str();
        }
    }
    return "";
}

bool getTradeExerciseAndStartDates(
    const quantra::PriceSwaption* p,
    QuantLib::Date& exerciseDate,
    QuantLib::Date& startDate) {
    if (!p || !p->swaption() || !p->swaption()->exercise_date()) return false;
    const auto* sw = p->swaption();
    exerciseDate = DateToQL(sw->exercise_date()->str());

    if (sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        const auto* u = sw->underlying_as_VanillaSwap();
        if (u && u->fixed_leg() && u->fixed_leg()->schedule() && u->fixed_leg()->schedule()->effective_date()) {
            startDate = DateToQL(u->fixed_leg()->schedule()->effective_date()->str());
            return true;
        }
    } else if (sw->underlying_type() == quantra::SwaptionUnderlying_OisSwap) {
        const auto* u = sw->underlying_as_OisSwap();
        if (u && u->fixed_leg() && u->fixed_leg()->schedule() && u->fixed_leg()->schedule()->effective_date()) {
            startDate = DateToQL(u->fixed_leg()->schedule()->effective_date()->str());
            return true;
        }
    }

    return false;
}

} // namespace

namespace quantra {

std::vector<double> computeServerAtmForwards(
    const quantra::SwaptionVolEntry& volEntry,
    const quantra::SwapIndexRuntime& sidx,
    const quantra::IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve) {
    if (volEntry.nExp <= 0 || volEntry.nTen <= 0) {
        QUANTRA_ERROR("Invalid smile cube dimensions for ATM computation");
    }
    std::vector<double> atms;
    atms.reserve(volEntry.nExp * volEntry.nTen);

    const QuantLib::Date anchorDate = volEntry.referenceDate;
    for (int i = 0; i < volEntry.nExp; ++i) {
        QuantLib::Date exercise = sidx.fixedCalendar.advance(anchorDate, volEntry.expiries[i], sidx.fixedBdc);
        QuantLib::Date start = exercise;
        if (sidx.spotDays > 0) {
            start = sidx.fixedCalendar.advance(exercise, sidx.spotDays, QuantLib::Days, sidx.fixedBdc);
        }
        for (int j = 0; j < volEntry.nTen; ++j) {
            QuantLib::Date tentativeEnd = sidx.fixedCalendar.advance(start, volEntry.tenors[j], sidx.fixedTermBdc);
            QuantLib::Schedule fixedSchedule(
                start, tentativeEnd, QuantLib::Period(sidx.fixedFrequency), sidx.fixedCalendar,
                sidx.fixedBdc, sidx.fixedTermBdc, sidx.fixedDateRule, sidx.fixedEom);
            QuantLib::Date maturity = fixedSchedule.endDate();
            double atm = 0.0;

            if (sidx.kind == quantra::SwapIndexKind_OisSwapIndex) {
                auto on = indices.getOvernightWithCurve(sidx.floatIndexId, forwardingCurve);
                auto ois = std::make_shared<QuantLib::OvernightIndexedSwap>(
                    QuantLib::Swap::Payer, 1.0, fixedSchedule, 0.0, sidx.fixedDayCounter, on);
                ois->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountCurve));
                atm = ois->fairRate();
            } else {
                auto ibor = indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);
                QuantLib::Period floatTenor = sidx.floatTenor.length() > 0 ? sidx.floatTenor : ibor->tenor();
                QuantLib::Schedule floatSchedule(
                    start, maturity, floatTenor, sidx.floatCalendar,
                    sidx.floatBdc, sidx.floatTermBdc, sidx.floatDateRule, sidx.floatEom);
                auto swap = std::make_shared<QuantLib::VanillaSwap>(
                    QuantLib::VanillaSwap::Payer, 1.0, fixedSchedule, 0.0, sidx.fixedDayCounter,
                    floatSchedule, ibor, 0.0, ibor->dayCounter());
                swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountCurve));
                atm = swap->fairRate();
            }
            atms.push_back(atm);
        }
    }
    if (static_cast<int>(atms.size()) != volEntry.nExp * volEntry.nTen) {
        QUANTRA_ERROR("Computed ATM matrix size mismatch for swaption smile cube");
    }
    return atms;
}

std::vector<double> computeServerAtmForwardsForExerciseDates(
    const std::vector<QuantLib::Date>& exerciseDates,
    const std::vector<QuantLib::Period>& tenors,
    const quantra::SwapIndexRuntime& sidx,
    const quantra::IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve) {
    if (exerciseDates.empty() || tenors.empty()) {
        QUANTRA_ERROR("Invalid exerciseDates/tenors for ATM computation");
    }
    std::vector<double> atms;
    atms.reserve(exerciseDates.size() * tenors.size());

    for (const auto& exerciseRaw : exerciseDates) {
        QuantLib::Date exercise = sidx.fixedCalendar.adjust(exerciseRaw, sidx.fixedBdc);
        QuantLib::Date start = exercise;
        if (sidx.spotDays > 0) {
            start = sidx.fixedCalendar.advance(exercise, sidx.spotDays, QuantLib::Days, sidx.fixedBdc);
        }
        for (const auto& tenor : tenors) {
            QuantLib::Date tentativeEnd = sidx.fixedCalendar.advance(start, tenor, sidx.fixedTermBdc);
            QuantLib::Schedule fixedSchedule(
                start, tentativeEnd, QuantLib::Period(sidx.fixedFrequency), sidx.fixedCalendar,
                sidx.fixedBdc, sidx.fixedTermBdc, sidx.fixedDateRule, sidx.fixedEom);
            QuantLib::Date maturity = fixedSchedule.endDate();
            double atm = 0.0;

            if (sidx.kind == quantra::SwapIndexKind_OisSwapIndex) {
                auto on = indices.getOvernightWithCurve(sidx.floatIndexId, forwardingCurve);
                auto ois = std::make_shared<QuantLib::OvernightIndexedSwap>(
                    QuantLib::Swap::Payer, 1.0, fixedSchedule, 0.0, sidx.fixedDayCounter, on);
                ois->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountCurve));
                atm = ois->fairRate();
            } else {
                auto ibor = indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);
                QuantLib::Period floatTenor = sidx.floatTenor.length() > 0 ? sidx.floatTenor : ibor->tenor();
                QuantLib::Schedule floatSchedule(
                    start, maturity, floatTenor, sidx.floatCalendar,
                    sidx.floatBdc, sidx.floatTermBdc, sidx.floatDateRule, sidx.floatEom);
                auto swap = std::make_shared<QuantLib::VanillaSwap>(
                    QuantLib::VanillaSwap::Payer, 1.0, fixedSchedule, 0.0, sidx.fixedDayCounter,
                    floatSchedule, ibor, 0.0, ibor->dayCounter());
                swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountCurve));
                atm = swap->fairRate();
            }
            atms.push_back(atm);
        }
    }
    const size_t expected = exerciseDates.size() * tenors.size();
    if (atms.size() != expected) {
        QUANTRA_ERROR("Computed ATM matrix size mismatch for exercise-date based smile cube");
    }
    return atms;
}

namespace {

void validateTradeAgainstSwapIndex(
    const quantra::PriceSwaption* trade,
    const std::string& swapIndexId,
    const quantra::SwapIndexRuntime& sidx) {
    if (trade == nullptr) return;
    std::string tradeIndexId = getTradeFloatingIndexId(trade);
    if (!tradeIndexId.empty() && tradeIndexId != sidx.floatIndexId) {
        QUANTRA_ERROR(
            "Swap index '" + swapIndexId + "' float_index_id '" + sidx.floatIndexId +
            "' does not match swaption floating index '" + tradeIndexId + "'");
    }

    QuantLib::Date tradeExerciseDate, tradeStartDate;
    if (getTradeExerciseAndStartDates(trade, tradeExerciseDate, tradeStartDate)) {
        QuantLib::Date tradeExerciseAdjusted = sidx.fixedCalendar.adjust(tradeExerciseDate, sidx.fixedBdc);
        QuantLib::Date expectedStart = sidx.fixedCalendar.advance(
            tradeExerciseAdjusted, sidx.spotDays, QuantLib::Days, sidx.fixedBdc);
        QuantLib::Date tradeStartAdjusted = sidx.fixedCalendar.adjust(tradeStartDate, sidx.fixedBdc);
        if (expectedStart != tradeStartAdjusted) {
            std::ostringstream err;
            err << "Swap index '" << swapIndexId
                << "' spot_days mismatch against trade start convention: expected start "
                << DateToIso(expectedStart) << " from exercise "
                << DateToIso(tradeExerciseDate)
                << " (adjusted: " << DateToIso(tradeExerciseAdjusted) << ")"
                << " with spot_days=" << sidx.spotDays
                << ", but trade start is " << DateToIso(tradeStartDate)
                << " (adjusted: " << DateToIso(tradeStartAdjusted) << ")";
            QUANTRA_ERROR(err.str());
        }
    }
}

} // namespace

namespace {

// Resolve a curve cache key by curve id, returning "" when the curve cache is
// disabled or the curve is missing from the keys map. An empty key disables
// downstream caching for the SABR cube (safe fallback — never wrong, only slower).
std::string getCurveCacheKey(const PricingRegistry& reg, const std::string& curveId) {
    auto it = reg.rates.curveKeys.find(curveId);
    return (it == reg.rates.curveKeys.end()) ? std::string() : it->second;
}

std::string getCurveIdFromTrade(const quantra::PriceSwaption* trade, bool discounting) {
    if (!trade) return "";
    auto* id = discounting ? trade->discounting_curve() : trade->forwarding_curve();
    return id ? id->str() : "";
}

} // namespace

SwaptionVolEntry finalizeSwaptionVolEntryForPricing(
    const SwaptionVolEntry& raw,
    const quantra::PriceSwaption* trade,
    const PricingRegistry& reg,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    bool forceRecomputeAtm,
    const std::string& discountCurveId,
    const std::string& forwardingCurveId) {
    const bool isSmileCubeSpread =
        raw.volKind == quantra::enums::SwaptionVolKind_SmileCube3D &&
        raw.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM;
    const bool isSabrParams = raw.volKind == quantra::enums::SwaptionVolKind_SabrParams;
    const bool isSabrCalibrate = raw.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate;

    if (!isSmileCubeSpread && !isSabrParams && !isSabrCalibrate) {
        return raw;
    }
    if (raw.referenceDate == QuantLib::Date()) {
        QUANTRA_ERROR("Swaption vol surface requires a valid referenceDate");
    }
    if (raw.swapIndexId.empty()) {
        QUANTRA_ERROR("Swaption vol surface requires swap_index_id for forward resolution");
    }

    if (isSmileCubeSpread) {
        if (raw.allowExternalAtm && !raw.atmForwardsFlat.empty()) {
            return raw;
        }
        if (!forceRecomputeAtm && !raw.atmForwardsFlat.empty()) {
            return raw;
        }
    } else {
        // SABR variants: handle is built from atmForwardsFlat (and, for
        // calibrate, the QL cube), so an empty handle is the signal that
        // finalize hasn't run yet. If the handle is already built and forwards
        // are populated, only rebuild on explicit forceRecomputeAtm.
        if (!forceRecomputeAtm && !raw.handle.empty() && !raw.atmForwardsFlat.empty()) {
            return raw;
        }
    }

    if (!reg.rates.swapIndices.has(raw.swapIndexId)) {
        QUANTRA_ERROR("Missing swap index definition for id: " + raw.swapIndexId);
    }
    const auto& sidx = reg.rates.swapIndices.get(raw.swapIndexId);
    validateTradeAgainstSwapIndex(trade, raw.swapIndexId, sidx);

    auto atms = computeServerAtmForwards(
        raw, sidx, reg.rates.indices, discountCurve, forwardingCurve);

    if (isSabrParams) {
        return withSwaptionSabrParamsAtm(raw, atms);
    }
    if (isSabrCalibrate) {
        if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
            QUANTRA_ERROR(
                "SABR calibrate finalize: swap index '" + raw.swapIndexId +
                "' is not an Ibor swap index (OIS-shaped SABR calibrate not "
                "supported in v1)");
        }
        // Use the first tenor in the grid as the representative swap tenor
        // for the SwapIndex; XabrSwaptionVolatilityCube re-indexes internally
        // by exact (option, tenor) lookup so this choice is only the index's
        // "default" tenor for fallback/extrapolation paths.
        const QuantLib::Period swapIdxTenor = raw.tenors.front();
        auto swapIndexBase = reg.rates.swapIndices.getIborSwapIndexWithCurves(
            raw.swapIndexId, swapIdxTenor, reg.rates.indices, forwardingCurve, discountCurve);

        // Derive the discount/forwarding curve cache keys for the SABR cube
        // cache. Empty keys (e.g. bumped runs, or callers that didn't supply
        // ids and have no trade) disable caching for this run — safe; we
        // always rebuild rather than risk serving a stale cube.
        const std::string discId = !discountCurveId.empty()
            ? discountCurveId : getCurveIdFromTrade(trade, /*discounting=*/true);
        const std::string fwdId = !forwardingCurveId.empty()
            ? forwardingCurveId : getCurveIdFromTrade(trade, /*discounting=*/false);
        const std::string discCurveKey = getCurveCacheKey(reg, discId);
        const std::string fwdCurveKey = getCurveCacheKey(reg, fwdId);
        std::string cubeKey;
        if (!discCurveKey.empty() && !fwdCurveKey.empty()) {
            cubeKey = buildSabrCalibrateCacheKey(raw, atms, discCurveKey, fwdCurveKey);
        }
        return withSwaptionSabrCalibrateAtm(raw, atms, swapIndexBase, cubeKey);
    }
    return withSwaptionSmileCubeAtm(raw, atms);
}

} // namespace quantra
