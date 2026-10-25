#include "implied_forwards_mapper.h"

#include "date_convert.h"
#include "error.h"

namespace quantra {

namespace {

std::optional<double> opt(const flatbuffers::Optional<double>& v) {
    return v.has_value() ? std::optional<double>(v.value()) : std::nullopt;
}

} // namespace

ImpliedForwardsInputs ImpliedForwardsMapper::toInputs(
    const quantra::CalibrateImpliedForwardsRequest* req) const {
    if (!req || !req->pricing()) {
        QUANTRA_INVALID_ARGUMENT("CalibrateImpliedForwardsRequest requires pricing");
    }
    if (!req->sheet()) {
        QUANTRA_INVALID_ARGUMENT("CalibrateImpliedForwardsRequest requires sheet");
    }
    if (!req->discounting_curve() || req->discounting_curve()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateImpliedForwardsRequest requires discounting_curve");
    }
    if (!req->dividend_curve_id() || req->dividend_curve_id()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateImpliedForwardsRequest requires dividend_curve_id");
    }
    const auto* sheet = req->sheet();
    if (!sheet->id() || sheet->id()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("sheet.id is required");
    }
    if (!sheet->underlying_id() || sheet->underlying_id()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("sheet.underlying_id is required");
    }
    if (!sheet->quotes() || sheet->quotes()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("sheet.quotes must contain at least one quote");
    }

    ImpliedForwardsInputs in;
    in.sheetId = sheet->id()->str();
    in.underlyingId = sheet->underlying_id()->str();
    in.discountingCurveId = req->discounting_curve()->str();
    in.dividendCurveId = req->dividend_curve_id()->str();
    switch (sheet->option_style()) {
        case quantra::enums::ExerciseType_European: in.style = OptionSheetStyle::European; break;
        case quantra::enums::ExerciseType_Bermudan: in.style = OptionSheetStyle::Bermudan; break;
        case quantra::enums::ExerciseType_American: in.style = OptionSheetStyle::American; break;
        default: QUANTRA_INVALID_ARGUMENT("sheet.option_style is not a known ExerciseType");
    }
    if (req->filter()) {
        const auto* f = req->filter();
        in.filter.maxRelativeSpread = f->max_relative_spread();
        in.filter.minQuotesPerExpiry = f->min_quotes_per_expiry();
        in.filter.moneynessWindow = f->moneyness_window();
        in.filter.minOpenInterest = opt(f->min_open_interest());
    }
    in.quotes.reserve(sheet->quotes()->size());
    for (flatbuffers::uoffset_t i = 0; i < sheet->quotes()->size(); ++i) {
        const auto* q = sheet->quotes()->Get(i);
        if (!q || !q->expiry() || q->expiry()->size() == 0) {
            QUANTRA_INVALID_ARGUMENT("sheet.quotes[" + std::to_string(i) + "].expiry is required");
        }
        OptionQuoteLine line;
        line.expiryIso = q->expiry()->str();
        line.expiry = DateToQL(line.expiryIso);
        line.strike = opt(q->strike());
        line.callBid = opt(q->call_bid());
        line.callAsk = opt(q->call_ask());
        line.putBid = opt(q->put_bid());
        line.putAsk = opt(q->put_ask());
        line.callOpenInterest = opt(q->call_open_interest());
        line.putOpenInterest = opt(q->put_open_interest());
        in.quotes.push_back(std::move(line));
    }
    return in;
}

flatbuffers::Offset<quantra::CalibrateImpliedForwardsResponse> ImpliedForwardsMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& b, const ImpliedForwardsResult& r) const {
    // Per-expiry report.
    std::vector<flatbuffers::Offset<quantra::ImpliedForwardExpiry>> expiries;
    std::vector<flatbuffers::Offset<quantra::ImpliedForwardPoint>> forwards;
    expiries.reserve(r.expiries.size());
    for (const auto& e : r.expiries) {
        std::vector<flatbuffers::Offset<quantra::DroppedQuote>> dropped;
        dropped.reserve(e.dropped.size());
        for (const auto& d : e.dropped) {
            auto expOff = b.CreateString(d.expiryIso);
            auto reasonOff = b.CreateString(d.reason);
            quantra::DroppedQuoteBuilder db(b);
            db.add_expiry(expOff);
            if (d.strike) db.add_strike(*d.strike);
            db.add_reason(reasonOff);
            dropped.push_back(db.Finish());
        }
        auto droppedOff = b.CreateVector(dropped);
        auto expiryOff = b.CreateString(e.expiryIso);
        flatbuffers::Offset<flatbuffers::String> reasonOff;
        if (!e.usable) reasonOff = b.CreateString(e.droppedReason);
        quantra::ImpliedForwardExpiryBuilder eb(b);
        eb.add_expiry(expiryOff);
        eb.add_time(e.time);
        eb.add_quotes_used(e.quotesUsed);
        eb.add_quotes_dropped(static_cast<uint32_t>(e.dropped.size()));
        eb.add_dropped(droppedOff);
        eb.add_usable(e.usable);
        if (!e.usable) eb.add_dropped_reason(reasonOff);
        if (e.forward) eb.add_forward(*e.forward);
        if (e.impliedDiscountFactor) eb.add_implied_discount_factor(*e.impliedDiscountFactor);
        if (e.curveDiscountFactor) eb.add_curve_discount_factor(*e.curveDiscountFactor);
        if (e.impliedFundingBp) eb.add_implied_funding_bp(*e.impliedFundingBp);
        if (e.impliedDividendYield) eb.add_implied_dividend_yield(*e.impliedDividendYield);
        if (e.fitResidualRmse) eb.add_fit_residual_rmse(*e.fitResidualRmse);
        if (e.maxAbsResidual) eb.add_max_abs_residual(*e.maxAbsResidual);
        expiries.push_back(eb.Finish());

        if (e.usable) {
            auto fExpOff = b.CreateString(e.expiryIso);
            quantra::ImpliedForwardPointBuilder fb(b);
            fb.add_expiry(fExpOff);
            fb.add_time(e.time);
            fb.add_forward(*e.forward);
            fb.add_implied_dividend_yield(*e.impliedDividendYield);
            forwards.push_back(fb.Finish());
        }
    }
    auto expiriesOff = b.CreateVector(expiries);
    auto forwardsOff = b.CreateVector(forwards);

    // Dividend curve spec: InterpolatedZero, Act/365F, linear, continuous.
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points;
    points.reserve(r.dividendCurve.size());
    for (const auto& p : r.dividendCurve) {
        auto dateOff = b.CreateString(p.dateIso);
        quantra::ZeroRatePointBuilder zb(b);
        zb.add_date(dateOff);
        zb.add_zero_rate(p.zeroRate);
        zb.add_compounding(quantra::enums::Compounding_Continuous);
        zb.add_frequency(quantra::enums::Frequency_Annual);
        auto zOff = zb.Finish();
        points.push_back(
            quantra::CreatePointsWrapper(b, quantra::Point_ZeroRatePoint, zOff.Union()));
    }
    auto pointsOff = b.CreateVector(points);
    auto curveIdOff = b.CreateString(r.dividendCurveId);
    auto refDateOff = b.CreateString(r.asOfIso);
    quantra::TermStructureBuilder tb(b);
    tb.add_id(curveIdOff);
    tb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    tb.add_interpolator(quantra::enums::Interpolator_Linear);
    tb.add_bootstrap_trait(quantra::enums::BootstrapTrait_InterpolatedZero);
    tb.add_points(pointsOff);
    tb.add_reference_date(refDateOff);
    auto curveOff = tb.Finish();

    quantra::QuoteFilterBuilder qb(b);
    qb.add_max_relative_spread(r.filter.maxRelativeSpread);
    qb.add_min_quotes_per_expiry(r.filter.minQuotesPerExpiry);
    qb.add_moneyness_window(r.filter.moneynessWindow);
    if (r.filter.minOpenInterest) qb.add_min_open_interest(*r.filter.minOpenInterest);
    auto filterOff = qb.Finish();

    auto sheetIdOff = b.CreateString(r.sheetId);
    auto underlyingOff = b.CreateString(r.underlyingId);
    quantra::CalibrateImpliedForwardsResponseBuilder rb(b);
    rb.add_sheet_id(sheetIdOff);
    rb.add_underlying_id(underlyingOff);
    rb.add_spot(r.spot);
    rb.add_filter(filterOff);
    rb.add_expiries(expiriesOff);
    rb.add_forwards(forwardsOff);
    rb.add_dividend_curve_spec(curveOff);
    return rb.Finish();
}

} // namespace quantra
