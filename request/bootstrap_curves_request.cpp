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

    // =========================================================================
    // Bootstrap ALL curves together (dependency-aware)
    // =========================================================================
    // Collect all TermStructure specs from the request
    std::vector<const quantra::TermStructure*> tsSpecs;
    auto curveSpecs = request->curves();
    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++) {
        tsSpecs.push_back(curveSpecs->Get(i)->curve());
    }

    // Build a temporary FlatBuffers vector-like structure for the bootstrapper
    // We need to pass the TermStructure objects to CurveBootstrapper
    // Since they're already in the request, we can bootstrap per-curve with
    // shared registries, or use CurveBootstrapper if curves have inter-deps.
    //
    // Strategy: Use CurveBootstrapper to handle multi-curve deps if any curve
    // has HelperDependencies. Otherwise fall back to independent bootstrapping.

    // Check if any curve has dependencies
    bool hasDeps = false;
    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++) {
        auto ts = curveSpecs->Get(i)->curve();
        if (!ts->points()) continue;
        for (flatbuffers::uoffset_t j = 0; j < ts->points()->size(); j++) {
            auto ptype = ts->points()->Get(j)->point_type();
            if (ptype == Point_SwapHelper) {
                auto h = static_cast<const SwapHelper*>(ts->points()->Get(j)->point());
                if (h->deps() && h->deps()->discount_curve()) { hasDeps = true; break; }
            }
            // Check other helper types with deps...
            if (ptype == Point_TenorBasisSwapHelper || ptype == Point_FxSwapHelper ||
                ptype == Point_CrossCcyBasisHelper) {
                hasDeps = true; break;
            }
        }
        if (hasDeps) break;
    }

    // Bootstrap curves (with or without dependency awareness)
    // We'll use TermStructureParser directly for each curve, but with shared
    // registries if there are dependencies.
    quantra::QuoteRegistry quoteReg;
    quantra::CurveRegistry curveReg;
    quantra::IndexFactory indexFactory;
    quantra::TermStructureParser tsParser;

    // Pre-create empty handles for all curves (needed for dep resolution)
    std::map<std::string, std::shared_ptr<RelinkableHandle<YieldTermStructure>>> curveHandles;
    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++) {
        auto ts = curveSpecs->Get(i)->curve();
        std::string id = ts->id() ? ts->id()->str() : "";
        if (!id.empty()) {
            auto h = std::make_shared<RelinkableHandle<YieldTermStructure>>();
            curveHandles[id] = h;
            curveReg.put(id, *h);
        }
    }

    // If there are dependencies, we need to bootstrap in topological order
    // For simplicity and correctness, we always use the dependency-aware path
    // (it degrades gracefully to independent bootstrapping when there are no deps)
    std::unordered_map<std::string, std::vector<std::string>> deps;
    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++) {
        quantra::CurveBootstrapper::collectDeps(curveSpecs->Get(i)->curve(), deps);
    }
    auto order = quantra::CurveBootstrapper::topoSort(deps);

    // Index specs by id for ordered lookup
    std::map<std::string, flatbuffers::uoffset_t> specIndex;
    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++) {
        auto ts = curveSpecs->Get(i)->curve();
        std::string id = ts->id() ? ts->id()->str() : std::to_string(i);
        specIndex[id] = i;
    }

    // Bootstrap and link in order
    std::map<std::string, std::shared_ptr<YieldTermStructure>> builtCurves;
    for (const auto& id : order) {
        auto sit = specIndex.find(id);
        if (sit == specIndex.end()) continue; // referenced dep not in this request
        
        auto ts = curveSpecs->Get(sit->second)->curve();
        try {
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexFactory);
            builtCurves[id] = curve;
            if (curveHandles.count(id)) {
                curveHandles[id]->linkTo(curve);
            }
        } catch (std::exception& e) {
            // Will be handled per-curve in the results loop below
            builtCurves[id] = nullptr;
        }
    }

    // =========================================================================
    // Build results for each curve spec
    // =========================================================================
    std::vector<flatbuffers::Offset<BootstrapCurveResult>> results;

    for (flatbuffers::uoffset_t i = 0; i < curveSpecs->size(); i++)
    {
        auto spec = curveSpecs->Get(i);
        auto termStructure = spec->curve();
        std::string curveId = termStructure->id() ? termStructure->id()->str() : "";

        try
        {
            auto curve = builtCurves.count(curveId) ? builtCurves[curveId] : nullptr;
            if (!curve) {
                // Fallback: try independent bootstrap
                curve = tsParser.parse(termStructure, &quoteReg, &curveReg, &indexFactory);
            }
            curve->enableExtrapolation();

            Date referenceDate = curve->referenceDate();
            
            Calendar curveCalendar = getCalendarFromTermStructure(termStructure);

            // Build grid dates
            std::vector<Date> gridDates;
            Calendar gridCalendar = curveCalendar;
            BusinessDayConvention gridBdc = Following;

            auto query = spec->query();
            if (query && query->grid() && query->grid()->grid_type() != CurveGrid_NONE)
            {
                auto gridSpec = query->grid();
                gridCalendar = getCalendarFromGrid(gridSpec, curveCalendar);
                gridBdc = getBdcFromGrid(gridSpec);
                
                if (gridSpec->grid_type() == CurveGrid_TenorGrid)
                {
                    gridDates = buildTenorGrid(gridSpec->grid_as_TenorGrid(), referenceDate, curveCalendar);
                }
                else if (gridSpec->grid_type() == CurveGrid_RangeGrid)
                {
                    gridDates = buildRangeGrid(gridSpec->grid_as_RangeGrid(), asOfDate);
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

            // Extract pillar dates
            std::vector<Date> pillarDates = extractPillarDatesFromHelpers(termStructure, referenceDate);
            std::vector<flatbuffers::Offset<flatbuffers::String>> pillarDateStrings;
            for (const auto &d : pillarDates)
            {
                std::ostringstream os;
                os << io::iso_date(d);
                pillarDateStrings.push_back(builder->CreateString(os.str()));
            }

            // Build result
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

    auto resultsVec = builder->CreateVector(results);
    BootstrapCurvesResponseBuilder responseBuilder(*builder);
    responseBuilder.add_results(resultsVec);

    return responseBuilder.Finish();
}

// =============================================================================
// Helper methods (unchanged except extractPillarDatesFromHelpers)
// =============================================================================

Calendar BootstrapCurvesRequestHandler::getCalendarFromTermStructure(
    const quantra::TermStructure *termStructure) const
{
    if (termStructure->points() && termStructure->points()->size() > 0)
    {
        auto firstPoint = termStructure->points()->Get(0);
        if (firstPoint->point_as_DepositHelper())
            return CalendarToQL(firstPoint->point_as_DepositHelper()->calendar());
        else if (firstPoint->point_as_SwapHelper())
            return CalendarToQL(firstPoint->point_as_SwapHelper()->calendar());
        else if (firstPoint->point_as_FRAHelper())
            return CalendarToQL(firstPoint->point_as_FRAHelper()->calendar());
        else if (firstPoint->point_as_FutureHelper())
            return CalendarToQL(firstPoint->point_as_FutureHelper()->calendar());
        else if (firstPoint->point_as_OISHelper())
            return CalendarToQL(firstPoint->point_as_OISHelper()->calendar());
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
        if (grid->calendar() != enums::Calendar_NullCalendar)
            return CalendarToQL(grid->calendar());
    }
    else if (gridSpec->grid_type() == CurveGrid_RangeGrid)
    {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (grid->calendar() != enums::Calendar_NullCalendar)
            return CalendarToQL(grid->calendar());
    }
    
    return fallbackCalendar;
}

BusinessDayConvention BootstrapCurvesRequestHandler::getBdcFromGrid(
    const CurveGridSpec *gridSpec) const
{
    if (!gridSpec) return Following;
    
    if (gridSpec->grid_type() == CurveGrid_TenorGrid)
        return ConventionToQL(gridSpec->grid_as_TenorGrid()->business_day_convention());
    else if (gridSpec->grid_type() == CurveGrid_RangeGrid)
        return ConventionToQL(gridSpec->grid_as_RangeGrid()->business_day_convention());
    
    return Following;
}

std::vector<Date> BootstrapCurvesRequestHandler::extractPillarDatesFromHelpers(
    const quantra::TermStructure *termStructure,
    const Date &referenceDate) const
{
    std::set<Date> dateSet;
    dateSet.insert(referenceDate);
    
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
            Period startPeriod(fra->months_to_start(), Months);
            Period tenor(fra->months_to_end() - fra->months_to_start(), Months);
            Date startDate = calendar.advance(referenceDate, startPeriod);
            maturityDate = calendar.advance(startDate, tenor);
        }
        else if (auto future = point->point_as_FutureHelper())
        {
            Date startDate = DateToQL(future->future_start_date()->str());
            maturityDate = calendar.advance(startDate, Period(future->future_months(), Months));
        }
        else if (auto bond = point->point_as_BondHelper())
        {
            if (bond->schedule() && bond->schedule()->termination_date())
                maturityDate = DateToQL(bond->schedule()->termination_date()->str());
        }
        // NEW: OIS helpers
        else if (auto ois = point->point_as_OISHelper())
        {
            Period tenor(ois->tenor_number(), TimeUnitToQL(ois->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor,
                                           ConventionToQL(ois->fixed_leg_convention()));
        }
        // NEW: Dated OIS helpers
        else if (auto datedOis = point->point_as_DatedOISHelper())
        {
            maturityDate = DateToQL(datedOis->end_date()->str());
        }
        // NEW: TenorBasisSwapHelper
        else if (auto basis = point->point_as_TenorBasisSwapHelper())
        {
            Period tenor(basis->tenor_number(), TimeUnitToQL(basis->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor);
        }
        // NEW: FxSwapHelper
        else if (auto fx = point->point_as_FxSwapHelper())
        {
            Period tenor(fx->tenor_number(), TimeUnitToQL(fx->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor);
        }
        // NEW: CrossCcyBasisHelper
        else if (auto xccy = point->point_as_CrossCcyBasisHelper())
        {
            Period tenor(xccy->tenor_number(), TimeUnitToQL(xccy->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor);
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
        
        if (n == 0)
        {
            dates.push_back(referenceDate);
            continue;
        }
        
        Period period(n, unit);
        Date d;
        
        if (useCalendar)
            d = calendar.advance(referenceDate, period, bdc);
        else
            d = referenceDate + period;
        
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
    
    bool isNullCalendar = (grid->calendar() == enums::Calendar_NullCalendar);
    if (businessDaysOnly && isNullCalendar)
    {
        calendar = WeekendsOnly();
    }

    Date current = startDate;
    while (current <= endDate)
    {
        if (businessDaysOnly)
        {
            if (calendar.isBusinessDay(current))
                dates.push_back(current);
        }
        else
        {
            dates.push_back(current);
        }

        if (stepUnit == Days)
            current = current + stepNumber;
        else if (stepUnit == Weeks)
            current = current + stepNumber * 7;
        else
            current = calendar.advance(current, step, bdc);

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
        values.push_back(curve->discount(d));
    return values;
}

std::vector<double> BootstrapCurvesRequestHandler::computeZeroRates(
    const std::shared_ptr<YieldTermStructure> &curve,
    const std::vector<Date> &dates,
    const ZeroRateQuery *zeroQuery) const
{
    DayCounter dc = curve->dayCounter();
    Compounding comp = Continuous;
    Frequency freq = Annual;

    if (zeroQuery)
    {
        if (!zeroQuery->use_curve_day_counter())
            dc = DayCounterToQL(zeroQuery->day_counter());
        comp = CompoundingToQL(zeroQuery->compounding());
        freq = FrequencyToQL(zeroQuery->frequency());
    }

    std::vector<double> values;
    values.reserve(dates.size());
    Date refDate = curve->referenceDate();

    for (const auto &d : dates)
    {
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
            dc = DayCounterToQL(fwdQuery->day_counter());
        comp = CompoundingToQL(fwdQuery->compounding());
        freq = FrequencyToQL(fwdQuery->frequency());
        fwdType = fwdQuery->forward_type();
        epsNumber = fwdQuery->instantaneous_eps_number();
        epsUnit = TimeUnitToQL(fwdQuery->instantaneous_eps_time_unit());
        tenorNumber = fwdQuery->tenor_number();
        tenorUnit = TimeUnitToQL(fwdQuery->tenor_time_unit());
        useGridCalendar = fwdQuery->use_grid_calendar_for_advance();
    }

    Calendar calendar = useGridCalendar ? gridCalendar : curveCalendar;
    BusinessDayConvention bdc = useGridCalendar ? gridBdc : Following;

    std::vector<double> values;
    values.reserve(dates.size());

    for (const auto &d : dates)
    {
        Date endDate;
        
        if (fwdType == ForwardType_Instantaneous)
        {
            if (epsUnit == Days)
                endDate = d + epsNumber;
            else
                endDate = calendar.advance(d, Period(epsNumber, epsUnit), bdc);
        }
        else
        {
            endDate = calendar.advance(d, Period(tenorNumber, tenorUnit), bdc);
        }

        if (endDate <= d)
            endDate = d + 1;

        InterestRate rate = curve->forwardRate(d, endDate, dc, comp, freq);
        values.push_back(rate.rate());
    }

    return values;
}
