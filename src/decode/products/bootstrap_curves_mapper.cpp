#include "bootstrap_curves_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "fx_spot_parser.h"
#include "grid_utils.h"
#include "request_validation.h"

#include <ql/time/calendars/target.hpp>

#include <set>
#include <utility>

namespace quantra {

namespace {

const quantra::TermStructure* findCurveSpecById(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>* curves,
    const std::string& curveId) {
    if (!curves) return nullptr;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); ++i) {
        const auto* ts = curves->Get(i);
        if (ts && ts->id() && ts->id()->str() == curveId) return ts;
    }
    return nullptr;
}

/// Pull the calendar off the first helper that exposes one. Mirrors the
/// legacy fallback so pillar-date math stays byte-identical.
QuantLib::Calendar calendarFromTermStructure(const quantra::TermStructure* ts) {
    if (ts->points() && ts->points()->size() > 0) {
        const auto* first = ts->points()->Get(0);
        if (auto h = first->point_as_DepositHelper()) {
            if (!h->calendar().has_value())
                QUANTRA_INVALID_ARGUMENT("DepositHelper.calendar is required");
            return CalendarToQL(h->calendar().value());
        }
        if (auto h = first->point_as_SwapHelper()) {
            if (!h->calendar().has_value())
                QUANTRA_INVALID_ARGUMENT("SwapHelper.calendar is required");
            return CalendarToQL(h->calendar().value());
        }
        if (auto h = first->point_as_FRAHelper()) {
            if (!h->calendar().has_value())
                QUANTRA_INVALID_ARGUMENT("FRAHelper.calendar is required");
            return CalendarToQL(h->calendar().value());
        }
        if (auto h = first->point_as_FutureHelper()) {
            if (!h->calendar().has_value())
                QUANTRA_INVALID_ARGUMENT("FutureHelper.calendar is required");
            return CalendarToQL(h->calendar().value());
        }
        if (auto h = first->point_as_OISHelper())
            return CalendarToQL(requireEnum(h->calendar(), "OISHelper.calendar"));
    }
    return QuantLib::TARGET();
}

/// Reproduce the curve's bootstrap-time reference date from the FB spec:
/// TermStructureParser uses `ts->reference_date()` when present, else the
/// global evaluation date (= as_of_date).
QuantLib::Date resolveCurveReferenceDate(
    const quantra::TermStructure* ts, const QuantLib::Date& asOfDate) {
    if (ts->reference_date()) return DateToQL(ts->reference_date()->str());
    return asOfDate;
}

/// Settlement conventions of an FxSpotSpec, as the FX helpers consume them.
struct FxSettlement {
    int spotDays = 0;
    QuantLib::Calendar calendar;
    QuantLib::BusinessDayConvention convention = QuantLib::Following;
};

/// The spot named by a helper, from pricing.fx.spots. An unknown id is the
/// same 404 the bootstrap raises (the per-query fold reports it; the
/// transport-level error from the bootstrap wins).
FxSettlement fxSettlementOf(const quantra::FxMarketData* fx, const flatbuffers::String* spotId) {
    const std::string id = spotId ? spotId->str() : std::string{};
    const quantra::FxSpotSpec* spec = nullptr;
    if (fx && fx->spots()) {
        for (flatbuffers::uoffset_t i = 0; i < fx->spots()->size(); ++i) {
            const auto* s = fx->spots()->Get(i);
            if (s && s->id() && s->id()->str() == id) {
                spec = s;
                break;
            }
        }
    }
    if (!spec) QUANTRA_NOT_FOUND("FX spot not found: " + id);
    FxSettlement out;
    out.spotDays = requireNonNegativeInt(spec->spot_days(), "FxSpotSpec.spot_days");
    out.calendar = JointSettlementCalendar(spec->settlement_calendars(), id);
    out.convention = ConventionToQL(
        requireEnum(spec->business_day_convention(), "FxSpotSpec.business_day_convention"));
    return out;
}

