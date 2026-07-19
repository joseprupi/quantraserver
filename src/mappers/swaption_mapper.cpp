#include "swaption_mapper.h"

#include <memory>
#include <string>

#include <ql/settings.hpp>

#include "date_convert.h"
#include "schedule_parser.h"
#include "curve_bootstrapper.h"
#include "enum_convert.h"
#include "error.h"
#include "leg_notionals.h"
#include "eval_date_guard.h"
#include "index_registry_builder.h"
#include "swaption_generated.h"
#include "swaption_vol_diagnostics.h"

namespace quantra {

namespace {

/// Pull the trade's floating-index id out of the FB underlying swap. Matches
/// SwaptionModelParser::extractTradeFloatIndexId; the resulting plain string
/// flows into validations downstream of the pricer.
std::string getTradeFloatIndexId(const quantra::Swaption* sw) {
    if (!sw) return {};
    if (sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        const auto* u = sw->underlying_as_VanillaSwap();
        if (u && u->floating_leg() && u->floating_leg()->index() &&
            u->floating_leg()->index()->id()) {
            return u->floating_leg()->index()->id()->str();
        }
    }
    return {};
}

/// First (or only) exercise date from the FB swaption table — used by the
/// vol-runtime spot-days alignment validation. Bermudan uses exercise_dates[0].
QuantLib::Date getTradeExerciseDate(const quantra::Swaption* sw) {
    if (!sw) return {};
    if (sw->exercise_date() && sw->exercise_date()->size() > 0) {
        return DateToQL(sw->exercise_date()->str());
    }
    if (sw->exercise_dates() && sw->exercise_dates()->size() > 0) {
        return DateToQL(sw->exercise_dates()->Get(0)->str());
    }
    return {};
}

/// Underlying-swap fixed-leg effective date — the spot-days validation
/// compares this against advance(exerciseDate, spot_days).
QuantLib::Date getTradeUnderlyingStartDate(const quantra::Swaption* sw) {
    if (!sw) return {};
    auto pickFromSchedule = [](const auto* leg) -> QuantLib::Date {
        if (leg && leg->fixed_leg() && leg->fixed_leg()->schedule() &&
            leg->fixed_leg()->schedule()->effective_date()) {
            return DateToQL(leg->fixed_leg()->schedule()->effective_date()->str());
        }
        return {};
    };
    if (sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        return pickFromSchedule(sw->underlying_as_VanillaSwap());
    }
    if (sw->underlying_type() == quantra::SwaptionUnderlying_OisSwap) {
        return pickFromSchedule(sw->underlying_as_OisSwap());
    }
    return {};
}

/// Lift the FB VanillaSwap underlying into a plain VanillaSwapTrade. Mirrors
/// VanillaSwapParser::parse field-for-field (IBOR branch only — the swaption
/// underlying was never a CMS swap) so the pricer rebuilds it byte-identically.
VanillaSwapTrade extractVanillaUnderlying(const quantra::VanillaSwap* swap) {
    if (swap == nullptr)
        QUANTRA_INVALID_ARGUMENT("Swaption underlying VanillaSwap not found");
    if (swap->fixed_leg() == nullptr)
        QUANTRA_INVALID_ARGUMENT("VanillaSwap fixed_leg not found");
    if (swap->floating_leg() == nullptr)
        QUANTRA_INVALID_ARGUMENT("VanillaSwap floating_leg not found");

    VanillaSwapTrade trade;
    trade.branch = VanillaSwapTrade::Branch::Ibor;
    switch (swap->swap_type()) {
        case quantra::enums::SwapType_Payer:
            trade.swapType = QuantLib::VanillaSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            trade.swapType = QuantLib::VanillaSwap::Receiver;
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid swap type");
    }

    ScheduleParser scheduleParser;

    auto fixedLeg = swap->fixed_leg();
    if (fixedLeg->schedule() == nullptr)
        QUANTRA_INVALID_ARGUMENT("VanillaSwap fixed_leg schedule not found");
    if (!fixedLeg->day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("SwapFixedLeg.day_counter is required");
    // The swaption underlying does not support per-period notionals yet.
    rejectUnsupportedNotionals(fixedLeg->notionals(),
                               "Swaption underlying VanillaSwap fixed leg");
    trade.fixed.schedule = *scheduleParser.parse(fixedLeg->schedule());
    trade.fixed.notional = fixedLeg->notional();
    trade.fixed.rate = fixedLeg->rate();
    trade.fixed.dayCounter = DayCounterToQL(fixedLeg->day_counter().value());

    auto floatingLeg = swap->floating_leg();
    if (floatingLeg->schedule() == nullptr)
        QUANTRA_INVALID_ARGUMENT("VanillaSwap floating_leg schedule not found");
    if (!floatingLeg->index() || !floatingLeg->index()->id())
        QUANTRA_INVALID_ARGUMENT("VanillaSwap floating_leg index.id is required");
    if (!floatingLeg->day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("SwapFloatingLeg.day_counter is required");
    rejectUnsupportedNotionals(floatingLeg->notionals(),
                               "Swaption underlying VanillaSwap floating leg");
    trade.ibor.schedule = *scheduleParser.parse(floatingLeg->schedule());
    trade.ibor.notional = floatingLeg->notional();
    trade.ibor.indexId = floatingLeg->index()->id()->str();
    trade.ibor.spread = floatingLeg->spread();
    trade.ibor.dayCounter = DayCounterToQL(floatingLeg->day_counter().value());
    return trade;
}

/// Lift the FB OisSwap underlying into a plain OisSwapTrade. Mirrors
/// OisSwapParser::parse field-for-field so the pricer rebuilds it byte-identically.
OisSwapTrade extractOisUnderlying(const quantra::OisSwap* swap) {
    if (swap == nullptr)
        QUANTRA_INVALID_ARGUMENT("Swaption underlying OisSwap not found");
    if (swap->fixed_leg() == nullptr)
        QUANTRA_INVALID_ARGUMENT("OisSwap fixed_leg not found");
    if (swap->overnight_leg() == nullptr)
        QUANTRA_INVALID_ARGUMENT("OisSwap overnight_leg not found");

    OisSwapTrade trade;
    switch (swap->swap_type()) {
        case quantra::enums::SwapType_Payer:
            trade.swapType = QuantLib::OvernightIndexedSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            trade.swapType = QuantLib::OvernightIndexedSwap::Receiver;
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid swap type");
    }

    ScheduleParser scheduleParser;

    auto fixedLeg = swap->fixed_leg();
    if (fixedLeg->schedule() == nullptr)
        QUANTRA_INVALID_ARGUMENT("OisSwap fixed_leg schedule not found");
    if (!fixedLeg->day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("SwapFixedLeg.day_counter is required");
    // The swaption OIS underlying does not support per-period notionals yet.
    rejectUnsupportedNotionals(fixedLeg->notionals(),
                               "Swaption underlying OisSwap fixed leg");
    trade.fixed.schedule = *scheduleParser.parse(fixedLeg->schedule());
    trade.fixed.notional = fixedLeg->notional();
    trade.fixed.rate = fixedLeg->rate();
    trade.fixed.dayCounter = DayCounterToQL(fixedLeg->day_counter().value());

    auto overnightLeg = swap->overnight_leg();
    if (overnightLeg->schedule() == nullptr)
        QUANTRA_INVALID_ARGUMENT("OisSwap overnight_leg schedule not found");
    if (!overnightLeg->index() || !overnightLeg->index()->id())
        QUANTRA_INVALID_ARGUMENT("OisSwap overnight_leg index.id is required");
    trade.overnight.schedule = *scheduleParser.parse(overnightLeg->schedule());
    trade.overnight.notional = overnightLeg->notional();
    trade.overnight.indexId = overnightLeg->index()->id()->str();
    trade.overnight.spread = overnightLeg->spread();
    trade.overnight.paymentConvention = ConventionToQL(overnightLeg->payment_convention());
    trade.overnight.paymentCalendar = CalendarToQL(overnightLeg->payment_calendar());
    trade.overnight.paymentLag = overnightLeg->payment_lag();
    trade.overnight.averagingMethod = RateAveragingToQL(overnightLeg->averaging_method());
    trade.overnight.lookbackDays = overnightLeg->lookback_days();
    trade.overnight.lockoutDays = overnightLeg->lockout_days();
    trade.overnight.applyObservationShift = overnightLeg->apply_observation_shift();
    trade.overnight.telescopicValueDates = overnightLeg->telescopic_value_dates();
    return trade;
}

/// Lift the whole FB swaption table into the plain SwaptionInstrument. Reads
/// every field SwaptionParser::parse consumes (exercise dates, settlement,
/// underlying). The eval-date-dependent validation stays in the pricer's
/// buildSwaptionInstrument so the theta-leg rebuild behaves exactly as the
/// legacy per-call parse().
SwaptionInstrument extractInstrument(const quantra::Swaption* sw) {
    SwaptionInstrument inst;
    inst.exerciseType = sw->exercise_type();

    inst.hasExerciseDate = (sw->exercise_date() != nullptr);
    if (inst.hasExerciseDate) {
        inst.exerciseDate = DateToQL(sw->exercise_date()->str());
    }
    if (sw->exercise_dates() != nullptr) {
        inst.bermudanExerciseDates.reserve(sw->exercise_dates()->size());
        for (flatbuffers::uoffset_t i = 0; i < sw->exercise_dates()->size(); ++i) {
            auto* exDate = sw->exercise_dates()->Get(i);
            if (!exDate) {
                QUANTRA_INVALID_ARGUMENT("Swaption exercise_dates contains null entry");
            }
            inst.bermudanExerciseDates.push_back(DateToQL(exDate->str()));
        }
    }

    inst.settlementType = sw->settlement_type();
    inst.settlementMethod = SettlementMethodToQL(sw->settlement_method());

    switch (sw->underlying_type()) {
        case quantra::SwaptionUnderlying_VanillaSwap:
            inst.underlyingIsVanilla = true;
            inst.vanillaUnderlying = extractVanillaUnderlying(sw->underlying_as_VanillaSwap());
            break;
        case quantra::SwaptionUnderlying_OisSwap:
            inst.underlyingIsVanilla = false;
            inst.oisUnderlying = extractOisUnderlying(sw->underlying_as_OisSwap());
            break;
        case quantra::SwaptionUnderlying_NONE:
            QUANTRA_INVALID_ARGUMENT("Swaption underlying not found");
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid swaption underlying type");
    }
    return inst;
}

SwaptionTrade extractTrade(const quantra::PriceSwaption* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry is null");
    }
    if (!pricing->swaption()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires swaption");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires discounting_curve");
    }
    if (!pricing->forwarding_curve() || pricing->forwarding_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires forwarding_curve");
    }
    if (!pricing->volatility() || pricing->volatility()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires volatility");
    }
    if (!pricing->model() || pricing->model()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires model");
    }

    SwaptionTrade trade;
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();
    trade.volatilityId = pricing->volatility()->str();
    trade.modelId = pricing->model()->str();

    const auto* sw = pricing->swaption();
    trade.tradeFloatIndexId = getTradeFloatIndexId(sw);
    trade.exerciseDate = getTradeExerciseDate(sw);
    trade.underlyingStartDate = getTradeUnderlyingStartDate(sw);
    trade.underlyingIsVanillaSwap =
        sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap;

    // Lift the swaption into the plain instrument. The pricer rebuilds the QL
    // Swaption from it for the base path and again per rebump step against the
    // bumped forwarding curve and freshly built IndexRegistry — reproducing the
    // legacy per-call SwaptionParser::parse, but without re-reading FlatBuffers.
    trade.instrument = extractInstrument(sw);

    return trade;
}

} // namespace

SwaptionInputs SwaptionMapper::toInputs(const quantra::PriceSwaptionRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaptionRequest is null");
    }
    const auto* list = req->swaptions();
    if (list == nullptr || list->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceSwaptionRequest.swaptions is required and must be non-empty");
    }

