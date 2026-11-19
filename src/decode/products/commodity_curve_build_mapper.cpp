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
    std::optional<std::string>& curveId,
    bool spot) {
    if (q == nullptr) {
        QuantraInvalidArgumentAt(path, path + " is null");
    }
    commodities::AverageQuote out;
    out.price = requireScalarPresent(q->price(), path + ".price");
    out.periods = DecodeCommodityPeriods(q->periods(), path + ".periods");
    if (spot) {
        // A spot price has no contract months to choose from.
        for (const auto& [given, name] :
             {std::make_pair(q->nearby().has_value(), std::string("nearby")),
              std::make_pair(
                  q->roll_days_before_expiry().has_value(),
                  std::string("roll_days_before_expiry"))}) {
            if (given) {
                QuantraInvalidArgumentAt(
                    path + "." + name, path + "." + name +
                                           " is not used by a spot price build (it chooses a "
                                           "contract month); leave it out");
            }
        }
    } else {
        out.nearby = requireInt(q->nearby(), path + ".nearby");
        out.rollDaysBeforeExpiry =
            requireInt(q->roll_days_before_expiry(), path + ".roll_days_before_expiry");
    }
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

commodities::BasisInputs decodeBasis(
    const quantra::CommodityCurveBasis* basis, const std::string& path, std::string& baseId) {
    commodities::BasisInputs out;
    baseId = requireNonEmptyString(basis->base(), path + ".base");
    const std::string priceField = path + ".base_price";
    switch (requireEnum(basis->base_price(), priceField)) {
        case quantra::enums::CommodityBasisBasePrice_SameMonth:
            out.basePrice = commodities::BasisBasePrice::SameMonth;
            break;
        case quantra::enums::CommodityBasisBasePrice_MonthAverage:
            out.basePrice = commodities::BasisBasePrice::MonthAverage;
            break;
        default:
            QuantraInvalidArgumentAt(priceField, priceField + " must be SameMonth or MonthAverage");
    }
    out.baseMonthOffset = basis->base_month_offset();
    if (basis->base_nearby().has_value()) {
        out.baseNearby = basis->base_nearby().value();
    }
    if (basis->base_roll_days_before_expiry().has_value()) {
        out.baseRollDaysBeforeExpiry = basis->base_roll_days_before_expiry().value();
    }
    if (const auto* quotes = basis->quotes()) {
        for (flatbuffers::uoffset_t k = 0; k < quotes->size(); ++k) {
            const auto* q = quotes->Get(k);
            const std::string at = path + ".quotes[" + std::to_string(k) + "]";
            if (q == nullptr) {
                QuantraInvalidArgumentAt(at, at + " is null");
            }
            commodities::BasisQuote quote;
            quote.month = requireNonEmptyString(q->month(), at + ".month");
            quote.value = requireScalarPresent(q->value(), at + ".value");
            out.quotes.push_back(quote);
        }
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
        case commodities::MonthSource::Basis: return quantra::enums::CommodityMonthSource_Basis;
    }
    return quantra::enums::CommodityMonthSource_Quoted;
}

using SensOffset = flatbuffers::Offset<quantra::CommodityQuoteSensitivity>;

flatbuffers::Offset<flatbuffers::Vector<SensOffset>> serializeSensitivities(
    flatbuffers::grpc::MessageBuilder& builder,
    const std::vector<commodities::QuoteSensitivity>& sensitivities) {
    std::vector<SensOffset> rows;
    for (const auto& s : sensitivities) {
        flatbuffers::Offset<flatbuffers::String> month = 0;
        flatbuffers::Offset<flatbuffers::String> date = 0;
        if (s.month) {
            month = builder.CreateString(*s.month);
        }
        if (s.date) {
            date = builder.CreateString(*s.date);
        }
        quantra::CommodityQuoteSensitivityBuilder sb(builder);
        if (s.month) {
            sb.add_month(month);
        }
        if (s.averageQuote) {
            sb.add_average_quote(static_cast<int>(*s.averageQuote));
        }
        sb.add_value(s.value);
        if (s.date) {
            sb.add_date(date);
        }
        rows.push_back(sb.Finish());
    }
    return builder.CreateVector(rows);
}

flatbuffers::Offset<quantra::CommodityCurveReport> serializeReport(
    flatbuffers::grpc::MessageBuilder& builder,
    const commodities::CurveBuildReport& report,
    bool spot) {
    using WeightOffset = flatbuffers::Offset<quantra::CommodityQuoteWeight>;
    std::vector<flatbuffers::Offset<quantra::CommodityCurveMonth>> months;
    for (const auto& m : report.months) {
        flatbuffers::Offset<flatbuffers::Vector<SensOffset>> sens = 0;
        if (!m.sensitivities.empty()) {
            sens = serializeSensitivities(builder, m.sensitivities);
        }
        auto name = builder.CreateString(m.month);
        quantra::CommodityCurveMonthBuilder mb(builder);
        mb.add_month(name);
        mb.add_price(m.price);
        mb.add_source(wireSource(m.source));
        if (!m.sensitivities.empty()) {
            mb.add_sensitivities(sens);
        }
        if (m.basePrice) {
            mb.add_base_price(*m.basePrice);
        }
        if (m.basis) {
            mb.add_basis(*m.basis);
        }
        months.push_back(mb.Finish());
    }
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CommodityCurveMonth>>>
        monthsVec = 0;
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CommodityCurvePoint>>>
        pointsVec = 0;
    if (spot) {
        std::vector<flatbuffers::Offset<quantra::CommodityCurvePoint>> points;
        for (const auto& pt : report.points) {
            flatbuffers::Offset<flatbuffers::Vector<SensOffset>> sens = 0;
            if (!pt.sensitivities.empty()) {
                sens = serializeSensitivities(builder, pt.sensitivities);
            }
            auto date = builder.CreateString(DateToIso(pt.date));
            quantra::CommodityCurvePointBuilder pb(builder);
            pb.add_date(date);
            pb.add_price(pt.price);
            pb.add_source(wireSource(pt.source));
            if (!pt.sensitivities.empty()) {
                pb.add_sensitivities(sens);
            }
            points.push_back(pb.Finish());
        }
        pointsVec = builder.CreateVector(points);
    } else {
        monthsVec = builder.CreateVector(months);
    }

    std::vector<flatbuffers::Offset<quantra::CommodityAverageQuoteResult>> quotes;
    for (const auto& q : report.averageQuotes) {
        flatbuffers::Offset<flatbuffers::Vector<WeightOffset>> weights = 0;
        const bool details = !q.weights.empty();
        if (details) {
            std::vector<WeightOffset> rows;
            for (const auto& w : q.weights) {
                rows.push_back(
                    quantra::CreateCommodityQuoteWeightDirect(
                        builder, w.month.empty() ? nullptr : w.month.c_str(), w.weight,
                        w.date.empty() ? nullptr : w.date.c_str()));
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
    if (!spot) {
        rb.add_months(monthsVec);
    }
    rb.add_average_quotes(quotesVec);
    if (report.maxSensitivity) {
        rb.add_max_sensitivity(*report.maxSensitivity);
    }
    if (spot) {
        rb.add_points(pointsVec);
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
    const auto* spotSpec = entry->spot();
    if ((spec == nullptr) == (spotSpec == nullptr)) {
        invalidAt(
            base,
            base + ": give exactly one of contract (a futures contract to build) and spot (a "
                   "spot price to build)",
            index);
    }
    CommodityCurveBuildSpec out;
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* holidays = nullptr;
    if (spec != nullptr) {
        out.contract = withInputDetails(
            index, std::nullopt, [&] { return DecodeCommodityContract(spec, base + ".contract"); });
        // DecodeCommodityContract has checked the calendar is present.
        out.pricingCalendarWire = static_cast<std::int8_t>(spec->pricing_calendar().value());
        holidays = spec->pricing_holidays();
        out.fixingsGiven = spec->fixings() != nullptr;
    } else {
        out.spot = withInputDetails(index, std::nullopt, [&] {
            return DecodeCommoditySpot(spotSpec, base + ".spot", false);
        });
        out.pricingCalendarWire = static_cast<std::int8_t>(spotSpec->pricing_calendar().value());
        out.spotInterpolationWire = static_cast<std::int8_t>(spotSpec->interpolation().value());
        holidays = spotSpec->pricing_holidays();
        out.fixingsGiven = spotSpec->fixings() != nullptr;
        if (entry->interpolation().has_value()) {
            invalidAt(
                base + ".interpolation",
                base + ".interpolation is not used by a spot price build: the points are read "
                       "with spot.interpolation",
                index);
        }
    }
    if (entry->interpolation().has_value()) {
        out.interpolation = withInputDetails(index, std::nullopt, [&] {
            return decodeInterpolation(entry->interpolation().value(), base + ".interpolation");
        });
    }
    out.extrapolateFlat = entry->extrapolate_flat();
    if (holidays != nullptr) {
        std::vector<std::string> sent;
        sent.reserve(holidays->size());
        for (flatbuffers::uoffset_t i = 0; i < holidays->size(); ++i) {
            sent.push_back(holidays->Get(i)->str());
        }
        out.pricingHolidays = std::move(sent);
    }
    if (const auto* quotes = entry->average_quotes()) {
        for (flatbuffers::uoffset_t k = 0; k < quotes->size(); ++k) {
            std::optional<std::string> curveId;
            out.averageQuotes.push_back(withInputDetails(index, std::size_t(k), [&] {
                return decodeQuote(
                    quotes->Get(k), base + ".average_quotes[" + std::to_string(k) + "]", curveId,
                    spotSpec != nullptr);
            }));
            out.discountingCurveIds.push_back(curveId);
        }
    }
    if (const auto* basis = entry->basis()) {
        if (spotSpec != nullptr) {
            invalidAt(
                base + ".basis",
                base + ".basis is not used by a spot price build (basis curves are contract "
                       "builds)",
                index);
        }
        out.basis = withInputDetails(
            index, std::nullopt, [&] { return decodeBasis(basis, base + ".basis", out.baseId); });
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

flatbuffers::Offset<quantra::CommoditySpotSpec> serializeSpot(
    flatbuffers::grpc::MessageBuilder& builder, const BuiltCommodityCurveResult& curve) {
    std::vector<flatbuffers::Offset<quantra::CommoditySpotPoint>> points;
    points.reserve(curve.points.size());
    for (const auto& p : curve.points) {
        auto date = builder.CreateString(DateToIso(p.date));
        quantra::CommoditySpotPointBuilder pb(builder);
        pb.add_date(date);
        if (p.price) {
            pb.add_price(*p.price);
        }
        points.push_back(pb.Finish());
    }
    auto pointsVec = builder.CreateVector(points);

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CommoditySpotFixing>>>
        fixingsVec = 0;
    if (curve.fixingsGiven) {
        std::vector<flatbuffers::Offset<quantra::CommoditySpotFixing>> fixings;
        fixings.reserve(curve.spotFixings.size());
        for (const auto& f : curve.spotFixings) {
            fixings.push_back(
                quantra::CreateCommoditySpotFixingDirect(
                    builder, DateToIso(f.date).c_str(), f.value));
        }
        fixingsVec = builder.CreateVector(fixings);
    }
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> holidaysVec =
        0;
    if (curve.pricingHolidays) {
        holidaysVec = builder.CreateVectorOfStrings(*curve.pricingHolidays);
    }
    auto id = builder.CreateString(curve.id);

    quantra::CommoditySpotSpecBuilder sb(builder);
    sb.add_id(id);
    sb.add_pricing_calendar(static_cast<quantra::enums::Calendar>(curve.pricingCalendarWire));
    if (curve.pricingHolidays) {
        sb.add_pricing_holidays(holidaysVec);
    }
    sb.add_interpolation(static_cast<quantra::enums::Interpolator>(curve.spotInterpolationWire));
    sb.add_extrapolate_flat(curve.extrapolateFlat);
    sb.add_points(pointsVec);
    if (curve.fixingsGiven) {
        sb.add_fixings(fixingsVec);
    }
    return sb.Finish();
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
        flatbuffers::Offset<quantra::CommodityContractSpec> contract = 0;
        flatbuffers::Offset<quantra::CommoditySpotSpec> spot = 0;
        if (curve.isSpot) {
            spot = serializeSpot(builder, curve);
        } else {
            contract = serializeContract(builder, curve);
        }
        auto report = serializeReport(builder, curve.report, curve.isSpot);
        quantra::BuiltCommodityCurveBuilder rb(builder);
        if (curve.isSpot) {
            rb.add_spot(spot);
        } else {
            rb.add_contract(contract);
        }
        rb.add_report(report);
        rows.push_back(rb.Finish());
    }
    auto rowsVec = builder.CreateVector(rows);
    quantra::BuildCommodityCurvesResponseBuilder rb(builder);
    rb.add_curves(rowsVec);
    return rb.Finish();
}

} // namespace quantra
