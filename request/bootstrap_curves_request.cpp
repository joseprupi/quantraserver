#include "bootstrap_curves_request.h"

#include <set>

using namespace QuantLib;
using namespace quantra;

namespace {

const quantra::TermStructure* findCurveSpecById(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>* curves,
    const std::string& curveId) {
    if (!curves) {
        return nullptr;
    }
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); i++) {
        const auto* ts = curves->Get(i);
        if (ts && ts->id() && ts->id()->str() == curveId) {
            return ts;
        }
    }
    return nullptr;
}

} // namespace

flatbuffers::Offset<BootstrapCurvesResponse> BootstrapCurvesRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const BootstrapCurvesRequest* request) const {
    if (!request || !request->pricing()) {
        QUANTRA_ERROR("BootstrapCurvesRequest.pricing is required");
    }
    if (!request->queries()) {
        QUANTRA_ERROR("BootstrapCurvesRequest.queries is required");
    }

    PricingRegistry reg;
    std::string pricingBuildError;
    try {
        PricingRegistryBuilder regBuilder;
        reg = regBuilder.build(request->pricing());
    } catch (const std::exception& e) {
        pricingBuildError = e.what();
    }
    const Date asOfDate = DateToQL(request->pricing()->as_of_date()->str());

    std::vector<flatbuffers::Offset<BootstrapCurveResult>> results;
    auto querySpecs = request->queries();
    for (flatbuffers::uoffset_t i = 0; i < querySpecs->size(); i++) {
        const auto* query = querySpecs->Get(i);
        std::string curveId = (query && query->curve_id()) ? query->curve_id()->str() : "";

        try {
            if (!pricingBuildError.empty()) {
                QUANTRA_ERROR(pricingBuildError);
            }
            if (curveId.empty()) {
                QUANTRA_ERROR("CurveQuerySpec.curve_id is required");
            }
            if (!query->grid()) {
                QUANTRA_ERROR("CurveQuerySpec.grid is required for curve_id: " + curveId);
            }
            if (!query->measures() || query->measures()->size() == 0) {
                QUANTRA_ERROR("CurveQuerySpec.measures is required for curve_id: " + curveId);
            }

            auto regIt = reg.curves.find(curveId);
            if (regIt == reg.curves.end() || !regIt->second || regIt->second->empty()) {
                QUANTRA_ERROR("Curve id not found in PricingRegistry: " + curveId);
            }
            auto curve = regIt->second->currentLink();
            if (!curve) {
                QUANTRA_ERROR("Curve handle has no linked curve for id: " + curveId);
            }

            const auto* tsSpec = findCurveSpecById(request->pricing()->curves(), curveId);
            if (!tsSpec) {
                QUANTRA_ERROR("curve_id '" + curveId + "' not found in pricing.curves");
            }

            const auto* options = query->options();
            const bool allowExtrapolation = !options || options->allow_extrapolation();
            if (allowExtrapolation) {
                curve->enableExtrapolation();
            } else {
                curve->disableExtrapolation();
            }

            const Date referenceDate = curve->referenceDate();
            const Calendar curveCalendar = getCalendarFromTermStructure(tsSpec);

            std::vector<Date> gridDates;
            const Calendar gridCalendar = getCalendarFromGrid(query->grid(), options, curveCalendar);
            const BusinessDayConvention gridBdc = getBdcFromGrid(query->grid(), options);
            if (query->grid()->grid_type() == DateGrid_TenorGrid) {
                gridDates = buildTenorGrid(query->grid()->grid_as_TenorGrid(), referenceDate, curveCalendar);
            } else if (query->grid()->grid_type() == DateGrid_RangeGrid) {
                const int maxPoints = (options && options->max_points() > 0) ? options->max_points() : 50000;
                gridDates = buildRangeGrid(query->grid()->grid_as_RangeGrid(), asOfDate, maxPoints);
            } else {
                QUANTRA_ERROR("DateGridSpec.grid is required for curve_id: " + curveId);
            }

            std::vector<flatbuffers::Offset<flatbuffers::String>> gridDateStrings;
            for (const auto& d : gridDates) {
                std::ostringstream os;
                os << io::iso_date(d);
                gridDateStrings.push_back(builder->CreateString(os.str()));
            }

            std::vector<flatbuffers::Offset<CurveSeries>> seriesVector;
            for (flatbuffers::uoffset_t m = 0; m < query->measures()->size(); m++) {
                CurveMeasure measure = static_cast<CurveMeasure>(query->measures()->Get(m));
                std::vector<double> values;

                switch (measure) {
                    case CurveMeasure_DF:
                        values = computeDiscountFactors(curve, gridDates);
                        break;
                    case CurveMeasure_ZERO:
                        values = computeZeroRates(curve, gridDates, query->zero());
                        break;
                    case CurveMeasure_FWD:
                        values = computeForwardRates(
                            curve, gridDates, query->fwd(), gridCalendar, gridBdc, curveCalendar);
                        break;
                    default:
                        QUANTRA_ERROR("Unsupported CurveMeasure for curve_id: " + curveId);
                }

                auto valuesVec = builder->CreateVector(values);
                CurveSeriesBuilder seriesBuilder(*builder);
                seriesBuilder.add_measure(measure);
                seriesBuilder.add_values(valuesVec);
                seriesVector.push_back(seriesBuilder.Finish());
            }

            std::vector<Date> pillarDates = extractPillarDatesFromHelpers(tsSpec, referenceDate);
            std::vector<flatbuffers::Offset<flatbuffers::String>> pillarDateStrings;
            for (const auto& d : pillarDates) {
                std::ostringstream os;
                os << io::iso_date(d);
                pillarDateStrings.push_back(builder->CreateString(os.str()));
            }

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
        } catch (const std::exception& e) {
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

Calendar BootstrapCurvesRequestHandler::getCalendarFromTermStructure(
    const quantra::TermStructure* termStructure) const {
    if (termStructure->points() && termStructure->points()->size() > 0) {
        auto firstPoint = termStructure->points()->Get(0);
        if (firstPoint->point_as_DepositHelper())
            return CalendarToQL(firstPoint->point_as_DepositHelper()->calendar());
        if (firstPoint->point_as_SwapHelper())
            return CalendarToQL(firstPoint->point_as_SwapHelper()->calendar());
        if (firstPoint->point_as_FRAHelper())
            return CalendarToQL(firstPoint->point_as_FRAHelper()->calendar());
        if (firstPoint->point_as_FutureHelper())
            return CalendarToQL(firstPoint->point_as_FutureHelper()->calendar());
        if (firstPoint->point_as_OISHelper())
            return CalendarToQL(firstPoint->point_as_OISHelper()->calendar());
    }
    return TARGET();
}

Calendar BootstrapCurvesRequestHandler::getCalendarFromGrid(
    const DateGridSpec* gridSpec,
    const QueryOptions* options,
    const Calendar& fallbackCalendar) const {
    if (options && options->calendar() != enums::Calendar_NullCalendar) {
        return CalendarToQL(options->calendar());
    }
    if (!gridSpec) return fallbackCalendar;

    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        auto grid = gridSpec->grid_as_TenorGrid();
        if (grid->calendar() != enums::Calendar_NullCalendar) {
            return CalendarToQL(grid->calendar());
        }
    } else if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (grid->calendar() != enums::Calendar_NullCalendar) {
            return CalendarToQL(grid->calendar());
        }
    }

    return fallbackCalendar;
}

BusinessDayConvention BootstrapCurvesRequestHandler::getBdcFromGrid(
    const DateGridSpec* gridSpec,
    const QueryOptions* options) const {
    if (options) {
        return ConventionToQL(options->business_day_convention());
    }
    if (!gridSpec) return Following;

    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        return ConventionToQL(gridSpec->grid_as_TenorGrid()->business_day_convention());
    }
    if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        return ConventionToQL(gridSpec->grid_as_RangeGrid()->business_day_convention());
    }

    return Following;
}

