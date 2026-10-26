#include "fx_forward_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "fx_spot_parser.h"
#include "request_validation.h"

namespace quantra {

namespace {

std::string requireId(const flatbuffers::String* value, const char* field) {
    if (!value || value->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(std::string(field) + " is required");
    }
    return value->str();
}

FxForwardTrade extractTrade(const quantra::PriceFxForward* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFxForward entry is null");
    }
    const auto* fwd = pricing->fx_forward();
    if (!fwd) {
        QUANTRA_INVALID_ARGUMENT("PriceFxForward.fx_forward is required");
    }

    FxForwardTrade trade;
    // The pair: two explicit ISO codes, never a pair string, never equal.
    trade.baseCurrency = RequireIsoCurrency(fwd->base_currency(), "FxForward.base_currency");
    trade.quoteCurrency = RequireIsoCurrency(fwd->quote_currency(), "FxForward.quote_currency");
    if (trade.baseCurrency == trade.quoteCurrency) {
        QUANTRA_INVALID_ARGUMENT(
            "FxForward.base_currency and FxForward.quote_currency must differ, both are '" +
            trade.baseCurrency + "'");
    }
    trade.baseNotional = requirePositive(fwd->base_notional(), "FxForward.base_notional");
    trade.forwardRate = requirePositive(fwd->forward_rate(), "FxForward.forward_rate");
    trade.maturityDate = DateToQL(requireId(fwd->maturity_date(), "FxForward.maturity_date"));
    trade.direction = FxDirectionToDomain(requireEnum(fwd->direction(), "FxForward.direction"));

    trade.fxSpotId = requireId(pricing->fx_spot(), "PriceFxForward.fx_spot");
    trade.baseDiscountingCurveId =
        requireId(pricing->base_discounting_curve(), "PriceFxForward.base_discounting_curve");
    trade.quoteDiscountingCurveId =
        requireId(pricing->quote_discounting_curve(), "PriceFxForward.quote_discounting_curve");
    trade.reportingCurrency =
        RequireIsoCurrency(pricing->reporting_currency(), "PriceFxForward.reporting_currency");
    if (trade.reportingCurrency != trade.baseCurrency &&
        trade.reportingCurrency != trade.quoteCurrency) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceFxForward.reporting_currency '" + trade.reportingCurrency + "' must be " +
            trade.baseCurrency + " or " + trade.quoteCurrency +
            " (reporting in a third currency is not supported)");
    }
    return trade;
}

} // namespace

FxForwardInputs FxForwardMapper::toInputs(const quantra::PriceFxForwardRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFxForwardRequest is null");
    }
    const auto* forwards = req->fx_forwards();
    if (forwards == nullptr || forwards->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceFxForwardRequest.fx_forwards is required and must be non-empty");
    }

    FxForwardInputs inputs;
    inputs.trades.reserve(forwards->size());
    for (auto it = forwards->begin(); it != forwards->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceFxForwardResponse> FxForwardMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const FxForwardResult& result) const {

    std::vector<flatbuffers::Offset<quantra::FxForwardResponse>> rows;
    rows.reserve(result.trades.size());

    for (const auto& trade : result.trades) {
        auto spotDate = builder.CreateString(trade.spotDate);
        auto maturityDate = builder.CreateString(trade.maturityDate);

        quantra::FxForwardResponseBuilder rb(builder);
        rb.add_npv(trade.npv);
        rb.add_fair_forward_rate(trade.fairForwardRate);
        rb.add_forward_points(trade.forwardPoints);
        rb.add_base_leg_pv(trade.baseLegPv);
        rb.add_quote_leg_pv(trade.quoteLegPv);
        rb.add_spot_date(spotDate);
        rb.add_maturity_date(maturityDate);
        rb.add_fx_spot_used(trade.fxSpotUsed);
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceFxForwardResponseBuilder rb(builder);
    rb.add_fx_forwards(rowsVec);
    return rb.Finish();
}

} // namespace quantra
