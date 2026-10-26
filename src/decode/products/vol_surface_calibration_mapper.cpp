#include "vol_surface_calibration_mapper.h"

#include "date_convert.h"
#include "error.h"
#include "request_validation.h"

namespace quantra {

namespace {

std::optional<double> opt(const flatbuffers::Optional<double>& v) {
    return v.has_value() ? std::optional<double>(v.value()) : std::nullopt;
}

quantra::enums::VolSurfaceFitMethod toWire(VolSurfaceFitMethod m) {
    return m == VolSurfaceFitMethod::SviPerExpiry
               ? quantra::enums::VolSurfaceFitMethod_SviPerExpiry
               : quantra::enums::VolSurfaceFitMethod_AndreasenHuge;
}

flatbuffers::Offset<quantra::QuoteFilter> buildFilter(
    flatbuffers::grpc::MessageBuilder& b, const QuoteFilterData& f) {
    quantra::QuoteFilterBuilder qb(b);
    qb.add_max_relative_spread(f.maxRelativeSpread);
    qb.add_min_quotes_per_expiry(f.minQuotesPerExpiry);
    qb.add_moneyness_window(f.moneynessWindow);
    if (f.minOpenInterest) qb.add_min_open_interest(*f.minOpenInterest);
    return qb.Finish();
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::DroppedQuote>>> buildDropped(
    flatbuffers::grpc::MessageBuilder& b, const std::vector<DroppedQuoteData>& dropped) {
    std::vector<flatbuffers::Offset<quantra::DroppedQuote>> v;
    v.reserve(dropped.size());
    for (const auto& d : dropped) {
        auto expOff = b.CreateString(d.expiryIso);
        auto reasonOff = b.CreateString(d.reason);
        quantra::DroppedQuoteBuilder db(b);
        db.add_expiry(expOff);
        if (d.strike) db.add_strike(*d.strike);
        db.add_reason(reasonOff);
        v.push_back(db.Finish());
    }
    return b.CreateVector(v);
}

} // namespace

VolSurfaceCalibrationInputs VolSurfaceCalibrationMapper::toInputs(
    const quantra::CalibrateVolSurfaceRequest* req) const {
    if (!req || !req->pricing()) {
        QUANTRA_INVALID_ARGUMENT("CalibrateVolSurfaceRequest requires pricing");
    }
    if (!req->sheet()) {
        QUANTRA_INVALID_ARGUMENT("CalibrateVolSurfaceRequest requires sheet");
    }
    if (!req->discounting_curve() || req->discounting_curve()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateVolSurfaceRequest requires discounting_curve");
    }
    if (!req->vol_surface_id() || req->vol_surface_id()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("CalibrateVolSurfaceRequest requires vol_surface_id");
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

    VolSurfaceCalibrationInputs in;
    ImpliedForwardsInputs& base = in.base;
    base.sheetId = sheet->id()->str();
    base.underlyingId = sheet->underlying_id()->str();
    base.discountingCurveId = req->discounting_curve()->str();
    base.dividendCurveId = "unused";
    switch (sheet->option_style()) {
        case quantra::enums::ExerciseType_European: base.style = OptionSheetStyle::European; break;
        case quantra::enums::ExerciseType_Bermudan: base.style = OptionSheetStyle::Bermudan; break;
        case quantra::enums::ExerciseType_American: base.style = OptionSheetStyle::American; break;
        default: QUANTRA_INVALID_ARGUMENT("sheet.option_style is not a known ExerciseType");
    }
    if (req->filter()) {
        const auto* f = req->filter();
        base.filter.maxRelativeSpread = f->max_relative_spread();
        base.filter.minQuotesPerExpiry = f->min_quotes_per_expiry();
        base.filter.moneynessWindow = f->moneyness_window();
        base.filter.minOpenInterest = opt(f->min_open_interest());
    }
    base.quotes.reserve(sheet->quotes()->size());
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
        base.quotes.push_back(std::move(line));
    }

    if (req->forwards()) {
        std::vector<VolSurfaceForwardInput> fwds;
        for (flatbuffers::uoffset_t i = 0; i < req->forwards()->size(); ++i) {
            const auto* f = req->forwards()->Get(i);
            if (!f || !f->expiry() || f->expiry()->size() == 0) {
                QUANTRA_INVALID_ARGUMENT("forwards[" + std::to_string(i) + "].expiry is required");
            }
            if (!f->usable()) continue;
            if (!f->forward().has_value() || !f->implied_discount_factor().has_value()) {
                QUANTRA_INVALID_ARGUMENT(
                    "forwards[" + std::to_string(i) + "] (" + f->expiry()->str() +
                    ") is usable but lacks forward or implied_discount_factor");
            }
            VolSurfaceForwardInput v;
            v.expiryIso = f->expiry()->str();
            v.expiry = DateToQL(v.expiryIso);
            v.forward = f->forward().value();
            v.impliedDiscountFactor = f->implied_discount_factor().value();
            fwds.push_back(v);
        }
        in.forwards = std::move(fwds);
    }

    switch (req->method()) {
        case quantra::enums::VolSurfaceFitMethod_AndreasenHuge:
            in.method = VolSurfaceFitMethod::AndreasenHuge;
            break;
        case quantra::enums::VolSurfaceFitMethod_SviPerExpiry:
            in.method = VolSurfaceFitMethod::SviPerExpiry;
            break;
        default: QUANTRA_INVALID_ARGUMENT("method is not a known VolSurfaceFitMethod");
    }
    if (req->output_grid()) {
        const auto* g = req->output_grid();
        if (g->strikes_as_moneyness()) {
            for (flatbuffers::uoffset_t j = 0; j < g->strikes_as_moneyness()->size(); ++j)
                in.moneyness.push_back(g->strikes_as_moneyness()->Get(j));
        }
        if (g->expiries()) {
            for (flatbuffers::uoffset_t i = 0; i < g->expiries()->size(); ++i) {
                in.gridExpiries.push_back(requirePeriod(
                    g->expiries()->Get(i), "output_grid.expiries[" + std::to_string(i) + "]"));
            }
        }
    }
    in.volSurfaceId = req->vol_surface_id()->str();
    if (req->svi()) {
        in.enforceButterfly = req->svi()->enforce_butterfly();
        in.enforceCalendar = req->svi()->enforce_calendar();
    }
    return in;
}

flatbuffers::Offset<quantra::CalibrateVolSurfaceResponse> VolSurfaceCalibrationMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& b, const VolSurfaceCalibrationResult& r) const {
    // Forwards used.
    std::vector<flatbuffers::Offset<quantra::VolSurfaceForward>> fwds;
    for (const auto& f : r.forwards) {
        auto expOff = b.CreateString(f.expiryIso);
        quantra::VolSurfaceForwardBuilder fb(b);
        fb.add_expiry(expOff);
        fb.add_time(f.time);
        fb.add_forward(f.forward);
        fb.add_implied_discount_factor(f.impliedDiscountFactor);
        fwds.push_back(fb.Finish());
    }
    auto fwdsOff = b.CreateVector(fwds);

    // Per-expiry report.
    std::vector<flatbuffers::Offset<quantra::VolSurfaceExpiryReport>> reps;
    for (const auto& e : r.expiries) {
        auto droppedOff = buildDropped(b, e.dropped);
        auto expOff = b.CreateString(e.expiryIso);
        flatbuffers::Offset<flatbuffers::String> reasonOff, bActOff, cActOff;
        if (!e.usable) reasonOff = b.CreateString(e.droppedReason);
        if (!e.butterflyAction.empty()) bActOff = b.CreateString(e.butterflyAction);
        if (!e.calendarAction.empty()) cActOff = b.CreateString(e.calendarAction);
        flatbuffers::Offset<quantra::SviParameters> sviOff;
        if (e.svi) {
            sviOff = quantra::CreateSviParameters(
                b, e.svi->a, e.svi->b, e.svi->sigma, e.svi->rho, e.svi->m);
        }
        quantra::VolSurfaceExpiryReportBuilder eb(b);
        eb.add_expiry(expOff);
        eb.add_time(e.time);
        eb.add_usable(e.usable);
        if (!e.usable) eb.add_dropped_reason(reasonOff);
        if (e.forward) eb.add_forward(*e.forward);
        if (e.impliedDiscountFactor) eb.add_implied_discount_factor(*e.impliedDiscountFactor);
        eb.add_quotes_used(e.quotesUsed);
        eb.add_quotes_dropped(static_cast<uint32_t>(e.dropped.size()));
        eb.add_dropped(droppedOff);
        if (e.rmseVol) eb.add_rmse_vol(*e.rmseVol);
        if (e.maxAbsErrorVol) eb.add_max_abs_error_vol(*e.maxAbsErrorVol);
        if (e.svi) eb.add_svi(sviOff);
        if (e.butterflyMinG) eb.add_butterfly_min_g(*e.butterflyMinG);
        eb.add_butterfly_violation(e.butterflyViolation);
        if (!e.butterflyAction.empty()) eb.add_butterfly_action(bActOff);
        eb.add_calendar_violations(e.calendarViolations);
        if (e.calendarMaxClipVol) eb.add_calendar_max_clip_vol(*e.calendarMaxClipVol);
        if (!e.calendarAction.empty()) eb.add_calendar_action(cActOff);
        eb.add_grid_points_filled(e.gridPointsFilled);
        reps.push_back(eb.Finish());
    }
    auto repsOff = b.CreateVector(reps);

    auto moneynessOff = b.CreateVector(r.moneyness);
    auto gridStrikesOff = b.CreateVector(r.gridStrikes);
    auto gridExpiriesOff = b.CreateVectorOfStrings(r.gridExpiryIso);

    flatbuffers::Offset<quantra::AndreasenHugeCalibrationError> ahOff;
    if (r.andreasenHugeError) {
        ahOff = quantra::CreateAndreasenHugeCalibrationError(
            b, r.andreasenHugeError->minVol, r.andreasenHugeError->maxVol,
            r.andreasenHugeError->avgVol);
    }

    // The surface spec: BlackVolSpec SmileCube3D on NullCalendar / Unadjusted /
    // Act/365F with calendar-day expiries from the as-of date.
    std::vector<flatbuffers::Offset<quantra::Period>> periods;
    for (int days : r.gridExpiryDays) {
        quantra::PeriodBuilder pb(b);
        pb.add_n(days);
        pb.add_unit(quantra::enums::TimeUnit_Days);
        periods.push_back(pb.Finish());
    }
    auto periodsOff = b.CreateVector(periods);
    auto strikesOff = b.CreateVector(r.gridStrikes);
    auto valuesOff = b.CreateVector(r.gridVols);
    quantra::QuoteMatrix2DBuilder mb(b);
    mb.add_n_rows(static_cast<int32_t>(r.gridExpiryDays.size()));
    mb.add_n_cols(static_cast<int32_t>(r.gridStrikes.size()));
    mb.add_values(valuesOff);
    auto matrixOff = mb.Finish();
    auto refDateOff = b.CreateString(r.asOfIso);
    quantra::BlackVolBaseSpecBuilder bb(b);
    bb.add_reference_date(refDateOff);
    bb.add_calendar(quantra::enums::Calendar_NullCalendar);
    bb.add_business_day_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    bb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    bb.add_shape(quantra::enums::VolSurfaceShape_SmileCube3D);
    auto baseOff = bb.Finish();
    quantra::BlackVolSpecBuilder vb(b);
    vb.add_base(baseOff);
    vb.add_expiries(periodsOff);
    vb.add_strikes(strikesOff);
    vb.add_surface_vols(matrixOff);
    vb.add_allow_extrapolation(true);
    vb.add_surface_interpolator(quantra::enums::SurfaceInterpolator2D_Bilinear);
    auto payloadOff = vb.Finish();
    auto specIdOff = b.CreateString(r.volSurfaceId);
    quantra::VolSurfaceSpecBuilder sb(b);
    sb.add_id(specIdOff);
    sb.add_payload_type(quantra::VolPayload_BlackVolSpec);
    sb.add_payload(payloadOff.Union());
    auto specOff = sb.Finish();

    // Vanilla repricing.
    std::vector<flatbuffers::Offset<quantra::VanillaRepricePoint>> pts;
    pts.reserve(r.repricing.quotes.size());
    for (const auto& q : r.repricing.quotes) {
        auto expOff = b.CreateString(q.expiryIso);
        quantra::VanillaRepricePointBuilder pb(b);
        pb.add_expiry(expOff);
        pb.add_strike(q.strike);
        pb.add_option_type(
            q.isCall ? quantra::enums::EquityOptionType_Call
                     : quantra::enums::EquityOptionType_Put);
        pb.add_bid(q.bid);
        pb.add_ask(q.ask);
        pb.add_mid(q.mid);
        pb.add_market_vol(q.marketVol);
        pb.add_model_vol(q.modelVol);
        pb.add_model_price(q.modelPrice);
        pb.add_inside_bid_ask(q.insideBidAsk);
        pb.add_price_error(q.priceError);
        pb.add_vol_error(q.volError);
        pts.push_back(pb.Finish());
    }
    auto ptsOff = b.CreateVector(pts);
    quantra::VanillaRepricingBuilder rb(b);
    rb.add_count(r.repricing.count);
    rb.add_inside_bid_ask(r.repricing.insideBidAsk);
    rb.add_max_abs_price_error(r.repricing.maxAbsPriceError);
    rb.add_max_abs_vol_error(r.repricing.maxAbsVolError);
    rb.add_quotes(ptsOff);
    auto repricingOff = rb.Finish();

    auto filterOff = buildFilter(b, r.filter);
    auto sheetIdOff = b.CreateString(r.sheetId);
    auto underlyingOff = b.CreateString(r.underlyingId);
    auto sourceOff = b.CreateString(r.forwardsSource);
    quantra::CalibrateVolSurfaceResponseBuilder ob(b);
    ob.add_sheet_id(sheetIdOff);
    ob.add_underlying_id(underlyingOff);
    ob.add_spot(r.spot);
    ob.add_method(toWire(r.method));
    ob.add_forwards_source(sourceOff);
    ob.add_filter(filterOff);
    ob.add_forwards(fwdsOff);
    ob.add_expiries(repsOff);
    ob.add_strikes_as_moneyness(moneynessOff);
    ob.add_grid_strikes(gridStrikesOff);
    ob.add_grid_expiries(gridExpiriesOff);
    if (r.andreasenHugeError) ob.add_andreasen_huge_error(ahOff);
    ob.add_vol_surface_spec(specOff);
    ob.add_vanilla_repricing(repricingOff);
    return ob.Finish();
}

} // namespace quantra
