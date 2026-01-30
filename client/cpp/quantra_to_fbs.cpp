#include "quantra_to_fbs.h"

flatbuffers::Offset<quantra::Yield> yield_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Yield> yield)
{
    auto yield_builder = quantra::YieldBuilder(*builder);
    yield_builder.add_day_counter(yield->day_counter);
    yield_builder.add_compounding(yield->compounding);
    yield_builder.add_frequency(yield->frequency);
    return yield_builder.Finish();
}

flatbuffers::Offset<quantra::Pricing> pricing_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Pricing> pricing)
{
    std::vector<flatbuffers::Offset<quantra::TermStructure>> term_structures_vector;

    for (auto it = pricing->curves.begin(); it < pricing->curves.end(); it++)
    {
        auto term_structure = term_structure_to_fbs(builder, *it);
        term_structures_vector.push_back(term_structure);
    }

    auto term_structures = builder->CreateVector(term_structures_vector);

    auto as_of_date = builder->CreateString(pricing->as_of_date);
    auto settlement_date = builder->CreateString(pricing->settlement_date);
    auto pricing_builder = quantra::PricingBuilder(*builder);
    pricing_builder.add_as_of_date(as_of_date);
    pricing_builder.add_settlement_date(settlement_date);
    pricing_builder.add_curves(term_structures);
    pricing_builder.add_bond_pricing_details(pricing->bond_pricing_details);
    pricing_builder.add_bond_pricing_flows(pricing->bond_pricing_flows);
    return pricing_builder.Finish();
}

flatbuffers::Offset<quantra::DepositHelper> deposit_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::DepositHelper> deposit)
{
    auto deposit_builder = quantra::DepositHelperBuilder(*builder);
    deposit_builder.add_tenor_number(deposit->tenor_number);
    deposit_builder.add_tenor_time_unit(deposit->tenor_time_unit);
    deposit_builder.add_fixing_days(deposit->fixing_days);
    deposit_builder.add_calendar(deposit->calendar);
    deposit_builder.add_business_day_convention(deposit->business_day_convention);
    deposit_builder.add_day_counter(deposit->day_counter);
    return deposit_builder.Finish();
}

flatbuffers::Offset<quantra::Schedule> schedule_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Schedule> schedule)
{
    auto effective_date = builder->CreateString(schedule->effective_date);
    auto termination_date = builder->CreateString(schedule->termination_date);
    auto schedule_builder = quantra::ScheduleBuilder(*builder);
    schedule_builder.add_calendar(schedule->calendar);
    schedule_builder.add_effective_date(effective_date);
    schedule_builder.add_termination_date(termination_date);
    schedule_builder.add_convention(schedule->convention);
    schedule_builder.add_frequency(schedule->frequency);
    schedule_builder.add_termination_date_convention(schedule->termination_date_convention);
    schedule_builder.add_date_generation_rule(schedule->date_generation_rule);
    schedule_builder.add_end_of_month(schedule->end_of_mont);
    return schedule_builder.Finish();
}

flatbuffers::Offset<quantra::Index> index_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::Index> index)
{
    std::vector<flatbuffers::Offset<quantra::Fixing>> fixings_vector;

    for (auto it = index->fixings.begin(); it < index->fixings.end(); it++)
    {
        auto date = builder->CreateString(it->date);
        auto fixing_builder = quantra::FixingBuilder(*builder);
        fixing_builder.add_rate(it->rate);
        fixing_builder.add_date(date);
        auto fixing = fixing_builder.Finish();

        fixings_vector.push_back(fixing);
    }

    auto fixings = builder->CreateVector(fixings_vector);

    auto index_builder = quantra::IndexBuilder(*builder);
    index_builder.add_period_number(index->period_number);
    index_builder.add_period_time_unit(index->period_time_unit);
    index_builder.add_settlement_days(index->settlement_days);
    index_builder.add_calendar(index->calendar);
    index_builder.add_business_day_convention(index->business_day_convention);
    index_builder.add_end_of_month(index->end_of_month);
    index_builder.add_day_counter(index->day_counter);
    index_builder.add_fixings(fixings);

    return index_builder.Finish();
}

flatbuffers::Offset<quantra::PointsWrapper> deposit_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::DepositHelper> deposit_helper)
{
    auto deposit_builder = quantra::DepositHelperBuilder(*builder);
    deposit_builder.add_rate(deposit_helper->rate);
    deposit_builder.add_tenor_number(deposit_helper->tenor_number);
    deposit_builder.add_tenor_time_unit(deposit_helper->tenor_time_unit);
    deposit_builder.add_fixing_days(deposit_helper->fixing_days);
    deposit_builder.add_calendar(deposit_helper->calendar);
    deposit_builder.add_business_day_convention(deposit_helper->business_day_convention);
    deposit_builder.add_day_counter(deposit_helper->day_counter);
    auto deposit = deposit_builder.Finish();

    auto deposit_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    deposit_wrapper_builder.add_point_type(quantra::Point_DepositHelper);
    deposit_wrapper_builder.add_point(deposit.Union());
    return deposit_wrapper_builder.Finish();
}