    SwaptionInputs inputs;
    inputs.includeDiagnostics = req->include_diagnostics();
    inputs.trades.reserve(list->size());
    for (auto it = list->begin(); it != list->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }

    // Eagerly pre-build the rebump market snapshots when (and only when) the
    // request asks for rebump greeks. The pricer is FB-free and consumes these
    // as plain data — no callable. Each snapshot's curves are bootstrapped at
    // the SAME evaluation date the pricer later sets when pricing against them:
    //   curveUp/curveDown @ asOf, roll @ asOf + rollDays. This equivalence is
    // what makes the eager snapshots byte-identical to the legacy lazy path.
    //
    // EVAL-DATE ORDERING: toInputs runs BEFORE the registry build sets the
    // evaluation date to asOf, so we set it ourselves per snapshot (the
    // bootstrap reads Settings::evaluationDate() as the implicit reference) and
    // use an EvalDateGuard to restore on exit, leaking nothing into the build.
    const auto* pricing = req->pricing();
    const auto* options = pricing ? pricing->options() : nullptr;
    if (options && options->swaption_pricing_rebump()) {
        if (!pricing->as_of_date()) {
            QUANTRA_INVALID_ARGUMENT("as_of_date is required");
        }
        // toInputs runs BEFORE the registry build (which owns the usual
        // rates-presence guard), so the rebump snapshots must validate the
        // market data themselves. Without this a request that omits
        // pricing.rates would segfault on the derefs below.
        if (!pricing->rates() || !pricing->rates()->curves()) {
            QUANTRA_INVALID_ARGUMENT(
                "Swaption rebump pricing requires pricing.rates.curves (at least one curve needed)");
        }
        const QuantLib::Date asOf = DateToQL(pricing->as_of_date()->str());
        const auto& cfg = kSwaptionRebumpConfig;

        auto buildSnapshot = [&](double curveBump, QuantLib::Date evalDate) {
            QuantLib::Settings::instance().evaluationDate() = evalDate;
            SwaptionRebumpedMarket out;
            CurveBootstrapper bootstrapper;
            out.curves = bootstrapper.bootstrapAll(
                pricing->rates()->curves(),
                pricing->quotes(),
                pricing->rates()->indices(),
                curveBump);
            IndexRegistryBuilder indexBuilder;
            out.indices = indexBuilder.build(pricing->rates()->indices());
            return out;
        };

        EvalDateGuard guard;
        inputs.rebumpMarkets.curveUp = buildSnapshot(cfg.curveBump, asOf);
        inputs.rebumpMarkets.curveDown = buildSnapshot(-cfg.curveBump, asOf);
        inputs.rebumpMarkets.roll = buildSnapshot(0.0, asOf + cfg.rollDays);
        inputs.rebumpMarkets.present = true;
    }

