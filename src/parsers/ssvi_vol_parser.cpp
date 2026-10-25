#include "ssvi_vol_parser.h"

#include "date_convert.h"
#include "error.h"
#include "ssvi_calibrate_cache.h"

#include <cmath>
#include <limits>
#include <sstream>

namespace quantra {

namespace {

template <class V> bool nonEmpty(const V* v) {
    return v && v->size() > 0;
}

std::string forId(const std::string& id) {
    return " for vol id: " + id;
}

struct ParsedSlices {
    std::vector<QuantLib::Date> expiries;
    std::vector<ssvi::Slice> slices;
    std::vector<SsviSliceParams> params;
    std::vector<double> forwards; // empty unless every slice carries one
};

double requireValue(
    const flatbuffers::Optional<double>& v,
    const char* field,
    std::size_t i,
    const std::string& id) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(
            "BlackVolSpec.ssvi_slices[" + std::to_string(i) + "]." + field + " is required" +
            forId(id));
    }
    if (!std::isfinite(v.value())) {
        QUANTRA_INVALID_ARGUMENT(
            "BlackVolSpec.ssvi_slices[" + std::to_string(i) + "]." + field + " must be finite" +
            forId(id));
    }
    return v.value();
}

QuantLib::Date requireExpiry(
    const flatbuffers::String* s,
    const std::string& what,
    const QuantLib::Date& ref,
    const QuantLib::Date& previous,
    const std::string& id) {
    if (!s || s->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(what + ".expiry_date is required" + forId(id));
    }
    const QuantLib::Date d = DateToQL(s->str());
    if (d <= ref) {
        QUANTRA_INVALID_ARGUMENT(what + ".expiry_date must be after reference_date" + forId(id));
    }
    if (previous != QuantLib::Date() && d <= previous) {
        QUANTRA_INVALID_ARGUMENT(what + ".expiry_date must be strictly increasing" + forId(id));
    }
    return d;
}

ParsedSlices parseSlices(
    const quantra::BlackVolSpec* payload,
    const QuantLib::Date& ref,
    const QuantLib::DayCounter& dc,
    const std::string& id) {
    ParsedSlices out;
    const auto* fb = payload->ssvi_slices();
    std::size_t withForward = 0;
    for (flatbuffers::uoffset_t i = 0; i < fb->size(); ++i) {
        const quantra::SsviSlice* s = fb->Get(i);
        const std::string what = "BlackVolSpec.ssvi_slices[" + std::to_string(i) + "]";
        const QuantLib::Date previous =
            out.expiries.empty() ? QuantLib::Date() : out.expiries.back();
        const QuantLib::Date d = requireExpiry(s->expiry_date(), what, ref, previous, id);
        const double theta = requireValue(s->theta(), "theta", i, id);
        const double rho = requireValue(s->rho(), "rho", i, id);
        const double psi = requireValue(s->psi(), "psi", i, id);
        out.expiries.push_back(d);
        out.slices.push_back(ssvi::Slice{dc.yearFraction(ref, d), theta, rho, psi});
        out.params.push_back(SsviSliceParams{theta, rho, psi});
        if (s->forward().has_value()) {
            const double f = requireValue(s->forward(), "forward", i, id);
            if (!(f > 0.0)) {
                QUANTRA_INVALID_ARGUMENT(what + ".forward must be > 0" + forId(id));
            }
            out.forwards.push_back(f);
            ++withForward;
        }
    }
    if (withForward != 0 && withForward != fb->size()) {
        QUANTRA_INVALID_ARGUMENT(
            "BlackVolSpec.ssvi_slices: forward must be given on every slice or on none" +
            forId(id));
    }
    return out;
}

QuantLib::Option::Type toOptionType(quantra::enums::EquityOptionType t) {
    return t == quantra::enums::EquityOptionType_Put ? QuantLib::Option::Put
                                                     : QuantLib::Option::Call;
}

std::optional<double> optionalOf(const flatbuffers::Optional<double>& v) {
    if (v.has_value()) return v.value();
    return std::nullopt;
}

