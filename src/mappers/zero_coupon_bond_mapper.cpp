#include "zero_coupon_bond_mapper.h"

#include "enum_convert.h"
#include "error.h"
#include "zero_coupon_bond_parser.h"

namespace quantra {

ZeroCouponBondInputs ZeroCouponBondMapper::toInputs(
    const quantra::PriceZeroCouponBondRequest* req) const {

    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponBondRequest is null");
    }
    const auto* bondPricings = req->bonds();
    if (bondPricings == nullptr || bondPricings->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponBondRequest.bonds is required and must be non-empty");
    }

    ZeroCouponBondParser bondParser;
    ZeroCouponBondInputs inputs;
    inputs.trades.reserve(bondPricings->size());

    for (auto it = bondPricings->begin(); it != bondPricings->end(); ++it) {
        if (it->zero_coupon_bond() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("PriceZeroCouponBond.zero_coupon_bond is required");
        }
        if (it->discounting_curve() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("PriceZeroCouponBond.discounting_curve is required");
        }
        if (it->yield() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("PriceZeroCouponBond.yield is required");
        }

        ZeroCouponBondTrade trade;
        trade.bond = bondParser.parse(it->zero_coupon_bond());
        trade.discountingCurveId = it->discounting_curve()->str();
        if (!it->yield()->day_counter().has_value()) {
            QUANTRA_INVALID_ARGUMENT("Yield.day_counter is required");
        }
        if (!it->yield()->compounding().has_value()) {
            QUANTRA_INVALID_ARGUMENT("Yield.compounding is required");
        }
        if (!it->yield()->frequency().has_value()) {
            QUANTRA_INVALID_ARGUMENT("Yield.frequency is required");
        }
        trade.yieldDc = DayCounterToQL(it->yield()->day_counter().value());
        trade.yieldComp = CompoundingToQL(it->yield()->compounding().value());
        trade.yieldFreq = FrequencyToQL(it->yield()->frequency().value());
        inputs.trades.push_back(std::move(trade));
    }

    return inputs;
}

flatbuffers::Offset<quantra::PriceZeroCouponBondResponse> ZeroCouponBondMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const ZeroCouponBondResult& result) const {

    std::vector<flatbuffers::Offset<quantra::ZeroCouponBondResponse>> bondsVector;
    bondsVector.reserve(result.bonds.size());

    for (const auto& bond : result.bonds) {
        auto settlementDate = builder.CreateString(bond.settlementDate);

        quantra::ZeroCouponBondResponseBuilder rb(builder);
        rb.add_npv(bond.npv);
        rb.add_settlement_date(settlementDate);
        if (bond.hasDetails) {
            rb.add_clean_price(bond.cleanPrice);
            rb.add_dirty_price(bond.dirtyPrice);
            rb.add_accrued_amount(bond.accruedAmount);
            rb.add_yield(bond.yield);
            rb.add_accrued_days(bond.accruedDays);
            rb.add_modified_duration(bond.modifiedDuration);
            rb.add_macaulay_duration(bond.macaulayDuration);
            rb.add_convexity(bond.convexity);
            rb.add_bps(bond.bps);
        }
        bondsVector.push_back(rb.Finish());
    }

    auto bonds = builder.CreateVector(bondsVector);
    quantra::PriceZeroCouponBondResponseBuilder rb(builder);
    rb.add_bonds(bonds);
    return rb.Finish();
}

} // namespace quantra
