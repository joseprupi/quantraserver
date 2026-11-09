#include "vol_surface_validation_mapper.h"

#include "date_convert.h"
#include "error.h"
#include "implied_forwards_mapper.h"
#include "structured_note_mapper.h"
#include "vol_surface_calibration_mapper.h"

namespace quantra {

namespace {

using flatbuffers::Offset;

Offset<quantra::QuoteFilter> buildFilter(
    flatbuffers::grpc::MessageBuilder& b, const QuoteFilterData& f) {
    quantra::QuoteFilterBuilder qb(b);
    qb.add_max_relative_spread(f.maxRelativeSpread);
    qb.add_min_quotes_per_expiry(f.minQuotesPerExpiry);
    qb.add_moneyness_window(f.moneynessWindow);
    if (f.minOpenInterest) qb.add_min_open_interest(*f.minOpenInterest);
    return qb.Finish();
}

Offset<flatbuffers::Vector<Offset<quantra::DroppedQuote>>> buildDropped(
    flatbuffers::grpc::MessageBuilder& b, const std::vector<DroppedQuoteData>& dropped) {
    std::vector<Offset<quantra::DroppedQuote>> v;
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

Offset<flatbuffers::Vector<Offset<quantra::VolSurfaceBandCell>>> buildBands(
    flatbuffers::grpc::MessageBuilder& b, const std::vector<BandCell>& cells) {
    std::vector<Offset<quantra::VolSurfaceBandCell>> v;
    for (const auto& c : cells) {
        quantra::VolSurfaceBandCellBuilder cb(b);
        if (c.moneynessMin) cb.add_moneyness_min(*c.moneynessMin);
        if (c.moneynessMax) cb.add_moneyness_max(*c.moneynessMax);
        if (c.timeMin) cb.add_time_min(*c.timeMin);
        if (c.timeMax) cb.add_time_max(*c.timeMax);
        cb.add_count(c.count);
        cb.add_inside(c.inside);
        if (c.rmsVolError) cb.add_rms_vol_error(*c.rmsVolError);
        if (c.maxAbsVolError) cb.add_max_abs_vol_error(*c.maxAbsVolError);
        v.push_back(cb.Finish());
    }
    return b.CreateVector(v);
}

quantra::enums::EquityOptionType optionType(bool isCall) {
    return isCall ? quantra::enums::EquityOptionType_Call : quantra::enums::EquityOptionType_Put;
}

Offset<quantra::VolSurfaceFitValidation> buildFit(
    flatbuffers::grpc::MessageBuilder& b, const FitValidationData& f) {
    std::vector<Offset<quantra::VolSurfaceQuoteFit>> qs;
    for (const auto& q : f.quotes) {
        auto expOff = b.CreateString(q.expiryIso);
        quantra::VolSurfaceQuoteFitBuilder qb(b);
        qb.add_expiry(expOff);
        qb.add_time(q.time);
        qb.add_strike(q.strike);
        qb.add_moneyness(q.moneyness);
        qb.add_option_type(optionType(q.isCall));
        qb.add_bid(q.bid);
        qb.add_ask(q.ask);
        qb.add_mid(q.mid);
        qb.add_market_vol(q.marketVol);
        qb.add_model_vol(q.modelVol);
        qb.add_model_price(q.modelPrice);
        qb.add_inside_spread(q.insideSpread);
        qb.add_vol_error(q.volError);
        qb.add_price_error(q.priceError);
        if (q.bidVol) qb.add_bid_vol(*q.bidVol);
        if (q.askVol) qb.add_ask_vol(*q.askVol);
        qs.push_back(qb.Finish());
    }
    auto qsOff = b.CreateVector(qs);
    auto bandsOff = buildBands(b, f.bands);
    std::vector<Offset<quantra::VolSurfaceAtmError>> atm;
    for (const auto& a : f.atm) {
        auto expOff = b.CreateString(a.expiryIso);
        quantra::VolSurfaceAtmErrorBuilder ab(b);
        ab.add_expiry(expOff);
        ab.add_time(a.time);
        ab.add_strike(a.strike);
        ab.add_moneyness(a.moneyness);
        ab.add_market_vol(a.marketVol);
        ab.add_model_vol(a.modelVol);
        ab.add_vol_error(a.volError);
        atm.push_back(ab.Finish());
    }
    auto atmOff = b.CreateVector(atm);
    auto verdictOff = b.CreateString(f.verdict);
    quantra::VolSurfaceFitValidationBuilder fb(b);
    fb.add_count(f.count);
    fb.add_inside(f.inside);
    fb.add_rms_vol_error(f.rmsVolError);
    fb.add_max_abs_vol_error(f.maxAbsVolError);
    fb.add_max_abs_price_error(f.maxAbsPriceError);
    fb.add_quotes(qsOff);
    fb.add_bands(bandsOff);
    fb.add_atm_error_per_expiry(atmOff);
    fb.add_verdict(verdictOff);
    return fb.Finish();
}

Offset<quantra::LocalVolCheck> buildLocalVolCheck(
    flatbuffers::grpc::MessageBuilder& b, const LocalVolCheckData& c) {
    Offset<flatbuffers::String> firstOff;
    if (!c.firstRefusal.empty()) firstOff = b.CreateString(c.firstRefusal);
    quantra::LocalVolCheckBuilder cb(b);
    cb.add_arbitrage_free(c.arbitrageFree);
    cb.add_times(c.times);
    cb.add_strikes(c.strikes);
    cb.add_points(c.points);
    cb.add_refused(c.refused);
    if (!c.firstRefusal.empty()) cb.add_first_refusal(firstOff);
    if (c.minLocalVariance) cb.add_min_local_variance(*c.minLocalVariance);
    if (c.minLocalVarianceTime) cb.add_min_local_variance_time(*c.minLocalVarianceTime);
    if (c.minLocalVarianceStrike) cb.add_min_local_variance_strike(*c.minLocalVarianceStrike);
    return cb.Finish();
}

Offset<quantra::VolSurfaceArbitrageValidation> buildArbitrage(
    flatbuffers::grpc::MessageBuilder& b, const ArbitrageValidationData& a) {
    auto lvOff = buildLocalVolCheck(b, a.localVol);
    Offset<flatbuffers::Vector<Offset<quantra::SviSliceCheck>>> sviOff;
    if (a.sviSlices) {
        std::vector<Offset<quantra::SviSliceCheck>> v;
        for (const auto& s : *a.sviSlices) {
            auto expOff = b.CreateString(s.expiryIso);
            quantra::SviSliceCheckBuilder sb(b);
            sb.add_expiry(expOff);
            sb.add_time(s.time);
            sb.add_butterfly_min_g(s.butterflyMinG);
            sb.add_butterfly_min_g_k(s.butterflyMinGK);
            sb.add_wing_slope(s.wingSlope);
            if (s.calendarMinDw) sb.add_calendar_min_dw(*s.calendarMinDw);
            if (s.calendarMinDwK) sb.add_calendar_min_dw_k(*s.calendarMinDwK);
            if (s.calendarMinForwardVol) sb.add_calendar_min_forward_vol(*s.calendarMinForwardVol);
            v.push_back(sb.Finish());
        }
        sviOff = b.CreateVector(v);
    }
    Offset<quantra::SsviArbitrageCheck> ssviOff;
    if (a.ssvi) {
        const auto& s = *a.ssvi;
        auto violationsOff = b.CreateVectorOfStrings(s.violations);
        ssviOff = quantra::CreateSsviArbitrageCheck(
            b, violationsOff, s.scanMinG, s.scanMinGTime, s.scanMinGK, s.scanMinDwDt,
            s.scanMinDwDtTime, s.scanMinDwDtK);
    }
    Offset<flatbuffers::Vector<Offset<quantra::SsviSliceCheck>>> ssviSlicesOff;
    if (a.ssviSlices) {
        std::vector<Offset<quantra::SsviSliceCheck>> v;
        for (const auto& s : *a.ssviSlices) {
            auto expOff = b.CreateString(s.expiryIso);
            quantra::SsviSliceCheckBuilder sb(b);
            sb.add_expiry(expOff);
            sb.add_time(s.time);
            sb.add_theta(s.theta);
            sb.add_rho(s.rho);
            sb.add_psi(s.psi);
            sb.add_margin_butterfly_psi(s.marginButterflyPsi);
            sb.add_margin_butterfly_psi2(s.marginButterflyPsi2);
            if (s.marginCalendarTheta) sb.add_margin_calendar_theta(*s.marginCalendarTheta);
            if (s.marginCalendarPsi) sb.add_margin_calendar_psi(*s.marginCalendarPsi);
            if (s.marginCalendarA) sb.add_margin_calendar_a(*s.marginCalendarA);
            v.push_back(sb.Finish());
        }
        ssviSlicesOff = b.CreateVector(v);
    }
    Offset<quantra::LocalVolGrid> gridOff;
    if (a.grid) {
        std::vector<Offset<quantra::LocalVolGridRow>> rows;
        for (const auto& r : a.grid->rows) {
            auto kOff = b.CreateVector(r.strikes);
            auto vOff = b.CreateVector(r.localVols);
            auto refOff = b.CreateVector(r.refused);
            quantra::LocalVolGridRowBuilder rb(b);
            rb.add_time(r.time);
            rb.add_strikes(kOff);
            rb.add_local_vols(vOff);
            rb.add_refused(refOff);
            rows.push_back(rb.Finish());
        }
        auto rowsOff = b.CreateVector(rows);
        quantra::LocalVolGridBuilder gb(b);
        gb.add_rows(rowsOff);
        gridOff = gb.Finish();
    }
    std::vector<Offset<quantra::ClampEdge>> edges;
    for (const auto& e : a.clampEdges) {
        auto scopeOff = b.CreateString(e.scope);
        Offset<flatbuffers::String> expOff;
        if (e.expiryIso) expOff = b.CreateString(*e.expiryIso);
        quantra::ClampEdgeBuilder eb(b);
        eb.add_scope(scopeOff);
        if (e.expiryIso) eb.add_expiry(expOff);
        if (e.time) eb.add_time(*e.time);
        if (e.strikeMin) eb.add_strike_min(*e.strikeMin);
        if (e.strikeMax) eb.add_strike_max(*e.strikeMax);
        edges.push_back(eb.Finish());
    }
    auto edgesOff = b.CreateVector(edges);
    auto violationsOff = b.CreateVectorOfStrings(a.violations);
    auto verdictOff = b.CreateString(a.verdict);
    quantra::VolSurfaceArbitrageValidationBuilder ab(b);
    ab.add_arbitrage_free(a.arbitrageFree);
    ab.add_local_vol_check(lvOff);
    if (a.sviSlices) ab.add_svi_slices(sviOff);
    if (a.ssvi) ab.add_ssvi_check(ssviOff);
    if (a.ssviSlices) ab.add_ssvi_slices(ssviSlicesOff);
    ab.add_violations(violationsOff);
    ab.add_verdict(verdictOff);
    if (a.grid) ab.add_local_vol_grid(gridOff);
    ab.add_clamp_edges(edgesOff);
    return ab.Finish();
}

Offset<quantra::VolSurfaceSimulationValidation> buildSimulation(
    flatbuffers::grpc::MessageBuilder& b, const SimulationValidationData& s) {
    std::vector<Offset<quantra::VolSurfaceQuoteSimulation>> qs;
    for (const auto& q : s.quotes) {
        auto expOff = b.CreateString(q.expiryIso);
        quantra::VolSurfaceQuoteSimulationBuilder qb(b);
        qb.add_expiry(expOff);
        qb.add_time(q.time);
        qb.add_strike(q.strike);
        qb.add_moneyness(q.moneyness);
        qb.add_option_type(optionType(q.isCall));
        qb.add_bid(q.bid);
        qb.add_ask(q.ask);
        qb.add_mid(q.mid);
        qb.add_market_vol(q.marketVol);
        qb.add_mc_price(q.mcPrice);
        qb.add_standard_error(q.standardError);
        if (q.mcVol) qb.add_mc_vol(*q.mcVol);
        qb.add_inside_spread(q.insideSpread);
        qb.add_within_noise(q.withinNoise);
        if (q.volError) qb.add_vol_error(*q.volError);
        qb.add_price_error(q.priceError);
        qs.push_back(qb.Finish());
    }
    auto qsOff = b.CreateVector(qs);
    auto bandsOff = buildBands(b, s.bands);
    auto verdictOff = b.CreateString(s.verdict);
    quantra::VolSurfaceSimulationValidationBuilder sb(b);
    sb.add_paths(s.paths);
    sb.add_time_steps(s.timeSteps);
    sb.add_seed(s.seed);
    sb.add_count(s.count);
    sb.add_inside(s.inside);
    sb.add_within_noise(s.withinNoise);
    sb.add_max_abs_price_error(s.maxAbsPriceError);
    if (s.maxAbsVolError) sb.add_max_abs_vol_error(*s.maxAbsVolError);
    sb.add_max_standard_error(s.maxStandardError);
    sb.add_quotes(qsOff);
    sb.add_bands(bandsOff);
    sb.add_verdict(verdictOff);
    return sb.Finish();
}

} // namespace

VolSurfaceValidationInputs VolSurfaceValidationMapper::toInputs(
    const quantra::ValidateVolSurfaceRequest* req) const {
    if (!req || !req->pricing()) {
        QUANTRA_INVALID_ARGUMENT("ValidateVolSurfaceRequest requires pricing");
    }
    auto requireString = [](const flatbuffers::String* s, const char* name) {
        if (!s || s->size() == 0) {
            QUANTRA_INVALID_ARGUMENT(std::string("ValidateVolSurfaceRequest requires ") + name);
        }
        return s->str();
    };
    VolSurfaceValidationInputs in;
    in.volatilityId = requireString(req->volatility_id(), "volatility_id");
    in.underlyingId = requireString(req->underlying_id(), "underlying_id");
    in.discountingCurveId = requireString(req->discounting_curve(), "discounting_curve");
    if (!req->quotes()) {
        QUANTRA_INVALID_ARGUMENT("ValidateVolSurfaceRequest requires quotes");
    }
    DecodedQuoteSheet sheet = decodeOptionQuoteSheet(req->quotes(), "quotes");
    in.sheetId = std::move(sheet.sheetId);
    in.sheetUnderlyingId = std::move(sheet.underlyingId);
    in.style = sheet.style;
    in.quotes = std::move(sheet.quotes);
    in.filter = decodeQuoteFilter(req->filter());
    if (req->forwards()) in.forwards = decodeVolSurfaceForwards(req->forwards());
    if (req->checks()) {
        in.checkFit = req->checks()->fit();
        in.checkArbitrage = req->checks()->arbitrage();
        in.checkSimulation = req->checks()->simulation();
        in.checkArbitrageGrid = req->checks()->arbitrage_grid();
    }
    if (req->simulation()) in.simulation = decodeMonteCarloSpec(req->simulation(), "simulation");
    if (req->bands()) {
        if (req->bands()->moneyness()) {
            for (flatbuffers::uoffset_t i = 0; i < req->bands()->moneyness()->size(); ++i)
                in.bands.moneyness.push_back(req->bands()->moneyness()->Get(i));
        }
        if (req->bands()->maturities()) {
            for (flatbuffers::uoffset_t i = 0; i < req->bands()->maturities()->size(); ++i)
                in.bands.maturities.push_back(req->bands()->maturities()->Get(i));
        }
    }
    // Echo: the underlying's dividend curve id, from the request's spec.
    const auto* eq = req->pricing()->equity();
    if (eq && eq->equity_underlyings()) {
        for (const auto* u : *eq->equity_underlyings()) {
            if (u && u->id() && u->id()->str() == in.underlyingId && u->dividend_yield_curve_id())
                in.dividendCurveId = u->dividend_yield_curve_id()->str();
        }
    }
    return in;
}

flatbuffers::Offset<quantra::ValidateVolSurfaceResponse> VolSurfaceValidationMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& b, const VolSurfaceValidationResult& r) const {
    // Expiries.
    std::vector<Offset<quantra::VolSurfaceValidationExpiry>> exps;
    for (const auto& e : r.expiries) {
        auto droppedOff = buildDropped(b, e.dropped);
        auto expOff = b.CreateString(e.expiryIso);
        Offset<flatbuffers::String> reasonOff;
        if (!e.usable) reasonOff = b.CreateString(e.droppedReason);
        quantra::VolSurfaceValidationExpiryBuilder eb(b);
        eb.add_expiry(expOff);
        eb.add_time(e.time);
        eb.add_usable(e.usable);
        if (!e.usable) eb.add_dropped_reason(reasonOff);
        if (e.forward) eb.add_forward(*e.forward);
        if (e.discountFactor) eb.add_discount_factor(*e.discountFactor);
        eb.add_quotes_used(static_cast<uint32_t>(e.used.size()));
        eb.add_quotes_dropped(static_cast<uint32_t>(e.dropped.size()));
        eb.add_dropped(droppedOff);
        exps.push_back(eb.Finish());
    }
    auto expsOff = b.CreateVector(exps);

    Offset<quantra::VolSurfaceFitValidation> fitOff;
    if (r.fit) fitOff = buildFit(b, *r.fit);
    Offset<quantra::VolSurfaceArbitrageValidation> arbOff;
    if (r.arbitrage) arbOff = buildArbitrage(b, *r.arbitrage);
    Offset<quantra::VolSurfaceSimulationValidation> simOff;
    if (r.simulation) simOff = buildSimulation(b, *r.simulation);

    // Echo.
    auto volIdOff = b.CreateString(r.volatilityId);
    auto undOff = b.CreateString(r.underlyingId);
    auto discOff = b.CreateString(r.discountingCurveId);
    auto divOff = b.CreateString(r.dividendCurveId);
    auto srcOff = b.CreateString(r.forwardsSource);
    auto sheetOff = b.CreateString(r.sheetId);
    auto filterOff = buildFilter(b, r.filter);
    auto checksOff = quantra::CreateVolSurfaceValidationChecks(
        b, r.checkFit, r.checkArbitrage, r.checkSimulation, r.checkArbitrageGrid);
    auto mOff = b.CreateVector(r.bands.moneyness);
    auto tOff = b.CreateVector(r.bands.maturities);
    quantra::VolSurfaceValidationInputsEchoBuilder ib(b);
    ib.add_volatility_id(volIdOff);
    ib.add_shape(r.shape);
    ib.add_underlying_id(undOff);
    ib.add_spot(r.spot);
    ib.add_discounting_curve(discOff);
    ib.add_dividend_curve(divOff);
    ib.add_forwards_source(srcOff);
    ib.add_sheet_id(sheetOff);
    ib.add_filter(filterOff);
    ib.add_checks(checksOff);
    ib.add_moneyness_edges(mOff);
    ib.add_maturity_edges(tOff);
    auto echoOff = ib.Finish();

    quantra::VolSurfaceValidationTimingBuilder tb(b);
    if (r.fitMilliseconds) tb.add_fit_milliseconds(*r.fitMilliseconds);
    if (r.arbitrageMilliseconds) tb.add_arbitrage_milliseconds(*r.arbitrageMilliseconds);
    if (r.simulationMilliseconds) tb.add_simulation_milliseconds(*r.simulationMilliseconds);
    tb.add_total_milliseconds(r.totalMilliseconds);
    auto timingOff = tb.Finish();

    quantra::ValidateVolSurfaceResponseBuilder ob(b);
    ob.add_inputs_echo(echoOff);
    ob.add_expiries(expsOff);
    if (r.fit) ob.add_fit(fitOff);
    if (r.arbitrage) ob.add_arbitrage(arbOff);
    if (r.simulation) ob.add_simulation(simOff);
    ob.add_timing(timingOff);
    return ob.Finish();
}

} // namespace quantra
