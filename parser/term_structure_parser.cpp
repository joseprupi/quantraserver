#include "term_structure_parser.h"

using namespace quantra;

std::shared_ptr<YieldTermStructure> TermStructureParser::parse(const quantra::TermStructure *ts)
{

    if (ts == NULL)
        QUANTRA_ERROR("Fixed Rate Bond not found");

    auto points = ts->points();

    if (points->size() == 0)
        QUANTRA_ERROR("Empty list of points for term structure");

    TermStructurePointParser tsparser = TermStructurePointParser();
    std::vector<std::shared_ptr<RateHelper>> instruments;

    for (int i = 0; i < points->size(); i++)
    {
        auto point = points->Get(i)->point();
        auto type = points->Get(i)->point_type();
        std::shared_ptr<RateHelper> rate_helper = tsparser.parse(type, point);
        instruments.push_back(rate_helper);
    }

    //std::shared_ptr<YieldTermStructure> termStructure;
    double tolerance = 1.0e-15;

    switch (ts->interpolator())
    {
    case enums::Interpolator_BackwardFlat:
        switch (ts->bootstrap_trait())
        {

        case enums::BootstrapTrait_Discount:

            return std::make_shared<PiecewiseYieldCurve<Discount, BackwardFlat>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<Discount, BackwardFlat>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_ZeroRate:

            return std::make_shared<PiecewiseYieldCurve<ZeroYield, BackwardFlat>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ZeroYield, BackwardFlat>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_FwdRate:

            return std::make_shared<PiecewiseYieldCurve<ForwardRate, BackwardFlat>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ForwardRate, BackwardFlat>::bootstrap_type(tolerance));

            break;
        }
        break;

    case enums::Interpolator_ForwardFlat:
        switch (ts->bootstrap_trait())
        {

        case enums::BootstrapTrait_Discount:

            return std::make_shared<PiecewiseYieldCurve<Discount, ForwardFlat>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<Discount, ForwardFlat>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_ZeroRate:

            return std::make_shared<PiecewiseYieldCurve<ZeroYield, ForwardFlat>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ZeroYield, ForwardFlat>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_FwdRate:

            return std::make_shared<PiecewiseYieldCurve<ForwardRate, ForwardFlat>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ForwardRate, ForwardFlat>::bootstrap_type(tolerance));

            break;
        }
        break;

    case enums::Interpolator_Linear:
        switch (ts->bootstrap_trait())
        {

        case enums::BootstrapTrait_Discount:

            return std::make_shared<PiecewiseYieldCurve<Discount, Linear>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<Discount, Linear>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_ZeroRate:

            return std::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ZeroYield, Linear>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_FwdRate:

            return std::make_shared<PiecewiseYieldCurve<ForwardRate, Linear>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ForwardRate, Linear>::bootstrap_type(tolerance));

            break;
        }
        break;

    case enums::Interpolator_LogLinear:
        switch (ts->bootstrap_trait())
        {

        case enums::BootstrapTrait_Discount:

            return std::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<Discount, LogLinear>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_ZeroRate:

            return std::make_shared<PiecewiseYieldCurve<ZeroYield, LogLinear>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ZeroYield, LogLinear>::bootstrap_type(tolerance));

            break;

        case enums::BootstrapTrait_FwdRate:

            return std::make_shared<PiecewiseYieldCurve<ForwardRate, LogLinear>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                PiecewiseYieldCurve<ForwardRate, LogLinear>::bootstrap_type(tolerance));

            break;
        }
        break;

    case enums::Interpolator_LogCubic:

        switch (ts->bootstrap_trait())
        {

        case enums::BootstrapTrait_Discount:

            return std::make_shared<PiecewiseYieldCurve<Discount, LogCubic>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                MonotonicLogCubic());

            break;

        case enums::BootstrapTrait_ZeroRate:

            return std::make_shared<PiecewiseYieldCurve<ZeroYield, LogCubic>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                MonotonicLogCubic());

            break;

        case enums::BootstrapTrait_FwdRate:

            return std::make_shared<PiecewiseYieldCurve<ForwardRate, LogCubic>>(
                DateToQL(ts->reference_date()->str()), instruments,
                DayCounterToQL(ts->day_counter()),
                MonotonicLogCubic());
            break;
        }
        break;
    }

    return nullptr;
}