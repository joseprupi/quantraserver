#include "bootstrap_curves_request.h"
#include <algorithm>
#include <set>

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<BootstrapCurvesResponse> BootstrapCurvesRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const BootstrapCurvesRequest *request) const
{
    // Set evaluation date
    Date asOfDate = DateToQL(request->as_of_date()->str());
    Settings::instance().evaluationDate() = asOfDate;

    std::vector<flatbuffers::Offset<BootstrapCurveResult>> results;

    // Process each curve
    auto curveSpecs = request->curves();
    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++)
    {
        auto spec = curveSpecs->Get(i);
        auto termStructure = spec->curve();
        std::string curveId = termStructure->id() ? termStructure->id()->str() : "";

        try
        {
            // Bootstrap the curve using existing parser
            TermStructureParser tsParser;
            auto curve = tsParser.parse(termStructure);
            curve->enableExtrapolation();

            Date referenceDate = curve->referenceDate();
            
            // Get fallback calendar from curve's instruments
            Calendar curveCalendar = getCalendarFromTermStructure(termStructure);

            // Build grid dates
            std::vector<Date> gridDates;
            Calendar gridCalendar = curveCalendar;  // Default to curve's calendar
            BusinessDayConvention gridBdc = Following;

            auto query = spec->query();
            if (query && query->grid() && query->grid()->grid_type() != CurveGrid_NONE)
            {
                auto gridSpec = query->grid();
                
                // Extract calendar/bdc from grid
                gridCalendar = getCalendarFromGrid(gridSpec, curveCalendar);
                gridBdc = getBdcFromGrid(gridSpec);
                
                if (gridSpec->grid_type() == CurveGrid_TenorGrid)
                {
                    auto tenorGrid = gridSpec->grid_as_TenorGrid();
                    gridDates = buildTenorGrid(tenorGrid, referenceDate, curveCalendar);
                }
                else if (gridSpec->grid_type() == CurveGrid_RangeGrid)
                {
                    auto rangeGrid = gridSpec->grid_as_RangeGrid();
                    gridDates = buildRangeGrid(rangeGrid, asOfDate);
                }
            }

            // Convert grid dates to strings
            std::vector<flatbuffers::Offset<flatbuffers::String>> gridDateStrings;
            for (const auto &d : gridDates)
            {
                std::ostringstream os;
                os << io::iso_date(d);
                gridDateStrings.push_back(builder->CreateString(os.str()));
            }

            // Compute requested measures
            std::vector<flatbuffers::Offset<CurveSeries>> seriesVector;

            if (query && query->measures())
            {
                auto measures = query->measures();
                for (flatbuffers::uoffset_t m = 0; m < measures->size(); m++)
                {
                    CurveMeasure measure = static_cast<CurveMeasure>(measures->Get(m));
                    std::vector<double> values;

                    switch (measure)
                    {
                    case CurveMeasure_DF:
                        values = computeDiscountFactors(curve, gridDates);
                        break;
                    case CurveMeasure_ZERO:
                        values = computeZeroRates(curve, gridDates, query->zero());
                        break;
                    case CurveMeasure_FWD:
                        values = computeForwardRates(curve, gridDates, query->fwd(), 
                                                     gridCalendar, gridBdc, curveCalendar);
                        break;
                    }

                    auto valuesVec = builder->CreateVector(values);
                    CurveSeriesBuilder seriesBuilder(*builder);
                    seriesBuilder.add_measure(measure);
                    seriesBuilder.add_values(valuesVec);
                    seriesVector.push_back(seriesBuilder.Finish());
                }
            }

            // Extract pillar dates from helpers (always works, no type checks needed)
            std::vector<Date> pillarDates = extractPillarDatesFromHelpers(termStructure, referenceDate);
            std::vector<flatbuffers::Offset<flatbuffers::String>> pillarDateStrings;
            for (const auto &d : pillarDates)
            {
                std::ostringstream os;
                os << io::iso_date(d);
                pillarDateStrings.push_back(builder->CreateString(os.str()));
            }

            // Build result for this curve
            auto idStr = builder->CreateString(curveId);
            std::ostringstream refDateOs;
            refDateOs << io::iso_date(referenceDate);
            auto refDateStr = builder->CreateString(refDateOs.str());
            auto gridDatesVec = builder->CreateVector(gridDateStrings);
            auto seriesVec = builder->CreateVector(seriesVector);
            auto pillarDatesVec = builder->CreateVector(pillarDateStrings);

            BootstrapCurveResultBuilder resultBuilder(*builder);
            resultBuilder.add_id(idStr);
            resultBuilder.add_reference_date(refDateStr);
            resultBuilder.add_grid_dates(gridDatesVec);
            resultBuilder.add_series(seriesVec);
            resultBuilder.add_pillar_dates(pillarDatesVec);

            results.push_back(resultBuilder.Finish());
        }
        catch (std::exception &e)
        {
            // Build error result for this curve
            auto idStr = builder->CreateString(curveId);
            auto errorMsg = builder->CreateString(e.what());

            ErrorBuilder errorBuilder(*builder);
            errorBuilder.add_error_message(errorMsg);
            auto error = errorBuilder.Finish();

            BootstrapCurveResultBuilder resultBuilder(*builder);
            resultBuilder.add_id(idStr);
            resultBuilder.add_error(error);

            results.push_back(resultBuilder.Finish());
        }
    }

    // Build response
    auto resultsVec = builder->CreateVector(results);
    BootstrapCurvesResponseBuilder responseBuilder(*builder);
    responseBuilder.add_results(resultsVec);

    return responseBuilder.Finish();
}