flatbuffers::Offset<quantra::PointsWrapper> FRAhelper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FRAHelper> FRA_helper)
{
    auto FRA_helper_builder = quantra::FRAHelperBuilder(*builder);
    FRA_helper_builder.add_rate(FRA_helper->rate);
    FRA_helper_builder.add_months_to_start(FRA_helper->months_to_start);
    FRA_helper_builder.add_months_to_end(FRA_helper->months_to_end);
    FRA_helper_builder.add_calendar(FRA_helper->calendar);
    FRA_helper_builder.add_business_day_convention(FRA_helper->business_day_convention);
    FRA_helper_builder.add_day_counter(FRA_helper->day_counter);
    auto FRA = FRA_helper_builder.Finish();

    auto FRA_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    FRA_wrapper_builder.add_point_type(quantra::Point_FRAHelper);
    FRA_wrapper_builder.add_point(FRA.Union());
    return FRA_wrapper_builder.Finish();
}

flatbuffers::Offset<quantra::PointsWrapper> future_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FutureHelper> future_helper)
{
    auto future_start_date = builder->CreateString(future_helper->future_start_date);

    auto future_helper_builder = quantra::FutureHelperBuilder(*builder);
    future_helper_builder.add_rate(future_helper->rate);
    future_helper_builder.add_future_start_date(future_start_date);
    future_helper_builder.add_future_months(future_helper->future_months);
    future_helper_builder.add_calendar(future_helper->calendar);
    future_helper_builder.add_business_day_convention(future_helper->business_day_convention);
    future_helper_builder.add_day_counter(future_helper->day_counter);
    auto future = future_helper_builder.Finish();

    auto future_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    future_wrapper_builder.add_point_type(quantra::Point_FutureHelper);
    future_wrapper_builder.add_point(future.Union());
    return future_wrapper_builder.Finish();
}

flatbuffers::Offset<quantra::PointsWrapper> swap_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::SwapHelper> swap_helper)
{
    auto swap_helper_builder = quantra::SwapHelperBuilder(*builder);

    swap_helper_builder.add_rate(swap_helper->rate);
    swap_helper_builder.add_tenor_number(swap_helper->tenor_number);
    swap_helper_builder.add_calendar(swap_helper->calendar);
    swap_helper_builder.add_sw_fixed_leg_frequency(swap_helper->sw_fixed_leg_frequency);
    swap_helper_builder.add_sw_fixed_leg_convention(swap_helper->sw_fixed_leg_convention);
    swap_helper_builder.add_sw_fixed_leg_day_counter(swap_helper->sw_fixed_leg_day_counter);
    swap_helper_builder.add_sw_floating_leg_index(swap_helper->sw_floating_leg_index);
    swap_helper_builder.add_spread(swap_helper->spread);
    swap_helper_builder.add_fwd_start_days(swap_helper->fwd_start_days);
    auto swap = swap_helper_builder.Finish();

    auto swap_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    swap_wrapper_builder.add_point_type(quantra::Point_SwapHelper);
    swap_wrapper_builder.add_point(swap.Union());
    return swap_wrapper_builder.Finish();
}

flatbuffers::Offset<quantra::PointsWrapper> bond_helper_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::BondHelper> bond_helper)
{
    flatbuffers::Offset<quantra::Schedule> schedule;

    if (bond_helper->schedule != NULL)
        schedule = schedule_to_fbs(builder, bond_helper->schedule);

    auto issue_date = builder->CreateString(bond_helper->issue_date);

    auto bond_helper_builder = quantra::BondHelperBuilder(*builder);

    bond_helper_builder.add_rate(bond_helper->rate);
    bond_helper_builder.add_settlement_days(bond_helper->settlement_days);
    bond_helper_builder.add_face_amount(bond_helper->face_amount);

    if (bond_helper->schedule != NULL)
        bond_helper_builder.add_schedule(schedule);

    bond_helper_builder.add_coupon_rate(bond_helper->coupon_rate);
    bond_helper_builder.add_day_counter(bond_helper->day_counter);
    bond_helper_builder.add_business_day_convention(bond_helper->business_day_convention);
    bond_helper_builder.add_redemption(bond_helper->redemption);
    bond_helper_builder.add_issue_date(issue_date);
    auto bond = bond_helper_builder.Finish();

    auto bond_wrapper_builder = quantra::PointsWrapperBuilder(*builder);
    bond_wrapper_builder.add_point_type(quantra::Point_BondHelper);
    bond_wrapper_builder.add_point(bond.Union());
    return bond_wrapper_builder.Finish();
}