/// Pillar of an FxSwapHelper: QuantLib's FxSwapRateHelper settles spot_days
/// after the (adjusted) evaluation date on the spot's joint calendar and
/// advances the tenor with the spot's convention and the helper's
/// end-of-month rule. The global evaluation date, not the curve's reference
/// date, is what the helper uses.
QuantLib::Date fxSwapPillarDate(
    const QuantLib::Date& asOfDate,
    const FxSettlement& spot,
    const QuantLib::Period& tenor,
    bool endOfMonth) {
    const QuantLib::Date start =
        spot.calendar.advance(spot.calendar.adjust(asOfDate), spot.spotDays * QuantLib::Days);
    return spot.calendar.advance(start, tenor, spot.convention, endOfMonth);
}

/// Pillar of a CrossCcyBasisHelper: the swap settles spot_days after the
/// evaluation date on the HELPER's calendar, the schedule ends at settlement
/// + tenor adjusted with the helper's convention (an end-of-month schedule
/// adjusts only the intermediate dates, except under Unadjusted), and the
/// pillar is the last payment date: the schedule end shifted by the payment
/// lag with QuantLib's floating-leg default (Following on the schedule
/// calendar). This is the date QuantLib's helper reports as pillarDate().
QuantLib::Date crossCcyPillarDate(
    const QuantLib::Date& asOfDate,
    int spotDays,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention convention,
    bool endOfMonth,
    const QuantLib::Period& tenor,
    int paymentLag) {
    const QuantLib::Date settlement =
        calendar.advance(calendar.adjust(asOfDate), spotDays * QuantLib::Days, convention);
    const QuantLib::Date termination = settlement + tenor;
    QuantLib::Date end;
    if (convention == QuantLib::Unadjusted) {
        end = (endOfMonth && calendar.isEndOfMonth(termination))
                  ? QuantLib::Date::endOfMonth(termination)
                  : termination;
    } else {
        end = calendar.adjust(termination, convention);
    }
    return calendar.advance(end, paymentLag * QuantLib::Days, QuantLib::Following);
}

