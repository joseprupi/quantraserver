#ifndef QUANTRA_STRUCTS_H
#define QUANTRA_STRUCTS_H

#include "enums_generated.h"

using namespace quantra::enums;

namespace structs
{

    struct Yield;
    struct Pricing;
    struct Schedule;
    struct Fixing;
    struct Index;
    struct DepositHelper;
    struct FRAHelper;
    struct FutureHelper;
    struct SwapHelper;
    struct BondHelper;
    struct Point;
    struct TermStructure;
    struct FixedRateBond;
    // struct FloatingRateBond;
    struct PriceFixedRateBond;
    struct PriceFixedRateBondRequest;

    struct Yield
    {
        quantra::enums::DayCounter day_counter;
        quantra::enums::Compounding compounding;
        quantra::enums::Frequency frequency;
    };

    struct Pricing
    {
        char as_of_date[11];
        char settlement_date[11];
        std::vector<std::shared_ptr<TermStructure>> curves;
        bool bond_pricing_details;
        bool bond_pricing_flows;
    };

    struct Schedule
    {
        quantra::enums::Calendar calendar;
        char effective_date[11];
        char termination_date[11];
        quantra::enums::Frequency frequency;
        quantra::enums::BusinessDayConvention convention;
        quantra::enums::BusinessDayConvention termination_date_convention;
        quantra::enums::DateGenerationRule date_generation_rule;
        bool end_of_mont;
    };

    struct Fixing
    {
        char date[11];
        float rate;
    };

    struct Index
    {
        int period_number;
        quantra::enums::TimeUnit period_time_unit;
        int settlement_days;
        quantra::enums::Calendar calendar;
        quantra::enums::BusinessDayConvention business_day_convention;
        bool end_of_month;
        quantra::enums::DayCounter day_counter;
        std::vector<Fixing> fixings;
    };

    struct DepositHelper
    {
        float rate;
        quantra::enums::TimeUnit tenor_time_unit;
        int tenor_number;
        int fixing_days;
        quantra::enums::Calendar calendar;
        quantra::enums::BusinessDayConvention business_day_convention;
        quantra::enums::DayCounter day_counter;
    };

    struct FRAHelper
    {
        float rate;
        int months_to_start;
        int months_to_end;
        int fixing_days;
        quantra::enums::Calendar calendar;
        quantra::enums::BusinessDayConvention business_day_convention;
        quantra::enums::DayCounter day_counter;
    };

    struct FutureHelper
    {
        float rate;
        char future_start_date[11];
        int future_months;
        quantra::enums::Calendar calendar;
        quantra::enums::BusinessDayConvention business_day_convention;
        quantra::enums::DayCounter day_counter;
    };

    struct SwapHelper
    {
        float rate;
        quantra::enums::TimeUnit tenor_time_unit;
        int tenor_number;
        quantra::enums::Calendar calendar;
        quantra::enums::Frequency sw_fixed_leg_frequency;
        quantra::enums::BusinessDayConvention sw_fixed_leg_convention;
        quantra::enums::DayCounter sw_fixed_leg_day_counter;
        quantra::enums::Ibor sw_floating_leg_index;
        float spread;
        int fwd_start_days;
    };

    struct BondHelper
    {
        BondHelper()
        {
            schedule = NULL;
        }
        float rate;
        int settlement_days;
        float face_amount;
        std::shared_ptr<Schedule> schedule;
        float coupon_rate;
        quantra::enums::DayCounter day_counter;
        quantra::enums::BusinessDayConvention business_day_convention;
        float redemption;
        char issue_date[11];
    };

    enum PointType
    {
        Deposit,
        FRA,
        Future,
        Swap,
        Bond
    };

    struct Point
    {
        Point()
        {
        }
        ~Point() {}
        PointType point_type;

        std::shared_ptr<DepositHelper> deposit_helper = std::make_shared<DepositHelper>();
        std::shared_ptr<FRAHelper> FRA_helper = std::make_shared<FRAHelper>();
        std::shared_ptr<FutureHelper> future_helper = std::make_shared<FutureHelper>();
        std::shared_ptr<SwapHelper> swap_helper = std::make_shared<SwapHelper>();
        std::shared_ptr<BondHelper> bond_helper = std::make_shared<BondHelper>();
    };

    struct TermStructure
    {
        char id[11];
        quantra::enums::DayCounter day_counter;
        quantra::enums::Interpolator interpolator;
        quantra::enums::BootstrapTrait bootstrap_trait;
        char reference_date[11];
        std::vector<std::shared_ptr<structs::Point>> points;
    };

    struct FixedRateBond
    {
        int settlement_days;
        double face_amount;
        double rate;
        quantra::enums::DayCounter accrual_day_counter;
        quantra::enums::BusinessDayConvention payment_convention;
        double redemption;
        char issue_date[11];
        std::shared_ptr<Schedule> schedule;
    };

    struct PriceFixedRateBond
    {
        std::shared_ptr<FixedRateBond> fixed_rate_bond;
        char discounting_curve[11];
        std::shared_ptr<Yield> yield;
    };

    struct PriceFixedRateBondRequest
    {
        std::shared_ptr<structs::Pricing> pricing;
        std::vector<std::shared_ptr<structs::PriceFixedRateBond>> bonds;
    };

    struct FloatingRateBond
    {
        int settlement_days;
        double face_amount;
        std::shared_ptr<Schedule> schedule;
        std::shared_ptr<Index> index;
        quantra::enums::DayCounter accrual_day_counter;
        quantra::enums::BusinessDayConvention payment_convention;
        int fixing_days;
        double spread;
        bool in_arrears;
        double redemption;
        std::string issue_date;
    };

    struct PriceFloatingRateBond
    {
        std::shared_ptr<FloatingRateBond> floating_rate_bond;
        char discounting_curve[11];
        char forecasting_curve[11];
        std::shared_ptr<Yield> yield;
    };

    struct PriceFloatingRateBondRequest
    {
        std::shared_ptr<Pricing> pricing;
        std::vector<std::shared_ptr<structs::PriceFloatingRateBond>> bonds;
    };

    struct PriceFixedRateBondValues
    {
        float npv;
    };

    struct PriceFixedRateBondResponse
    {
        std::vector<std::shared_ptr<PriceFixedRateBondValues>> price;
    };

    struct PriceFloatingRateBondValues
    {
        float npv;
    };

    struct PriceFloatingRateBondResponse
    {
        std::vector<std::shared_ptr<PriceFloatingRateBondValues>> price;
    };

}

#endif // QUANTRA_STRUCTS_H