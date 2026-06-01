#include "equity_option_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "equity_option_parser.h"
#include "error.h"

namespace quantra {

namespace {

EquityOptionTrade extractTrade(const quantra::PriceEquityOption* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOption entry is null");
    }
    if (!pricing->option()) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOption.option is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOption.discounting_curve is required");
    }
    if (!pricing->volatility() || pricing->volatility()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOption.volatility is required");
    }
    if (!pricing->model() || pricing->model()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOption.model is required");
    }

    // Delegate FB→QL field extraction (exercise/payoff/barrier + enum
    // conversions) to the existing parser so the inline barrier/payoff parsing
    // logic stays in a single place.
    EquityOptionParser parser;
    ParsedEquityOption parsed = parser.parse(pricing->option());

    EquityOptionTrade trade;
    trade.tradeId = std::move(parsed.tradeId);
    trade.quantity = parsed.quantity;
    trade.settlement = EquitySettlementTypeToQL(parsed.settlement);
    trade.optionType = parsed.optionType;
    trade.strike = parsed.strike;
    trade.exercise = std::move(parsed.exercise);
    trade.hasBarrier = parsed.hasBarrier;
    trade.barrierType = parsed.barrierType;
    trade.barrierLevel = parsed.barrierLevel;
    trade.rebate = parsed.rebate;

    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.underlyingId = std::move(parsed.underlyingId);
    trade.volatilityId = pricing->volatility()->str();
    trade.modelId = pricing->model()->str();
    return trade;
}

} // namespace

EquityOptionInputs EquityOptionMapper::toInputs(
    const quantra::PriceEquityOptionRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceEquityOptionRequest is null");
    }
    const auto* list = req->options();
    if (list == nullptr || list->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceEquityOptionRequest.options is required and must be non-empty");
    }

    EquityOptionInputs inputs;
    inputs.trades.reserve(list->size());
    for (auto it = list->begin(); it != list->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceEquityOptionResponse> EquityOptionMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const EquityOptionResult& result) const {

    std::vector<flatbuffers::Offset<quantra::EquityOptionResponse>> offsets;
    offsets.reserve(result.trades.size());
    for (const auto& t : result.trades) {
        auto tradeId = builder.CreateString(t.tradeId);
        quantra::EquityOptionResponseBuilder rb(builder);
        rb.add_trade_id(tradeId);
        rb.add_npv(t.npv);
        rb.add_delta(t.delta);
        rb.add_gamma(t.gamma);
        rb.add_vega(t.vega);
        rb.add_theta(t.theta);
        rb.add_rho(t.rho);
        rb.add_implied_volatility(t.impliedVolatility);
        rb.add_used_spot(t.usedSpot);
        rb.add_used_strike(t.usedStrike);
        rb.add_used_settlement(EquitySettlementTypeToFb(t.usedSettlement));
        offsets.push_back(rb.Finish());
    }

    auto vec = builder.CreateVector(offsets);
    quantra::PriceEquityOptionResponseBuilder rb(builder);
    rb.add_options(vec);
    return rb.Finish();
}

} // namespace quantra
