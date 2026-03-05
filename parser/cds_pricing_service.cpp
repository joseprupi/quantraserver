#include "cds_pricing_service.h"

#include <ql/pricingengines/credit/isdacdsengine.hpp>

#include "cds_parser.h"
#include "enums.h"
#include "error.h"

namespace quantra {

CdsPriceResult CdsPricingService::price(const quantra::PriceCDS* tradePricing, const PricingRegistry& reg) const {
    if (!tradePricing) {
        QUANTRA_ERROR("PriceCDS entry is required");
    }

    CDSParser cdsParser;
    CreditCurveParser creditCurveParser;

    auto discountIt = reg.rates.curves.find(tradePricing->discounting_curve()->str());
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_ERROR("Discounting curve not found: " + tradePricing->discounting_curve()->str());
    }
    if (!tradePricing->credit_curve_id()) {
        QUANTRA_ERROR("credit_curve_id is required");
    }
    auto creditIt = reg.credit.creditCurveSpecs.find(tradePricing->credit_curve_id()->str());
    if (creditIt == reg.credit.creditCurveSpecs.end()) {
        QUANTRA_ERROR("Credit curve not found: " + tradePricing->credit_curve_id()->str());
    }
    if (!tradePricing->model()) {
        QUANTRA_ERROR("model is required for CDS pricing");
    }
    auto modelIt = reg.volatility.models.find(tradePricing->model()->str());
    if (modelIt == reg.volatility.models.end()) {
        QUANTRA_ERROR("Model not found: " + tradePricing->model()->str());
    }
    const auto* model = modelIt->second;
    if (model->payload_type() != quantra::ModelPayload_CdsModelSpec) {
        QUANTRA_ERROR("Model payload is not CdsModelSpec for model: " + tradePricing->model()->str());
    }

    auto creditCurveSpec = creditIt->second;
    QuantLib::Date referenceDate = DateToQL(creditCurveSpec->reference_date()->str());
    auto creditCurve = creditCurveParser.parse(
        creditCurveSpec,
        referenceDate,
        QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink()),
        &reg.quoteRegistry);
    QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure> creditHandle(creditCurve);
    double recoveryRate = creditCurveSpec->recovery_rate();
    auto cds = cdsParser.parse(tradePricing->cds());

    std::shared_ptr<QuantLib::PricingEngine> engine;
    const auto* cdsModel = model->payload_as_CdsModelSpec();
    switch (cdsModel->engine_type()) {
        case quantra::enums::CdsEngineType_ISDA: {
            auto includeFlows = cdsModel->include_settlement_date_flows();
            QuantLib::IsdaCdsEngine::NumericalFix fix =
                cdsModel->isda_numerical_fix() == quantra::enums::CdsIsdaNumericalFix_None
                    ? QuantLib::IsdaCdsEngine::None
                    : QuantLib::IsdaCdsEngine::Taylor;
            QuantLib::IsdaCdsEngine::AccrualBias bias =
                cdsModel->isda_accrual_bias() == quantra::enums::CdsIsdaAccrualBias_NoBias
                    ? QuantLib::IsdaCdsEngine::NoBias
                    : QuantLib::IsdaCdsEngine::HalfDayBias;
            QuantLib::IsdaCdsEngine::ForwardsInCouponPeriod fwd =
                cdsModel->isda_forwards_in_coupon_period() == quantra::enums::CdsIsdaForwardsInCouponPeriod_Flat
                    ? QuantLib::IsdaCdsEngine::Flat
                    : QuantLib::IsdaCdsEngine::Piecewise;

            engine = std::make_shared<QuantLib::IsdaCdsEngine>(
                creditHandle,
                recoveryRate,
                QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink()),
                includeFlows,
                fix,
                bias,
                fwd);
            break;
        }
        case quantra::enums::CdsEngineType_MidPoint:
        default:
            engine = std::make_shared<QuantLib::MidPointCdsEngine>(
                creditHandle,
                recoveryRate,
                *discountIt->second);
            break;
    }
    cds->setPricingEngine(engine);

    CdsPriceResult out;
    out.npv = cds->NPV();
    out.fairSpread = cds->fairSpread();
    out.fairUpfront = cds->fairUpfront();
    out.defaultLegNpv = cds->defaultLegNPV();
    out.premiumLegNpv = cds->couponLegNPV();
    return out;
}

} // namespace quantra
