#include "fra_request_fbs.h"

flatbuffers::grpc::Message<quantra::PriceFRARequest> fra_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder)
{
    // =========================================================================
    // Settings
    // =========================================================================
    auto calendar = quantra::enums::Calendar_TARGET;
    int fixing_days = 2;

    // =========================================================================
    // Build the yield curve (EUR curve)
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
    // Create the FRA (3x6 FRA - starts in 3 months, ends in 6 months)
    // Strike at 3.5%, Long position (benefit if rates rise)
    // =========================================================================

    // Index (3M Euribor)
    auto index_builder = quantra::IndexBuilder(*builder);
    index_builder.add_period_number(3);
    index_builder.add_period_time_unit(quantra::enums::TimeUnit_Months);
    index_builder.add_settlement_days(2);
    index_builder.add_calendar(calendar);
    index_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    index_builder.add_end_of_month(false);
    index_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
    auto index = index_builder.Finish();

    // FRA
    auto start_date = builder->CreateString("2025/04/29");   // 3 months from now
    auto maturity_date = builder->CreateString("2025/07/29"); // 6 months from now
    
    auto fra_builder = quantra::FRABuilder(*builder);
    fra_builder.add_fra_type(quantra::FRAType_Long);
    fra_builder.add_notional(10000000.0);  // 10 million EUR
    fra_builder.add_start_date(start_date);
    fra_builder.add_maturity_date(maturity_date);
    fra_builder.add_strike(0.035);         // 3.5% strike
    fra_builder.add_index(index);
    fra_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
    fra_builder.add_calendar(calendar);
    fra_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fra = fra_builder.Finish();

    // =========================================================================
    // Create PriceFRA
    // =========================================================================
    auto disc_curve_ref = builder->CreateString("eur_curve");
    auto fwd_curve_ref = builder->CreateString("eur_curve");
    
    auto price_fra_builder = quantra::PriceFRABuilder(*builder);
    price_fra_builder.add_fra(fra);
    price_fra_builder.add_discounting_curve(disc_curve_ref);
    price_fra_builder.add_forwarding_curve(fwd_curve_ref);
    auto price_fra = price_fra_builder.Finish();

    std::vector<flatbuffers::Offset<quantra::PriceFRA>> fras_vector;
    fras_vector.push_back(price_fra);
    auto fras = builder->CreateVector(fras_vector);

    // =========================================================================
    // Create the final request
    // =========================================================================
    auto request_builder = quantra::PriceFRARequestBuilder(*builder);
    request_builder.add_pricing(pricing);
    request_builder.add_fras(fras);
    auto request = request_builder.Finish();
    
    builder->Finish(request);

    auto p = builder->GetBufferPointer();
    auto size = builder->GetSize();

    ::grpc::Slice slice((char *)p, size);
    flatbuffers::grpc::Message<quantra::PriceFRARequest> msg(slice);
    std::cout << "Verify:" << msg.Verify() << std::endl;

    return msg;
}