ssvi::OptionQuote parseQuote(
    const quantra::EquityOptionQuote* q,
    std::size_t i,
    const QuantLib::Date& ref,
    const std::string& id) {
    const std::string what = "BlackVolSpec.option_quotes[" + std::to_string(i) + "]";
    ssvi::OptionQuote out;
    out.expiry = requireExpiry(q->expiry_date(), what, ref, QuantLib::Date(), id);
    if (!q->strike().has_value()) {
        QUANTRA_INVALID_ARGUMENT(what + ".strike is required" + forId(id));
    }
    if (!q->option_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT(what + ".option_type is required" + forId(id));
    }
    out.strike = q->strike().value();
    out.type = toOptionType(q->option_type().value());
    out.price = optionalOf(q->price());
    out.impliedVol = optionalOf(q->implied_vol());
    out.bid = optionalOf(q->bid());
    out.ask = optionalOf(q->ask());
    out.weight = q->weight();
    if (out.bid.has_value() != out.ask.has_value()) {
        QUANTRA_INVALID_ARGUMENT(what + ".bid and ask must be given together" + forId(id));
    }
    if (out.price && out.impliedVol) {
        QUANTRA_INVALID_ARGUMENT(what + ": give price or implied_vol, not both" + forId(id));
    }
    if (!out.price && !out.impliedVol) {
        if (!out.bid) {
            QUANTRA_INVALID_ARGUMENT(
                what + ": needs price, implied_vol, or bid and ask" + forId(id));
        }
        out.price = 0.5 * (*out.bid + *out.ask); // bid / ask are premiums here
    }
    return out;
}

ssvi::QuoteWeighting toWeighting(quantra::enums::SsviQuoteWeighting w) {
    switch (w) {
        case quantra::enums::SsviQuoteWeighting_Uniform: return ssvi::QuoteWeighting::Uniform;
        case quantra::enums::SsviQuoteWeighting_BidAsk: return ssvi::QuoteWeighting::BidAsk;
        default: return ssvi::QuoteWeighting::Vega;
    }
}

std::shared_ptr<const SsviVolData> calibrateData(
    const quantra::BlackVolSpec* payload,
    const QuantLib::Date& ref,
    const QuantLib::DayCounter& dc,
    const BlackVolMarket& market,
    const std::string& id,
    SsviVolData data) {
    ssvi::CalibrationInput in;
    in.referenceDate = ref;
    in.dayCounter = dc;
    in.spot = market.spot->value();
    in.discount = market.discount;
    in.dividend = market.dividend;
    if (const auto* fit = payload->ssvi_fit()) {
        in.weighting = toWeighting(fit->weighting());
        in.maxAbsLogMoneyness = fit->max_abs_log_moneyness();
    }
    const auto* fb = payload->option_quotes();
    for (flatbuffers::uoffset_t i = 0; i < fb->size(); ++i) {
        in.quotes.push_back(parseQuote(fb->Get(i), i, ref, id));
        if (in.weighting == ssvi::QuoteWeighting::BidAsk && !in.quotes.back().bid) {
            QUANTRA_INVALID_ARGUMENT(
                "BlackVolSpec.option_quotes[" + std::to_string(i) +
                "]: bid and ask are required when weighting=BidAsk" + forId(id));
        }
    }
    data.fit = calibrateCached(in);
    data.quotes = in.quotes;
    data.weighting = in.weighting;
    data.maxAbsLogMoneyness = in.maxAbsLogMoneyness;
    data.expiries = data.fit->expiries;
    data.params = data.fit->params;
    data.forwards = data.fit->fitForwards;
    return std::make_shared<const SsviVolData>(std::move(data));
}

} // namespace

bool isSsviShape(quantra::enums::VolSurfaceShape shape) {
    return shape == quantra::enums::VolSurfaceShape_SsviParams ||
           shape == quantra::enums::VolSurfaceShape_SsviCalibrate;
}

bool hasSsviFields(const quantra::BlackVolSpec* payload) {
    return nonEmpty(payload->ssvi_slices()) || nonEmpty(payload->option_quotes()) ||
           payload->ssvi_fit() != nullptr;
}

void validateSsviFields(
    const quantra::BlackVolSpec* payload,
    quantra::enums::VolSurfaceShape shape,
    const std::string& id) {
    const bool calibrate = shape == quantra::enums::VolSurfaceShape_SsviCalibrate;
    const char* name = calibrate ? "SsviCalibrate" : "SsviParams";
    if (nonEmpty(payload->expiries()) || nonEmpty(payload->strikes()) || payload->term_vols() ||
        payload->surface_vols() || nonEmpty(payload->price_expiries()) ||
        nonEmpty(payload->price_strikes()) || payload->surface_prices()) {
        QUANTRA_INVALID_ARGUMENT(
            std::string("BlackVolSpec shape=") + name +
            " forbids the grid, matrix and price-surface fields" + forId(id));
    }
    if (calibrate) {
        if (!nonEmpty(payload->option_quotes())) {
            QUANTRA_INVALID_ARGUMENT(
                "BlackVolSpec shape=SsviCalibrate requires option_quotes" + forId(id));
        }
        if (nonEmpty(payload->ssvi_slices())) {
            QUANTRA_INVALID_ARGUMENT(
                "BlackVolSpec shape=SsviCalibrate forbids ssvi_slices" + forId(id));
        }
        return;
    }
    if (!nonEmpty(payload->ssvi_slices())) {
        QUANTRA_INVALID_ARGUMENT("BlackVolSpec shape=SsviParams requires ssvi_slices" + forId(id));
    }
    if (nonEmpty(payload->option_quotes()) || payload->ssvi_fit()) {
        QUANTRA_INVALID_ARGUMENT(
            "BlackVolSpec shape=SsviParams forbids option_quotes and ssvi_fit" + forId(id));
    }
}

