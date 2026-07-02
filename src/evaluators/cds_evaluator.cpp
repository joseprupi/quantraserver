#include "cds_evaluator.h"

#include <string>
#include <variant>

#include <ql/handle.hpp>
#include <ql/pricingengines/credit/isdacdsengine.hpp>
#include <ql/pricingengines/credit/midpointcdsengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/credit/defaultprobabilityhelpers.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>
#include <ql/termstructures/credit/piecewisedefaultcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include "credit_curve_domain.h"
#include "error.h"
#include "model_domain.h"
#include "quote_registry.h"

namespace quantra {

namespace {

/// Human-readable name for a credit-curve interpolator enum value; used in
/// fail-closed error messages. Out-of-range values (the raw wire
/// cast in pricing_registry.cpp) fall through to the numeric form.
std::string creditCurveInterpolatorName(CreditCurveInterpolatorKind kind) {
    switch (kind) {
        case CreditCurveInterpolatorKind::BackwardFlat: return "BackwardFlat";
        case CreditCurveInterpolatorKind::ForwardFlat: return "ForwardFlat";
        case CreditCurveInterpolatorKind::Linear: return "Linear";
        case CreditCurveInterpolatorKind::LogCubic: return "LogCubic";
        case CreditCurveInterpolatorKind::LogLinear: return "LogLinear";
    }
    return "enum value " + std::to_string(static_cast<int>(kind));
}

/**
 * Bootstrap the default-probability term structure from a plain-domain
 * CreditCurveDomain. Mirrors the FB-driven path in cds_parser.cpp verbatim:
 *  - flat hazard rate is used when flat_hazard_rate > 0 OR no quotes were
 *    provided (the same dual-trigger the legacy parser uses; the default
 *    1% hazard rate falls out of the same fallback);
 *  - otherwise we build SpreadCdsHelper / UpfrontCdsHelper instances and
 *    dispatch on curve_interpolator for the PiecewiseDefaultCurve choice.
 *
 * Defaults for the helper conventions block mirror the FB schema defaults
 * the legacy parser applies when `helper_conventions` is absent: Quarterly /
 * Following / TwentiethIMM / Actual365Fixed / settlement_days = 0 / MidPoint.
 */
std::shared_ptr<QuantLib::DefaultProbabilityTermStructure> buildCreditCurve(
    const CreditCurveDomain& curve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuoteRegistry& quoteRegistry) {

    if (curve.calendar.empty()) {
        QUANTRA_INVALID_ARGUMENT("CreditCurveSpec.calendar is required");
    }

    if (curve.flat_hazard_rate > 0.0 || curve.quotes.empty()) {
        double hazardRate = curve.flat_hazard_rate > 0.0 ? curve.flat_hazard_rate : 0.01;
        return std::make_shared<QuantLib::FlatHazardRate>(
            curve.reference_date,
            hazardRate,
            curve.day_counter);
    }

    const auto& hc = curve.helper_conventions;
    QuantLib::Frequency freq = hc ? hc->frequency : QuantLib::Quarterly;
    QuantLib::BusinessDayConvention bdc =
        hc ? hc->business_day_convention : QuantLib::Following;
    QuantLib::DateGeneration::Rule rule =
        hc ? hc->date_generation_rule : QuantLib::DateGeneration::TwentiethIMM;
    QuantLib::DayCounter lastPeriodDc =
        hc ? hc->last_period_day_counter : QuantLib::Actual365Fixed();
    bool settlesAccrual = hc ? hc->settles_accrual : true;
    bool paysAtDefaultTime = hc ? hc->pays_at_default_time : true;
    bool rebatesAccrual = hc ? hc->rebates_accrual : true;
    int settlementDays = hc ? hc->settlement_days : 0;
    CdsHelperModelKind helperModelKind =
        hc ? hc->helper_model : CdsHelperModelKind::MidPoint;
    QuantLib::CreditDefaultSwap::PricingModel helperModel =
        helperModelKind == CdsHelperModelKind::ISDA
            ? QuantLib::CreditDefaultSwap::ISDA
            : QuantLib::CreditDefaultSwap::Midpoint;

    std::vector<std::shared_ptr<QuantLib::DefaultProbabilityHelper>> helpers;
    helpers.reserve(curve.quotes.size());
    for (const auto& quote : curve.quotes) {
        QuantLib::Handle<QuantLib::Quote> quoteHandle;
        if (!quote.quote_id.empty()) {
            quoteHandle = quoteRegistry.getHandle(quote.quote_id, quantra::QuoteType_Credit);
        }

        switch (quote.quote_type) {
            case CdsQuoteTypeKind::Upfront: {
                if (quote.running_coupon == 0.0) {
                    QUANTRA_INVALID_ARGUMENT("CdsQuote.running_coupon is required for upfront quotes");
                }
                auto upfrontQuote = quoteHandle;
                if (upfrontQuote.empty()) {
                    upfrontQuote = QuantLib::Handle<QuantLib::Quote>(
                        std::make_shared<QuantLib::SimpleQuote>(quote.quoted_upfront));
                }
                helpers.push_back(std::make_shared<QuantLib::UpfrontCdsHelper>(
                    upfrontQuote,
                    quote.running_coupon,
                    quote.tenor,
                    settlementDays,
                    curve.calendar,
                    freq,
                    bdc,
                    rule,
                    curve.day_counter,
                    curve.recovery_rate,
                    discountCurve,
                    settlementDays,
                    settlesAccrual,
                    paysAtDefaultTime,
                    QuantLib::Date(),
                    lastPeriodDc,
                    rebatesAccrual,
                    helperModel));
                break;
            }
            case CdsQuoteTypeKind::ParSpread:
            default: {
                auto spreadQuote = quoteHandle;
                if (spreadQuote.empty()) {
                    spreadQuote = QuantLib::Handle<QuantLib::Quote>(
                        std::make_shared<QuantLib::SimpleQuote>(quote.quoted_par_spread));
                }
                helpers.push_back(std::make_shared<QuantLib::SpreadCdsHelper>(
                    spreadQuote,
                    quote.tenor,
                    settlementDays,
                    curve.calendar,
                    freq,
                    bdc,
                    rule,
                    curve.day_counter,
                    curve.recovery_rate,
                    discountCurve,
                    settlesAccrual,
                    paysAtDefaultTime,
                    QuantLib::Date(),
                    lastPeriodDc,
                    rebatesAccrual,
                    helperModel));
                break;
            }
        }
    }

    // Fail closed on the interpolator. QuantLib's default-
    // probability bootstrap only supports flat-forward interpolation, i.e.
    // LogLinear on survival probabilities. Every other enum value used to
    // either masquerade as LogLinear silently (ForwardFlat/LogCubic hit the
    // old `default:` branch) or die inside the bootstrap with an opaque
    // QuantLib error (Linear/BackwardFlat). All of them — and any
    // out-of-range value from the raw wire cast — are now rejected with a
    // clear INVALID_ARGUMENT.
    switch (curve.curve_interpolator) {
        case CreditCurveInterpolatorKind::LogLinear:
            return std::make_shared<
                QuantLib::PiecewiseDefaultCurve<QuantLib::SurvivalProbability,
                                                QuantLib::LogLinear>>(
                curve.reference_date, helpers, curve.day_counter);
        case CreditCurveInterpolatorKind::BackwardFlat:
        case CreditCurveInterpolatorKind::ForwardFlat:
        case CreditCurveInterpolatorKind::Linear:
        case CreditCurveInterpolatorKind::LogCubic:
            break;
    }
    throw QuantraInvalidArgument(
        "Unsupported credit curve interpolator: " +
        creditCurveInterpolatorName(curve.curve_interpolator) +
        "; the CDS credit-curve bootstrap supports LogLinear only");
}

/**
 * Build the QL CDS instrument from the plain trade fields. Mirrors the
 * legacy CDSParser::parse branching: the upfront-bearing constructor is
 * selected when upfront != 0.0 OR an upfront-date string was supplied.
 */
std::shared_ptr<QuantLib::CreditDefaultSwap> buildCds(const CdsTrade& trade) {
    if (trade.upfront != 0.0 || trade.hasUpfrontDate) {
        return std::make_shared<QuantLib::CreditDefaultSwap>(
            trade.side,
            trade.notional,
            trade.upfront,
            trade.runningCoupon,
            trade.schedule,
            trade.businessDayConvention,
            trade.dayCounter,
            trade.settlesAccrual,
            trade.paysAtDefaultTime,
            trade.protectionStart,
            trade.upfrontDate,
            nullptr,
            trade.lastPeriodDayCounter,
            trade.rebatesAccrual,
            trade.tradeDate,
            trade.cashSettlementDays);
    }
    return std::make_shared<QuantLib::CreditDefaultSwap>(
        trade.side,
        trade.notional,
        trade.runningCoupon,
        trade.schedule,
        trade.businessDayConvention,
        trade.dayCounter,
        trade.settlesAccrual,
        trade.paysAtDefaultTime,
        trade.protectionStart,
        nullptr,
        trade.lastPeriodDayCounter,
        trade.rebatesAccrual,
        trade.tradeDate,
        trade.cashSettlementDays);
}

std::shared_ptr<QuantLib::PricingEngine> buildEngine(
    const CdsModelDomain& model,
    const QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure>& creditHandle,
    double recoveryRate,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountHandle) {

    switch (model.engine_type) {
        case CdsEngineTypeKind::ISDA: {
            QuantLib::IsdaCdsEngine::NumericalFix fix =
                model.isda_numerical_fix == CdsIsdaNumericalFixKind::None
                    ? QuantLib::IsdaCdsEngine::None
                    : QuantLib::IsdaCdsEngine::Taylor;
            QuantLib::IsdaCdsEngine::AccrualBias bias =
                model.isda_accrual_bias == CdsIsdaAccrualBiasKind::NoBias
                    ? QuantLib::IsdaCdsEngine::NoBias
                    : QuantLib::IsdaCdsEngine::HalfDayBias;
            QuantLib::IsdaCdsEngine::ForwardsInCouponPeriod fwd =
                model.isda_forwards_in_coupon_period == CdsIsdaForwardsInCouponPeriodKind::Flat
                    ? QuantLib::IsdaCdsEngine::Flat
                    : QuantLib::IsdaCdsEngine::Piecewise;
            return std::make_shared<QuantLib::IsdaCdsEngine>(
                creditHandle,
                recoveryRate,
                discountHandle,
                model.include_settlement_date_flows,
                fix,
                bias,
                fwd);
        }
        case CdsEngineTypeKind::MidPoint:
            return std::make_shared<QuantLib::MidPointCdsEngine>(
                creditHandle,
                recoveryRate,
                discountHandle);
    }
    // Fail closed: an out-of-range engine enum from the raw wire
    // cast must not silently price with the MidPoint engine.
    throw QuantraInvalidArgument(
        "Unsupported CDS engine type: enum value " +
        std::to_string(static_cast<int>(model.engine_type)));
}

CdsPerTrade priceTrade(const CdsTrade& trade,
                       const PricingRegistry& reg,
                       const PricingContext& /*ctx*/) {
    auto discountIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }

