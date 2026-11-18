#include "commodity_curve_build_mapper.h"

#include "commodity_parsers.h"
#include "commodity_swap_mapper.h"
#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

#include <optional>
#include <string>
#include <vector>

namespace quantra {

namespace {

commodities::CurveInterpolation decodeInterpolation(
    quantra::enums::CommodityCurveInterpolation value, const std::string& field) {
    using W = quantra::enums::CommodityCurveInterpolation;
    using C = commodities::CurveInterpolation;
    switch (value) {
        case W::CommodityCurveInterpolation_Linear: return C::Linear;
        case W::CommodityCurveInterpolation_LogLinear: return C::LogLinear;
        case W::CommodityCurveInterpolation_BackwardFlat: return C::BackwardFlat;
        case W::CommodityCurveInterpolation_ForwardFlat: return C::ForwardFlat;
        case W::CommodityCurveInterpolation_KrugerCubic: return C::KrugerCubic;
        case W::CommodityCurveInterpolation_NaturalCubic: return C::NaturalCubic;
        default: break;
    }
    QuantraInvalidArgumentAt(
        field, field + " must be Linear, LogLinear, BackwardFlat, ForwardFlat, KrugerCubic or "
                       "NaturalCubic");
    return C::Linear; // unreachable
}

commodities::AverageQuote decodeQuote(
    const quantra::CommodityAverageQuote* q,
    const std::string& path,
    std::optional<std::string>& curveId) {
    if (q == nullptr) {
        QuantraInvalidArgumentAt(path, path + " is null");
    }
    commodities::AverageQuote out;
    out.price = requireScalarPresent(q->price(), path + ".price");
    out.periods = DecodeCommodityPeriods(q->periods(), path + ".periods");
    out.nearby = requireInt(q->nearby(), path + ".nearby");
    out.rollDaysBeforeExpiry =
        requireInt(q->roll_days_before_expiry(), path + ".roll_days_before_expiry");
    if (q->quantity_basis().has_value()) {
        switch (q->quantity_basis().value()) {
            case quantra::enums::CommodityQuantityBasis_PerPeriod:
                out.quantityBasis = commodities::QuantityBasis::PerPeriod;
                break;
            case quantra::enums::CommodityQuantityBasis_PerPricingDay:
                out.quantityBasis = commodities::QuantityBasis::PerPricingDay;
                break;
            default:
                QuantraInvalidArgumentAt(
                    path + ".quantity_basis", path + ".quantity_basis is not a known value");
        }
    }
    if (q->payment_calendar().has_value()) {
        out.paymentCalendar = CalendarToQL(q->payment_calendar().value());
    }
    if (q->payment_lag().has_value()) {
        out.paymentLag = q->payment_lag().value();
    }
    if (q->payment_convention().has_value()) {
        out.paymentConvention = ConventionToQL(q->payment_convention().value());
    }
    if (q->discounting_curve() != nullptr) {
        curveId = requireNonEmptyString(q->discounting_curve(), path + ".discounting_curve");
    }
    return out;
}

quantra::enums::CommodityMonthSource wireSource(commodities::MonthSource source) {
    switch (source) {
        case commodities::MonthSource::Quoted: return quantra::enums::CommodityMonthSource_Quoted;
        case commodities::MonthSource::Solved: return quantra::enums::CommodityMonthSource_Solved;
        case commodities::MonthSource::Assumed: return quantra::enums::CommodityMonthSource_Assumed;
        case commodities::MonthSource::Interpolated:
            return quantra::enums::CommodityMonthSource_Interpolated;
        case commodities::MonthSource::Extrapolated:
            return quantra::enums::CommodityMonthSource_Extrapolated;
    }
    return quantra::enums::CommodityMonthSource_Quoted;
}

flatbuffers::Offset<quantra::CommodityCurveReport> serializeReport(
    flatbuffers::grpc::MessageBuilder& builder, const commodities::CurveBuildReport& report) {
    using SensOffset = flatbuffers::Offset<quantra::CommodityQuoteSensitivity>;
    using WeightOffset = flatbuffers::Offset<quantra::CommodityQuoteWeight>;
    std::vector<flatbuffers::Offset<quantra::CommodityCurveMonth>> months;
    for (const auto& m : report.months) {
        flatbuffers::Offset<flatbuffers::Vector<SensOffset>> sens = 0;
        if (!m.sensitivities.empty()) {
            std::vector<SensOffset> rows;
            for (const auto& s : m.sensitivities) {
                flatbuffers::Offset<flatbuffers::String> month = 0;
                if (s.month) {
                    month = builder.CreateString(*s.month);
                }
                quantra::CommodityQuoteSensitivityBuilder sb(builder);
                if (s.month) {
                    sb.add_month(month);
                }
                if (s.averageQuote) {
                    sb.add_average_quote(static_cast<int>(*s.averageQuote));
                }
                sb.add_value(s.value);
                rows.push_back(sb.Finish());
            }
            sens = builder.CreateVector(rows);
        }
        auto name = builder.CreateString(m.month);
        quantra::CommodityCurveMonthBuilder mb(builder);
        mb.add_month(name);
        mb.add_price(m.price);
        mb.add_source(wireSource(m.source));
        if (!m.sensitivities.empty()) {
            mb.add_sensitivities(sens);
        }
        months.push_back(mb.Finish());
    }
    auto monthsVec = builder.CreateVector(months);

    std::vector<flatbuffers::Offset<quantra::CommodityAverageQuoteResult>> quotes;
    for (const auto& q : report.averageQuotes) {
        flatbuffers::Offset<flatbuffers::Vector<WeightOffset>> weights = 0;
        const bool details = !q.weights.empty();
        if (details) {
            std::vector<WeightOffset> rows;
            for (const auto& w : q.weights) {
                rows.push_back(
                    quantra::CreateCommodityQuoteWeightDirect(builder, w.month.c_str(), w.weight));
            }
            weights = builder.CreateVector(rows);
        }
        quantra::CommodityAverageQuoteResultBuilder qb(builder);
        qb.add_repriced(q.repriced);
        qb.add_difference(q.difference);
        qb.add_status(
            q.status == commodities::QuoteStatus::Used
                ? quantra::enums::CommodityQuoteStatus_Used
                : quantra::enums::CommodityQuoteStatus_Redundant);
        if (details) {
            qb.add_weights(weights);
            qb.add_constant(q.constant);
        }
        quotes.push_back(qb.Finish());
    }
    auto quotesVec = builder.CreateVector(quotes);

    quantra::CommodityCurveReportBuilder rb(builder);
    rb.add_status(
        report.status == commodities::CurveStatus::Unique
            ? quantra::enums::CommodityCurveStatus_Unique
            : quantra::enums::CommodityCurveStatus_Underdetermined);
    if (report.rule) {
        rb.add_rule(
            *report.rule == commodities::CurveRule::Flat
                ? quantra::enums::CommodityCurveRule_Flat
                : quantra::enums::CommodityCurveRule_Smooth);
    }
    rb.add_unknowns(report.unknowns);
    rb.add_equations(report.equations);
    rb.add_rank(report.rank);
    rb.add_max_repricing_difference(report.maxRepricingDifference);
    rb.add_months(monthsVec);
    rb.add_average_quotes(quotesVec);
    if (report.maxSensitivity) {
        rb.add_max_sensitivity(*report.maxSensitivity);
    }
    return rb.Finish();
}

/// Run `decode`; an invalid argument that names its field (QuantraError::path)
/// is raised again with the same message and InvalidInput details (the path,
/// and the curve and average quote indexes when known).
template <typename Decode>
auto withInputDetails(
    std::optional<std::size_t> curve, std::optional<std::size_t> quote, Decode decode)
    -> decltype(decode()) {
    try {
        return decode();
    } catch (const QuantraInvalidArgument& e) {
        if (e.path().empty() || !e.details().empty()) {
            throw;
        }
        commodities::CurveBuildRefusal r;
        r.reason = commodities::CurveBuildRefusal::Reason::InvalidInput;
        r.path = e.path();
        r.averageQuote = quote;
        throw QuantraInvalidArgument(e.what(), CurveBuildRefusalJson(r, curve));
    }
}

[[noreturn]] void invalidAt(
    const std::string& path, const std::string& message, std::optional<std::size_t> curve) {
    commodities::CurveBuildRefusal r;
    r.reason = commodities::CurveBuildRefusal::Reason::InvalidInput;
    r.path = path;
    throw QuantraInvalidArgument(message, CurveBuildRefusalJson(r, curve));
}

CommodityCurveBuildSpec extractBuild(const quantra::CommodityCurveBuild* entry, std::size_t index) {
    const std::string base = "curves[" + std::to_string(index) + "]";
    if (entry == nullptr) {
        invalidAt(base, base + " is null", index);
    }
    const auto* spec = entry->contract();
    if (spec == nullptr) {
        invalidAt(base + ".contract", base + ".contract is required", index);
    }
    CommodityCurveBuildSpec out{withInputDetails(
        index, std::nullopt, [&] { return DecodeCommodityContract(spec, base + ".contract"); })};
    if (entry->interpolation().has_value()) {
        out.interpolation = withInputDetails(index, std::nullopt, [&] {
            return decodeInterpolation(entry->interpolation().value(), base + ".interpolation");
        });
    }
    out.extrapolateFlat = entry->extrapolate_flat();
    // DecodeCommodityContract has checked the calendar is present.
    out.pricingCalendarWire = static_cast<std::int8_t>(spec->pricing_calendar().value());
    if (const auto* holidays = spec->pricing_holidays()) {
        std::vector<std::string> sent;
        sent.reserve(holidays->size());
        for (flatbuffers::uoffset_t i = 0; i < holidays->size(); ++i) {
            sent.push_back(holidays->Get(i)->str());
        }
        out.pricingHolidays = std::move(sent);
    }
    out.fixingsGiven = spec->fixings() != nullptr;
    if (const auto* quotes = entry->average_quotes()) {
        for (flatbuffers::uoffset_t k = 0; k < quotes->size(); ++k) {
            std::optional<std::string> curveId;
            out.averageQuotes.push_back(withInputDetails(index, std::size_t(k), [&] {
                return decodeQuote(
                    quotes->Get(k), base + ".average_quotes[" + std::to_string(k) + "]", curveId);
            }));
            out.discountingCurveIds.push_back(curveId);
        }
    }
    if (entry->rule().has_value()) {
        switch (entry->rule().value()) {
            case quantra::enums::CommodityCurveRule_Flat:
                out.rule = commodities::CurveRule::Flat;
                break;
            case quantra::enums::CommodityCurveRule_Smooth:
                out.rule = commodities::CurveRule::Smooth;
                break;
            default: invalidAt(base + ".rule", base + ".rule must be Flat or Smooth", index);
        }
    }
    return out;
}

flatbuffers::Offset<quantra::CommodityContractSpec> serializeContract(
    flatbuffers::grpc::MessageBuilder& builder, const BuiltCommodityCurveResult& curve) {
    std::vector<flatbuffers::Offset<quantra::CommodityContractMonth>> months;
    months.reserve(curve.months.size());
    for (const auto& m : curve.months) {
        auto month = builder.CreateString(m.month);
        auto lastTrade = builder.CreateString(DateToIso(m.lastTradeDate));
        quantra::CommodityContractMonthBuilder mb(builder);
        mb.add_month(month);
        mb.add_last_trade_date(lastTrade);
        // Absent stays absent: an expired month without a price is read
        // through fixings, never a 0 stand-in.
        if (m.price) {
            mb.add_price(*m.price);
        }
        months.push_back(mb.Finish());
    }
    auto monthsVec = builder.CreateVector(months);

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CommodityContractFixing>>>
        fixingsVec = 0;
    if (curve.fixingsGiven) {
        std::vector<flatbuffers::Offset<quantra::CommodityContractFixing>> fixings;
        fixings.reserve(curve.fixings.size());
        for (const auto& f : curve.fixings) {
            fixings.push_back(
                quantra::CreateCommodityContractFixingDirect(
                    builder, f.month.c_str(), DateToIso(f.date).c_str(), f.value));
        }
        fixingsVec = builder.CreateVector(fixings);
    }

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> holidaysVec =
        0;
    if (curve.pricingHolidays) {
        holidaysVec = builder.CreateVectorOfStrings(*curve.pricingHolidays);
    }
    auto id = builder.CreateString(curve.id);

    quantra::CommodityContractSpecBuilder cb(builder);
    cb.add_id(id);
    cb.add_pricing_calendar(static_cast<quantra::enums::Calendar>(curve.pricingCalendarWire));
    if (curve.pricingHolidays) {
        cb.add_pricing_holidays(holidaysVec);
    }
    cb.add_months(monthsVec);
    if (curve.fixingsGiven) {
        cb.add_fixings(fixingsVec);
    }
    return cb.Finish();
}

} // namespace

bool CommodityCurveBuildMapper::needsPricingRegistry(
    const CommodityCurveBuildInputs& inputs) const {
    for (const auto& build : inputs.builds) {
        for (const auto& id : build.discountingCurveIds) {
            if (id) {
                return true;
            }
        }
    }
    return false;
}

CommodityCurveBuildInputs CommodityCurveBuildMapper::toInputs(
    const quantra::BuildCommodityCurvesRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("BuildCommodityCurvesRequest is null");
    }
    const auto* pricing = req->pricing();
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("pricing is required");
    }
    CommodityCurveBuildInputs inputs;
    inputs.asOf = withInputDetails(std::nullopt, std::nullopt, [&] {
        return requireIsoDate(pricing->as_of_date(), "pricing.as_of_date");
    });
    if (pricing->commodity() != nullptr) {
        invalidAt(
            "pricing.commodity",
            "pricing.commodity is not accepted by /build-commodity-curves: the contracts to "
            "complete are given under curves[].contract",
            std::nullopt);
    }
    const auto* curves = req->curves();
    if (curves == nullptr || curves->size() == 0) {
        invalidAt("curves", "curves is required and must be non-empty", std::nullopt);
    }
    inputs.includeDetails = req->include_details();
    inputs.builds.reserve(curves->size());
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); ++i) {
        inputs.builds.push_back(extractBuild(curves->Get(i), i));
    }
    return inputs;
}

flatbuffers::Offset<quantra::BuildCommodityCurvesResponse> CommodityCurveBuildMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const CommodityCurveBuildResultSet& result) const {
    std::vector<flatbuffers::Offset<quantra::BuiltCommodityCurve>> rows;
    rows.reserve(result.curves.size());
    for (const auto& curve : result.curves) {
        auto contract = serializeContract(builder, curve);
        auto report = serializeReport(builder, curve.report);
        quantra::BuiltCommodityCurveBuilder rb(builder);
        rb.add_contract(contract);
        rb.add_report(report);
        rows.push_back(rb.Finish());
    }
    auto rowsVec = builder.CreateVector(rows);
    quantra::BuildCommodityCurvesResponseBuilder rb(builder);
    rb.add_curves(rowsVec);
    return rb.Finish();
}

} // namespace quantra
