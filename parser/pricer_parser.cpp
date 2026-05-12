#include "pricer_parser.h"

std::shared_ptr<QuantLib::IborCouponPricer> PricerParser::parse(const quantra::CouponPricer *pricer)
{
    if (!pricer) {
        QUANTRA_INVALID_ARGUMENT("Coupon pricer not found");
    }

    const auto* black_ibor_pricer = pricer->black_ibor_coupon_pricer();
    if (!black_ibor_pricer) {
        QUANTRA_INVALID_ARGUMENT("Coupon pricer black_ibor_coupon_pricer not found");
    }

    const auto* optionlet_volatility = black_ibor_pricer->optionlet_volatility();
    if (!optionlet_volatility) {
        QUANTRA_INVALID_ARGUMENT("Coupon pricer optionlet_volatility not found");
    }

    auto ibor_coupon_pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
    QuantLib::Volatility volatility = optionlet_volatility->volatility();
    Handle<QuantLib::OptionletVolatilityStructure> vol;
    vol = Handle<QuantLib::OptionletVolatilityStructure>(
        ext::shared_ptr<QuantLib::OptionletVolatilityStructure>(
            new QuantLib::ConstantOptionletVolatility(
                optionlet_volatility->settlement_days(),
                CalendarToQL(optionlet_volatility->calendar()),
                ConventionToQL(optionlet_volatility->business_day_convention()),
                volatility,
                DayCounterToQL(optionlet_volatility->day_counter()))));

    ibor_coupon_pricer->setCapletVolatility(vol);
    return ibor_coupon_pricer;
}