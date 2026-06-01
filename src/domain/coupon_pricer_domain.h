#ifndef QUANTRA_COUPON_PRICER_DOMAIN_H
#define QUANTRA_COUPON_PRICER_DOMAIN_H

/**
 * Plain-domain (FB-free) representation of a CouponPricer spec.
 *
 * Mirror of flatbuffers/fbs/coupon_pricer.fbs:CouponPricer, expressed in
 * QL/std types so consumers (the floating-rate bond pricer, future variants)
 * need not include any *_generated.h header. Populated by
 * PricingRegistryBuilder alongside the legacy FB pointer vector (kept until
 * every consumer has been cut over).
 *
 * Today the schema admits a single payload — BlackIborCouponPricer with a
 * constant optionlet-vol block. The std::variant wrapper is intentional so
 * additional payloads (e.g. forward-curve aware Black, SABR-shifted) drop in
 * as new alternatives without changing every consumer's lookup site.
 *
 * The fields on BlackIborCouponPricerDomain are the inputs of QuantLib's
 * ConstantOptionletVolatility (settlement_days, calendar, business day
 * convention, volatility, day counter); these are exactly the fields the
 * legacy FB-aware PricerParser consumed verbatim.
 */

#include <string>
#include <variant>

#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>

namespace quantra {

struct BlackIborCouponPricerDomain {
    int settlement_days = 0;
    QuantLib::Calendar calendar;
    QuantLib::BusinessDayConvention business_day_convention = QuantLib::ModifiedFollowing;
    double volatility = 0.0;
    QuantLib::DayCounter day_counter;
};

struct CouponPricerDomain {
    std::string id;
    std::variant<BlackIborCouponPricerDomain> payload;
};

} // namespace quantra

#endif // QUANTRA_COUPON_PRICER_DOMAIN_H
