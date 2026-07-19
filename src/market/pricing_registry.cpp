/**
 * Pricing Registry Builder Implementation
 *
 * Builds the PricingRegistry by delegating to domain-specific parsers.
 */

#include "pricing_registry.h"

#include "credit_curve_generated.h"
#include "coupon_pricer_generated.h"
#include "model_generated.h"

#include <ql/settings.hpp>

#include "curve_bootstrapper.h"
#include "equity_underlying_registry.h"
#include "index_registry_builder.h"
#include "swap_index_registry.h"
#include "enum_convert.h"
#include "date_convert.h"
#include "quote_registry.h"
#include "inflation_curve_parsers.h"

namespace quantra {

PricingRegistry PricingRegistryBuilder::build(const quantra::Pricing* pricing,
                                              const RequestBudget& budget) const {
    // ==========================================================================
    // Validation
    // ==========================================================================
    if (!pricing) {
        QUANTRA_INVALID_ARGUMENT("Pricing not found");
    }
    if (!pricing->as_of_date()) {
        QUANTRA_INVALID_ARGUMENT("as_of_date is required");
    }

    // Set evaluation date
    QuantLib::Date asOf = DateToQL(pricing->as_of_date()->str());
    QuantLib::Settings::instance().evaluationDate() = asOf;

    PricingRegistry reg;

    // ==========================================================================
    // Parse Quotes (optional)
    // ==========================================================================
    QuoteRegistry quoteRegistry;
    if (pricing->quotes()) {
        for (auto it = pricing->quotes()->begin(); it != pricing->quotes()->end(); ++it) {
            if (!it->id()) {
                QUANTRA_INVALID_ARGUMENT("QuoteSpec.id is required");
            }
            std::string id = it->id()->str();
            auto sq = std::make_shared<QuantLib::SimpleQuote>(it->value());
            quoteRegistry.upsert(id, it->value(), it->quote_type());
        }
    }
    reg.quoteRegistry = quoteRegistry;

    const auto* rates = pricing->rates();
    const auto* credit = pricing->credit();
    const auto* volatility = pricing->volatility();
    const auto* inflation = pricing->inflation();
    const auto* options = pricing->options();

    // ==========================================================================
    // Build rates domain
    // ==========================================================================
    IndexRegistryBuilder indexBuilder;
    reg.rates.indices = indexBuilder.build(rates ? rates->indices() : nullptr);
    SwapIndexRegistryBuilder swapIndexBuilder;
    reg.rates.swapIndices = swapIndexBuilder.build(
        rates ? rates->swap_indices() : nullptr,
        reg.rates.indices);

    // ==========================================================================
    // Parse Curves (dependency-aware via CurveBootstrapper)
    // ==========================================================================
    if (!rates || !rates->curves()) {
        QUANTRA_INVALID_ARGUMENT("pricing.rates.curves is required (at least one curve needed)");
    }

    CurveBootstrapper bootstrapper;
    auto booted = bootstrapper.bootstrapAll(
        rates->curves(),
        pricing->quotes(),
        rates ? rates->indices() : nullptr,
        0.0,
        budget
    );

    reg.rates.curveKeys = booted.keys;
    for (auto& kv : booted.handles) {
        reg.rates.curves.emplace(kv.first, kv.second);
    }

    // ==========================================================================
    // Parse equity domain (optional)
    //
    // Equity underlyings reference dividend yield curves bootstrapped above, so
    // this block runs after `rates.curves` is populated. Output is plain (no
    // FB pointers) — consumed by EquityOptionEvaluator via `reg.equity`.
    // ==========================================================================
    if (pricing->equity()) {
        EquityUnderlyingRegistryBuilder equityBuilder;
        reg.equity.equityUnderlyings = equityBuilder.build(pricing->equity(), reg);
    }

    // ==========================================================================
    // Parse inflation domain (optional)
    // ==========================================================================
    if (inflation && inflation->inflation_curves()) {
        reg.inflation.curveMetadata = buildInflationCurves(
            inflation->inflation_curves(),
            inflation->inflation_indices(),
            &quoteRegistry,
            reg);
        buildInflationIndices(inflation->inflation_indices(), reg.inflation.curveMetadata, reg);
    } else if (inflation && inflation->inflation_indices()) {
        buildInflationIndices(inflation->inflation_indices(), reg.inflation.curveMetadata, reg);
    }

    // ==========================================================================
    // Parse volatility domain (optional)
    // Parsed after rates so equity SurfaceFromPrices can resolve curve ids.
    // ==========================================================================
    if (volatility && volatility->vol_surfaces()) {
        for (auto it = volatility->vol_surfaces()->begin(); it != volatility->vol_surfaces()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec.id is required");
            }
            std::string id = spec->id()->str();

            switch (spec->payload_type()) {
                case quantra::VolPayload_OptionletVolSpec:
                    reg.volatility.optionletVols.emplace(id, parseOptionletVol(spec, &quoteRegistry));
                    break;

                case quantra::VolPayload_SwaptionVolSpec:
                    reg.volatility.swaptionVols.emplace(id, parseSwaptionVol(spec, &quoteRegistry));
                    break;

                case quantra::VolPayload_BlackVolSpec:
                    reg.volatility.blackVols.emplace(id, parseBlackVol(spec, &quoteRegistry, &reg.rates.curves));
                    break;

                case quantra::VolPayload_YoYOptionletVolSpec:
                    reg.volatility.yoyOptionletVols.emplace(id, parseYoYOptionletVol(spec));
                    break;

                case quantra::VolPayload_NONE:
                    QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec.payload is required for vol id: " + id);

                default:
                    QUANTRA_INVALID_ARGUMENT("Unknown VolPayload type for vol id: " + id);
            }
        }
    }
    for (const auto& kv : reg.volatility.swaptionVols) {
        const auto& entry = kv.second;
        const bool needsSwapIndex =
            entry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM ||
            entry.volKind == quantra::enums::SwaptionVolKind_SabrParams ||
            entry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate;
        if (needsSwapIndex) {
            if (entry.swapIndexId.empty()) {
                QUANTRA_INVALID_ARGUMENT(
                    "Swaption vol '" + kv.first +
                    "' requires swap_index_id (forward-aware surface)");
            }
            if (!reg.rates.swapIndices.has(entry.swapIndexId)) {
                QUANTRA_NOT_FOUND(
                    "Swaption vol '" + kv.first + "' references unknown swap_index_id: " +
                    entry.swapIndexId);
            }
        }
        // SABR calibrate path requires an Ibor swap index because QuantLib's
        // XabrSwaptionVolatilityCube takes a QuantLib::SwapIndex (Ibor-shaped)
        // and we route through SwapIndexRegistry::getIborSwapIndexWithCurves.
        // OIS-shaped indices need a parallel helper (e.g. an OIS-aware
        // construction with OvernightIndexedSwapIndex); deferred to a
        // follow-up.
        if (entry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate) {
            const auto& sidx = reg.rates.swapIndices.get(entry.swapIndexId);
            if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
                QUANTRA_NOT_IMPLEMENTED(
                    "Swaption vol '" + kv.first +
                    "' uses SabrCalibrate with swap_index_id '" + entry.swapIndexId +
                    "' which is not an Ibor swap index; OIS swap index support for the "
                    "SABR calibrate path is not implemented yet (TODO).");
            }
        }
    }

    // ==========================================================================
    // Parse models (optional)
    //
    // Consumers (cap_floor, swaption, equity, calibrate_swaption, cds) read the
    // plain-domain `modelDomains` map populated below.
    // ==========================================================================
    if (volatility && volatility->models()) {
        for (auto it = volatility->models()->begin(); it != volatility->models()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("ModelSpec.id is required");
            }
            std::string id = spec->id()->str();

            if (spec->payload_type() == quantra::ModelPayload_NONE) {
                QUANTRA_INVALID_ARGUMENT("ModelSpec.payload is required for model id: " + id);
            }
            // A union whose type byte is set but whose value offset is absent
            // passes verification; the payload_as_*() casts below would then
            // return null and crash on deref. Reject the crafted buffer.
            if (spec->payload() == nullptr) {
                QUANTRA_INVALID_ARGUMENT(
                    "ModelSpec.payload type is set but its value is missing for model id: " + id);
            }

            // Plain-domain mirror. Mirror enums are bit-compatible with the
            // FB enums by construction (see parser/model_domain.h); QL types
            // (Period) are constructed via TimeUnitToQL.
            ModelDomain domain;
            domain.id = id;
            switch (spec->payload_type()) {
                case quantra::ModelPayload_CapFloorModelSpec: {
                    const auto* p = spec->payload_as_CapFloorModelSpec();
                    CapFloorModelDomain d;
                    d.model_type = static_cast<IrModelTypeKind>(p->model_type());
                    domain.payload = d;
                    break;
                }
                case quantra::ModelPayload_SwaptionModelSpec: {
                    const auto* p = spec->payload_as_SwaptionModelSpec();
                    SwaptionModelDomain d;
                    d.model_type = static_cast<IrModelTypeKind>(p->model_type());
                    d.hw_a = p->hw_a();
                    d.hw_sigma = p->hw_sigma();
                    d.lattice_steps = p->lattice_steps();
                    d.param_mode = static_cast<ModelParamModeKind>(p->param_mode());
                    if (const auto* c = p->hw_calibration()) {
                        SwaptionHwCalibrationDomain hw;
                        if (c->swaption_vol_id()) hw.swaption_vol_id = c->swaption_vol_id()->str();
                        if (c->discount_curve_id()) hw.discount_curve_id = c->discount_curve_id()->str();
                        if (c->forwarding_curve_id()) hw.forwarding_curve_id = c->forwarding_curve_id()->str();
                        if (c->swap_index_id()) hw.swap_index_id = c->swap_index_id()->str();
                        if (c->expiries()) {
                            for (auto pit = c->expiries()->begin(); pit != c->expiries()->end(); ++pit) {
                                hw.expiries.emplace_back(pit->n(), TimeUnitToQL(pit->unit()));
                            }
                        }
                        if (c->tenors()) {
                            for (auto pit = c->tenors()->begin(); pit != c->tenors()->end(); ++pit) {
                                hw.tenors.emplace_back(pit->n(), TimeUnitToQL(pit->unit()));
                            }
                        }
                        hw.calibrate_a = c->calibrate_a();
                        hw.calibrate_sigma = c->calibrate_sigma();
                        hw.a_init = c->a_init();
                        hw.sigma_init = c->sigma_init();
                        hw.max_iterations = c->max_iterations();
                        hw.function_evaluations = c->function_evaluations();
                        hw.end_criteria_eps = c->end_criteria_eps();
                        d.hw_calibration = std::move(hw);
                    }
                    domain.payload = std::move(d);
                    break;
                }
                case quantra::ModelPayload_CdsModelSpec: {
                    const auto* p = spec->payload_as_CdsModelSpec();
                    CdsModelDomain d;
                    d.engine_type = static_cast<CdsEngineTypeKind>(p->engine_type());
                    d.include_settlement_date_flows = p->include_settlement_date_flows();
                    d.isda_numerical_fix = static_cast<CdsIsdaNumericalFixKind>(p->isda_numerical_fix());
                    d.isda_accrual_bias = static_cast<CdsIsdaAccrualBiasKind>(p->isda_accrual_bias());
                    d.isda_forwards_in_coupon_period =
                        static_cast<CdsIsdaForwardsInCouponPeriodKind>(p->isda_forwards_in_coupon_period());
                    domain.payload = d;
                    break;
                }
                case quantra::ModelPayload_EquityVanillaModelSpec: {
                    const auto* p = spec->payload_as_EquityVanillaModelSpec();
                    EquityVanillaModelDomain d;
                    d.model_type = static_cast<EquityModelTypeKind>(p->model_type());
                    d.binomial_steps = p->binomial_steps();
                    domain.payload = d;
                    break;
                }
                default:
                    QUANTRA_INVALID_ARGUMENT("Unknown ModelPayload type for model id: " + id);
            }
            reg.volatility.modelDomains.emplace(id, std::move(domain));
        }
    }

    // ==========================================================================
    // Register credit curve specs (optional)
    //
    // Consumers (cds_evaluator) read the plain-domain `creditCurves` map
    // populated below.
    // ==========================================================================
    if (credit && credit->credit_curves()) {
        for (auto it = credit->credit_curves()->begin(); it != credit->credit_curves()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("CreditCurveSpec.id is required");
            }
            if (!spec->reference_date()) {
                QUANTRA_INVALID_ARGUMENT("CreditCurveSpec.reference_date is required");
            }
            std::string id = spec->id()->str();

            // Plain-domain mirror. QL conversions (Calendar/DayCounter/BDC/
            // Frequency/DateGeneration::Rule/TimeUnit) go through
            // common/enums.*; date strings via DateToQL. Mirror
            // enum kinds (HelperModel/QuoteType/Interpolator) are
            // bit-compatible with the FB enums (see credit_curve_domain.h).
            CreditCurveDomain d;
            d.id = id;
            d.reference_date = DateToQL(spec->reference_date()->str());
            d.calendar = CalendarToQL(spec->calendar());
            d.day_counter = DayCounterToQL(spec->day_counter());
            d.recovery_rate = spec->recovery_rate();
            d.curve_interpolator =
                static_cast<CreditCurveInterpolatorKind>(spec->curve_interpolator());
            if (const auto* h = spec->helper_conventions()) {
                CdsHelperConventionsDomain hc;
                hc.settlement_days = h->settlement_days();
                hc.frequency = FrequencyToQL(h->frequency());
                hc.business_day_convention = ConventionToQL(h->business_day_convention());
                hc.date_generation_rule = DateGenerationToQL(h->date_generation_rule());
                hc.last_period_day_counter = DayCounterToQL(h->last_period_day_counter());
                hc.settles_accrual = h->settles_accrual();
                hc.pays_at_default_time = h->pays_at_default_time();
                hc.rebates_accrual = h->rebates_accrual();
                hc.helper_model = static_cast<CdsHelperModelKind>(h->helper_model());
                d.helper_conventions = hc;
            }
            if (spec->flat_hazard_rate().has_value()) {
                d.flat_hazard_rate = spec->flat_hazard_rate().value();
            }
            if (const auto* qs = spec->quotes()) {
                d.quotes.reserve(qs->size());
                for (auto qit = qs->begin(); qit != qs->end(); ++qit) {
                    CdsQuoteDomain q;
                    if (qit->tenor()) {
                        q.tenor = QuantLib::Period(qit->tenor()->n(),
                                                   TimeUnitToQL(qit->tenor()->unit()));
                    }
                    q.quote_type = static_cast<CdsQuoteTypeKind>(qit->quote_type());
                    if (qit->quote_id()) q.quote_id = qit->quote_id()->str();
                    q.quoted_par_spread = qit->quoted_par_spread();
                    q.quoted_upfront = qit->quoted_upfront();
                    if (qit->running_coupon().has_value()) {
                        q.running_coupon = qit->running_coupon().value();
                    }
                    d.quotes.push_back(std::move(q));
                }
            }
            reg.credit.creditCurves.emplace(id, std::move(d));
        }
    }

    // ==========================================================================
    // Coupon pricers (optional)
    //
    // Consumers read the plain-domain `couponPricerDomains` map populated
    // below. Enum conversion routes through common/enums.* exclusively.
    // ==========================================================================
    if (rates && rates->coupon_pricers()) {
        for (auto it = rates->coupon_pricers()->begin(); it != rates->coupon_pricers()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("CouponPricer.id is required");
            }
            std::string id = spec->id()->str();

            const auto* black = spec->black_ibor_coupon_pricer();
            if (!black) {
                QUANTRA_INVALID_ARGUMENT("CouponPricer '" + id +
                              "' is missing black_ibor_coupon_pricer payload");
            }
            const auto* ov = black->optionlet_volatility();
            if (!ov) {
                QUANTRA_INVALID_ARGUMENT("CouponPricer '" + id +
                              "' is missing optionlet_volatility block");
            }

            if (!ov->calendar().has_value()) {
                QUANTRA_INVALID_ARGUMENT("ConstantOptionletVolatility.calendar is required");
            }
            if (!ov->business_day_convention().has_value()) {
                QUANTRA_INVALID_ARGUMENT("ConstantOptionletVolatility.business_day_convention is required");
            }
            if (!ov->day_counter().has_value()) {
                QUANTRA_INVALID_ARGUMENT("ConstantOptionletVolatility.day_counter is required");
            }

            CouponPricerDomain domain;
            domain.id = id;
            BlackIborCouponPricerDomain d;
            d.settlement_days = ov->settlement_days();
            d.calendar = CalendarToQL(ov->calendar().value());
            d.business_day_convention = ConventionToQL(ov->business_day_convention().value());
            d.volatility = ov->volatility();
            d.day_counter = DayCounterToQL(ov->day_counter().value());
            domain.payload = std::move(d);
            reg.rates.couponPricerDomains.emplace(id, std::move(domain));
        }
    }

    if (options) {
        reg.options.bondPricingDetails = options->bond_pricing_details();
        reg.options.bondPricingFlows = options->bond_pricing_flows();
        reg.options.swaptionPricingDetails = options->swaption_pricing_details();
        reg.options.swaptionPricingRebump = options->swaption_pricing_rebump();
    }

    return reg;
}

} // namespace quantra
