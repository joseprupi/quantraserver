#include "fra_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

namespace {

FraTrade extractTrade(const quantra::PriceFRA* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFRA entry is null");
    }
    if (!pricing->fra()) {
        QUANTRA_INVALID_ARGUMENT("PriceFRA.fra is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceFRA.discounting_curve is required");
    }
    if (!pricing->forwarding_curve() || pricing->forwarding_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceFRA.forwarding_curve is required");
    }

    const auto* fra = pricing->fra();
    if (!fra->start_date()) {
        QUANTRA_INVALID_ARGUMENT("FRA.start_date is required");
    }
    if (!fra->maturity_date()) {
        QUANTRA_INVALID_ARGUMENT("FRA.maturity_date is required");
    }
    if (!fra->index() || !fra->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("FRA.index.id is required");
    }
    if (!fra->fra_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("FRA.fra_type is required");
    }
    if (!fra->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("FRA.day_counter is required");
    }
    if (!fra->calendar().has_value()) {
        QUANTRA_INVALID_ARGUMENT("FRA.calendar is required");
    }
    if (!fra->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("FRA.business_day_convention is required");
    }

    FraTrade trade;
    trade.startDate = DateToQL(fra->start_date()->str());
    trade.maturityDate = DateToQL(fra->maturity_date()->str());
    trade.position = FRATypeToQL(fra->fra_type().value());
    trade.notional = fra->notional();
    trade.strike = fra->strike();
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();
    trade.indexId = fra->index()->id()->str();
    return trade;
}

} // namespace

FraInputs FraMapper::toInputs(const quantra::PriceFRARequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFRARequest is null");
    }
    const auto* fras = req->fras();
    if (fras == nullptr || fras->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceFRARequest.fras is required and must be non-empty");
    }

    FraInputs inputs;
    inputs.trades.reserve(fras->size());
    for (auto it = fras->begin(); it != fras->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceFRAResponse> FraMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const FraResult& result) const {

    std::vector<flatbuffers::Offset<quantra::FRAResponse>> frasVector;
    frasVector.reserve(result.trades.size());

    for (const auto& trade : result.trades) {
        auto settlementDate = builder.CreateString(trade.settlementDate);

        quantra::FRAResponseBuilder rb(builder);
        rb.add_npv(trade.npv);
        rb.add_forward_rate(trade.forwardRate);
        rb.add_spot_value(trade.spotValue);
        rb.add_settlement_date(settlementDate);
        frasVector.push_back(rb.Finish());
    }

    auto frasVec = builder.CreateVector(frasVector);
    quantra::PriceFRAResponseBuilder rb(builder);
    rb.add_fras(frasVec);
    return rb.Finish();
}

} // namespace quantra