Calendar BootstrapCurvesRequestHandler::getCalendarFromTermStructure(
    const quantra::TermStructure *termStructure) const
{
    // Try to extract calendar from first point
    if (termStructure->points() && termStructure->points()->size() > 0)
    {
        auto firstPoint = termStructure->points()->Get(0);
        if (firstPoint->point_as_DepositHelper())
        {
            return CalendarToQL(firstPoint->point_as_DepositHelper()->calendar());
        }
        else if (firstPoint->point_as_SwapHelper())
        {
            return CalendarToQL(firstPoint->point_as_SwapHelper()->calendar());
        }
        else if (firstPoint->point_as_FRAHelper())
        {
            return CalendarToQL(firstPoint->point_as_FRAHelper()->calendar());
        }
        else if (firstPoint->point_as_FutureHelper())
        {
            return CalendarToQL(firstPoint->point_as_FutureHelper()->calendar());
        }
    }
    return TARGET();
}

Calendar BootstrapCurvesRequestHandler::getCalendarFromGrid(
    const CurveGridSpec *gridSpec,
    const Calendar &fallbackCalendar) const
{
    if (!gridSpec) return fallbackCalendar;
    
    if (gridSpec->grid_type() == CurveGrid_TenorGrid)
    {
        auto grid = gridSpec->grid_as_TenorGrid();
        // Check if calendar is set (not NullCalendar)
        if (grid->calendar() != enums::Calendar_NullCalendar)
        {
            return CalendarToQL(grid->calendar());
        }
    }
    else if (gridSpec->grid_type() == CurveGrid_RangeGrid)
    {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (grid->calendar() != enums::Calendar_NullCalendar)
        {
            return CalendarToQL(grid->calendar());
        }
    }
    
    return fallbackCalendar;
}

BusinessDayConvention BootstrapCurvesRequestHandler::getBdcFromGrid(
    const CurveGridSpec *gridSpec) const
{
    if (!gridSpec) return Following;
    
    if (gridSpec->grid_type() == CurveGrid_TenorGrid)
    {
        auto grid = gridSpec->grid_as_TenorGrid();
        return ConventionToQL(grid->business_day_convention());
    }
    else if (gridSpec->grid_type() == CurveGrid_RangeGrid)
    {
        auto grid = gridSpec->grid_as_RangeGrid();
        return ConventionToQL(grid->business_day_convention());
    }
    
    return Following;
}

