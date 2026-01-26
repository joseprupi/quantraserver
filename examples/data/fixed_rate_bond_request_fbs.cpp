#include "fixed_rate_bond_request_fbs.h"

flatbuffers::grpc::Message<quantra::PriceFixedRateBondRequest> bond_request_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder)
{

    // Settings
    auto calendar = quantra::enums::Calendar_TARGET;
    auto settlement_date = builder->CreateString("2008/09/18");
    auto fixing_days = 3;
    auto settlement_days = 3;

    std::vector<flatbuffers::Offset<PointsWrapper>> points_vector;

    // Create deposit
    auto deposit_helper_zc3m_builder = quantra::DepositHelperBuilder(*builder);
    deposit_helper_zc3m_builder.add_rate(0.0096);
    deposit_helper_zc3m_builder.add_tenor_number(3);
    deposit_helper_zc3m_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
    deposit_helper_zc3m_builder.add_fixing_days(fixing_days);
    deposit_helper_zc3m_builder.add_calendar(calendar);
    deposit_helper_zc3m_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    deposit_helper_zc3m_builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    auto deposit_helper_zc3m = deposit_helper_zc3m_builder.Finish();

    auto deposit_helper_zc3m_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    deposit_helper_zc3m_wrapper_builder.add_point_type(quantra::Point_DepositHelper);
    deposit_helper_zc3m_wrapper_builder.add_point(deposit_helper_zc3m.Union());
    auto deposit_helper_zc3m_wrapper = deposit_helper_zc3m_wrapper_builder.Finish();

    points_vector.push_back(deposit_helper_zc3m_wrapper);

    // Create deposit
    auto deposit_helper_zc6m_builder = quantra::DepositHelperBuilder(*builder);
    deposit_helper_zc6m_builder.add_rate(0.0145);
    deposit_helper_zc6m_builder.add_tenor_number(6);
    deposit_helper_zc6m_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
    deposit_helper_zc6m_builder.add_fixing_days(fixing_days);
    deposit_helper_zc6m_builder.add_calendar(calendar);
    deposit_helper_zc6m_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    deposit_helper_zc6m_builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    auto deposit_helper_zc6m = deposit_helper_zc6m_builder.Finish();

    auto deposit_helper_zc6m_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    deposit_helper_zc6m_wrapper_builder.add_point_type(quantra::Point_DepositHelper);
    deposit_helper_zc6m_wrapper_builder.add_point(deposit_helper_zc6m.Union());
    auto deposit_helper_zc6m_wrapper = deposit_helper_zc6m_wrapper_builder.Finish();

    points_vector.push_back(deposit_helper_zc6m_wrapper);

    // Create deposit
    auto deposit_helper_zc1y_builder = quantra::DepositHelperBuilder(*builder);
    deposit_helper_zc1y_builder.add_rate(0.0194);
    deposit_helper_zc1y_builder.add_tenor_number(1);
    deposit_helper_zc1y_builder.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
    deposit_helper_zc1y_builder.add_fixing_days(fixing_days);
    deposit_helper_zc1y_builder.add_calendar(calendar);
    deposit_helper_zc1y_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    deposit_helper_zc1y_builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    auto deposit_helper_zc1y = deposit_helper_zc1y_builder.Finish();

    auto deposit_helper_zc1y_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    deposit_helper_zc1y_wrapper_builder.add_point_type(quantra::Point_DepositHelper);
    deposit_helper_zc1y_wrapper_builder.add_point(deposit_helper_zc1y.Union());
    auto deposit_helper_zc1y_wrapper = deposit_helper_zc1y_wrapper_builder.Finish();

    points_vector.push_back(deposit_helper_zc1y_wrapper);

    float redemption = 100.0;

    int number_of_bonds = 5;

    std::string issue_dates[] = {
        "2005/03/15",
        "2005/06/15",
        "2006/06/30",
        "2002/11/15",
        "1987/05/15"};

    std::string maturities[] = {
        "2010/08/31",
        "2011/08/31",
        "2013/08/31",
        "2018/08/15",
        "2038/05/15"};

    float coupon_rates[] = {
        0.02375,
        0.04625,
        0.03125,
        0.04000,
        0.04500};

    float market_quotes[] = {
        100.390625,
        106.21875,
        100.59375,
        101.6875,
        102.140625};

    for (int i = 0; i < number_of_bonds; i++)
    {
        // Create schedule for bond helper
        auto effective_date = builder->CreateString(issue_dates[i]);
        auto termination_date = builder->CreateString(maturities[i]);
        auto schedule_builder = quantra::ScheduleBuilder(*builder);
        schedule_builder.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
        schedule_builder.add_effective_date(effective_date);
        schedule_builder.add_termination_date(termination_date);
        schedule_builder.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        schedule_builder.add_frequency(quantra::enums::Frequency_Semiannual);
        schedule_builder.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        schedule_builder.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
        schedule_builder.add_end_of_month(false);
        auto schedule = schedule_builder.Finish();

        // Create bond helper
        auto bond_helper_builder = quantra::BondHelperBuilder(*builder);
        bond_helper_builder.add_rate(market_quotes[i]);
        bond_helper_builder.add_settlement_days(settlement_days);
        bond_helper_builder.add_face_amount(100.0);
        bond_helper_builder.add_schedule(schedule);
        bond_helper_builder.add_coupon_rate(coupon_rates[i]);
        bond_helper_builder.add_day_counter(quantra::enums::DayCounter_ActualActual365);
        bond_helper_builder.add_business_day_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        bond_helper_builder.add_redemption(redemption);
        bond_helper_builder.add_issue_date(effective_date);
        auto bond_helper = bond_helper_builder.Finish();

        auto point_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
        point_wrapper_builder.add_point_type(quantra::Point_BondHelper);
        point_wrapper_builder.add_point(bond_helper.Union());
        auto point_wrapper = point_wrapper_builder.Finish();

        points_vector.push_back(point_wrapper);
    }

    auto points = builder->CreateVector(points_vector);

    // Create the termstructure curve
    auto id = builder->CreateString("depos_curve");
    auto term_structure_builder = quantra::TermStructureBuilder(*builder);
    term_structure_builder.add_bootstrap_trait(quantra::enums::BootstrapTrait_Discount);
    term_structure_builder.add_interpolator(quantra::enums::Interpolator_LogLinear);
    term_structure_builder.add_id(id);
    term_structure_builder.add_day_counter(quantra::enums::DayCounter_ActualActual365);
    term_structure_builder.add_reference_date(settlement_date);
    term_structure_builder.add_points(points);
    auto term_structure = term_structure_builder.Finish();

    // *****************************************************
    // Create the pricing information. Curves and as of date
    // *****************************************************

    std::vector<flatbuffers::Offset<quantra::TermStructure>> term_structures_vector;
    term_structures_vector.push_back(term_structure);
    auto term_structures = builder->CreateVector(term_structures_vector);

    auto as_of_date = builder->CreateString("2008/09/16");
    auto settlement_date_pricing = builder->CreateString("2008/09/18");
    auto pricing_builder = quantra::PricingBuilder(*builder);
    pricing_builder.add_as_of_date(as_of_date);
    pricing_builder.add_settlement_date(settlement_date_pricing);
    pricing_builder.add_curves(term_structures);
    auto pricing = pricing_builder.Finish();

    // **************************
    // Create the Fixed Rate Bond
    // **************************

    // Create the Schedule table

    auto effective_date = builder->CreateString("2007/05/15");
    auto termination_date = builder->CreateString("2017/05/15");
    auto schedule_builder = quantra::ScheduleBuilder(*builder);
    schedule_builder.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    schedule_builder.add_effective_date(effective_date);
    schedule_builder.add_termination_date(termination_date);
    schedule_builder.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    schedule_builder.add_frequency(quantra::enums::Frequency_Semiannual);
    schedule_builder.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    schedule_builder.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    schedule_builder.add_end_of_month(false);
    auto schedule = schedule_builder.Finish();

    // Create the fixed rate bond
    auto issue_date = builder->CreateString("2007/05/15");
    auto fixed_rate_bond_builder = quantra::FixedRateBondBuilder(*builder);
    fixed_rate_bond_builder.add_settlement_days(settlement_days);
    fixed_rate_bond_builder.add_face_amount(100);
    fixed_rate_bond_builder.add_schedule(schedule);
    fixed_rate_bond_builder.add_rate(0.045);
    fixed_rate_bond_builder.add_accrual_day_counter(quantra::enums::DayCounter_ActualActualBond);
    fixed_rate_bond_builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixed_rate_bond_builder.add_redemption(100);
    fixed_rate_bond_builder.add_issue_date(issue_date);
    auto bond = fixed_rate_bond_builder.Finish();

    // ***********************************
    // Create the PriceFixedRateBond table
    // ***********************************

    // Create the Yield table
    auto yield_builder = quantra::YieldBuilder(*builder);
    yield_builder.add_day_counter(quantra::enums::DayCounter_Actual360);
    yield_builder.add_compounding(quantra::enums::Compounding_Compounded);
    yield_builder.add_frequency(quantra::enums::Frequency_Annual);
    auto yield = yield_builder.Finish();

    // Create the BondPricing table
    auto curve_id = builder->CreateString("depos_curve");
    auto bond_pricing_builder = quantra::PriceFixedRateBondBuilder(*builder);
    bond_pricing_builder.add_fixed_rate_bond(bond);
    bond_pricing_builder.add_discounting_curve(curve_id);
    bond_pricing_builder.add_yield(yield);
    auto bond_pricing = bond_pricing_builder.Finish();

    std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>> bonds_vector_vector;
    bonds_vector_vector.push_back(bond_pricing);
    auto bonds_vector = builder->CreateVector(bonds_vector_vector);

    // ******************************************
    // Create the PriceFixedRateBondRequest table
    // ******************************************

    auto bond_request_builder = quantra::PriceFixedRateBondRequestBuilder(*builder);
    bond_request_builder.add_pricing(pricing);
    bond_request_builder.add_bonds(bonds_vector);
    auto bond_request = bond_request_builder.Finish();
    builder->Finish(bond_request);

    auto p = builder->GetBufferPointer();
    auto size = builder->GetSize();

    //::grpc::Slice slice = grpc_slice_from_copied_buffer((char *)p, size);
    ::grpc::Slice slice((char *)p, size);
    flatbuffers::grpc::Message<quantra::PriceFixedRateBondRequest> msg(slice);
    std::cout << "Verify:" << msg.Verify() << std::endl;

    return msg;
}