std::vector<QuantLib::Date> extractPillarDates(
    const quantra::TermStructure* ts,
    const QuantLib::Date& referenceDate,
    const QuantLib::Date& asOfDate,
    const quantra::FxMarketData* fx) {
    std::set<QuantLib::Date> dateSet;
    dateSet.insert(referenceDate);
    if (!ts->points()) {
        return std::vector<QuantLib::Date>(dateSet.begin(), dateSet.end());
    }

    const auto* points = ts->points();
    QuantLib::Calendar calendar = calendarFromTermStructure(ts);
    for (flatbuffers::uoffset_t i = 0; i < points->size(); ++i) {
        const auto* point = points->Get(i);
        QuantLib::Date maturityDate;
        if (auto deposit = point->point_as_DepositHelper()) {
            QuantLib::Period tenor = requirePeriod(deposit->tenor(), "DepositHelper.tenor");
            if (!deposit->business_day_convention().has_value())
                QUANTRA_INVALID_ARGUMENT("DepositHelper.business_day_convention is required");
            maturityDate = calendar.advance(
                referenceDate, tenor, ConventionToQL(deposit->business_day_convention().value()));
        } else if (auto swap = point->point_as_SwapHelper()) {
            QuantLib::Period tenor = requirePeriod(swap->tenor(), "SwapHelper.tenor");
            if (!swap->sw_fixed_leg_convention().has_value())
                QUANTRA_INVALID_ARGUMENT("SwapHelper.sw_fixed_leg_convention is required");
            maturityDate = calendar.advance(
                referenceDate, tenor, ConventionToQL(swap->sw_fixed_leg_convention().value()));
        } else if (auto fra = point->point_as_FRAHelper()) {
            QuantLib::Period startPeriod(fra->months_to_start(), QuantLib::Months);
            QuantLib::Period tenor(fra->months_to_end() - fra->months_to_start(), QuantLib::Months);
            QuantLib::Date startDate = calendar.advance(referenceDate, startPeriod);
            maturityDate = calendar.advance(startDate, tenor);
        } else if (auto future = point->point_as_FutureHelper()) {
            if (!future->future_start_date()) {
                QUANTRA_INVALID_ARGUMENT("FutureHelper.future_start_date is required");
            }
            QuantLib::Date startDate = DateToQL(future->future_start_date()->str());
            maturityDate = calendar.advance(
                startDate, QuantLib::Period(future->future_months(), QuantLib::Months));
        } else if (auto bond = point->point_as_BondHelper()) {
            if (bond->schedule() && bond->schedule()->termination_date()) {
                maturityDate = DateToQL(bond->schedule()->termination_date()->str());
            }
        } else if (auto ois = point->point_as_OISHelper()) {
            QuantLib::Period tenor = requirePeriod(ois->tenor(), "OISHelper.tenor");
            maturityDate = calendar.advance(
                referenceDate, tenor,
                ConventionToQL(
                    requireEnum(ois->fixed_leg_convention(), "OISHelper.fixed_leg_convention")));
        } else if (auto datedOis = point->point_as_DatedOISHelper()) {
            maturityDate = DateToQL(datedOis->end_date()->str());
        } else if (auto basis = point->point_as_TenorBasisSwapHelper()) {
            QuantLib::Period tenor = requirePeriod(basis->tenor(), "TenorBasisSwapHelper.tenor");
            maturityDate = calendar.advance(referenceDate, tenor);
        } else if (auto fxh = point->point_as_FxSwapHelper()) {
            const QuantLib::Period tenor = requirePeriod(fxh->tenor(), "FxSwapHelper.tenor");
            maturityDate = fxSwapPillarDate(
                asOfDate, fxSettlementOf(fx, fxh->fx_spot()), tenor,
                requireBool(fxh->end_of_month(), "FxSwapHelper.end_of_month"));
        } else if (auto xccy = point->point_as_CrossCcyBasisHelper()) {
            const QuantLib::Period tenor =
                requirePeriod(xccy->tenor(), "CrossCcyBasisHelper.tenor");
            maturityDate = crossCcyPillarDate(
                asOfDate, fxSettlementOf(fx, xccy->fx_spot()).spotDays,
                CalendarToQL(requireEnum(xccy->calendar(), "CrossCcyBasisHelper.calendar")),
                ConventionToQL(requireEnum(
                    xccy->business_day_convention(),
                    "CrossCcyBasisHelper.business_day_convention")),
                requireBool(xccy->end_of_month(), "CrossCcyBasisHelper.end_of_month"), tenor,
                requireNonNegativeInt(xccy->payment_lag(), "CrossCcyBasisHelper.payment_lag"));
        }
        if (maturityDate != QuantLib::Date()) {
            dateSet.insert(maturityDate);
        }
    }
    return std::vector<QuantLib::Date>(dateSet.begin(), dateSet.end());
}

ZeroSampleSpec extractZeroSpec(const quantra::ZeroRateQuery* q) {
    ZeroSampleSpec s;
    if (q == nullptr) return s;
    s.useCurveDayCounter = q->use_curve_day_counter();
    s.dayCounter = DayCounterToQL(q->day_counter());
    s.compounding = CompoundingToQL(q->compounding());
    s.frequency = FrequencyToQL(q->frequency());
    return s;
}

ForwardSampleSpec extractForwardSpec(
    const quantra::ForwardRateQuery* q,
    const QuantLib::Calendar& gridCalendar,
    QuantLib::BusinessDayConvention gridBdc,
    const QuantLib::Calendar& curveCalendar) {
    ForwardSampleSpec s;
    bool useGridCalendar = true;
    if (q != nullptr) {
        s.useCurveDayCounter = q->use_curve_day_counter();
        s.dayCounter = DayCounterToQL(q->day_counter());
        s.compounding = CompoundingToQL(q->compounding());
        s.frequency = FrequencyToQL(q->frequency());
        s.instantaneous = (q->forward_type() == quantra::ForwardType_Instantaneous);
        s.epsNumber = q->instantaneous_eps_number();
        s.epsUnit = TimeUnitToQL(q->instantaneous_eps_time_unit());
        if (q->tenor()) {
            s.tenor = requirePeriod(q->tenor(), "ForwardRateQuery.tenor");
        }
        useGridCalendar = q->use_grid_calendar_for_advance();
    }
    s.calendar = useGridCalendar ? gridCalendar : curveCalendar;
    s.bdc = useGridCalendar ? gridBdc : QuantLib::Following;
    return s;
}

