/**
 * Pricing Registry Builder Implementation
 *
 * Builds the PricingRegistry by delegating to domain-specific parsers.
 */

#include "pricing_registry.h"

#include <ql/settings.hpp>

#include "curve_bootstrapper.h"
#include "index_registry_builder.h"
#include "swap_index_registry.h"
#include "enums.h"
#include "common.h"
#include "quote_registry.h"
#include "inflation_curve_parsers.h"

namespace quantra {

PricingRegistry PricingRegistryBuilder::build(const quantra::Pricing* pricing) const {
    // ==========================================================================
    // Validation
    // ==========================================================================
    if (!pricing) {
        QUANTRA_ERROR("Pricing not found");
    }
    if (!pricing->as_of_date()) {
        QUANTRA_ERROR("as_of_date is required");
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
                QUANTRA_ERROR("QuoteSpec.id is required");
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
        QUANTRA_ERROR("pricing.rates.curves is required (at least one curve needed)");
    }

    CurveBootstrapper bootstrapper;
    auto booted = bootstrapper.bootstrapAll(
        rates->curves(),
        pricing->quotes(),
        rates ? rates->indices() : nullptr
    );

    reg.rates.curveKeys = booted.keys;
    for (auto& kv : booted.handles) {
        reg.rates.curves.emplace(kv.first, kv.second);
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
                QUANTRA_ERROR("VolSurfaceSpec.id is required");
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

                case quantra::VolPayload_NONE:
                    QUANTRA_ERROR("VolSurfaceSpec.payload is required for vol id: " + id);

                default:
                    QUANTRA_ERROR("Unknown VolPayload type for vol id: " + id);
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
                QUANTRA_ERROR(
                    "Swaption vol '" + kv.first +
                    "' requires swap_index_id (forward-aware surface)");
            }
            if (!reg.rates.swapIndices.has(entry.swapIndexId)) {
                QUANTRA_ERROR(
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
                QUANTRA_ERROR(
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
    // We populate both the legacy FB-pointer map and the plain-domain mirror
    // for every entry. Existing consumers (cap_floor, swaption, equity,
    // calibrate_swaption, cds) keep reading from the legacy map; future
    // cutovers consume `modelDomains`.
    // ==========================================================================
    if (volatility && volatility->models()) {
        for (auto it = volatility->models()->begin(); it != volatility->models()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_ERROR("ModelSpec.id is required");
            }
            std::string id = spec->id()->str();

            if (spec->payload_type() == quantra::ModelPayload_NONE) {
                QUANTRA_ERROR("ModelSpec.payload is required for model id: " + id);
            }

            reg.volatility.models[id] = spec;

            // Plain-domain mirror. Mirror enums are bit-compatible with the
            // FB enums by construction (see parser/model_domain.h); QL types
            // (Period) are constructed via TimeUnitToQL (D16).
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
                    QUANTRA_ERROR("Unknown ModelPayload type for model id: " + id);
            }
            reg.volatility.modelDomains.emplace(id, std::move(domain));
        }
    }

    // ==========================================================================
    // Register credit curve specs (optional)
    //
    // We populate both the legacy FB-pointer map and the plain-domain mirror
    // for every entry. Existing consumers (cds_pricing_service) keep reading
    // from the legacy map; future cutovers consume `creditCurves`.
    // ==========================================================================
    if (credit && credit->credit_curves()) {
        for (auto it = credit->credit_curves()->begin(); it != credit->credit_curves()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_ERROR("CreditCurveSpec.id is required");
            }
            if (!spec->reference_date()) {
                QUANTRA_ERROR("CreditCurveSpec.reference_date is required");
            }
            std::string id = spec->id()->str();
            reg.credit.creditCurveSpecs.emplace(id, spec);

            // Plain-domain mirror. QL conversions (Calendar/DayCounter/BDC/
            // Frequency/DateGeneration::Rule/TimeUnit) go through
            // common/enums.* (D16); date strings via DateToQL (D17). Mirror
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
            d.flat_hazard_rate = spec->flat_hazard_rate();
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
                    q.running_coupon = qit->running_coupon();
                    d.quotes.push_back(std::move(q));
                }
            }
            reg.credit.creditCurves.emplace(id, std::move(d));
        }
    }

    // ==========================================================================
    // Coupon pricers (optional)
    // ==========================================================================
    if (rates && rates->coupon_pricers()) {
        for (auto it = rates->coupon_pricers()->begin(); it != rates->coupon_pricers()->end(); ++it) {
            reg.rates.couponPricers.emplace_back(*it);
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
