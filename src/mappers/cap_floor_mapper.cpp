#include "cap_floor_mapper.h"

#include "date_convert.h"
#include "schedule_parser.h"
#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

namespace quantra {

namespace {

CapFloorTrade extractTrade(const quantra::PriceCapFloor* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloor entry is null");
    }
    const auto* capFloor = pricing->cap_floor();
    if (capFloor == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloor.cap_floor is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloor.discounting_curve is required");
    }
    if (!pricing->forwarding_curve() || pricing->forwarding_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloor.forwarding_curve is required");
    }
    if (!pricing->volatility() || pricing->volatility()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloor.volatility is required");
    }
    if (!pricing->model() || pricing->model()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloor.model is required");
    }
    if (capFloor->schedule() == nullptr) {
        QUANTRA_INVALID_ARGUMENT("CapFloor.schedule is required");
    }
    if (!capFloor->index() || !capFloor->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("CapFloor.index.id is required");
    }
    if (!capFloor->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CapFloor.day_counter is required");
    }
    if (!capFloor->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CapFloor.business_day_convention is required");
    }
    if (!capFloor->cap_floor_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CapFloor.cap_floor_type is required");
    }

    ScheduleParser scheduleParser;
    auto schedule = scheduleParser.parse(capFloor->schedule());

    CapFloorTrade trade;
    trade.capFloorType = CapFloorTypeToQL(capFloor->cap_floor_type().value());
    trade.schedule = *schedule;
    trade.notional = requirePositive(capFloor->notional(), "CapFloor.notional");
    trade.strike = requireFinite(capFloor->strike(), "CapFloor.strike");
    trade.dayCounter = DayCounterToQL(capFloor->day_counter().value());
    trade.businessDayConvention = ConventionToQL(capFloor->business_day_convention().value());
    trade.includeDetails = pricing->include_details();

    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();
    trade.indexId = capFloor->index()->id()->str();
    trade.volatilityId = pricing->volatility()->str();
    trade.modelId = pricing->model()->str();
    return trade;
}

} // namespace

CapFloorInputs CapFloorMapper::toInputs(const quantra::PriceCapFloorRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCapFloorRequest is null");
    }
    const auto* list = req->cap_floors();
    if (list == nullptr || list->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCapFloorRequest.cap_floors is required and must be non-empty");
    }

    CapFloorInputs inputs;
    inputs.trades.reserve(list->size());
    for (auto it = list->begin(); it != list->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceCapFloorResponse> CapFloorMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CapFloorResult& result) const {

    std::vector<flatbuffers::Offset<quantra::CapFloorResponse>> capFloors;
    capFloors.reserve(result.trades.size());

    for (const auto& trade : result.trades) {
        std::vector<flatbuffers::Offset<quantra::CapFloorLet>> letOffsets;
        letOffsets.reserve(trade.details.size());
        for (const auto& d : trade.details) {
            auto paymentDate = builder.CreateString(d.paymentDate);
            auto accrualStart = builder.CreateString(d.accrualStartDate);
            auto accrualEnd = builder.CreateString(d.accrualEndDate);
            auto fixingDate = builder.CreateString(d.fixingDate);

            quantra::CapFloorLetBuilder lb(builder);
            lb.add_payment_date(paymentDate);
            lb.add_accrual_start_date(accrualStart);
            lb.add_accrual_end_date(accrualEnd);
            lb.add_fixing_date(fixingDate);
            lb.add_forward_rate(d.forwardRate);
            lb.add_discount(d.discount);
            letOffsets.push_back(lb.Finish());
        }
        auto letsVec = builder.CreateVector(letOffsets);

        quantra::CapFloorResponseBuilder rb(builder);
        rb.add_npv(trade.npv);
        rb.add_atm_rate(trade.atmRate);
        rb.add_cap_floor_lets(letsVec);
        capFloors.push_back(rb.Finish());
    }

    auto capFloorsVec = builder.CreateVector(capFloors);
    quantra::PriceCapFloorResponseBuilder rb(builder);
    rb.add_cap_floors(capFloorsVec);
    return rb.Finish();
}

} // namespace quantra