flatbuffers::Offset<quantra::TermStructure> term_structure_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::TermStructure> term_structure)
{

    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;

    for (auto it = term_structure->points.begin(); it < term_structure->points.end(); it++)
    {
        switch ((*it)->point_type)
        {
        case Deposit:
        {
            auto deposit = deposit_helper_to_fbs(builder, (*it)->deposit_helper);
            points_vector.push_back(deposit);

            break;
        }
        case FRA:
        {
            auto FRA = FRAhelper_to_fbs(builder, (*it)->FRA_helper);
            points_vector.push_back(FRA);

            break;
        }
        case Future:
        {
            auto future = future_helper_to_fbs(builder, (*it)->future_helper);
            points_vector.push_back(future);

            break;
        }
        case Swap:
        {
            auto swap = swap_helper_to_fbs(builder, (*it)->swap_helper);
            points_vector.push_back(swap);

            break;
        }
        case Bond:
        {
            auto bond = bond_helper_to_fbs(builder, (*it)->bond_helper);
            points_vector.push_back(bond);

            break;
        }
        default:
            break;
        }
    }

    auto points = builder->CreateVector(points_vector);

    auto id = builder->CreateString(term_structure->id);
    auto reference_date = builder->CreateString(term_structure->reference_date);
    auto term_structure_builder = quantra::TermStructureBuilder(*builder);
    term_structure_builder.add_bootstrap_trait(term_structure->bootstrap_trait);
    term_structure_builder.add_interpolator(term_structure->interpolator);
    term_structure_builder.add_id(id);
    term_structure_builder.add_day_counter(term_structure->day_counter);
    term_structure_builder.add_reference_date(reference_date);
    term_structure_builder.add_points(points);
    return term_structure_builder.Finish();
}

flatbuffers::Offset<quantra::FixedRateBond> fixed_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FixedRateBond> fixed_rate_bond)
{
    flatbuffers::Offset<quantra::Schedule> schedule;

    if (fixed_rate_bond->schedule != NULL)
        schedule = schedule_to_fbs(builder, fixed_rate_bond->schedule);

    auto issue_date = builder->CreateString(fixed_rate_bond->issue_date);
    auto fixed_rate_bond_builder = quantra::FixedRateBondBuilder(*builder);
    fixed_rate_bond_builder.add_settlement_days(fixed_rate_bond->settlement_days);
    fixed_rate_bond_builder.add_face_amount(fixed_rate_bond->face_amount);
    fixed_rate_bond_builder.add_rate(fixed_rate_bond->rate);
    fixed_rate_bond_builder.add_accrual_day_counter(fixed_rate_bond->accrual_day_counter);
    fixed_rate_bond_builder.add_payment_convention(fixed_rate_bond->payment_convention);
    fixed_rate_bond_builder.add_redemption(fixed_rate_bond->redemption);
    fixed_rate_bond_builder.add_issue_date(issue_date);

    if (fixed_rate_bond->schedule != nullptr)
        fixed_rate_bond_builder.add_schedule(schedule);

    return fixed_rate_bond_builder.Finish();
}
flatbuffers::Offset<quantra::PriceFixedRateBond> price_fixed_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFixedRateBond> price_bond)
{
    flatbuffers::Offset<quantra::FixedRateBond> fixed_rate_bond;

    if (price_bond->fixed_rate_bond != NULL)
        fixed_rate_bond = fixed_rate_bond_to_fbs(builder, price_bond->fixed_rate_bond);

    auto discounting_curve = builder->CreateString(price_bond->discounting_curve);

    flatbuffers::Offset<quantra::Yield> yield;

    if (price_bond->yield != NULL)
        yield = yield_to_fbs(builder, price_bond->yield);

    auto price_fixed_rate_bond_builder = quantra::PriceFixedRateBondBuilder(*builder);

    if (price_bond->fixed_rate_bond != NULL)
        price_fixed_rate_bond_builder.add_fixed_rate_bond(fixed_rate_bond);

    price_fixed_rate_bond_builder.add_discounting_curve(discounting_curve);

    if (price_bond->yield != NULL)
        price_fixed_rate_bond_builder.add_yield(yield);

    return price_fixed_rate_bond_builder.Finish();
}
flatbuffers::Offset<quantra::PriceFixedRateBondRequest> price_fixed_rate_bond_request_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFixedRateBondRequest> price_bond)
{
    flatbuffers::Offset<quantra::Pricing> pricing;

    if (price_bond->pricing != nullptr)
        pricing = pricing_to_fbs(builder, price_bond->pricing);

    std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>> price_fixed_rate_bond_vector;

    for (auto it = price_bond->bonds.begin(); it < price_bond->bonds.end(); it++)
    {
        auto price_fixed_rate_bond = price_fixed_rate_bond_to_fbs(builder, *it);
        price_fixed_rate_bond_vector.push_back(price_fixed_rate_bond);
    }

    auto bonds = builder->CreateVector(price_fixed_rate_bond_vector);
    auto price_fixed_rate_bond_request_builder = quantra::PriceFixedRateBondRequestBuilder(*builder);

    if (price_bond->pricing != NULL)
        price_fixed_rate_bond_request_builder.add_pricing(pricing);

    price_fixed_rate_bond_request_builder.add_bonds(bonds);

    return price_fixed_rate_bond_request_builder.Finish();
}

