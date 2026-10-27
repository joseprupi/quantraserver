#include "cap_floor_evaluator.h"

#include "date_convert.h"
#include "error.h"
#include "model_domain.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/pricingengines/capfloor/bacheliercapfloorengine.hpp>
#include <ql/pricingengines/capfloor/blackcapfloorengine.hpp>
#include <ql/termstructures/volatility/volatilitytype.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include <memory>
#include <variant>
#include <vector>

namespace quantra {

namespace {

/// Builds the cap/floor pricing engine from a CapFloorModelDomain, after
/// checking that the model and the vol type are compatible.
std::shared_ptr<QuantLib::PricingEngine> buildEngine(
    const std::string& modelId,
    const CapFloorModelDomain& model,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const OptionletVolEntry& volEntry) {
    switch (model.model_type) {
        case IrModelTypeKind::Bachelier:
            if (volEntry.qlVolType != QuantLib::Normal) {
                QUANTRA_INVALID_ARGUMENT(
                    "Model '" + modelId +
                    "': Bachelier requires Normal vols, "
                    "but vol has type ShiftedLognormal");
            }
            return std::make_shared<QuantLib::BachelierCapFloorEngine>(
                discountCurve, volEntry.handle);

        case IrModelTypeKind::Black:
            if (volEntry.displacement != 0.0) {
                QUANTRA_INVALID_ARGUMENT(
                    "Model '" + modelId +
                    "': Black requires displacement=0, "
                    "but vol has displacement=" +
                    std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackCapFloorEngine>(discountCurve, volEntry.handle);

        case IrModelTypeKind::ShiftedBlack:
            if (volEntry.displacement <= 0.0) {
                QUANTRA_INVALID_ARGUMENT(
                    "Model '" + modelId +
                    "': ShiftedBlack requires displacement>0, "
                    "but vol has displacement=" +
                    std::to_string(volEntry.displacement));
            }
            // BlackCapFloorEngine reads displacement from the vol structure.
            return std::make_shared<QuantLib::BlackCapFloorEngine>(discountCurve, volEntry.handle);

        case IrModelTypeKind::HullWhiteLattice:
            QUANTRA_INVALID_ARGUMENT(
                "Model '" + modelId + "': HullWhiteLattice is not supported for cap/floor");
    }
    QUANTRA_INVALID_ARGUMENT(
        "Model '" + modelId + "': CapFloorModelSpec.model_type is not a known model type: " +
        std::to_string(static_cast<int>(model.model_type)));
    return nullptr;
}

/// Build the QL CapFloor instrument from the plain trade: a single-strike
/// vector for a Cap or a Floor; a Collar takes the cap strike (`strike`) and
/// the floor strike (`floorStrike`, present by the mapper's rule) and is
/// QuantLib's long cap / short floor on the same leg.
std::shared_ptr<QuantLib::CapFloor> buildCapFloor(
    const CapFloorTrade& trade, const std::shared_ptr<QuantLib::IborIndex>& iborIndex) {
    QuantLib::Leg leg = QuantLib::IborLeg(trade.schedule, iborIndex)
                            .withNotionals(trade.notional)
                            .withPaymentDayCounter(trade.dayCounter)
                            .withPaymentAdjustment(trade.businessDayConvention);

    std::vector<QuantLib::Rate> strikes(1, trade.strike);

    switch (trade.capFloorType) {
        case QuantLib::CapFloor::Cap: return std::make_shared<QuantLib::Cap>(leg, strikes);
        case QuantLib::CapFloor::Floor: return std::make_shared<QuantLib::Floor>(leg, strikes);
        case QuantLib::CapFloor::Collar: {
            if (!trade.floorStrike) {
                QUANTRA_INVALID_ARGUMENT("CapFloor.floor_strike is required for a Collar");
            }
            std::vector<QuantLib::Rate> floorStrikes(1, *trade.floorStrike);
            return std::make_shared<QuantLib::Collar>(leg, strikes, floorStrikes);
        }
    }
    QUANTRA_INVALID_ARGUMENT("Invalid CapFloor type");
    return nullptr;
}

std::vector<CapFloorLetDetail> buildDetails(
    const QuantLib::Leg& floatingLeg,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Date& asOf) {
    std::vector<CapFloorLetDetail> out;
    for (const auto& cf : floatingLeg) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::IborCoupon>(cf);
        if (!coupon || coupon->hasOccurred(asOf)) {
            continue;
        }
        CapFloorLetDetail row;
        row.paymentDate = DateToIso(coupon->date());
        row.accrualStartDate = DateToIso(coupon->accrualStartDate());
        row.accrualEndDate = DateToIso(coupon->accrualEndDate());
        row.fixingDate = DateToIso(coupon->fixingDate());
        row.forwardRate = coupon->indexFixing();
        row.discount = discountCurve->discount(coupon->date());
        out.push_back(std::move(row));
    }
    return out;
}

CapFloorPerTrade priceTrade(
    const CapFloorTrade& trade, const PricingRegistry& reg, const PricingContext& ctx) {
    auto discIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }
    auto fwdIt = reg.rates.curves.find(trade.forwardingCurveId);
    if (fwdIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve not found: " + trade.forwardingCurveId);
    }
    auto volIt = reg.volatility.optionletVols.find(trade.volatilityId);
    if (volIt == reg.volatility.optionletVols.end()) {
        QUANTRA_NOT_FOUND("Optionlet vol not found: " + trade.volatilityId);
    }
    auto modelIt = reg.volatility.modelDomains.find(trade.modelId);
    if (modelIt == reg.volatility.modelDomains.end()) {
        QUANTRA_NOT_FOUND("Model not found: " + trade.modelId);
    }
    const auto* capFloorModel = std::get_if<CapFloorModelDomain>(&modelIt->second.payload);
    if (capFloorModel == nullptr) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId + "' is not a cap/floor model");
    }
    if (!reg.rates.indices.has(trade.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + trade.indexId);
    }

    QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle(discIt->second->currentLink());
    QuantLib::Handle<QuantLib::YieldTermStructure> forwardingHandle(fwdIt->second->currentLink());

    auto iborIndex = reg.rates.indices.getIborWithCurve(trade.indexId, forwardingHandle);

    auto capFloor = buildCapFloor(trade, iborIndex);
    auto engine = buildEngine(trade.modelId, *capFloorModel, discountHandle, volIt->second);
    capFloor->setPricingEngine(engine);

    CapFloorPerTrade out;
    out.npv = capFloor->NPV();
    out.atmRate = capFloor->atmRate(*discIt->second->currentLink());

    if (trade.includeDetails) {
        out.details =
            buildDetails(capFloor->floatingLeg(), discIt->second->currentLink(), ctx.asOf);
    }
    return out;
}

} // namespace

CapFloorResult CapFloorEvaluator::evaluate(
    const CapFloorInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const {
    CapFloorResult result;
    result.trades.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check(); // honor the per-request deadline before each trade
        result.trades.push_back(priceTrade(trade, reg, ctx));
    }
    return result;
}

} // namespace quantra
