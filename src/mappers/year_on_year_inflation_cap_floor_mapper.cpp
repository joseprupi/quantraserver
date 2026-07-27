#include "year_on_year_inflation_cap_floor_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "schedule_parser.h"
#include "error.h"
#include "request_validation.h"

namespace quantra {

namespace {

YoYInflationCapFloorTrade extractTrade(const quantra::PriceYoYInflationCapFloor* pricing) {
    if (!pricing) {
        QUANTRA_INVALID_ARGUMENT("PriceYoYInflationCapFloor entry is null");
    }
    if (!pricing->year_on_year_inflation_cap_floor()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceYoYInflationCapFloor entry requires year_on_year_inflation_cap_floor");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceYoYInflationCapFloor.discounting_curve is required");
    }
    if (!pricing->inflation_curve() || pricing->inflation_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceYoYInflationCapFloor.inflation_curve is required");
    }
    if (!pricing->volatility() || pricing->volatility()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceYoYInflationCapFloor.volatility is required");
    }

    const auto* cf = pricing->year_on_year_inflation_cap_floor();
    if (!cf->cap_floor_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor.cap_floor_type is required");
    }
    if (!cf->schedule()) {
        QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor.schedule not found");
    }
    if (!cf->inflation_index_id() || cf->inflation_index_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor.inflation_index_id is required");
    }
    if (!cf->observation_lag()) {
        QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor.observation_lag not found");
    }
    if (!cf->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor.day_counter is required");
    }
    if (!cf->payment_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor.payment_convention is required");
    }

    const auto capFloorType = CapFloorTypeToQL(cf->cap_floor_type().value());
    const bool hasCap = cf->cap_rate().has_value();
    const bool hasFloor = cf->floor_rate().has_value();

    switch (capFloorType) {
        case QuantLib::CapFloor::Cap:
            if (!hasCap) {
                QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor of type Cap requires cap_rate");
            }
            if (hasFloor) {
                QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor of type Cap must not carry floor_rate");
            }
            break;
        case QuantLib::CapFloor::Floor:
            if (!hasFloor) {
                QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor of type Floor requires floor_rate");
            }
            if (hasCap) {
                QUANTRA_INVALID_ARGUMENT("YoYInflationCapFloor of type Floor must not carry cap_rate");
            }
            break;
        case QuantLib::CapFloor::Collar:
            if (!hasCap || !hasFloor) {
                QUANTRA_INVALID_ARGUMENT(
                    "YoYInflationCapFloor of type Collar requires both cap_rate and floor_rate");
            }
            if (cf->cap_rate().value() < cf->floor_rate().value()) {
                QUANTRA_INVALID_ARGUMENT(
                    "YoYInflationCapFloor Collar cap_rate must be >= floor_rate");
            }
            break;
    }

    ScheduleParser scheduleParser;
    auto schedule = scheduleParser.parse(cf->schedule());

    YoYInflationCapFloorTrade trade;
    trade.capFloorType = static_cast<QuantLib::YoYInflationCapFloor::Type>(capFloorType);
    trade.notional = requirePositive(cf->notional(), "YoYInflationCapFloor.notional");
    trade.schedule = *schedule;
    trade.inflationIndexId = cf->inflation_index_id()->str();
    trade.observationLag = requirePeriod(
        cf->observation_lag(), "YoYInflationCapFloor.observation_lag");
    trade.dayCounter = DayCounterToQL(cf->day_counter().value());
    trade.paymentConvention = ConventionToQL(cf->payment_convention().value());

    if (hasCap) {
        trade.hasCapRate = true;
        trade.capRate = cf->cap_rate().value();
    }
    if (hasFloor) {
        trade.hasFloorRate = true;
        trade.floorRate = cf->floor_rate().value();
    }
    if (cf->gearing().has_value()) {
        trade.hasGearing = true;
        trade.gearing = cf->gearing().value();
    }
    if (cf->spread().has_value()) {
        trade.hasSpread = true;
        trade.spread = cf->spread().value();
    }

    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.inflationCurveId = pricing->inflation_curve()->str();
    trade.volatilityId = pricing->volatility()->str();
    return trade;
}

} // namespace

YoYInflationCapFloorInputs YearOnYearInflationCapFloorMapper::toInputs(
    const quantra::PriceYearOnYearInflationCapFloorRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceYearOnYearInflationCapFloorRequest is null");
    }
    const auto* capFloors = req->cap_floors();
    if (capFloors == nullptr || capFloors->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceYearOnYearInflationCapFloorRequest.cap_floors is required and must be non-empty");
    }

    YoYInflationCapFloorInputs inputs;
    inputs.trades.reserve(capFloors->size());
    for (auto it = capFloors->begin(); it != capFloors->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceYearOnYearInflationCapFloorResponse>
YearOnYearInflationCapFloorMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const YoYInflationCapFloorResult& result) const {

    std::vector<flatbuffers::Offset<quantra::YoYInflationCapFloorResponse>> capFloorsVector;
    capFloorsVector.reserve(result.capFloors.size());

    for (const auto& cf : result.capFloors) {
        quantra::YoYInflationCapFloorResponseBuilder rb(builder);
        rb.add_npv(cf.npv);
        rb.add_atm_rate(cf.atmRate);
        capFloorsVector.push_back(rb.Finish());
    }

    auto capFloorsVec = builder.CreateVector(capFloorsVector);
    quantra::PriceYearOnYearInflationCapFloorResponseBuilder rb(builder);
    rb.add_cap_floors(capFloorsVec);
    return rb.Finish();
}

} // namespace quantra