    return inputs;
}

flatbuffers::Offset<quantra::PriceSwaptionResponse> SwaptionMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const SwaptionResult& result) const {

    std::vector<flatbuffers::Offset<quantra::SwaptionResponse>> rowOffsets;
    rowOffsets.reserve(result.values.size());
    for (const auto& r : result.values) {
        auto expiryOff = builder.CreateString(r.usedOptionExpiry);
        auto tenorOff = builder.CreateString(r.usedSwapTenor);

        quantra::SwaptionResponseBuilder rb(builder);
        rb.add_npv(r.npv);
        rb.add_implied_volatility(r.impliedVolatility);
        rb.add_atm_forward(r.atmForward);
        rb.add_annuity(r.annuity);
        rb.add_delta(r.delta);
        rb.add_vega(r.vega);
        rb.add_gamma(r.gamma);
        rb.add_theta(r.theta);
        rb.add_dv01(r.dv01);
        rb.add_used_volatility(r.usedVolatility);
        rb.add_used_option_expiry(expiryOff);
        rb.add_used_swap_tenor(tenorOff);
        rb.add_used_strike(r.usedStrike);
        rb.add_used_atm_forward(r.usedAtmForward);
        rb.add_used_strike_kind(r.usedStrikeKind);
        rb.add_used_spread_from_atm(r.usedSpreadFromAtm);
        rb.add_used_cube_node_atm(r.usedCubeNodeAtm);
        rb.add_vol_kind(r.volKind);
        rb.add_used_model_param_mode(r.usedModelParamMode);
        rb.add_used_hw_a(r.usedHwA);
        rb.add_used_hw_sigma(r.usedHwSigma);
        rb.add_used_hw_rmse(r.usedHwRmse);
        rb.add_used_hw_num_helpers(r.usedHwNumHelpers);
        rb.add_used_hw_grid_rows(r.usedHwGridRows);
        rb.add_used_hw_grid_cols(r.usedHwGridCols);
        rb.add_used_hw_grid_points(r.usedHwGridPoints);
        rowOffsets.push_back(rb.Finish());
    }

    auto swaptionsVec = builder.CreateVector(rowOffsets);

    std::vector<flatbuffers::Offset<quantra::SwaptionVolDiagnostics>> diagnosticsOffs;
    if (result.includeDiagnostics) {
        for (const auto& vid : result.sabrVolIdsInOrder) {
            auto eIt = result.finalizedSabrEntries.find(vid);
            if (eIt == result.finalizedSabrEntries.end()) continue;
            diagnosticsOffs.push_back(
                buildSwaptionVolDiagnostics(builder, vid, eIt->second, /*extraWarnings=*/{}));
        }
    }
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwaptionVolDiagnostics>>>
        diagVec = 0;
    if (!diagnosticsOffs.empty()) {
        diagVec = builder.CreateVector(diagnosticsOffs);
    }

    quantra::PriceSwaptionResponseBuilder rb(builder);
    rb.add_swaptions(swaptionsVec);
    if (diagVec.o != 0) {
        rb.add_diagnostics(diagVec);
    }
    return rb.Finish();
}

} // namespace quantra