quantra::CurveMeasure toFbMeasure(CurveSampleMeasure m) {
    switch (m) {
        case CurveSampleMeasure::DiscountFactor: return quantra::CurveMeasure_DF;
        case CurveSampleMeasure::ZeroRate: return quantra::CurveMeasure_ZERO;
        case CurveSampleMeasure::ForwardRate: return quantra::CurveMeasure_FWD;
    }
    return quantra::CurveMeasure_DF;
}

CurveSampleMeasure fromFbMeasure(quantra::CurveMeasure m, const std::string& curveId) {
    switch (m) {
        case quantra::CurveMeasure_DF: return CurveSampleMeasure::DiscountFactor;
        case quantra::CurveMeasure_ZERO: return CurveSampleMeasure::ZeroRate;
        case quantra::CurveMeasure_FWD: return CurveSampleMeasure::ForwardRate;
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CurveMeasure for curve_id: " + curveId);
    return CurveSampleMeasure::DiscountFactor; // unreachable
}

BootstrapCurvesQuery extractQuery(
    const quantra::CurveQuerySpec* query,
    const quantra::TermStructure* tsSpec,
    const QuantLib::Date& asOfDate,
    const quantra::FxMarketData* fx) {
    if (query == nullptr) {
        QUANTRA_INVALID_ARGUMENT("CurveQuerySpec is null");
    }
    if (!query->curve_id() || query->curve_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("CurveQuerySpec.curve_id is required");
    }
    const std::string curveId = query->curve_id()->str();
    if (!query->grid()) {
        QUANTRA_INVALID_ARGUMENT("CurveQuerySpec.grid is required for curve_id: " + curveId);
    }
    if (!query->measures() || query->measures()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CurveQuerySpec.measures is required for curve_id: " + curveId);
    }
    if (tsSpec == nullptr) {
        QUANTRA_INVALID_ARGUMENT("curve_id '" + curveId + "' not found in pricing.rates.curves");
    }

    BootstrapCurvesQuery out;
    out.curveId = curveId;

    const auto* options = query->options();
    out.allowExtrapolation = !options || options->allow_extrapolation();

    const QuantLib::Calendar curveCalendar = calendarFromTermStructure(tsSpec);
    const QuantLib::Date referenceDate = resolveCurveReferenceDate(tsSpec, asOfDate);

    const QuantLib::Calendar gridCalendar =
        grid_utils::ResolveCalendar(query->grid(), options, curveCalendar);
    const QuantLib::BusinessDayConvention gridBdc =
        grid_utils::ResolveBusinessDayConvention(query->grid(), options, QuantLib::Following);

    if (query->grid()->grid_type() == quantra::DateGrid_TenorGrid) {
        out.gridDates = grid_utils::BuildTenorGrid(
            query->grid()->grid_as_TenorGrid(), referenceDate, curveCalendar,
            /*forceCalendarAdvance=*/false);
    } else if (query->grid()->grid_type() == quantra::DateGrid_RangeGrid) {
        const int maxPoints =
            (options && options->max_points() > 0) ? options->max_points() : 50000;
        out.gridDates =
            grid_utils::BuildRangeGrid(query->grid()->grid_as_RangeGrid(), asOfDate, maxPoints);
    } else {
        QUANTRA_INVALID_ARGUMENT("DateGridSpec.grid is required for curve_id: " + curveId);
    }

    out.pillarDates = extractPillarDates(tsSpec, referenceDate, asOfDate, fx);

    out.measures.reserve(query->measures()->size());
    for (flatbuffers::uoffset_t m = 0; m < query->measures()->size(); ++m) {
        auto fbMeasure = static_cast<quantra::CurveMeasure>(query->measures()->Get(m));
        out.measures.push_back(fromFbMeasure(fbMeasure, curveId));
    }

    out.zero = extractZeroSpec(query->zero());
    out.fwd = extractForwardSpec(query->fwd(), gridCalendar, gridBdc, curveCalendar);
    return out;
}

} // namespace

