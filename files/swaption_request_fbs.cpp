#include "swaption_request_fbs.h"

flatbuffers::grpc::Message<quantra::PriceSwaptionRequest> swaption_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder)
{
    // =========================================================================
    // Settings
    // =========================================================================
    auto calendar = quantra::enums::Calendar_TARGET;
    int fixing_days = 2;

    // =========================================================================
    // Build the yield curve
    // =========================================================================
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;

    // Deposits
    double depo_rates[] = {0.035, 0.036, 0.037, 0.038};
    int depo_tenors[] = {1, 3, 6, 12};
    quantra::enums::TimeUnit depo_units[] = {
        quantra::enums::TimeUnit_Months,
        quantra::enums::TimeUnit_Months,
        quantra::enums::TimeUnit_Months,
        quantra::enums::TimeUnit_Months
    };

    for (int i = 0; i < 4; i++) {
        auto deposit_builder = quantra::DepositHelperBuilder(*builder);
        deposit_builder.add_rate(depo_rates[i]);
        deposit_builder.add_tenor_number(depo_tenors[i]);
        deposit_builder.add_tenor_time_unit(depo_units[i]);
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

    // Swaps
    double swap_rates[] = {0.039, 0.040, 0.042, 0.044};
    int swap_tenors[] = {2, 5, 10, 20};

    for (int i = 0; i < 4; i++) {
        auto swap_builder = quantra::SwapHelperBuilder(*builder);
        swap_builder.add_rate(swap_rates[i]);
        swap_builder.add_tenor_number(swap_tenors[i]);
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
    auto curve_id = builder->CreateString("eur_curve");
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
    // Create Pricing
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
    // Create the Underlying Swap (5Y swap starting in 1Y)
    // =========================================================================

    // Fixed leg schedule (starts 1Y from now, 5Y tenor)
    auto fixed_effective = builder->CreateString("2026/01/29");
    auto fixed_termination = builder->CreateString("2031/01/29");
    
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
    fixed_leg_builder.add_notional(10000000.0);
    fixed_leg_builder.add_rate(0.04);  // 4% strike
    fixed_leg_builder.add_day_counter(quantra::enums::DayCounter_Thirty360);
    fixed_leg_builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixed_leg = fixed_leg_builder.Finish();

    // Floating leg schedule
    auto float_effective = builder->CreateString("2026/01/29");
    auto float_termination = builder->CreateString("2031/01/29");
    
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

    // Index
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
    float_leg_builder.add_notional(10000000.0);
    float_leg_builder.add_index(index);
    float_leg_builder.add_spread(0.0);
    float_leg_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
    float_leg_builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    float_leg_builder.add_fixing_days(2);
    float_leg_builder.add_in_arrears(false);
    auto float_leg = float_leg_builder.Finish();

    // Underlying swap (Payer = right to pay fixed)
    auto swap_builder = quantra::VanillaSwapBuilder(*builder);
    swap_builder.add_swap_type(quantra::SwapType_Payer);
    swap_builder.add_fixed_leg(fixed_leg);
    swap_builder.add_floating_leg(float_leg);
    auto underlying_swap = swap_builder.Finish();

    // =========================================================================
    // Create the Swaption (1Y expiry into 5Y swap)
    // =========================================================================
    auto exercise_date = builder->CreateString("2026/01/27");
    
    auto swaption_builder = quantra::SwaptionBuilder(*builder);
    swaption_builder.add_exercise_type(quantra::ExerciseType_European);
    swaption_builder.add_settlement_type(quantra::SettlementType_Physical);
    swaption_builder.add_exercise_date(exercise_date);
    swaption_builder.add_underlying_swap(underlying_swap);
    auto swaption = swaption_builder.Finish();

    // =========================================================================
    // Create Volatility (20% Black vol)
    // =========================================================================
    auto vol_id = builder->CreateString("swaption_vol");
    auto vol_ref_date = builder->CreateString("2025/01/29");
    
    auto vol_builder = quantra::VolatilityTermStructureBuilder(*builder);
    vol_builder.add_id(vol_id);
    vol_builder.add_reference_date(vol_ref_date);
    vol_builder.add_calendar(calendar);
    vol_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    vol_builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    vol_builder.add_volatility_type(quantra::VolatilityType_ShiftedLognormal);
    vol_builder.add_constant_vol(0.20);
    auto volatility = vol_builder.Finish();

    // =========================================================================
    // Create PriceSwaption
    // =========================================================================
    auto disc_curve_ref = builder->CreateString("eur_curve");
    auto fwd_curve_ref = builder->CreateString("eur_curve");
    
    auto price_swaption_builder = quantra::PriceSwaptionBuilder(*builder);
    price_swaption_builder.add_swaption(swaption);
    price_swaption_builder.add_discounting_curve(disc_curve_ref);
    price_swaption_builder.add_forwarding_curve(fwd_curve_ref);
    price_swaption_builder.add_volatility(volatility);
    auto price_swaption = price_swaption_builder.Finish();

    std::vector<flatbuffers::Offset<quantra::PriceSwaption>> swaptions_vector;
    swaptions_vector.push_back(price_swaption);
    auto swaptions = builder->CreateVector(swaptions_vector);

    // =========================================================================
    // Create the final request
    // =========================================================================
    auto request_builder = quantra::PriceSwaptionRequestBuilder(*builder);
    request_builder.add_pricing(pricing);
    request_builder.add_swaptions(swaptions);
    auto request = request_builder.Finish();
    
    builder->Finish(request);

    auto p = builder->GetBufferPointer();
    auto size = builder->GetSize();

    ::grpc::Slice slice((char *)p, size);
    flatbuffers::grpc::Message<quantra::PriceSwaptionRequest> msg(slice);
    std::cout << "Verify:" << msg.Verify() << std::endl;

    return msg;
}
