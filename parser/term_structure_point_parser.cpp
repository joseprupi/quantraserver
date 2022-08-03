#include "term_structure_point_parser.h"

std::shared_ptr<RateHelper> TermStructurePointParser::parse(uint8_t point_type, const void *data)
{

    if (point_type == quantra::Point_DepositHelper)
    {
        auto point = static_cast<const DepositHelper *>(data);

        return std::make_shared<DepositRateHelper>(
            point->rate(),
            point->tenor_number() * TimeUnitToQL(point->tenor_time_unit()),
            point->fixing_days(),
            CalendarToQL(point->calendar()),
            ConventionToQL(point->business_day_convention()),
            true,
            DayCounterToQL(point->day_counter()));
    }
    else if (point_type == Point_FRAHelper)
    {
        auto point = static_cast<const FRAHelper *>(data);

        return std::make_shared<FraRateHelper>(
            point->rate(),
            point->months_to_start(),
            point->months_to_end(),
            point->fixing_days(),
            CalendarToQL(point->calendar()),
            ConventionToQL(point->business_day_convention()),
            true,
            DayCounterToQL(point->day_counter()));
    }
    else if (point_type == Point_FutureHelper)
    {
        auto point = static_cast<const FutureHelper *>(data);

        return std::make_shared<FuturesRateHelper>(
            point->rate(),
            DateToQL(point->future_start_date()->str()),
            point->future_months(),
            CalendarToQL(point->calendar()),
            ConventionToQL(point->business_day_convention()),
            true,
            DayCounterToQL(point->day_counter()));
    }
    else if (point_type == Point_SwapHelper)
    {
        auto point = static_cast<const quantra::SwapHelper *>(data);
        std::shared_ptr<Quote> spread(new SimpleQuote(point->spread()));

        return std::make_shared<SwapRateHelper>(
            point->rate(),
            point->tenor_number() * TimeUnitToQL(point->tenor_time_unit()),
            CalendarToQL(point->calendar()),
            FrequencyToQL(point->sw_fixed_leg_frequency()),
            ConventionToQL(point->sw_fixed_leg_convention()),
            DayCounterToQL(point->sw_fixed_leg_day_counter()),
            IborToQL(point->sw_floating_leg_index()),
            Handle<Quote>(spread), point->fwd_start_days() * Days);
    }

    else if (point_type == Point_BondHelper)
    {
        auto point = static_cast<const quantra::BondHelper *>(data);

        ScheduleParser schedule_parser = ScheduleParser();

        std::shared_ptr<SimpleQuote> rate(new SimpleQuote(point->rate()));
        RelinkableHandle<Quote> quote;
        quote.linkTo(rate);

        return std::make_shared<FixedRateBondHelper>(
            quote,
            point->settlement_days(),
            point->face_amount(),
            *schedule_parser.parse(point->schedule()),
            std::vector<Rate>(1, point->coupon_rate()),
            DayCounterToQL(point->day_counter()),
            ConventionToQL(point->business_day_convention()),
            point->redemption(),
            DateToQL(point->issue_date()->str()));
    }

    return nullptr;
}