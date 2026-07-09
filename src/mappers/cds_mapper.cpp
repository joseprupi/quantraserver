#include "cds_mapper.h"

#include "date_convert.h"
#include "schedule_parser.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

namespace {

CdsTrade extractTrade(const quantra::PriceCDS* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCDS entry is null");
    }
    if (!pricing->cds()) {
        QUANTRA_INVALID_ARGUMENT("PriceCDS entry requires cds");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCDS entry requires discounting_curve");
    }
    if (!pricing->credit_curve_id() || pricing->credit_curve_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCDS entry requires credit_curve_id");
    }
    if (!pricing->model() || pricing->model()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceCDS entry requires model");
    }

    const auto* cds = pricing->cds();
    if (!cds->schedule()) {
        QUANTRA_INVALID_ARGUMENT("CDS schedule is required");
    }

    ScheduleParser scheduleParser;
    auto schedule = scheduleParser.parse(cds->schedule());

    if (!cds->side().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CDS.side is required");
    }
    CdsTrade trade;
    trade.side = ProtectionSideToQL(cds->side().value());
    trade.notional = cds->notional();
    if (!cds->running_coupon().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CDS.running_coupon is required");
    }
    trade.runningCoupon = cds->running_coupon().value();
    if (cds->upfront().has_value()) {
        trade.upfront = cds->upfront().value();
    }
    trade.schedule = *schedule;
    if (!cds->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CDS.day_counter is required");
    }
    if (!cds->business_day_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("CDS.business_day_convention is required");
    }
    trade.dayCounter = DayCounterToQL(cds->day_counter().value());
    trade.businessDayConvention = ConventionToQL(cds->business_day_convention().value());
    trade.settlesAccrual = cds->settles_accrual();
    trade.paysAtDefaultTime = cds->pays_at_default_time();
    trade.rebatesAccrual = cds->rebates_accrual();
    trade.lastPeriodDayCounter = DayCounterToQL(cds->last_period_day_counter());
    if (cds->protection_start() && cds->protection_start()->size() > 0) {
        trade.protectionStart = DateToQL(cds->protection_start()->str());
    }
    if (cds->upfront_date() && cds->upfront_date()->size() > 0) {
        trade.upfrontDate = DateToQL(cds->upfront_date()->str());
        trade.hasUpfrontDate = true;
    }
    if (cds->trade_date() && cds->trade_date()->size() > 0) {
        trade.tradeDate = DateToQL(cds->trade_date()->str());
    }
    trade.cashSettlementDays = cds->cash_settlement_days();

    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.creditCurveId = pricing->credit_curve_id()->str();
    trade.modelId = pricing->model()->str();
    return trade;
}

} // namespace

CdsInputs CdsMapper::toInputs(const quantra::PriceCDSRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCDSRequest is null");
    }
    const auto* list = req->cds_list();
    if (list == nullptr || list->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCDSRequest.cds_list is required and must be non-empty");
    }

    CdsInputs inputs;
    inputs.trades.reserve(list->size());
    for (auto it = list->begin(); it != list->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceCDSResponse> CdsMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CdsResult& result) const {

    std::vector<flatbuffers::Offset<quantra::CDSValues>> valueOffsets;
    valueOffsets.reserve(result.values.size());
    for (const auto& v : result.values) {
        quantra::CDSValuesBuilder vb(builder);
        vb.add_npv(v.npv);
        if (v.fairSpread) {
            vb.add_fair_spread(*v.fairSpread);
        }
        vb.add_fair_upfront(v.fairUpfront);
        vb.add_default_leg_npv(v.defaultLegNpv);
        vb.add_premium_leg_npv(v.premiumLegNpv);
        valueOffsets.push_back(vb.Finish());
    }

    auto cdsList = builder.CreateVector(valueOffsets);
    quantra::PriceCDSResponseBuilder rb(builder);
    rb.add_cds_list(cdsList);
    return rb.Finish();
}

} // namespace quantra