flatbuffers::Offset<quantra::FloatingRateBond> floating_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::FloatingRateBond> floating_rate_bond)
{

    flatbuffers::Offset<quantra::Schedule> schedule;

    if (floating_rate_bond->schedule != nullptr)
        schedule = schedule_to_fbs(builder, floating_rate_bond->schedule);

    flatbuffers::Offset<quantra::Index> index;

    if (floating_rate_bond->index != NULL)
        index = index_to_fbs(builder, floating_rate_bond->index);

    auto issue_date = builder->CreateString(floating_rate_bond->issue_date);
    auto floating_rate_bond_builder = quantra::FloatingRateBondBuilder(*builder);
    floating_rate_bond_builder.add_settlement_days(floating_rate_bond->settlement_days);
    floating_rate_bond_builder.add_face_amount(floating_rate_bond->face_amount);

    if (floating_rate_bond->schedule != NULL)
        floating_rate_bond_builder.add_schedule(schedule);

    if (floating_rate_bond->index != NULL)
        floating_rate_bond_builder.add_index(index);

    floating_rate_bond_builder.add_accrual_day_counter(floating_rate_bond->accrual_day_counter);
    floating_rate_bond_builder.add_payment_convention(floating_rate_bond->payment_convention);
    floating_rate_bond_builder.add_fixing_days(floating_rate_bond->fixing_days);
    floating_rate_bond_builder.add_spread(floating_rate_bond->spread);
    floating_rate_bond_builder.add_in_arrears(floating_rate_bond->in_arrears);
    floating_rate_bond_builder.add_redemption(floating_rate_bond->redemption);
    floating_rate_bond_builder.add_issue_date(issue_date);

    return floating_rate_bond_builder.Finish();
}

flatbuffers::Offset<quantra::PriceFloatingRateBond> price_floating_rate_bond_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFloatingRateBond> price_bond)
{
    flatbuffers::Offset<quantra::FloatingRateBond> floating_rate_bond;

    if (price_bond->floating_rate_bond != NULL)
        floating_rate_bond = floating_rate_bond_to_fbs(builder, price_bond->floating_rate_bond);

    auto discounting_curve = builder->CreateString(price_bond->discounting_curve);
    auto forecasting_curve = builder->CreateString(price_bond->forecasting_curve);

    flatbuffers::Offset<quantra::Yield> yield;

    if (price_bond->yield != NULL)
        yield = yield_to_fbs(builder, price_bond->yield);

    auto price_floating_rate_bond_builder = quantra::PriceFloatingRateBondBuilder(*builder);

    if (price_bond->floating_rate_bond != NULL)
        price_floating_rate_bond_builder.add_floating_rate_bond(floating_rate_bond);

    price_floating_rate_bond_builder.add_discounting_curve(discounting_curve);
    price_floating_rate_bond_builder.add_forecasting_curve(forecasting_curve);

    if (price_bond->yield != NULL)
        price_floating_rate_bond_builder.add_yield(yield);

    return price_floating_rate_bond_builder.Finish();
}

flatbuffers::Offset<quantra::PriceFloatingRateBondRequest> price_floating_rate_bond_request_to_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, const std::shared_ptr<const structs::PriceFloatingRateBondRequest> price_bond)
{
    flatbuffers::Offset<quantra::Pricing> pricing;

    if (price_bond->pricing != nullptr)
        pricing = pricing_to_fbs(builder, price_bond->pricing);

    std::vector<flatbuffers::Offset<quantra::PriceFloatingRateBond>> price_floating_rate_bond_vector;

    for (auto it = price_bond->bonds.begin(); it < price_bond->bonds.end(); it++)
    {
        auto price_floating_rate_bond = price_floating_rate_bond_to_fbs(builder, *it);
        price_floating_rate_bond_vector.push_back(price_floating_rate_bond);
    }

    auto bonds = builder->CreateVector(price_floating_rate_bond_vector);
    auto price_floating_rate_bond_request_builder = quantra::PriceFloatingRateBondRequestBuilder(*builder);

    if (price_bond->pricing != NULL)
        price_floating_rate_bond_request_builder.add_pricing(pricing);

    price_floating_rate_bond_request_builder.add_bonds(bonds);

    return price_floating_rate_bond_request_builder.Finish();
}