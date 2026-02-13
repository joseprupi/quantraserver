#include "cds_pricing_request.h"

#include "pricing_registry.h"
#include "enums.h"

#include <ql/pricingengines/credit/isdacdsengine.hpp>

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceCDSResponse> CDSPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceCDSRequest *request) const
{
    // Build registry (handles curves with dependency ordering via CurveBootstrapper)
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    CDSParser cds_parser;
    CreditCurveParser credit_curve_parser;
    Date as_of_date = Settings::instance().evaluationDate();

    // Process each CDS
    auto cds_pricings = request->cds_list();
    std::vector<flatbuffers::Offset<CDSValues>> cds_vector;

    for (auto it = cds_pricings->begin(); it != cds_pricings->end(); it++)
    {
        try
        {
            // Lookup discounting curve by ID
            auto discounting_curve_it = reg.curves.find(it->discounting_curve()->str());
            if (discounting_curve_it == reg.curves.end())
            {
                // Create error response
                auto error_msg = builder->CreateString("Discounting curve not found: " + it->discounting_curve()->str());
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

            // Lookup credit curve by ID
            if (!it->credit_curve_id()) {
                auto error_msg = builder->CreateString("credit_curve_id is required");
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

            auto credit_curve_it = reg.creditCurveSpecs.find(it->credit_curve_id()->str());
            if (credit_curve_it == reg.creditCurveSpecs.end()) {
                auto error_msg = builder->CreateString("Credit curve not found: " + it->credit_curve_id()->str());
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

            auto credit_curve_spec = credit_curve_it->second;
            QuantLib::Date referenceDate = DateToQL(credit_curve_spec->reference_date()->str());
            auto credit_curve = credit_curve_parser.parse(
                credit_curve_spec,
                referenceDate,
                QuantLib::Handle<QuantLib::YieldTermStructure>(discounting_curve_it->second->currentLink()));
            QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure> creditHandle(credit_curve);
            double recoveryRate = credit_curve_spec->recovery_rate();

            // Parse the CDS instrument
            auto cds = cds_parser.parse(it->cds());

            // Set pricing engine
            std::shared_ptr<QuantLib::PricingEngine> engine;
            if (!it->model()) {
                auto error_msg = builder->CreateString("model is required for CDS pricing");
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

            auto model_it = reg.models.find(it->model()->str());
            if (model_it == reg.models.end()) {
                auto error_msg = builder->CreateString("Model not found: " + it->model()->str());
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

            const auto* model = model_it->second;
            if (model->payload_type() != quantra::ModelPayload_CdsModelSpec) {
                auto error_msg = builder->CreateString("Model payload is not CdsModelSpec for model: " + it->model()->str());
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

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
                        QuantLib::Handle<QuantLib::YieldTermStructure>(discounting_curve_it->second->currentLink()),
                        includeFlows,
                        fix,
                        bias,
                        fwd
                    );
                    break;
                }
                case quantra::enums::CdsEngineType_MidPoint:
                default:
                    engine = std::make_shared<QuantLib::MidPointCdsEngine>(
                        creditHandle,
                        recoveryRate,
                        *discounting_curve_it->second);
                    break;
            }
            cds->setPricingEngine(engine);

            // Calculate results
            double npv = cds->NPV();
            double fairSpread = cds->fairSpread();
            double fairUpfront = cds->fairUpfront();
            double defaultLegNPV = cds->defaultLegNPV();
            double premiumLegNPV = cds->couponLegNPV();

            std::cout << "CDS NPV: " << npv << ", Fair Spread: " << fairSpread * 10000 << " bps" << std::endl;

            // Build response
            CDSValuesBuilder response_builder(*builder);
            response_builder.add_npv(npv);
            response_builder.add_fair_spread(fairSpread);
            response_builder.add_fair_upfront(fairUpfront);
            response_builder.add_default_leg_npv(defaultLegNPV);
            response_builder.add_premium_leg_npv(premiumLegNPV);

            cds_vector.push_back(response_builder.Finish());
        }
        catch (const std::exception &e)
        {
            // Create error response
            auto error_msg = builder->CreateString(std::string("CDS pricing error: ") + e.what());
            auto error = quantra::CreateError(*builder, error_msg);
            CDSValuesBuilder response_builder(*builder);
            response_builder.add_error(error);
            cds_vector.push_back(response_builder.Finish());
        }
    }

    // Build final response
    auto cds_list = builder->CreateVector(cds_vector);
    PriceCDSResponseBuilder response_builder(*builder);
    response_builder.add_cds_list(cds_list);

    return response_builder.Finish();
}
