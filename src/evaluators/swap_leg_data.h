#ifndef QUANTRA_SWAP_LEG_DATA_H
#define QUANTRA_SWAP_LEG_DATA_H

/**
 * Plain (FlatBuffers-free) description of one `SwapLeg`, the leg model every
 * swap endpoint shares: a coupon stream of one kind (fixed, IBOR, overnight
 * or CMS) in one currency, paid or received by the reported party, with its
 * own discounting / forwarding curve ids and principal-exchange flags. The
 * mappers lift a wire `SwapLeg` into this struct (swap_leg_mapper.h, the one
 * decoder); the evaluators value it through swap_leg_pricer.h. Ids are
 * resolved against the PricingRegistry at evaluation time.
 */

#include "enum_convert.h" // quantra::enums (CMS pricer kinds), as the other evaluators use it
#include "swap_leg_builder.h"

#include <ql/time/period.hpp>
#include <ql/time/schedule.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

/// CMS coupon pricer parameters (the `CmsPricerSpec` table as plain values,
/// with the schema's defaults when the table is absent).
struct CmsPricerParams {
    quantra::enums::CmsPricerType pricerType = quantra::enums::CmsPricerType_LinearTsr;
    quantra::enums::CmsYieldCurveModel yieldCurveModel =
        quantra::enums::CmsYieldCurveModel_Standard;
    double meanReversion = 0.03;
    double haganLowerLimit = 0.0;
    double haganUpperLimit = 1.0;
    double haganPrecision = 1.0e-6;
    /// Negative sentinel preserved: the pricer translates <= 0 to QL's default.
    double haganHardUpperLimit = -1.0;
};

/// Plain description of a constant-maturity swap-rate coupon leg (`CmsCoupons`).
struct CmsLegSpec {
    QuantLib::Schedule schedule = EmptySchedule();
    double notional = 0.0;
    std::string swapIndexId;
    QuantLib::Period swapTenor;
    std::string swaptionVolId;
    /// Negative sentinel preserved: the pricer falls back to the swap index's
    /// spot days.
    int fixingDays = -1;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;
    double gear = 1.0;
    double spread = 0.0;
    /// Optional per-coupon cap / floor strikes (CappedFlooredCmsCoupons when
    /// present).
    std::optional<double> cap;
    std::optional<double> floor;
    CmsPricerParams pricerParams{};
};

/// One `SwapLeg` as the evaluators see it.
struct SwapLegData {
    enum class Kind { Fixed, Ibor, Overnight, Cms };
    enum class Side { Pay, Receive };
    Kind kind = Kind::Fixed;
    Side side = Side::Pay;
    std::string currency;
    /// Constant notional (the first entry of a per-period vector when one was
    /// given); the amount a notional exchange moves.
    double notional = 0.0;
    /// Populated when kind == Fixed.
    FixedRateLegSpec fixed;
    /// Populated when kind == Ibor, together with `indexId`.
    FloatingLegSpec ibor;
    /// Populated when kind == Overnight, together with `indexId`.
    OvernightLegSpec overnight;
    /// Populated when kind == Cms (the swap index id lives inside it).
    CmsLegSpec cms;
    std::string indexId;
    bool initialExchange = false;
    bool finalExchange = false;
    std::string discountingCurveId;
    /// Empty for a fixed leg (the mapper rejects a present value there).
    std::string forwardingCurveId;

    /// The leg's schedule (whichever coupon kind it is).
    const QuantLib::Schedule& schedule() const {
        switch (kind) {
            case Kind::Fixed: return fixed.schedule;
            case Kind::Ibor: return ibor.schedule;
            case Kind::Overnight: return overnight.schedule;
            case Kind::Cms: return cms.schedule;
        }
        return fixed.schedule;
    }
    double sign() const { return side == Side::Pay ? -1.0 : 1.0; }
    /// Fixed rate or floating spread: the quantity a fair value solves for.
    double level() const {
        switch (kind) {
            case Kind::Fixed: return fixed.rate;
            case Kind::Ibor: return ibor.spread;
            case Kind::Overnight: return overnight.spread;
            case Kind::Cms: return cms.spread;
        }
        return 0.0;
    }
};

/// Name of a coupon kind as the wire spells it (`coupons_type`).
inline const char* SwapCouponsName(SwapLegData::Kind kind) {
    switch (kind) {
        case SwapLegData::Kind::Fixed: return "FixedCoupons";
        case SwapLegData::Kind::Ibor: return "IborCoupons";
        case SwapLegData::Kind::Overnight: return "OvernightCoupons";
        case SwapLegData::Kind::Cms: return "CmsCoupons";
    }
    return "";
}

} // namespace quantra

#endif // QUANTRA_SWAP_LEG_DATA_H