    if (trade.creditCurveId.empty()) {
        QUANTRA_INVALID_ARGUMENT("credit_curve_id is required");
    }
    auto creditIt = reg.credit.creditCurves.find(trade.creditCurveId);
    if (creditIt == reg.credit.creditCurves.end()) {
        QUANTRA_NOT_FOUND("Credit curve not found: " + trade.creditCurveId);
    }

    if (trade.modelId.empty()) {
        QUANTRA_INVALID_ARGUMENT("model is required for CDS pricing");
    }
    auto modelIt = reg.volatility.modelDomains.find(trade.modelId);
    if (modelIt == reg.volatility.modelDomains.end()) {
        QUANTRA_NOT_FOUND("Model not found: " + trade.modelId);
    }
    const auto* cdsModel = std::get_if<CdsModelDomain>(&modelIt->second.payload);
    if (cdsModel == nullptr) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId + "' is not a CDS model");
    }

    QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle(
        discountIt->second->currentLink());

    const CreditCurveDomain& curve = creditIt->second;
    auto creditCurve = buildCreditCurve(curve, discountHandle, reg.quoteRegistry);
    QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure> creditHandle(creditCurve);

    auto cds = buildCds(trade);
    cds->setPricingEngine(buildEngine(*cdsModel, creditHandle, curve.recovery_rate, discountHandle));

    CdsPerTrade out;
    out.npv = cds->NPV();
    out.fairSpread = cds->fairSpread();
    out.fairUpfront = cds->fairUpfront();
    out.defaultLegNpv = cds->defaultLegNPV();
    out.premiumLegNpv = cds->couponLegNPV();
    return out;
}

} // namespace

CdsResult CdsEvaluator::evaluate(const CdsInputs& inputs,
                           const PricingRegistry& reg,
                           const PricingContext& ctx) const {
    CdsResult result;
    result.values.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.values.push_back(priceTrade(trade, reg, ctx));
    }
    return result;
}

} // namespace quantra
