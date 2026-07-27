#include "zero_coupon_swap_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "require_scalar.h"

namespace quantra {

namespace {

ZeroCouponSwapTrade extractTrade(const quantra::PriceZeroCouponSwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponSwap entry is null");
    }
    if (!pricing->zero_coupon_swap()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponSwap.zero_coupon_swap is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponSwap.discounting_curve is required");
    }
    if (!pricing->forwarding_curve() || pricing->forwarding_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponSwap.forwarding_curve is required");
    }

    const auto* swap = pricing->zero_coupon_swap();
    if (!swap->swap_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponSwap.swap_type is required");
    }
    if (!swap->start_date()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponSwap.start_date is required");
    }
    if (!swap->maturity_date()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponSwap.maturity_date is required");
    }
    if (!swap->index() || !swap->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponSwap.index.id is required");
    }
    if (!swap->payment_calendar().has_value()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponSwap.payment_calendar is required");
    }
    if (!swap->payment_convention().has_value()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponSwap.payment_convention is required");
    }

    // Exactly one of the two fixed-side quoting forms must be supplied:
    //   * fixed_payment (the known fixed cashflow), or
    //   * fixed_rate + fixed_rate_day_counter (compounded fixed rate).
    const bool hasPayment = swap->fixed_payment().has_value();
    const bool hasRate = swap->fixed_rate().has_value();
    if (hasPayment == hasRate) {
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponSwap must contain exactly one of fixed_payment or fixed_rate");
    }

    ZeroCouponSwapTrade trade;
    trade.swapType = ZeroCouponSwapTypeToQL(swap->swap_type().value());
    trade.baseNominal = requirePositive(swap->base_nominal(), "ZeroCouponSwap.base_nominal");
    trade.startDate = DateToQL(swap->start_date()->str());
    trade.maturityDate = DateToQL(swap->maturity_date()->str());
    if (trade.maturityDate <= trade.startDate) {
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponSwap.maturity_date must be after start_date");
    }
    trade.paymentCalendar = CalendarToQL(swap->payment_calendar().value());
    trade.paymentConvention = ConventionToQL(swap->payment_convention().value());
    trade.paymentDelay = swap->payment_delay();
    trade.indexId = swap->index()->id()->str();
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();

    if (hasRate) {
        if (!swap->fixed_rate_day_counter().has_value()) {
            QUANTRA_INVALID_ARGUMENT(
                "ZeroCouponSwap.fixed_rate_day_counter is required with fixed_rate");
        }
        trade.hasFixedRate = true;
        trade.fixedRate = swap->fixed_rate().value();
        trade.fixedRateDc = DayCounterToQL(swap->fixed_rate_day_counter().value());
    } else {
        trade.hasFixedRate = false;
        trade.fixedPayment = swap->fixed_payment().value();
    }

    return trade;
}

} // namespace

ZeroCouponSwapInputs ZeroCouponSwapMapper::toInputs(
    const quantra::PriceZeroCouponSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceZeroCouponSwapRequest.swaps is required and must be non-empty");
    }

    ZeroCouponSwapInputs inputs;
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceZeroCouponSwapResponse> ZeroCouponSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const ZeroCouponSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::ZeroCouponSwapResponse>> swapsVector;
    swapsVector.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        auto startDate = builder.CreateString(swap.startDate);

        quantra::ZeroCouponSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        rb.add_fixed_leg_npv(swap.fixedLegNpv);
        rb.add_floating_leg_npv(swap.floatingLegNpv);
        rb.add_fair_fixed_payment(swap.fairFixedPayment);
        if (swap.hasFairFixedRate) {
            rb.add_fair_fixed_rate(swap.fairFixedRate);
        }
        rb.add_fixed_payment(swap.fixedPayment);
        rb.add_start_date(startDate);
        swapsVector.push_back(rb.Finish());
    }

    auto swapsVec = builder.CreateVector(swapsVector);
    quantra::PriceZeroCouponSwapResponseBuilder rb(builder);
    rb.add_swaps(swapsVec);
    return rb.Finish();
}

} // namespace quantra