std::vector<Date> BootstrapCurvesRequestHandler::extractPillarDatesFromHelpers(
    const quantra::TermStructure *termStructure,
    const Date &referenceDate) const
{
    std::set<Date> dateSet;  // Use set for automatic sorting and uniqueness
    dateSet.insert(referenceDate);  // Always include reference date
    
    if (!termStructure->points()) return std::vector<Date>(dateSet.begin(), dateSet.end());
    
    auto points = termStructure->points();
    Calendar calendar = getCalendarFromTermStructure(termStructure);
    
    for (flatbuffers::uoffset_t i = 0; i < points->size(); i++)
    {
        auto point = points->Get(i);
        Date maturityDate;
        
        if (auto deposit = point->point_as_DepositHelper())
        {
            Period tenor(deposit->tenor_number(), TimeUnitToQL(deposit->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor, 
                                           ConventionToQL(deposit->business_day_convention()));
        }
        else if (auto swap = point->point_as_SwapHelper())
        {
            Period tenor(swap->tenor_number(), TimeUnitToQL(swap->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor,
                                           ConventionToQL(swap->sw_fixed_leg_convention()));
        }
        else if (auto fra = point->point_as_FRAHelper())
        {
            // FRA maturity is start + tenor
            Period startPeriod(fra->months_to_start(), Months);
            Period tenor(fra->months_to_end() - fra->months_to_start(), Months);
            Date startDate = calendar.advance(referenceDate, startPeriod);
            maturityDate = calendar.advance(startDate, tenor);
        }
        else if (auto future = point->point_as_FutureHelper())
        {
            // For futures, use future_start_date + future_months
            Date startDate = DateToQL(future->future_start_date()->str());
            maturityDate = calendar.advance(startDate, Period(future->future_months(), Months));
        }
        else if (auto bond = point->point_as_BondHelper())
        {
            // BondHelper has a schedule - get termination_date from it
            if (bond->schedule() && bond->schedule()->termination_date())
            {
                maturityDate = DateToQL(bond->schedule()->termination_date()->str());
            }
        }
        
        if (maturityDate != Date())
        {
            dateSet.insert(maturityDate);
        }
    }
    
    return std::vector<Date>(dateSet.begin(), dateSet.end());
}

std::vector<Date> BootstrapCurvesRequestHandler::buildTenorGrid(
    const TenorGrid *grid,
    const Date &referenceDate,
    const Calendar &fallbackCalendar) const
{
    std::vector<Date> dates;
    auto tenors = grid->tenors();
    
    // Determine calendar to use
    Calendar calendar = fallbackCalendar;
    BusinessDayConvention bdc = Following;
    bool useCalendar = false;
    
    if (grid->calendar() != enums::Calendar_NullCalendar)
    {
        calendar = CalendarToQL(grid->calendar());
        bdc = ConventionToQL(grid->business_day_convention());
        useCalendar = true;
    }

    for (flatbuffers::uoffset_t i = 0; i < tenors->size(); i++)
    {
        auto tenor = tenors->Get(i);
        int n = tenor->n();
        TimeUnit unit = TimeUnitToQL(tenor->unit());
        
        // Handle n=0 special case - returns reference date
        if (n == 0)
        {
            dates.push_back(referenceDate);
            continue;
        }
        
        Period period(n, unit);
        Date d;
        
        if (useCalendar)
        {
            // Use calendar.advance for business-day-adjusted dates
            d = calendar.advance(referenceDate, period, bdc);
        }
        else
        {
            // Raw date arithmetic (no business day adjustment)
            d = referenceDate + period;
        }
        
        dates.push_back(d);
    }

    return dates;
}

std::vector<Date> BootstrapCurvesRequestHandler::buildRangeGrid(
    const RangeGrid *grid,
    const Date &asOfDate) const
{
    std::vector<Date> dates;

    Date startDate = grid->start_date() ? DateToQL(grid->start_date()->str()) : asOfDate;
    Date endDate = DateToQL(grid->end_date()->str());

    int stepNumber = grid->step_number();
    TimeUnit stepUnit = TimeUnitToQL(grid->step_time_unit());
    Period step(stepNumber, stepUnit);

    bool businessDaysOnly = grid->business_days_only();
    Calendar calendar = CalendarToQL(grid->calendar());
    BusinessDayConvention bdc = ConventionToQL(grid->business_day_convention());
    
    // Check for misleading configuration
    bool isNullCalendar = (grid->calendar() == enums::Calendar_NullCalendar);
    if (businessDaysOnly && isNullCalendar)
    {
        // NullCalendar treats every day as a business day, so business_days_only
        // won't skip weekends. Use WeekendsOnly as a reasonable fallback.
        calendar = WeekendsOnly();
    }

    Date current = startDate;
    while (current <= endDate)
    {
        if (businessDaysOnly)
        {
            // Only add business days
            if (calendar.isBusinessDay(current))
            {
                dates.push_back(current);
            }
        }
        else
        {
            dates.push_back(current);
        }

        // Advance by step
        if (stepUnit == Days)
        {
            current = current + stepNumber;
        }
        else if (stepUnit == Weeks)
        {
            current = current + stepNumber * 7;
        }
        else
        {
            // For months/years, use calendar advance
            current = calendar.advance(current, step, bdc);
        }

        // Safety: prevent infinite loop or excessive grid size
        if (dates.size() > 50000)
        {
            QUANTRA_ERROR("Grid too large (>50000 points). Consider using a larger step or smaller date range.");
        }
    }

    return dates;
}

std::vector<double> BootstrapCurvesRequestHandler::computeDiscountFactors(
    const std::shared_ptr<YieldTermStructure> &curve,
    const std::vector<Date> &dates) const
{
    std::vector<double> values;
    values.reserve(dates.size());

    for (const auto &d : dates)
    {
        values.push_back(curve->discount(d));
    }

    return values;
}

std::vector<double> BootstrapCurvesRequestHandler::computeZeroRates(
    const std::shared_ptr<YieldTermStructure> &curve,
    const std::vector<Date> &dates,
    const ZeroRateQuery *zeroQuery) const
{
    // Defaults: continuous compounding, curve's day counter
    DayCounter dc = curve->dayCounter();
    Compounding comp = Continuous;
    Frequency freq = Annual;

    if (zeroQuery)
    {
        if (!zeroQuery->use_curve_day_counter())
        {
            dc = DayCounterToQL(zeroQuery->day_counter());
        }
        comp = CompoundingToQL(zeroQuery->compounding());
        freq = FrequencyToQL(zeroQuery->frequency());
    }

    std::vector<double> values;
    values.reserve(dates.size());

    Date refDate = curve->referenceDate();

    for (const auto &d : dates)
    {
        // For reference date or earlier, compute at refDate + 1 day to avoid division by zero
        // This gives a meaningful rate instead of 0 or NaN
        if (d <= refDate)
        {
            Date d1 = refDate + 1;
            InterestRate rate = curve->zeroRate(d1, dc, comp, freq);
            values.push_back(rate.rate());
        }
        else
        {
            InterestRate rate = curve->zeroRate(d, dc, comp, freq);
            values.push_back(rate.rate());
        }
    }

    return values;
}

std::vector<double> BootstrapCurvesRequestHandler::computeForwardRates(
    const std::shared_ptr<YieldTermStructure> &curve,
    const std::vector<Date> &dates,
    const ForwardRateQuery *fwdQuery,
    const Calendar &gridCalendar,
    BusinessDayConvention gridBdc,
    const Calendar &curveCalendar) const
{
    // Defaults: simple compounding, curve's day counter, instantaneous forward
    DayCounter dc = curve->dayCounter();
    Compounding comp = Simple;
    Frequency freq = Annual;
    ForwardType fwdType = ForwardType_Instantaneous;
    int epsNumber = 1;
    TimeUnit epsUnit = Days;
    int tenorNumber = 3;
    TimeUnit tenorUnit = Months;
    bool useGridCalendar = true;

    if (fwdQuery)
    {
        if (!fwdQuery->use_curve_day_counter())
        {
            dc = DayCounterToQL(fwdQuery->day_counter());
        }
        comp = CompoundingToQL(fwdQuery->compounding());
        freq = FrequencyToQL(fwdQuery->frequency());
        fwdType = fwdQuery->forward_type();
        epsNumber = fwdQuery->instantaneous_eps_number();
        epsUnit = TimeUnitToQL(fwdQuery->instantaneous_eps_time_unit());
        tenorNumber = fwdQuery->tenor_number();
        tenorUnit = TimeUnitToQL(fwdQuery->tenor_time_unit());
        useGridCalendar = fwdQuery->use_grid_calendar_for_advance();
    }

    // Choose calendar based on useGridCalendar flag
    Calendar calendar = useGridCalendar ? gridCalendar : curveCalendar;
    BusinessDayConvention bdc = useGridCalendar ? gridBdc : Following;

    std::vector<double> values;
    values.reserve(dates.size());

    for (const auto &d : dates)
    {
        Date endDate;
        
        if (fwdType == ForwardType_Instantaneous)
        {
            // Instantaneous forward: use small epsilon (default 1 day)
            // This approximates f(t) = -d/dt ln(P(t))
            if (epsUnit == Days)
            {
                endDate = d + epsNumber;
            }
            else
            {
                endDate = calendar.advance(d, Period(epsNumber, epsUnit), bdc);
            }
        }
        else
        {
            // Period forward: compute forward over [d, d + tenor]
            // This is the implied forward rate for the period (like 3M forward)
            endDate = calendar.advance(d, Period(tenorNumber, tenorUnit), bdc);
        }

        // Make sure endDate is after d
        if (endDate <= d)
        {
            endDate = d + 1;
        }

        InterestRate rate = curve->forwardRate(d, endDate, dc, comp, freq);
        values.push_back(rate.rate());
    }

    return values;
}