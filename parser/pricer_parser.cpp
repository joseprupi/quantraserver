#include "pricer_parser.h"

std::shared_ptr<QuantLib::IborCouponPricer> PricerParser::parse(const quantra::CouponPricer *pricer)
{

    auto pricer_type = pricer->pricer_type();
    ext::shared_ptr<QuantLib::IborCouponPricer> ibor_coupon_pricer;

    if (pricer_type == quantra::Pricer_BlackIborCouponPricer)
    {
        
        auto black_ibor_pricer = static_cast<const quantra::BlackIborCouponPricer *>(pricer->pricer());
        auto black_ibor_structuretype = black_ibor_pricer->optionlet_volatility_structure_type();

        if (black_ibor_structuretype == quantra::OptionletVolatilityStructure_ConstantOptionletVolatility){
            
            auto constant_optionlet_volatility = static_cast<const quantra::ConstantOptionletVolatility *>(black_ibor_pricer->optionlet_volatility_structure());
            ibor_coupon_pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();

            QuantLib::Volatility volatility = constant_optionlet_volatility->volatility();
            Handle<QuantLib::OptionletVolatilityStructure> vol;
            vol = Handle<QuantLib::OptionletVolatilityStructure>(ext::shared_ptr<QuantLib::OptionletVolatilityStructure>(new QuantLib::ConstantOptionletVolatility(
                constant_optionlet_volatility->settlement_days(),
                CalendarToQL(constant_optionlet_volatility->calendar()),
                ConventionToQL(constant_optionlet_volatility->business_day_convention()),
                volatility,
                DayCounterToQL(constant_optionlet_volatility->day_counter()))));

            ibor_coupon_pricer->setCapletVolatility(vol);
        }else{
            QUANTRA_ERROR("Optionlet Volatility Structure not supported");
        }
    }else{
        QUANTRA_ERROR("Coupon Pricer not supported");
    }
    
    return ibor_coupon_pricer;
}