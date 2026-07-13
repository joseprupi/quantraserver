#include "year_on_year_inflation_cap_floor_evaluator.h"

#include <memory>
#include <vector>

#include <ql/cashflows/yoyinflationcoupon.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/inflationindex.hpp>
#include <ql/pricingengines/inflation/inflationcapfloorengines.hpp>
#include <ql/termstructures/volatility/inflation/yoyinflationoptionletvolatilitystructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "error.h"

namespace quantra {

namespace {

/// Build the constant YoY optionlet vol surface from the resolved vol entry
/// plus the index conventions. Frequency and indexIsInterpolated come from the
/// YoY inflation curve metadata (i.e. the index the leg observes), so the
/// surface stays convention-consistent with its index. Settlement days are 0
/// (reference date == evaluation date); the pricing formula is picked by the
/// engine, so the surface itself uses QuantLib's default vol type — matching
/// the reference, whose QuantLib-Python ConstantYoYOptionletVolatility ctor
/// does not expose the vol-type argument.
std::shared_ptr<QuantLib::YoYOptionletVolatilitySurface> buildVolSurface(
    const YoYOptionletVolEntry& volEntry,
    QuantLib::Frequency frequency,
    bool indexInterpolated) {
    return std::make_shared<QuantLib::ConstantYoYOptionletVolatility>(
        volEntry.constantVol,
        0,
        volEntry.calendar,
        volEntry.businessDayConvention,
        volEntry.dayCounter,
        volEntry.observationLag,
        frequency,
        indexInterpolated);
}

std::shared_ptr<QuantLib::YoYInflationCapFloorEngine> buildEngine(
    YoYInflationEngineKind kind,
    const std::shared_ptr<QuantLib::YoYInflationIndex>& index,
    const QuantLib::Handle<QuantLib::YoYOptionletVolatilitySurface>& vol,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& nominal) {
    switch (kind) {
        case YoYInflationEngineKind::Black:
            return std::make_shared<QuantLib::YoYInflationBlackCapFloorEngine>(
                index, vol, nominal);
        case YoYInflationEngineKind::UnitDisplacedBlack:
            return std::make_shared<QuantLib::YoYInflationUnitDisplacedBlackCapFloorEngine>(
                index, vol, nominal);
        case YoYInflationEngineKind::Bachelier:
            return std::make_shared<QuantLib::YoYInflationBachelierCapFloorEngine>(
                index, vol, nominal);
    }
    QUANTRA_INVALID_ARGUMENT("Unknown YoY inflation cap/floor engine kind");
    return nullptr;
}

YoYInflationCapFloorPerTrade priceTrade(const YoYInflationCapFloorTrade& trade,
                                        const PricingRegistry& reg) {
    auto discountIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }

    auto curveIt = reg.inflation.yoyInflationCurves.find(trade.inflationCurveId);
    if (curveIt == reg.inflation.yoyInflationCurves.end() ||
        !curveIt->second || curveIt->second->empty()) {
        QUANTRA_NOT_FOUND("YoY inflation curve not found: " + trade.inflationCurveId);
    }

    auto metaIt = reg.inflation.curveMetadata.find(trade.inflationCurveId);
    if (metaIt == reg.inflation.curveMetadata.end()) {
        QUANTRA_ERROR("Inflation curve metadata not found: " + trade.inflationCurveId);
    }
    if (metaIt->second.kind != enums::InflationCurveKind_YoYInflation) {
        QUANTRA_INVALID_ARGUMENT("Inflation curve is not YoY inflation: " + trade.inflationCurveId);
    }
    if (metaIt->second.indexId != trade.inflationIndexId) {
        QUANTRA_INVALID_ARGUMENT(
            "YoYInflationCapFloor inflation_index_id does not match inflation_curve '" +
            trade.inflationCurveId + "'");
    }

    auto indexIt = reg.inflation.inflationIndices.find(trade.inflationIndexId);
    if (indexIt == reg.inflation.inflationIndices.end()) {
        QUANTRA_NOT_FOUND("Inflation index not found: " + trade.inflationIndexId);
    }
    auto inflationIndex =
        std::dynamic_pointer_cast<QuantLib::YoYInflationIndex>(indexIt->second);
    if (!inflationIndex) {
        QUANTRA_INVALID_ARGUMENT("Inflation index is not YoY inflation: " + trade.inflationIndexId);
    }

    auto volIt = reg.volatility.yoyOptionletVols.find(trade.volatilityId);
    if (volIt == reg.volatility.yoyOptionletVols.end()) {
        QUANTRA_NOT_FOUND("YoY optionlet vol not found: " + trade.volatilityId);
    }

    // Build the YoY optionlet strip. The observation interpolation is fixed to
    // CPI::AsIndex (there is no observation-interpolation field on this
    // instrument; the vol surface index-interpolation flag governs the surface,
    // while the leg observes the year-on-year rate as-indexed) — matching the
    // reference. The payment calendar is the schedule's own calendar.
    auto leg = QuantLib::yoyInflationLeg(
                   trade.schedule,
                   trade.schedule.calendar(),
                   inflationIndex,
                   trade.observationLag,
                   QuantLib::CPI::AsIndex)
                   .withNotionals(trade.notional)
                   .withPaymentDayCounter(trade.dayCounter)
                   .withPaymentAdjustment(trade.paymentConvention);
    if (trade.hasGearing) {
        leg.withGearings(trade.gearing);
    }
    if (trade.hasSpread) {
        leg.withSpreads(trade.spread);
    }
    QuantLib::Leg yoyLeg = leg;

    std::shared_ptr<QuantLib::YoYInflationCapFloor> instrument;
    switch (trade.capFloorType) {
        case QuantLib::YoYInflationCapFloor::Cap:
            instrument = std::make_shared<QuantLib::YoYInflationCap>(
                yoyLeg, std::vector<QuantLib::Rate>{trade.capRate});
            break;
        case QuantLib::YoYInflationCapFloor::Floor:
            instrument = std::make_shared<QuantLib::YoYInflationFloor>(
                yoyLeg, std::vector<QuantLib::Rate>{trade.floorRate});
            break;
        case QuantLib::YoYInflationCapFloor::Collar:
            instrument = std::make_shared<QuantLib::YoYInflationCollar>(
                yoyLeg,
                std::vector<QuantLib::Rate>{trade.capRate},
                std::vector<QuantLib::Rate>{trade.floorRate});
            break;
    }
    if (!instrument) {
        QUANTRA_INVALID_ARGUMENT("Invalid YoY inflation cap/floor type");
    }

    QuantLib::Handle<QuantLib::YieldTermStructure> nominalHandle(
        discountIt->second->currentLink());
    auto volSurface = buildVolSurface(
        volIt->second, metaIt->second.frequency, metaIt->second.indexInterpolated);
    QuantLib::Handle<QuantLib::YoYOptionletVolatilitySurface> volHandle(volSurface);

    instrument->setPricingEngine(
        buildEngine(volIt->second.engineKind, inflationIndex, volHandle, nominalHandle));

    YoYInflationCapFloorPerTrade out;
    out.npv = instrument->NPV();
    out.atmRate = instrument->atmRate(*discountIt->second->currentLink());
    return out;
}

} // namespace

YoYInflationCapFloorResult YearOnYearInflationCapFloorEvaluator::evaluate(
    const YoYInflationCapFloorInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    YoYInflationCapFloorResult result;
    result.capFloors.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();  // honor the per-request deadline before each trade
        result.capFloors.push_back(priceTrade(trade, reg));
    }
    return result;
}

} // namespace quantra
