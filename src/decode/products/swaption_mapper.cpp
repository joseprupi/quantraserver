#include "swaption_mapper.h"

#include "curve_bootstrapper.h"
#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "eval_date_guard.h"
#include "index_registry_builder.h"
#include "roll_offset.h"
#include "swap_leg_mapper.h"
#include "swaption_vol_diagnostics.h"

#include "swaption_generated.h"

#include <ql/settings.hpp>

#include <cmath>
#include <memory>
#include <string>

namespace quantra {

namespace {

/// Pull the trade's floating-index id out of the FB underlying swap (vanilla
/// underlying only; empty for OIS, as the swap-index consistency checks
/// downstream expect).
std::string getTradeFloatIndexId(const quantra::Swaption* sw) {
    if (!sw) return {};
    if (sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        const auto* u = sw->underlying_as_VanillaSwap();
        const auto* leg = u ? u->floating_leg() : nullptr;
        if (leg && leg->coupons_type() == quantra::SwapCoupons_IborCoupons) {
            const auto* c = leg->coupons_as_IborCoupons();
            if (c && c->index() && c->index()->id()) {
                return c->index()->id()->str();
            }
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

/// Underlying-swap fixed-leg effective date (the raw wire date, before any
/// schedule adjustment) — the spot-days validation compares this against
/// advance(exerciseDate, spot_days).
QuantLib::Date getTradeUnderlyingStartDate(const quantra::Swaption* sw) {
    if (!sw) return {};
    auto pickFromFixedLeg = [](const quantra::SwapLeg* leg) -> QuantLib::Date {
        if (leg && leg->schedule() && leg->schedule()->effective_date()) {
            return DateToQL(leg->schedule()->effective_date()->str());
        }
        return {};
    };
    if (sw->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        const auto* u = sw->underlying_as_VanillaSwap();
        return pickFromFixedLeg(u ? u->fixed_leg() : nullptr);
    }
    if (sw->underlying_type() == quantra::SwaptionUnderlying_OisSwap) {
        const auto* u = sw->underlying_as_OisSwap();
        return pickFromFixedLeg(u ? u->fixed_leg() : nullptr);
    }
    return {};
}

/// A swaption underlying's legs: one coupon kind per role, constant notional
/// only, no principal exchanges (the swaption is priced from QuantLib's own
/// VanillaSwap / OvernightIndexedSwap, which carry neither).
SwapLegRules underlyingLegRules(SwapLegData::Kind kind) {
    SwapLegRules rules;
    rules.allowedKinds = {kind};
    rules.exchangesAllowed = false;
    rules.endpoint = "swaption underlying";
    return rules;
}

/// The two legs of a swaption underlying, decoded by the same code as the
/// swap endpoints, plus the underlying-specific rules: opposite sides, one
/// currency, one discount curve (both legs must name it), an in-advance
/// floating leg.
void decodeUnderlyingLegs(
    const quantra::SwapLeg* fixedFb,
    const quantra::SwapLeg* floatingFb,
    const std::string& fixedName,
    const std::string& floatingName,
    SwapLegData::Kind floatingKind,
    SwapLegData& fixed,
    SwapLegData& floating) {
    fixed = DecodeSwapLeg(fixedFb, fixedName, underlyingLegRules(SwapLegData::Kind::Fixed));
    floating = DecodeSwapLeg(floatingFb, floatingName, underlyingLegRules(floatingKind));
    RequireOppositeSides(fixed, fixedName, floating, floatingName);
    RequireOneCurrency(fixed, fixedName, floating, floatingName, "a swaption underlying");
    if (fixed.discountingCurveId != floating.discountingCurveId) {
        QUANTRA_INVALID_ARGUMENT(
            fixedName + ".discounting_curve '" + fixed.discountingCurveId + "' and " +
            floatingName + ".discounting_curve '" + floating.discountingCurveId +
            "' must be equal: a swaption is priced on one discount curve");
    }
    // The swaption is priced from a QuantLib::VanillaSwap underlying, which
    // the swaption engines require and which cannot represent an in-arrears
    // floating leg. Reject rather than silently price it in advance.
    if (floating.kind == SwapLegData::Kind::Ibor && floating.ibor.inArrears) {
        QUANTRA_INVALID_ARGUMENT(
            floatingName + ".coupons.in_arrears is not supported on a swaption underlying "
                           "(the swaption is priced from a QuantLib::VanillaSwap, which fixes in "
                           "advance)");
    }
}

/// Lift the whole FB swaption table into the plain SwaptionInstrument. Reads
/// every field SwaptionParser::parse consumes (exercise dates, settlement,
/// underlying). The eval-date-dependent validation stays in the pricer's
/// buildSwaptionInstrument so the theta-leg rebuild behaves exactly as the
/// legacy per-call parse().
SwaptionInstrument extractInstrument(const quantra::Swaption* sw) {
    SwaptionInstrument inst;
    if (!sw->exercise_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("Swaption.exercise_type is required");
    }
    inst.exerciseType = sw->exercise_type().value();

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

    if (!sw->settlement_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("Swaption.settlement_type is required");
    }
    inst.settlementType = sw->settlement_type().value();
    inst.settlementMethod = SettlementMethodToQL(sw->settlement_method());

    switch (sw->underlying_type()) {
        case quantra::SwaptionUnderlying_VanillaSwap: {
            const auto* u = sw->underlying_as_VanillaSwap();
            if (u == nullptr) QUANTRA_INVALID_ARGUMENT("Swaption.underlying is required");
            inst.underlyingIsVanilla = true;
            decodeUnderlyingLegs(
                u->fixed_leg(), u->floating_leg(), "Swaption.underlying.fixed_leg",
                "Swaption.underlying.floating_leg", SwapLegData::Kind::Ibor,
                inst.vanillaUnderlying.fixedLeg, inst.vanillaUnderlying.floatingLeg);
            break;
        }
        case quantra::SwaptionUnderlying_OisSwap: {
            const auto* u = sw->underlying_as_OisSwap();
            if (u == nullptr) QUANTRA_INVALID_ARGUMENT("Swaption.underlying is required");
            inst.underlyingIsVanilla = false;
            decodeUnderlyingLegs(
                u->fixed_leg(), u->overnight_leg(), "Swaption.underlying.fixed_leg",
                "Swaption.underlying.overnight_leg", SwapLegData::Kind::Overnight,
                inst.oisUnderlying.fixedLeg, inst.oisUnderlying.overnightLeg);
            break;
        }
        case quantra::SwaptionUnderlying_NONE:
            QUANTRA_INVALID_ARGUMENT(
                "Swaption.underlying is required (underlying_type VanillaSwap or OisSwap)");
        default: QUANTRA_INVALID_ARGUMENT("Invalid swaption underlying type");
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
    if (!pricing->volatility() || pricing->volatility()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires volatility");
    }
    if (!pricing->model() || pricing->model()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceSwaption entry requires model");
    }

    SwaptionTrade trade;
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
    // bumped forwarding curve and freshly built IndexRegistry.
    trade.instrument = extractInstrument(sw);
    // The swaption's curves are the underlying legs' (the mapper has checked
    // both legs name one discount curve): discounting from the fixed leg,
    // forwarding from the floating / overnight leg.
    const SwapLegData& fixed = trade.instrument.underlyingIsVanilla
                                   ? trade.instrument.vanillaUnderlying.fixedLeg
                                   : trade.instrument.oisUnderlying.fixedLeg;
    const SwapLegData& floating = trade.instrument.underlyingIsVanilla
                                      ? trade.instrument.vanillaUnderlying.floatingLeg
                                      : trade.instrument.oisUnderlying.overnightLeg;
    trade.discountingCurveId = fixed.discountingCurveId;
    trade.forwardingCurveId = floating.forwardingCurveId;
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

        // `rollDays` != 0 only for the theta roll: the evaluation date moves AND
        // every explicit reference_date (curves, helper curves; the vol surface
        // is re-referenced by the evaluator for the roll leg) moves with it,
        // so the snapshot is a clean roll of the market rather than a hybrid
        // with the curve origins pinned to as-of. Date-anchored points/fixings
        // are data and stay put. The guard resets the offset on exit.
        auto buildSnapshot = [&](double curveBump, int rollDays) {
            RollOffsetGuard rollOffset(rollDays);
            QuantLib::Settings::instance().evaluationDate() = asOf + rollDays;
            SwaptionRebumpedMarket out;
            CurveBootstrapper bootstrapper;
            out.curves = bootstrapper.bootstrapAll(
                pricing->rates()->curves(), pricing->quotes(), pricing->rates()->indices(),
                curveBump, RequestBudget::unlimited(), pricing->fx());
            IndexRegistryBuilder indexBuilder;
            out.indices = indexBuilder.build(pricing->rates()->indices());
            return out;
        };

        EvalDateGuard guard;
        inputs.rebumpMarkets.curveUp = buildSnapshot(cfg.curveBump, 0);
        inputs.rebumpMarkets.curveDown = buildSnapshot(-cfg.curveBump, 0);
        inputs.rebumpMarkets.roll = buildSnapshot(0.0, cfg.rollDays);
        inputs.rebumpMarkets.present = true;
    }

    return inputs;
}

flatbuffers::Offset<quantra::PriceSwaptionResponse> SwaptionMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const SwaptionResult& result) const {

    std::vector<flatbuffers::Offset<quantra::SwaptionResponse>> rowOffsets;
    rowOffsets.reserve(result.values.size());
    for (const auto& r : result.values) {
        auto expiryOff = builder.CreateString(r.usedOptionExpiry);
        auto tenorOff = builder.CreateString(r.usedSwapTenor);

        quantra::SwaptionResponseBuilder rb(builder);
        rb.add_npv(r.npv);
        // Non-finite implied vol (no analytic value for this setup) is omitted,
        // matching the equity-option greeks: absent rather than a sentinel.
        // Conditionally computed scalars are `= null` optionals in the schema:
        // added only when the evaluator produced them (see SwaptionPerTrade).
        if (r.impliedVolatility) rb.add_implied_volatility(*r.impliedVolatility);
        if (r.atmForward) rb.add_atm_forward(*r.atmForward);
        if (r.annuity) rb.add_annuity(*r.annuity);
        if (r.delta) rb.add_delta(*r.delta);
        if (r.vega) rb.add_vega(*r.vega);
        if (r.gamma) rb.add_gamma(*r.gamma);
        if (r.theta) rb.add_theta(*r.theta);
        if (r.dv01) rb.add_dv01(*r.dv01);
        rb.add_used_volatility(r.usedVolatility);
        rb.add_used_option_expiry(expiryOff);
        rb.add_used_swap_tenor(tenorOff);
        rb.add_used_strike(r.usedStrike);
        if (r.usedAtmForward) rb.add_used_atm_forward(*r.usedAtmForward);
        rb.add_used_strike_kind(r.usedStrikeKind);
        if (r.usedSpreadFromAtm) rb.add_used_spread_from_atm(*r.usedSpreadFromAtm);
        if (r.usedCubeNodeAtm) rb.add_used_cube_node_atm(*r.usedCubeNodeAtm);
        rb.add_vol_kind(r.volKind);
        rb.add_used_model_param_mode(r.usedModelParamMode);
        // Hull-White diagnostics are added only when applicable; an unset
        // optional leaves the field absent in the response JSON.
        if (r.usedHwA) rb.add_used_hw_a(*r.usedHwA);
        if (r.usedHwSigma) rb.add_used_hw_sigma(*r.usedHwSigma);
        if (r.usedHwRmse) rb.add_used_hw_rmse(*r.usedHwRmse);
        if (r.usedHwNumHelpers) rb.add_used_hw_num_helpers(*r.usedHwNumHelpers);
        if (r.usedHwGridRows) rb.add_used_hw_grid_rows(*r.usedHwGridRows);
        if (r.usedHwGridCols) rb.add_used_hw_grid_cols(*r.usedHwGridCols);
        if (r.usedHwGridPoints) rb.add_used_hw_grid_points(*r.usedHwGridPoints);
        if (r.usedHwAFlatness) rb.add_used_hw_a_flatness(*r.usedHwAFlatness);
        if (r.usedHwAPoorlyIdentified)
            rb.add_used_hw_a_poorly_identified(*r.usedHwAPoorlyIdentified);
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