BootstrapCurvesInputs BootstrapCurvesMapper::toInputs(
    const quantra::BootstrapCurvesRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("BootstrapCurvesRequest is null");
    }
    if (!req->pricing()) {
        QUANTRA_INVALID_ARGUMENT("BootstrapCurvesRequest.pricing is required");
    }
    if (!req->queries() || req->queries()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("BootstrapCurvesRequest.queries is required");
    }

    const QuantLib::Date asOfDate = DateToQL(req->pricing()->as_of_date()->str());
    const auto* rates = req->pricing()->rates();
    const auto* curves = rates ? rates->curves() : nullptr;

    BootstrapCurvesInputs inputs;
    inputs.queries.reserve(req->queries()->size());
    for (flatbuffers::uoffset_t i = 0; i < req->queries()->size(); ++i) {
        const auto* qspec = req->queries()->Get(i);
        std::string curveId =
            (qspec && qspec->curve_id()) ? qspec->curve_id()->str() : std::string{};
        BootstrapCurvesQuery q;
        try {
            const auto* tsSpec = findCurveSpecById(curves, curveId);
            q = extractQuery(qspec, tsSpec, asOfDate, req->pricing()->fx());
        } catch (const std::exception& e) {
            q.curveId = curveId;
            q.prebuiltError = e.what();
        }
        inputs.queries.push_back(std::move(q));
    }
    return inputs;
}

flatbuffers::Offset<quantra::BootstrapCurvesResponse> BootstrapCurvesMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const BootstrapCurvesResult& result) const {

    std::vector<flatbuffers::Offset<quantra::BootstrapCurveResult>> results;
    results.reserve(result.curves.size());

    for (const auto& curve : result.curves) {
        auto idStr = builder.CreateString(curve.id);

        if (!curve.error.empty()) {
            auto errorMsg = builder.CreateString(curve.error);
            quantra::ErrorBuilder errorBuilder(builder);
            errorBuilder.add_error_message(errorMsg);
            auto error = errorBuilder.Finish();

            quantra::BootstrapCurveResultBuilder rb(builder);
            rb.add_id(idStr);
            rb.add_error(error);
            results.push_back(rb.Finish());
            continue;
        }

        auto refDateStr = builder.CreateString(DateToIso(curve.referenceDate));

        std::vector<flatbuffers::Offset<flatbuffers::String>> gridDateStrings;
        gridDateStrings.reserve(curve.gridDates.size());
        for (const auto& d : curve.gridDates) {
            gridDateStrings.push_back(builder.CreateString(DateToIso(d)));
        }
        auto gridDatesVec = builder.CreateVector(gridDateStrings);

        std::vector<flatbuffers::Offset<quantra::CurveSeries>> seriesVector;
        seriesVector.reserve(curve.series.size());
        for (const auto& s : curve.series) {
            auto valuesVec = builder.CreateVector(s.values);
            quantra::CurveSeriesBuilder sb(builder);
            sb.add_measure(toFbMeasure(s.measure));
            sb.add_values(valuesVec);
            seriesVector.push_back(sb.Finish());
        }
        auto seriesVec = builder.CreateVector(seriesVector);

        std::vector<flatbuffers::Offset<flatbuffers::String>> pillarDateStrings;
        pillarDateStrings.reserve(curve.pillarDates.size());
        for (const auto& d : curve.pillarDates) {
            pillarDateStrings.push_back(builder.CreateString(DateToIso(d)));
        }
        auto pillarDatesVec = builder.CreateVector(pillarDateStrings);

        quantra::BootstrapCurveResultBuilder rb(builder);
        rb.add_id(idStr);
        rb.add_reference_date(refDateStr);
        rb.add_grid_dates(gridDatesVec);
        rb.add_series(seriesVec);
        rb.add_pillar_dates(pillarDatesVec);
        results.push_back(rb.Finish());
    }

    auto resultsVec = builder.CreateVector(results);
    quantra::BootstrapCurvesResponseBuilder rb(builder);
    rb.add_results(resultsVec);
    return rb.Finish();
}

} // namespace quantra