void validateSsviSurface(const std::vector<ssvi::Slice>& slices, const std::string& context) {
    const ssvi::Surface surface(slices); // 400 naming the slice for invalid parameters
    const std::vector<ssvi::Violation> violations = ssvi::checkArbitrage(slices);
    if (!violations.empty()) {
        std::ostringstream os;
        os << "SSVI surface admits arbitrage (" << context << "):";
        for (const ssvi::Violation& v : violations) {
            os << " " << v.condition << " at slice " << v.slice;
            if (v.prevSlice != v.slice) os << " vs slice " << v.prevSlice;
            os << " (violated by " << v.margin << ");";
        }
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
    const ssvi::ScanResult scan = ssvi::defaultScan(surface);
    if (!(scan.minG >= 0.0) || !(scan.minWt >= 0.0)) {
        std::ostringstream os;
        os << "SSVI surface admits arbitrage (" << context
           << "): numeric scan finds min g = " << scan.minG << " at (t=" << scan.minGt
           << ", k=" << scan.minGk << ") and min dw/dt = " << scan.minWt << " at (t=" << scan.minWtT
           << ", k=" << scan.minWtK << ")";
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
}

SsviParseResult parseSsviVol(
    const quantra::BlackVolSpec* payload,
    quantra::enums::VolSurfaceShape shape,
    const std::string& id,
    const QuantLib::Date& ref,
    const QuantLib::Calendar& cal,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dc,
    const QuoteRegistry* quotes,
    const std::map<
        std::string,
        std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>>* curves) {
    const bool calibrate = shape == quantra::enums::VolSurfaceShape_SsviCalibrate;
    ParsedSlices given;
    if (!calibrate) {
        given = parseSlices(payload, ref, dc, id);
        validateSsviSurface(given.slices, "vol id " + id);
    }
    const bool fromForwards = !given.forwards.empty();
    const BlackVolMarket market = resolveBlackVolMarket(
        payload, ref, dc, quotes, curves, id, calibrate ? "SsviCalibrate" : "SsviParams",
        !calibrate && !fromForwards);

    SsviVolData data;
    data.spot0 = market.spot->value();
    data.riskFree = market.discount;
    if (calibrate) {
        data = *calibrateData(payload, ref, dc, market, id, data);
    } else {
        data.expiries = given.expiries;
        data.params = given.params;
    }
    if (calibrate || fromForwards) {
        if (!calibrate) data.forwards = given.forwards;
        data.dividend = buildImpliedDividendCurve(
            ref, dc, data.expiries, data.forwards, data.spot0, data.riskFree);
    } else {
        data.dividend = market.dividend;
    }
    auto vol = std::make_shared<SsviBlackVol>(
        ref, cal, bdc, dc, data.expiries, data.params, data.spot0, data.dividend, data.riskFree);
    if (data.forwards.empty()) {
        for (std::size_t i = 0; i < data.expiries.size(); ++i) {
            data.forwards.push_back(vol->forward(dc.yearFraction(ref, data.expiries[i])));
        }
    }
    return SsviParseResult{std::make_shared<const SsviVolData>(std::move(data)), vol};
}

std::shared_ptr<QuantLib::BlackVolTermStructure> shiftedSsviVol(
    const BlackVolEntry& entry, double shift) {
    if (!entry.ssvi) {
        QUANTRA_ERROR("Black vol entry has no SSVI data to shift");
    }
    const SsviVolData& d = *entry.ssvi;
    std::vector<SsviSliceParams> params;
    std::vector<ssvi::Slice> slices;
    for (std::size_t i = 0; i < d.expiries.size(); ++i) {
        const double t = entry.dayCounter.yearFraction(entry.referenceDate, d.expiries[i]);
        const double vol = std::sqrt(d.params[i].theta / t) + shift;
        if (!(vol > 0.0)) {
            std::ostringstream os;
            os << "Black vol shift " << shift << " makes the SSVI ATM vol of slice " << i
               << " non-positive (" << vol << ")";
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
        params.push_back(SsviSliceParams{vol * vol * t, d.params[i].rho, d.params[i].psi});
        slices.push_back(ssvi::Slice{t, vol * vol * t, d.params[i].rho, d.params[i].psi});
    }
    std::ostringstream ctx;
    ctx << "vol bump " << shift;
    validateSsviSurface(slices, ctx.str());
    return std::make_shared<SsviBlackVol>(
        entry.referenceDate, entry.calendar, entry.businessDayConvention, entry.dayCounter,
        d.expiries, params, d.spot0, d.dividend, d.riskFree);
}

} // namespace quantra