std::vector<Date> BootstrapCurvesRequestHandler::extractPillarDatesFromHelpers(
    const quantra::TermStructure* termStructure,
    const Date& referenceDate) const {
    std::set<Date> dateSet;
    dateSet.insert(referenceDate);
    if (!termStructure->points()) return std::vector<Date>(dateSet.begin(), dateSet.end());

    auto points = termStructure->points();
    Calendar calendar = getCalendarFromTermStructure(termStructure);
    for (flatbuffers::uoffset_t i = 0; i < points->size(); i++) {
        auto point = points->Get(i);
        Date maturityDate;
        if (auto deposit = point->point_as_DepositHelper()) {
            Period tenor(deposit->tenor_number(), TimeUnitToQL(deposit->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor, ConventionToQL(deposit->business_day_convention()));
        } else if (auto swap = point->point_as_SwapHelper()) {
            Period tenor(swap->tenor_number(), TimeUnitToQL(swap->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor, ConventionToQL(swap->sw_fixed_leg_convention()));
        } else if (auto fra = point->point_as_FRAHelper()) {
            Period startPeriod(fra->months_to_start(), Months);
            Period tenor(fra->months_to_end() - fra->months_to_start(), Months);
            Date startDate = calendar.advance(referenceDate, startPeriod);
            maturityDate = calendar.advance(startDate, tenor);
        } else if (auto future = point->point_as_FutureHelper()) {
            Date startDate = DateToQL(future->future_start_date()->str());
            maturityDate = calendar.advance(startDate, Period(future->future_months(), Months));
        } else if (auto bond = point->point_as_BondHelper()) {
            if (bond->schedule() && bond->schedule()->termination_date()) {
                maturityDate = DateToQL(bond->schedule()->termination_date()->str());
            }
        } else if (auto ois = point->point_as_OISHelper()) {
            Period tenor(ois->tenor_number(), TimeUnitToQL(ois->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor, ConventionToQL(ois->fixed_leg_convention()));
        } else if (auto datedOis = point->point_as_DatedOISHelper()) {
            maturityDate = DateToQL(datedOis->end_date()->str());
        } else if (auto basis = point->point_as_TenorBasisSwapHelper()) {
            Period tenor(basis->tenor_number(), TimeUnitToQL(basis->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor);
        } else if (auto fx = point->point_as_FxSwapHelper()) {
            Period tenor(fx->tenor_number(), TimeUnitToQL(fx->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor);
        } else if (auto xccy = point->point_as_CrossCcyBasisHelper()) {
            Period tenor(xccy->tenor_number(), TimeUnitToQL(xccy->tenor_time_unit()));
            maturityDate = calendar.advance(referenceDate, tenor);
        }
        if (maturityDate != Date()) {
            dateSet.insert(maturityDate);
        }
    }
    return std::vector<Date>(dateSet.begin(), dateSet.end());
}

std::vector<Date> BootstrapCurvesRequestHandler::buildTenorGrid(
    const TenorGrid* grid,
    const Date& referenceDate,
    const Calendar& fallbackCalendar) const {
    std::vector<Date> dates;
    auto tenors = grid->tenors();
    Calendar calendar = fallbackCalendar;
    BusinessDayConvention bdc = Following;
    bool useCalendar = false;

    if (grid->calendar() != enums::Calendar_NullCalendar) {
        calendar = CalendarToQL(grid->calendar());
        bdc = ConventionToQL(grid->business_day_convention());
        useCalendar = true;
    }

    for (flatbuffers::uoffset_t i = 0; i < tenors->size(); i++) {
        auto tenor = tenors->Get(i);
        int n = tenor->tenor_number();
        TimeUnit unit = TimeUnitToQL(tenor->tenor_time_unit());
        if (n == 0) {
            dates.push_back(referenceDate);
            continue;
        }
        Period period(n, unit);
        Date d = useCalendar ? calendar.advance(referenceDate, period, bdc) : referenceDate + period;
        dates.push_back(d);
    }

    return dates;
}

std::vector<Date> BootstrapCurvesRequestHandler::buildRangeGrid(
    const RangeGrid* grid,
    const Date& asOfDate,
    int maxPoints) const {
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
    if (businessDaysOnly && isNullCalendar) {
        calendar = WeekendsOnly();
    }

    Date current = startDate;
    while (current <= endDate) {
        if (businessDaysOnly) {
            if (calendar.isBusinessDay(current)) dates.push_back(current);
        } else {
            dates.push_back(current);
        }

        if (stepUnit == Days) {
            current = current + stepNumber;
        } else if (stepUnit == Weeks) {
            current = current + stepNumber * 7;
        } else {
            current = calendar.advance(current, step, bdc);
        }

        if (static_cast<int>(dates.size()) > maxPoints) {
            QUANTRA_ERROR("Grid too large (>" + std::to_string(maxPoints) + " points).");
        }
    }

    return dates;
}

std::vector<double> BootstrapCurvesRequestHandler::computeDiscountFactors(
    const std::shared_ptr<YieldTermStructure>& curve,
    const std::vector<Date>& dates) const {
    std::vector<double> values;
    values.reserve(dates.size());
    for (const auto& d : dates) values.push_back(curve->discount(d));
    return values;
}

std::vector<double> BootstrapCurvesRequestHandler::computeZeroRates(
    const std::shared_ptr<YieldTermStructure>& curve,
    const std::vector<Date>& dates,
    const ZeroRateQuery* zeroQuery) const {
    DayCounter dc = curve->dayCounter();
    Compounding comp = Continuous;
    Frequency freq = Annual;
    if (zeroQuery) {
        if (!zeroQuery->use_curve_day_counter()) dc = DayCounterToQL(zeroQuery->day_counter());
        comp = CompoundingToQL(zeroQuery->compounding());
        freq = FrequencyToQL(zeroQuery->frequency());
    }

    std::vector<double> values;
    values.reserve(dates.size());
    Date refDate = curve->referenceDate();
    for (const auto& d : dates) {
        Date dd = (d <= refDate) ? refDate + 1 : d;
        InterestRate rate = curve->zeroRate(dd, dc, comp, freq);
        values.push_back(rate.rate());
    }
    return values;
}

std::vector<double> BootstrapCurvesRequestHandler::computeForwardRates(
    const std::shared_ptr<YieldTermStructure>& curve,
    const std::vector<Date>& dates,
    const ForwardRateQuery* fwdQuery,
    const Calendar& gridCalendar,
    BusinessDayConvention gridBdc,
    const Calendar& curveCalendar) const {
    DayCounter dc = curve->dayCounter();
    Compounding comp = Simple;
    Frequency freq = Annual;
    ForwardType fwdType = ForwardType_Instantaneous;
    int epsNumber = 1;
    TimeUnit epsUnit = Days;
    int tenorNumber = 3;
    TimeUnit tenorUnit = Months;
    bool useGridCalendar = true;

    if (fwdQuery) {
        if (!fwdQuery->use_curve_day_counter()) dc = DayCounterToQL(fwdQuery->day_counter());
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
    for (const auto& d : dates) {
        Date endDate;
        if (fwdType == ForwardType_Instantaneous) {
            endDate = (epsUnit == Days) ? d + epsNumber : calendar.advance(d, Period(epsNumber, epsUnit), bdc);
        } else {
            endDate = calendar.advance(d, Period(tenorNumber, tenorUnit), bdc);
        }
        if (endDate <= d) endDate = d + 1;
        InterestRate rate = curve->forwardRate(d, endDate, dc, comp, freq);
        values.push_back(rate.rate());
    }
    return values;
}
