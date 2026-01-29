#include "vanilla_swap_request_fbs.h"

flatbuffers::grpc::Message<quantra::PriceVanillaSwapRequest> swap_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder)
{
    // =========================================================================
    // Settings
    // =========================================================================
    auto calendar = quantra::enums::Calendar_TARGET;
    int fixing_days = 2;
    int settlement_days = 2;

    // =========================================================================
    // Build the yield curve (EUR swap curve)
    // =========================================================================
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;

    // Deposit 1M
    {
        auto deposit_builder = quantra::DepositHelperBuilder(*builder);
        deposit_builder.add_rate(0.035);
        deposit_builder.add_tenor_number(1);
        deposit_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
        deposit_builder.add_fixing_days(fixing_days);
        deposit_builder.add_calendar(calendar);
        deposit_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        deposit_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
        auto deposit = deposit_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_DepositHelper);
        wrapper_builder.add_point(deposit.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    // Deposit 3M
    {
        auto deposit_builder = quantra::DepositHelperBuilder(*builder);
        deposit_builder.add_rate(0.036);
        deposit_builder.add_tenor_number(3);
        deposit_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
        deposit_builder.add_fixing_days(fixing_days);
        deposit_builder.add_calendar(calendar);
        deposit_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        deposit_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
        auto deposit = deposit_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_DepositHelper);
        wrapper_builder.add_point(deposit.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    // Deposit 6M
    {
        auto deposit_builder = quantra::DepositHelperBuilder(*builder);
        deposit_builder.add_rate(0.037);
        deposit_builder.add_tenor_number(6);
        deposit_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
        deposit_builder.add_fixing_days(fixing_days);
        deposit_builder.add_calendar(calendar);
        deposit_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        deposit_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
        auto deposit = deposit_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_DepositHelper);
        wrapper_builder.add_point(deposit.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    // Deposit 1Y
    {
        auto deposit_builder = quantra::DepositHelperBuilder(*builder);
        deposit_builder.add_rate(0.038);
        deposit_builder.add_tenor_number(1);
        deposit_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        deposit_builder.add_fixing_days(fixing_days);
        deposit_builder.add_calendar(calendar);
        deposit_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        deposit_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
        auto deposit = deposit_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_DepositHelper);
        wrapper_builder.add_point(deposit.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    // Swap 2Y
    {
        auto swap_builder = quantra::SwapHelperBuilder(*builder);
        swap_builder.add_rate(0.039);
        swap_builder.add_tenor_number(2);
        swap_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        swap_builder.add_calendar(calendar);
        swap_builder.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        swap_builder.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        swap_builder.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        swap_builder.add_sw_floating_leg_index(quantra::enums::Ibor_Euribor6M);
        auto swap = swap_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_SwapHelper);
        wrapper_builder.add_point(swap.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    // Swap 5Y
    {
        auto swap_builder = quantra::SwapHelperBuilder(*builder);
        swap_builder.add_rate(0.040);
        swap_builder.add_tenor_number(5);
        swap_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        swap_builder.add_calendar(calendar);
        swap_builder.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        swap_builder.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        swap_builder.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        swap_builder.add_sw_floating_leg_index(quantra::enums::Ibor_Euribor6M);
        auto swap = swap_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_SwapHelper);
        wrapper_builder.add_point(swap.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    // Swap 10Y
    {
        auto swap_builder = quantra::SwapHelperBuilder(*builder);
        swap_builder.add_rate(0.042);
        swap_builder.add_tenor_number(10);
        swap_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        swap_builder.add_calendar(calendar);
        swap_builder.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        swap_builder.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        swap_builder.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        swap_builder.add_sw_floating_leg_index(quantra::enums::Ibor_Euribor6M);
        auto swap = swap_builder.Finish();

        auto wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        wrapper_builder.add_point_type(quantra::Point_SwapHelper);
        wrapper_builder.add_point(swap.Union());
        points_vector.push_back(wrapper_builder.Finish());
    }

    auto points = builder->CreateVector(points_vector);

    // Create the term structure
    auto curve_id = builder->CreateString("eur_swap_curve");
    auto settlement_date_str = builder->CreateString("2025/01/29");
    
    auto term_structure_builder = quantra::TermStructureBuilder(*builder);
    term_structure_builder.add_bootstrap_trait(quantra::enums::BootstrapTrait_Discount);
    term_structure_builder.add_interpolator(quantra::enums::Interpolator_LogLinear);
    term_structure_builder.add_id(curve_id);
    term_structure_builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    term_structure_builder.add_reference_date(settlement_date_str);
    term_structure_builder.add_points(points);
    auto term_structure = term_structure_builder.Finish();

    // =========================================================================
    // Create Pricing (as_of_date + curves)
    // =========================================================================
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vector;
    curves_vector.push_back(term_structure);
    auto curves = builder->CreateVector(curves_vector);

    auto as_of_date = builder->CreateString("2025/01/27");
    auto pricing_settlement_date = builder->CreateString("2025/01/29");
    
    auto pricing_builder = quantra::PricingBuilder(*builder);
    pricing_builder.add_as_of_date(as_of_date);
    pricing_builder.add_settlement_date(pricing_settlement_date);
    pricing_builder.add_curves(curves);
    auto pricing = pricing_builder.Finish();

    // =========================================================================
    // Create the Vanilla Swap (5Y, pay 3.5% fixed, receive 6M Euribor)
    // =========================================================================

    // Fixed leg schedule
    auto fixed_effective = builder->CreateString("2025/01/29");
    auto fixed_termination = builder->CreateString("2030/01/29");
    
    auto fixed_schedule_builder = quantra::ScheduleBuilder(*builder);
    fixed_schedule_builder.add_calendar(calendar);
    fixed_schedule_builder.add_effective_date(fixed_effective);
    fixed_schedule_builder.add_termination_date(fixed_termination);
    fixed_schedule_builder.add_frequency(quantra::enums::Frequency_Annual);
    fixed_schedule_builder.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixed_schedule_builder.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixed_schedule_builder.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fixed_schedule_builder.add_end_of_month(false);
    auto fixed_schedule = fixed_schedule_builder.Finish();

    // Fixed leg
    auto fixed_leg_builder = quantra::SwapFixedLegBuilder(*builder);
    fixed_leg_builder.add_schedule(fixed_schedule);
    fixed_leg_builder.add_notional(10000000.0);  // 10 million EUR
    fixed_leg_builder.add_rate(0.035);           // 3.5% fixed rate
    fixed_leg_builder.add_day_counter(quantra::enums::DayCounter_Thirty360);
    fixed_leg_builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixed_leg = fixed_leg_builder.Finish();

    // Floating leg schedule
    auto float_effective = builder->CreateString("2025/01/29");
    auto float_termination = builder->CreateString("2030/01/29");
    
    auto float_schedule_builder = quantra::ScheduleBuilder(*builder);
    float_schedule_builder.add_calendar(calendar);
    float_schedule_builder.add_effective_date(float_effective);
    float_schedule_builder.add_termination_date(float_termination);
    float_schedule_builder.add_frequency(quantra::enums::Frequency_Semiannual);
    float_schedule_builder.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    float_schedule_builder.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    float_schedule_builder.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    float_schedule_builder.add_end_of_month(false);
    auto float_schedule = float_schedule_builder.Finish();

    // Floating leg index (6M Euribor)
    auto index_builder = quantra::IndexBuilder(*builder);
    index_builder.add_period_number(6);
    index_builder.add_period_time_unit(quantra::enums::TimeUnit_Months);
    index_builder.add_settlement_days(2);
    index_builder.add_calendar(calendar);
    index_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    index_builder.add_end_of_month(false);
    index_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
    auto index = index_builder.Finish();

    // Floating leg
    auto float_leg_builder = quantra::SwapFloatingLegBuilder(*builder);
    float_leg_builder.add_schedule(float_schedule);
    float_leg_builder.add_notional(10000000.0);  // 10 million EUR
    float_leg_builder.add_index(index);
    float_leg_builder.add_spread(0.0);           // No spread
    float_leg_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
    float_leg_builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    float_leg_builder.add_fixing_days(2);
    float_leg_builder.add_in_arrears(false);
    auto float_leg = float_leg_builder.Finish();

    // Vanilla Swap
    auto swap_builder = quantra::VanillaSwapBuilder(*builder);
    swap_builder.add_swap_type(quantra::enums::SwapType_Payer);  // Pay fixed, receive floating
    swap_builder.add_fixed_leg(fixed_leg);
    swap_builder.add_floating_leg(float_leg);
    auto vanilla_swap = swap_builder.Finish();

    // =========================================================================
    // Create PriceVanillaSwap (swap + curve references)
    // =========================================================================
    auto disc_curve_ref = builder->CreateString("eur_swap_curve");
    auto fwd_curve_ref = builder->CreateString("eur_swap_curve");  // Same curve for simplicity
    
    auto price_swap_builder = quantra::PriceVanillaSwapBuilder(*builder);
    price_swap_builder.add_vanilla_swap(vanilla_swap);
    price_swap_builder.add_discounting_curve(disc_curve_ref);
    price_swap_builder.add_forwarding_curve(fwd_curve_ref);
    auto price_swap = price_swap_builder.Finish();

    std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>> swaps_vector;
    swaps_vector.push_back(price_swap);
    auto swaps = builder->CreateVector(swaps_vector);

    // =========================================================================
    // Create the final request
    // =========================================================================
    auto request_builder = quantra::PriceVanillaSwapRequestBuilder(*builder);
    request_builder.add_pricing(pricing);
    request_builder.add_swaps(swaps);
    request_builder.add_include_flows(true);  // Include cashflows in response
    auto request = request_builder.Finish();
    
    builder->Finish(request);

    auto p = builder->GetBufferPointer();
    auto size = builder->GetSize();

    ::grpc::Slice slice((char *)p, size);
    flatbuffers::grpc::Message<quantra::PriceVanillaSwapRequest> msg(slice);
    std::cout << "Verify:" << msg.Verify() << std::endl;

    return msg;
}