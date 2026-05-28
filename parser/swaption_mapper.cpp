#include "swaption_mapper.h"

#include <memory>
#include <string>

#include "common.h"
#include "common_parser.h"
#include "curve_bootstrapper.h"
#include "enums.h"
#include "error.h"
#include "index_registry_builder.h"
#include "swaption_parser.h"
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

    // Capture the FB swaption pointer in the builder closure. The pricer
    // invokes this once for the base path and again per rebump step against
    // bumped forwarding curves and a freshly built IndexRegistry — re-parsing
    // is required because the legacy SwaptionParser binds the underlying
    // swap's floating leg to the forwarding curve via a RelinkableHandle.
    const quantra::Swaption* swPtr = sw;
    trade.buildSwaption =
        [swPtr](const IndexRegistry& indices,
                const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding) {
            SwaptionParser parser;
            parser.linkForwardingTermStructure(forwarding.currentLink());
            return parser.parse(swPtr, indices);
        };

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

    // Capture FB pointers needed by the rebump path. The pricer invokes this
    // closure only when reg.options.swaptionPricingRebump is true.
    const auto* pricing = req->pricing();
    inputs.bootstrapWithBump = [pricing](double curveBump) {
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
