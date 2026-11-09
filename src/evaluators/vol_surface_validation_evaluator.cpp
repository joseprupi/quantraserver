#include "vol_surface_validation_evaluator.h"

#include "date_convert.h"
#include "error.h"
#include "path_engine.h"
#include "smile_slice_vol_surface.h"
#include "ssvi.h"
#include "ssvi_calibrator.h"
#include "ssvi_vol_surface.h"
#include "time_grid.h"

#include <ql/math/matrix.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace quantra {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

constexpr unsigned kCheckStrikes = 201;
constexpr double kCheckTimeOffset = 2.0e-4;
// Log-strike offset of the scan points either side of a clamp edge.
constexpr double kEdgeProbe = 1.0e-3;
constexpr double kReachStdDevs = 5.0;
constexpr unsigned kSliceCheckPoints = 201;
constexpr double kLeeSlope = 2.0;
// Verdict thresholds.
constexpr double kFitPassInside = 0.90;
constexpr double kFitFailInside = 0.50;
constexpr double kFitPassAtmError = 0.005;
constexpr double kNoiseStdErrors = 3.5;
constexpr double kSimPassWithin = 0.95;
constexpr double kSimFailWithin = 0.80;

std::string trimMessage(const std::string& s) {
    std::string t = s;
    for (auto& c : t)
        if (c == '\n') c = ' ';
    return t.size() > 160 ? t.substr(0, 160) + "..." : t;
}

std::string fmt(double v, int precision = 6) {
    std::ostringstream os;
    os.precision(precision);
    os << v;
    return os.str();
}

std::string pct(unsigned a, unsigned n) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << (n > 0 ? 100.0 * a / n : 0.0) << " %";
    return os.str();
}

std::string volPoints(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << v * 100.0;
    return os.str();
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------

void applyLocalVolSettings(const BlackVolEntry& e, mc::BasketAsset& asset) {
    asset.localVolStrikeRange = e.localVolStrikeRange;
    asset.localVolStrikeBands.clear();
    for (const auto& b : e.localVolStrikeBands) {
        asset.localVolStrikeBands.push_back(
            {asset.vol->timeFromReference(b.expiry), b.minStrike, b.maxStrike});
    }
    asset.localVolTime = localVolTimeOf(&e);
}

mc::LocalVolTimeExtrapolation localVolTimeOf(const BlackVolEntry* entry) {
    return entry != nullptr && entry->flatLocalVolBeyondLastExpiry
               ? mc::LocalVolTimeExtrapolation::FlatLocalVol
               : mc::LocalVolTimeExtrapolation::FlatImpliedVol;
}

std::vector<double> localVolCheckTimes(const std::vector<double>& expiryTimes) {
    std::vector<double> t;
    double prev = 0.0;
    for (double T : expiryTimes) {
        t.push_back(0.5 * (prev + T));
        if (T - kCheckTimeOffset > 0.0) t.push_back(T - kCheckTimeOffset);
        // The expiry itself: a simulation grid steps on it, and the time
        // stencil of a read there straddles both neighbouring intervals.
        t.push_back(T);
        t.push_back(T + kCheckTimeOffset);
        prev = T;
    }
    if (!expiryTimes.empty() && expiryTimes.back() > 2.0 * mc::EdgeClampedLocalVol::kTimeMargin)
        t.push_back(expiryTimes.back() - mc::EdgeClampedLocalVol::kTimeMargin);
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    return t;
}

LocalVolCheckData checkLocalVol(
    const Handle<QuantLib::BlackVolTermStructure>& surface,
    const std::vector<mc::LocalVolStrikeBand>& bands,
    const std::optional<std::pair<double, double>>& strikeRange,
    mc::LocalVolTimeExtrapolation timeExtension,
    const Handle<QuantLib::Quote>& spot,
    const Handle<QuantLib::YieldTermStructure>& riskFree,
    const Handle<QuantLib::YieldTermStructure>& dividend,
    const std::vector<double>& expiryTimes,
    const std::vector<double>& regionTimes,
    const std::vector<std::pair<double, double>>& regions,
    std::vector<std::pair<double, bool>>* refusedTimes,
    LocalVolGridData* grid) {
    LocalVolCheckData c;
    if (regions.empty() || regions.size() != regionTimes.size()) {
        QUANTRA_ERROR("local vol check: one region per region time is required");
    }
    mc::BasketAsset asset = mc::MakeBasketAsset("check", spot, dividend, riskFree, surface);
    asset.localVolTime = timeExtension;
    asset.localVolStrikeRange = strikeRange;
    asset.localVolStrikeBands = bands;
    mc::LocalVolCache cache;
    cache.attach(asset);
    Handle<QuantLib::LocalVolTermStructure> lv = asset.localVol;
    if (lv.empty()) {
        // A constant or term vol (exact lognormal steps in a note): its
        // local vol, strike-independent.
        auto s = std::make_shared<QuantLib::LocalVolSurface>(surface, riskFree, dividend, spot);
        s->enableExtrapolation();
        lv = Handle<QuantLib::LocalVolTermStructure>(s);
    }
    // The clamp the note applies (absent for a constant or term vol).
    const auto* clamp = dynamic_cast<const mc::EdgeClampedLocalVol*>(lv.currentLink().get());
    const std::vector<double> times = localVolCheckTimes(expiryTimes);
    c.times = static_cast<unsigned>(times.size());
    c.strikes = kCheckStrikes;
    const std::size_t nr = regions.size();
    // The region of a time x: that of the first expiry at or after x (the
    // last beyond the last expiry).
    auto regionOf = [&](double x) {
        return std::min<std::size_t>(
            nr - 1,
            static_cast<std::size_t>(
                std::lower_bound(regionTimes.begin(), regionTimes.end(), x) - regionTimes.begin()));
    };
    for (double t : times) {
        // The strikes a simulation step at t can read: the union of the
        // regions of every time within the stencil window around t (one
        // region inside an interval, the two around an expiry).
        const std::size_t from = regionOf(t - mc::EdgeClampedLocalVol::kBandTimeWindow);
        const std::size_t to = regionOf(t + mc::EdgeClampedLocalVol::kBandTimeWindow);
        double kMin = regions[from].first, kMax = regions[from].second;
        for (std::size_t r = from + 1; r <= to; ++r) {
            kMin = std::min(kMin, regions[r].first);
            kMax = std::max(kMax, regions[r].second);
        }
        std::vector<double> strikes;
        strikes.reserve(kCheckStrikes + 6);
        for (unsigned j = 0; j < kCheckStrikes; ++j) {
            strikes.push_back(
                kMin *
                std::exp(std::log(kMax / kMin) * static_cast<double>(j) / (kCheckStrikes - 1)));
        }
        // The clamp's edges at the time read, and just inside and outside
        // them: where a level crossing the edge changes what is read.
        if (clamp) {
            const auto band = clamp->strikeBand(clamp->clampedTime(t));
            for (double edge : {band.first, band.second}) {
                // An open side (0, an eSSVI surface's QL_EPSILON, QL_MAX_REAL): no edge.
                if (!(edge > 2.0 * QL_EPSILON) || !(edge < 0.5 * QL_MAX_REAL)) continue;
                for (double d : {-kEdgeProbe, 0.0, kEdgeProbe})
                    strikes.push_back(edge * std::exp(d));
            }
            // Increasing, without a point repeated (an edge probe can fall
            // on a region end to the last bit).
            std::sort(strikes.begin(), strikes.end());
            strikes.erase(
                std::unique(
                    strikes.begin(), strikes.end(),
                    [](double x, double y) { return y - x <= 1e-12 * y; }),
                strikes.end());
        }
        LocalVolGridRowData* row = nullptr;
        if (grid) {
            grid->rows.emplace_back();
            row = &grid->rows.back();
            row->time = t;
        }
        for (unsigned j = 0; j < strikes.size(); ++j) {
            const double K = strikes[j];
            ++c.points;
            if (row) {
                row->strikes.push_back(K);
                row->localVols.push_back(0.0);
            }
            try {
                const double v = lv->localVol(t, K, true);
                if (!std::isfinite(v)) QUANTRA_ERROR("non-finite local vol");
                if (row) row->localVols.back() = v;
                const double var = v * v;
                if (!c.minLocalVariance || var < *c.minLocalVariance) {
                    c.minLocalVariance = var;
                    c.minLocalVarianceTime = t;
                    c.minLocalVarianceStrike = K;
                }
            } catch (const std::exception& ex) {
                const std::string what = ex.what();
                if (c.refused++ == 0) c.firstRefusal = trimMessage(what);
                if (row) row->refused.push_back(j);
                if (refusedTimes)
                    refusedTimes->emplace_back(
                        t, what.find("decreasing variance") != std::string::npos);
            }
        }
    }
    c.arbitrageFree = c.refused == 0 && c.minLocalVariance.has_value();
    return c;
}

std::vector<std::pair<double, double>> quoteRegions(
    const std::vector<RegionQuotes>& work, double clampLo, double clampHi) {
    // Log-moneyness reached on one side (direction +1 calls, -1 puts).
    auto reachOf = [&](const RegionQuotes& w, int direction) {
        std::vector<std::pair<double, double>> kv; // |k| beyond the forward, vol
        for (const auto& u : *w.used) {
            const double k = std::log(u.strike / w.forward) * direction;
            kv.emplace_back(k, u.vol);
        }
        std::sort(kv.begin(), kv.end());
        // Start at the quote nearest the forward.
        const auto start = std::min_element(kv.begin(), kv.end(), [](const auto& a, const auto& b) {
            return std::fabs(a.first) < std::fabs(b.first);
        });
        double budget = kReachStdDevs * std::sqrt(w.time);
        double k = 0.0, vol = start->second;
        for (auto it = start; it != kv.end(); ++it) {
            if (it->first <= k) continue;
            const double step = it->first - k;
            const double cost = step * 0.5 * (1.0 / vol + 1.0 / it->second);
            if (cost >= budget) return k + budget / (0.5 * (1.0 / vol + 1.0 / it->second));
            budget -= cost;
            k = it->first;
            vol = it->second;
        }
        return k + budget * vol;
    };
    std::vector<std::pair<double, double>> out;
    for (std::size_t i = 0; i < work.size(); ++i) {
        const auto& w = work[i];
        const std::size_t j = i == 0 ? 0 : i - 1;
        double lo = w.forward * std::exp(-std::max(reachOf(w, -1), reachOf(work[j], -1)));
        double hi = w.forward * std::exp(std::max(reachOf(w, +1), reachOf(work[j], +1)));
        const double qLo = std::max(w.used->front().strike, work[j].used->front().strike);
        const double qHi = std::min(w.used->back().strike, work[j].used->back().strike);
        if (qLo < qHi) {
            lo = std::min(lo, qLo);
            hi = std::max(hi, qHi);
        }
        lo = std::max(lo, clampLo);
        hi = std::min(hi, clampHi);
        out.emplace_back(lo, hi);
    }
    return out;
}

SheetForwardCurves sheetForwardCurves(
    const Date& asOf,
    double spot,
    const std::vector<Date>& expiries,
    const std::vector<double>& forwards,
    const std::vector<double>& discountFactors) {
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    std::vector<Date> curveDates{asOf};
    std::vector<QuantLib::DiscountFactor> rDfs{1.0}, qDfs{1.0};
    for (std::size_t i = 0; i < expiries.size(); ++i) {
        curveDates.push_back(expiries[i]);
        rDfs.push_back(discountFactors[i]);
        qDfs.push_back(forwards[i] * discountFactors[i] / spot);
    }
    SheetForwardCurves c;
    c.riskFree = Handle<QuantLib::YieldTermStructure>(
        std::make_shared<QuantLib::DiscountCurve>(curveDates, rDfs, dc));
    c.dividend = Handle<QuantLib::YieldTermStructure>(
        std::make_shared<QuantLib::DiscountCurve>(curveDates, qDfs, dc));
    return c;
}

FitValidationData fitQuotes(
    const QuantLib::BlackVolTermStructure& surface,
    const std::vector<ValidationExpiry>& expiries,
    const VolSurfaceBandEdges& edges) {
    FitValidationData f;
    std::vector<BandPoint> points;
    double sumSq = 0.0;
    double worstAtm = 0.0;
    std::string worstAtmExpiry;
    for (const auto& e : expiries) {
        if (!e.usable) continue;
        const double F = *e.forward, DF = *e.discountFactor, T = e.time;
        std::optional<AtmErrorData> atm;
        double atmDist = std::numeric_limits<double>::infinity();
        for (const auto& u : e.used) {
            QuoteFitData q;
            q.expiryIso = e.expiryIso;
            q.time = T;
            q.strike = u.strike;
            q.moneyness = u.strike / F;
            q.isCall = u.isCall;
            q.bid = u.bid;
            q.ask = u.ask;
            q.mid = u.mid;
            q.marketVol = u.vol;
            const auto side = u.isCall ? QuantLib::Option::Call : QuantLib::Option::Put;
            q.bidVol = impliedBlackVol(side, u.strike, F, u.bid, DF, T);
            q.askVol = impliedBlackVol(side, u.strike, F, u.ask, DF, T);
            q.modelVol = surface.blackVol(e.expiry, u.strike, true);
            q.modelPrice = QuantLib::blackFormula(
                u.isCall ? QuantLib::Option::Call : QuantLib::Option::Put, u.strike, F,
                q.modelVol * std::sqrt(T), DF);
            q.insideSpread = q.modelPrice >= u.bid && q.modelPrice <= u.ask;
            q.volError = q.modelVol - u.vol;
            q.priceError = q.modelPrice - u.mid;
            ++f.count;
            if (q.insideSpread) ++f.inside;
            sumSq += q.volError * q.volError;
            f.maxAbsVolError = std::max(f.maxAbsVolError, std::fabs(q.volError));
            f.maxAbsPriceError = std::max(f.maxAbsPriceError, std::fabs(q.priceError));
            points.push_back({q.moneyness, T, q.insideSpread, q.volError});
            const double dist = std::fabs(std::log(q.moneyness));
            if (dist < atmDist) {
                atmDist = dist;
                atm = AtmErrorData{e.expiryIso, T,          q.strike,  q.moneyness,
                                   q.marketVol, q.modelVol, q.volError};
            }
            f.quotes.push_back(q);
        }
        if (atm) {
            if (std::fabs(atm->volError) > std::fabs(worstAtm) || worstAtmExpiry.empty()) {
                worstAtm = atm->volError;
                worstAtmExpiry = atm->expiryIso;
            }
            f.atm.push_back(*atm);
        }
    }
    f.rmsVolError = f.count > 0 ? std::sqrt(sumSq / f.count) : 0.0;
    f.bands = summarizeBands(points, edges);
    const double insideShare = f.count > 0 ? static_cast<double>(f.inside) / f.count : 0.0;
    const bool atmOk = std::fabs(worstAtm) <= kFitPassAtmError;
    const std::string grade = insideShare >= kFitPassInside && atmOk ? "pass"
                              : insideShare < kFitFailInside         ? "fail"
                                                                     : "warn";
    f.verdict = grade + ": " + std::to_string(f.inside) + " of " + std::to_string(f.count) +
                " quotes (" + pct(f.inside, f.count) +
                ") priced inside the bid/ask, RMS vol error " + volPoints(f.rmsVolError) +
                " vol points, largest " + volPoints(f.maxAbsVolError) +
                ", worst at-the-money error " + volPoints(worstAtm) + " vol points" +
                (worstAtmExpiry.empty() ? "" : " (" + worstAtmExpiry + ")") +
                " (pass: >= 90 % inside and every at-the-money error within 0.5 vol point; "
                "fail: below 50 % inside)";
    return f;
}

// ---------------------------------------------------------------------------
// The endpoint
// ---------------------------------------------------------------------------

namespace {

/// The strike clamp LocalVolCache / EdgeClampedLocalVol applies to `asset`
/// (see mc::EdgeClampedLocalVol: each range moved inside by kEdgeMargin in
/// log-strike unless that leaves no interior); none for an exact-step vol.
std::vector<ClampEdgeData> clampEdges(const BlackVolEntry& entry, const mc::BasketAsset& asset) {
    std::vector<ClampEdgeData> out;
    if (asset.exactStepCapable) return out;
    const double m = mc::EdgeClampedLocalVol::kEdgeMargin;
    auto edge = [&](double lo, double hi, ClampEdgeData e) {
        double a = lo * std::exp(m), b = hi * std::exp(-m);
        if (!(a < b)) {
            a = lo;
            b = hi;
        }
        // An eSSVI surface's own minStrike is QL_EPSILON: an open side.
        if (lo > QL_EPSILON) e.strikeMin = a;
        if (b < 0.5 * QL_MAX_REAL) e.strikeMax = b;
        if (e.strikeMin || e.strikeMax) out.push_back(e);
    };
    if (!asset.localVolStrikeBands.empty()) {
        // The edges a step at each expiry reads at (its band intersected
        // with the next one, at the time read), from the note's own clamp.
        mc::BasketAsset probe = asset;
        mc::LocalVolCache cache;
        cache.attach(probe);
        const auto* clamp =
            dynamic_cast<const mc::EdgeClampedLocalVol*>(probe.localVol.currentLink().get());
        if (!clamp) QUANTRA_ERROR("local vol clamp: per-expiry bands without a clamp");
        for (std::size_t i = 0; i < asset.localVolStrikeBands.size(); ++i) {
            const auto& b = asset.localVolStrikeBands[i];
            ClampEdgeData e;
            e.scope = "PerExpiry";
            e.expiryIso = DateToIso(entry.localVolStrikeBands.at(i).expiry);
            e.time = b.time;
            const auto band = clamp->strikeBand(clamp->clampedTime(b.time));
            e.strikeMin = band.first;
            e.strikeMax = band.second;
            out.push_back(e);
        }
        return out;
    }
    const double lo =
        asset.localVolStrikeRange ? asset.localVolStrikeRange->first : asset.vol->minStrike();
    const double hi =
        asset.localVolStrikeRange ? asset.localVolStrikeRange->second : asset.vol->maxStrike();
    if (entry.pillarDates.empty()) {
        ClampEdgeData e;
        e.scope = "Surface";
        edge(lo, hi, e);
        return out;
    }
    for (const Date& d : entry.pillarDates) {
        ClampEdgeData e;
        e.scope = "Surface";
        e.expiryIso = DateToIso(d);
        e.time = asset.vol->timeFromReference(d);
        edge(lo, hi, e);
    }
    return out;
}

Handle<QuantLib::YieldTermStructure> findCurve(const PricingRegistry& reg, const std::string& id) {
    auto it = reg.rates.curves.find(id);
    if (it == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + id);
    }
    return Handle<QuantLib::YieldTermStructure>(it->second->currentLink());
}

std::string shapeName(quantra::enums::VolSurfaceShape s) {
    switch (s) {
        case quantra::enums::VolSurfaceShape_Constant: return "Constant";
        case quantra::enums::VolSurfaceShape_AtmMatrix2D: return "AtmMatrix2D";
        case quantra::enums::VolSurfaceShape_SmileCube3D: return "SmileCube3D";
        case quantra::enums::VolSurfaceShape_SurfaceFromPrices: return "SurfaceFromPrices";
        case quantra::enums::VolSurfaceShape_SviSlices: return "SviSlices";
        case quantra::enums::VolSurfaceShape_SsviSlices: return "SsviSlices";
        default: return "unknown";
    }
}

/// SVI payload slice checks.
std::vector<SviSliceCheckData> sviSliceChecks(
    const SmileSliceVolSurface& s,
    const BlackVolEntry& entry,
    double quotedLo,
    double quotedHi,
    std::vector<std::string>& violations) {
    std::vector<SviSliceCheckData> out;
    const auto& slices = s.slices();
    std::vector<SviSliceParams> params;
    for (const auto& sl : slices) {
        const auto* svi = dynamic_cast<const SviSmileSlice*>(sl.get());
        if (!svi) QUANTRA_ERROR("SVI payload slice is not an SVI slice");
        params.push_back(svi->params());
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const SviSliceParams& p = params[i];
        SviSliceCheckData c;
        const Date d = entry.pillarDates.at(i);
        c.expiryIso = DateToIso(d);
        c.time = s.timeFromReference(d);
        double lo = quotedLo, hi = quotedHi;
        if (i < entry.localVolStrikeBands.size()) {
            lo = entry.localVolStrikeBands[i].minStrike;
            hi = entry.localVolStrikeBands[i].maxStrike;
        }
        const double kLo = std::log(lo / p.forward), kHi = std::log(hi / p.forward);
        auto w = [](const SviSliceParams& q, double k) {
            return sviRawTotalVariance(q.a, q.b, q.sigma, q.rho, q.m, k);
        };
        c.butterflyMinG = std::numeric_limits<double>::infinity();
        std::optional<double> minDw;
        double minDwK = 0.0;
        for (unsigned j = 0; j < kSliceCheckPoints; ++j) {
            const double k = kLo + (kHi - kLo) * j / (kSliceCheckPoints - 1);
            const double dd = k - p.m;
            const double R = std::sqrt(dd * dd + p.sigma * p.sigma);
            const double wv = w(p, k);
            double g = -std::numeric_limits<double>::infinity();
            if (wv > 0.0) {
                const double w1 = p.b * (p.rho + dd / R);
                const double w2 = p.b * p.sigma * p.sigma / (R * R * R);
                const double t1 = 1.0 - k * w1 / (2.0 * wv);
                g = t1 * t1 - (w1 * w1 / 4.0) * (1.0 / wv + 0.25) + w2 / 2.0;
            }
            if (g < c.butterflyMinG) {
                c.butterflyMinG = g;
                c.butterflyMinGK = k;
            }
            if (i > 0) {
                const double dw = wv - w(params[i - 1], k);
                if (!minDw || dw < *minDw) {
                    minDw = dw;
                    minDwK = k;
                }
            }
        }
        c.wingSlope = p.b * (1.0 + std::fabs(p.rho));
        if (c.butterflyMinG < 0.0) {
            violations.push_back(
                "butterfly at SVI slice " + c.expiryIso + ": min g " + fmt(c.butterflyMinG) +
                " at k " + fmt(c.butterflyMinGK));
        }
        if (c.wingSlope > kLeeSlope) {
            violations.push_back(
                "wing slope at SVI slice " + c.expiryIso + ": b (1 + |rho|) " + fmt(c.wingSlope) +
                " above Lee's bound 2");
        }
        if (minDw) {
            const double dT = c.time - out.back().time;
            c.calendarMinDw = *minDw;
            c.calendarMinDwK = minDwK;
            c.calendarMinForwardVol =
                *minDw >= 0.0 ? std::sqrt(*minDw / dT) : -std::sqrt(-*minDw / dT);
            if (*minDw < 0.0) {
                violations.push_back(
                    "calendar at SVI slice " + c.expiryIso + " vs " + out.back().expiryIso +
                    ": total variance decreases by " + fmt(-*minDw) + " at k " + fmt(minDwK));
            }
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

VolSurfaceValidationResult VolSurfaceValidationEvaluator::evaluate(
    const VolSurfaceValidationInputs& in,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    const auto tStart = std::chrono::steady_clock::now();
    if (!in.checkFit && !in.checkArbitrage && !in.checkSimulation) {
        QUANTRA_INVALID_ARGUMENT("checks: at least one of fit, arbitrage, simulation must be true");
    }
    if (in.checkSimulation && !in.simulation) {
        QUANTRA_INVALID_ARGUMENT(
            "simulation: required with checks.simulation = true (the Monte Carlo settings)");
    }
    if (!in.checkSimulation && in.simulation) {
        QUANTRA_INVALID_ARGUMENT("simulation: must be absent unless checks.simulation = true");
    }
    if (in.sheetUnderlyingId != in.underlyingId) {
        QUANTRA_INVALID_ARGUMENT(
            "quotes.underlying_id '" + in.sheetUnderlyingId + "' must equal underlying_id '" +
            in.underlyingId + "': the sheet belongs to another underlying");
    }
    if (in.style != OptionSheetStyle::European) {
        QUANTRA_INVALID_ARGUMENT(
            "quotes.option_style must be European: the quotes are read with Black-76 on the "
            "forward");
    }
    if (in.quotes.empty()) {
        QUANTRA_INVALID_ARGUMENT("quotes.quotes must contain at least one quote");
    }
    validateQuoteFilter(in.filter);

    VolSurfaceValidationResult out;
    out.volatilityId = in.volatilityId;
    out.underlyingId = in.underlyingId;
    out.discountingCurveId = in.discountingCurveId;
    out.dividendCurveId = in.dividendCurveId;
    out.sheetId = in.sheetId;
    out.filter = in.filter;
    out.checkFit = in.checkFit;
    out.checkArbitrage = in.checkArbitrage;
    out.checkSimulation = in.checkSimulation;
    out.checkArbitrageGrid = in.checkArbitrageGrid;
    if (in.checkArbitrageGrid && !in.checkArbitrage) {
        QUANTRA_INVALID_ARGUMENT("checks.arbitrage_grid: requires checks.arbitrage = true");
    }
    out.bands = resolveBandEdges(in.bands, "bands");

    // Market.
    auto eqIt = reg.equity.equityUnderlyings.find(in.underlyingId);
    if (eqIt == reg.equity.equityUnderlyings.end()) {
        QUANTRA_NOT_FOUND("Equity underlying not found: " + in.underlyingId);
    }
    auto volIt = reg.volatility.blackVols.find(in.volatilityId);
    if (volIt == reg.volatility.blackVols.end()) {
        if (reg.volatility.swaptionVols.count(in.volatilityId) ||
            reg.volatility.optionletVols.count(in.volatilityId) ||
            reg.volatility.yoyOptionletVols.count(in.volatilityId)) {
            QUANTRA_INVALID_ARGUMENT(
                "volatility_id '" + in.volatilityId +
                "' is an interest-rate or inflation vol; an equity Black vol (BlackVolSpec, "
                "SviVolSpec or SsviVolSpec) is required");
        }
        QUANTRA_NOT_FOUND("Black vol not found: " + in.volatilityId);
    }
    const BlackVolEntry& entry = volIt->second;
    out.shape = entry.shapeFb;
    if (entry.shapeFb == quantra::enums::VolSurfaceShape_SabrParams ||
        entry.shapeFb == quantra::enums::VolSurfaceShape_SabrCalibrate) {
        QUANTRA_INVALID_ARGUMENT(
            "volatility_id '" + in.volatilityId + "' has shape " + shapeName(entry.shapeFb) +
            ", not an equity Black vol surface");
    }
    const Handle<QuantLib::Quote> spotH = eqIt->second.spot;
    const double spot = spotH->value();
    if (!std::isfinite(spot) || !(spot > 0.0)) {
        QUANTRA_INVALID_ARGUMENT(
            "spot of underlying '" + in.underlyingId + "' must be finite and > 0");
    }
    out.spot = spot;
    const Handle<QuantLib::YieldTermStructure> curve = findCurve(reg, in.discountingCurveId);
    const Handle<QuantLib::YieldTermStructure> dividend = eqIt->second.dividend;
    const Date asOf = ctx.asOf;
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();

    std::map<Date, VolSurfaceForwardInput> forwardByDate;
    if (in.forwards) {
        out.forwardsSource = "request";
        for (const auto& f : *in.forwards) {
            if (!(f.forward > 0.0) || !std::isfinite(f.forward)) {
                QUANTRA_INVALID_ARGUMENT(
                    "forwards[" + f.expiryIso + "].forward must be finite and > 0");
            }
            if (!(f.impliedDiscountFactor > 0.0) || !std::isfinite(f.impliedDiscountFactor)) {
                QUANTRA_INVALID_ARGUMENT(
                    "forwards[" + f.expiryIso + "].implied_discount_factor must be finite and > 0");
            }
            if (f.expiry <= asOf) {
                QUANTRA_INVALID_ARGUMENT(
                    "forwards[" + f.expiryIso + "]: expiry must be after as_of_date");
            }
            if (!forwardByDate.emplace(f.expiry, f).second) {
                QUANTRA_INVALID_ARGUMENT("forwards lists expiry " + f.expiryIso + " twice");
            }
        }
        if (forwardByDate.empty()) {
            QUANTRA_INVALID_ARGUMENT(
                "forwards carries no usable expiry (an entry needs usable: true, forward and "
                "implied_discount_factor)");
        }
    } else {
        out.forwardsSource = "curves";
    }

    // Quotes per expiry.
    std::map<Date, std::vector<const OptionQuoteLine*>> byExpiry;
    for (const auto& q : in.quotes)
        byExpiry[q.expiry].push_back(&q);
    for (const auto& [expiry, lines] : byExpiry) {
        ValidationExpiry e;
        e.expiry = expiry;
        e.expiryIso = lines.front()->expiryIso;
        e.time = dc.yearFraction(asOf, expiry);
        auto dropExpiry = [&](const std::string& reason) {
            e.usable = false;
            e.droppedReason = reason;
            e.dropped.push_back(DroppedQuoteData{e.expiryIso, std::nullopt, reason});
            out.expiries.push_back(e);
        };
        if (expiry <= asOf) {
            for (const auto* q : lines)
                e.dropped.push_back(
                    DroppedQuoteData{q->expiryIso, q->strike, "expiry not after as_of_date"});
            dropExpiry("expiry not after as_of_date");
            continue;
        }
        double F = 0.0, DF = 0.0;
        if (in.forwards) {
            auto fIt = forwardByDate.find(expiry);
            if (fIt == forwardByDate.end()) {
                dropExpiry("no forward for this expiry in forwards");
                continue;
            }
            F = fIt->second.forward;
            DF = fIt->second.impliedDiscountFactor;
        } else {
            DF = curve->discount(e.time, true);
            F = spot * dividend->discount(e.time, true) / DF;
        }
        e.forward = F;
        e.discountFactor = DF;
        e.used = selectOutOfTheMoneyQuotes(
            lines, in.filter, e.expiryIso, spot, F, DF, e.time, e.dropped);
        if (e.used.size() < in.filter.minQuotesPerExpiry) {
            e.used.clear();
            dropExpiry(
                "fewer than " + std::to_string(in.filter.minQuotesPerExpiry) +
                " quotes with an implied vol; filter.min_quotes_per_expiry");
            continue;
        }
        e.usable = true;
        out.expiries.push_back(e);
    }
    std::vector<const ValidationExpiry*> work;
    for (const auto& e : out.expiries)
        if (e.usable) work.push_back(&e);
    if (work.empty()) {
        std::ostringstream os;
        os << "quotes: no expiry of sheet '" << in.sheetId << "' is usable:";
        for (const auto& e : out.expiries)
            os << " [" << e.expiryIso << ": " << e.droppedReason << "]";
        QUANTRA_INVALID_ARGUMENT(os.str());
    }

    // Curves of the checks.
    Handle<QuantLib::YieldTermStructure> rTS = curve, qTS = dividend;
    if (in.forwards) {
        std::vector<Date> dates;
        std::vector<double> fwds, dfs;
        for (const auto& [d, f] : forwardByDate) {
            dates.push_back(d);
            fwds.push_back(f.forward);
            dfs.push_back(f.impliedDiscountFactor);
        }
        const SheetForwardCurves sc = sheetForwardCurves(asOf, spot, dates, fwds, dfs);
        rTS = sc.riskFree;
        qTS = sc.dividend;
    }

    // --- fit -------------------------------------------------------------------------
    if (in.checkFit) {
        const auto t0 = std::chrono::steady_clock::now();
        out.fit = fitQuotes(*entry.handle.currentLink(), out.expiries, out.bands);
        out.fitMilliseconds = msSince(t0);
    }

    // --- arbitrage -------------------------------------------------------------------
    if (in.checkArbitrage) {
        ctx.budget.check();
        const auto t0 = std::chrono::steady_clock::now();
        ArbitrageValidationData a;
        double quotedLo = QL_MAX_REAL, quotedHi = 0.0;
        std::vector<RegionQuotes> rq;
        std::vector<double> regionTimes;
        for (const auto* e : work) {
            quotedLo = std::min(quotedLo, e->used.front().strike);
            quotedHi = std::max(quotedHi, e->used.back().strike);
            rq.push_back({e->time, *e->forward, &e->used});
            regionTimes.push_back(e->time);
        }
        // The surface's own strike limits widen the clamp: a grid's edge
        // strikes, a payload's strike_min / strike_max envelope.
        double clampLo = quotedLo, clampHi = quotedHi;
        if (!entry.strikes.empty()) {
            clampLo = std::min(clampLo, entry.strikes.front());
            clampHi = std::max(clampHi, entry.strikes.back());
        } else if (entry.localVolStrikeRange) {
            clampLo = std::min(clampLo, entry.localVolStrikeRange->first);
            clampHi = std::max(clampHi, entry.localVolStrikeRange->second);
        } else if (entry.localVolStrikeBands.empty()) {
            // A payload read unclamped (an SVI or eSSVI payload without
            // strike limits): the regions reach as far as the quotes' vols
            // carry a path, no strike limit.
            clampLo = 0.0;
            clampHi = QL_MAX_REAL;
        }
        const auto regions = quoteRegions(rq, clampLo, clampHi);
        std::vector<double> expiryTimes;
        for (const Date& d : entry.pillarDates)
            expiryTimes.push_back(entry.handle->timeFromReference(d));
        if (expiryTimes.empty()) expiryTimes = regionTimes;
        mc::BasketAsset probe = mc::MakeBasketAsset(in.underlyingId, spotH, qTS, rTS, entry.handle);
        applyLocalVolSettings(entry, probe);
        if (in.checkArbitrageGrid) a.grid = LocalVolGridData{};
        a.localVol = checkLocalVol(
            entry.handle, probe.localVolStrikeBands, probe.localVolStrikeRange, probe.localVolTime,
            spotH, rTS, qTS, expiryTimes, regionTimes, regions, nullptr,
            a.grid ? &*a.grid : nullptr);
        a.clampEdges = clampEdges(entry, probe);
        if (entry.shapeFb == quantra::enums::VolSurfaceShape_SviSlices) {
            const auto s =
                std::dynamic_pointer_cast<SmileSliceVolSurface>(entry.handle.currentLink());
            if (!s) QUANTRA_ERROR("SVI payload is not a slice surface");
            a.sviSlices = sviSliceChecks(*s, entry, quotedLo, quotedHi, a.violations);
        }
        if (entry.shapeFb == quantra::enums::VolSurfaceShape_SsviSlices) {
            const auto s = std::dynamic_pointer_cast<SsviBlackVol>(entry.handle.currentLink());
            if (!s) QUANTRA_ERROR("SSVI payload is not an eSSVI surface");
            const auto& slices = s->surface().slices();
            SsviArbitrageCheckData sc;
            for (const auto& v : ssvi::checkArbitrage(slices)) {
                std::ostringstream os;
                os << v.condition << " at slice " << v.slice;
                if (v.prevSlice != v.slice) os << " vs slice " << v.prevSlice;
                os << " (by " << fmt(v.margin, 10) << ")";
                sc.violations.push_back(os.str());
                a.violations.push_back("SSVI " + os.str());
            }
            const ssvi::ScanResult scan = ssvi::defaultScan(s->surface());
            sc.scanMinG = scan.minG;
            sc.scanMinGTime = scan.minGt;
            sc.scanMinGK = scan.minGk;
            sc.scanMinDwDt = scan.minWt;
            sc.scanMinDwDtTime = scan.minWtT;
            sc.scanMinDwDtK = scan.minWtK;
            a.ssvi = sc;
            std::vector<SsviSliceCheckData> rows;
            for (std::size_t i = 0; i < slices.size(); ++i) {
                const auto& sl = slices[i];
                SsviSliceCheckData r;
                r.expiryIso = DateToIso(s->expiries().at(i));
                r.time = sl.t;
                r.theta = sl.theta;
                r.rho = sl.rho;
                r.psi = sl.psi;
                const double ar = 1.0 + std::fabs(sl.rho);
                r.marginButterflyPsi = 4.0 - sl.psi * ar;
                r.marginButterflyPsi2 = 4.0 * sl.theta - sl.psi * sl.psi * ar;
                if (i > 0) {
                    const auto& p = slices[i - 1];
                    r.marginCalendarTheta = sl.theta - p.theta;
                    r.marginCalendarPsi = sl.psi - p.psi;
                    r.marginCalendarA =
                        (sl.psi - p.psi) - std::fabs(sl.rho * sl.psi - p.rho * p.psi);
                }
                rows.push_back(r);
            }
            a.ssviSlices = rows;
        }
        a.arbitrageFree = a.localVol.arbitrageFree && a.violations.empty();
        std::ostringstream os;
        os << (a.arbitrageFree ? "pass" : "fail") << ": local vol evaluated at "
           << (a.localVol.points - a.localVol.refused) << " of " << a.localVol.points
           << " scan points";
        if (a.localVol.refused > 0) os << " (first refusal: " << a.localVol.firstRefusal << ")";
        if (a.localVol.minLocalVariance) {
            os << ", smallest local vol " << fmt(std::sqrt(*a.localVol.minLocalVariance), 4)
               << " at t " << fmt(*a.localVol.minLocalVarianceTime, 4) << ", K "
               << fmt(*a.localVol.minLocalVarianceStrike, 8);
        }
        if (a.sviSlices || a.ssviSlices) {
            os << "; " << a.violations.size() << " slice condition(s) violated";
        }
        a.verdict = os.str();
        out.arbitrage = a;
        out.arbitrageMilliseconds = msSince(t0);
    }

    // --- simulation ------------------------------------------------------------------
    if (in.checkSimulation) {
        ctx.budget.check();
        const auto t0 = std::chrono::steady_clock::now();
        const MonteCarloData& mcd = *in.simulation;
        mc::BasketAsset asset = mc::MakeBasketAsset(in.underlyingId, spotH, qTS, rTS, entry.handle);
        applyLocalVolSettings(entry, asset);
        const bool exact = asset.exactStepCapable;
        if (!exact && !mcd.stepsPerYear) {
            QUANTRA_INVALID_ARGUMENT(
                "simulation.steps_per_year: required, volatility '" + in.volatilityId +
                "' is a strike x expiry surface (local-vol evolution)");
        }
        if (exact && mcd.stepsPerYear) {
            QUANTRA_INVALID_ARGUMENT(
                "simulation.steps_per_year: must be absent, volatility '" + in.volatilityId +
                "' is constant or term (exact lognormal steps between expiries)");
        }
        if (exact && mcd.localVolFloor) {
            QUANTRA_INVALID_ARGUMENT(
                "simulation.local_vol_floor: must be absent, volatility '" + in.volatilityId +
                "' is constant or term (not simulated as local vol)");
        }
        if (!exact) asset.localVolFloor = mcd.localVolFloor;
        mc::LocalVolCache cache;
        cache.attach(asset);
        const auto procs = mc::BasketModel::build(
            std::vector<mc::BasketAsset>{asset}, QuantLib::Matrix(1, 1, 1.0));
        std::vector<Date> dates;
        for (const auto* e : work)
            dates.push_back(e->expiry);
        std::optional<QuantLib::Size> steps;
        if (mcd.stepsPerYear) steps = static_cast<QuantLib::Size>(*mcd.stepsPerYear);
        const mc::SimulationGrid grid = mc::BuildSimulationGrid(asOf, dates, steps);
        struct Opt {
            QuantLib::Size gridIndex;
            double strike;
            bool isCall;
        };
        std::vector<Opt> opts;
        SimulationValidationData sd;
        for (const auto* e : work) {
            const QuantLib::Size gi = grid.indexOf(e->expiry);
            for (const auto& u : e->used) {
                opts.push_back({gi, u.strike, u.isCall});
                QuoteSimulationData q;
                q.expiryIso = e->expiryIso;
                q.time = e->time;
                q.strike = u.strike;
                q.moneyness = u.strike / *e->forward;
                q.isCall = u.isCall;
                q.bid = u.bid;
                q.ask = u.ask;
                q.mid = u.mid;
                q.marketVol = u.vol;
                sd.quotes.push_back(q);
            }
        }
        mc::PathEngineConfig cfg;
        cfg.paths = mcd.paths;
        cfg.seed = mcd.seed;
        cfg.sequence = mcd.sequence == RandomSequenceKind::Sobol
                           ? mc::PathEngineConfig::Sequence::Sobol
                           : mc::PathEngineConfig::Sequence::PseudoRandom;
        cfg.antithetic = mcd.antithetic;
        cfg.brownianBridge = mcd.brownianBridge;
        const QuantLib::Size n = opts.size();
        const mc::PathFunctor functor = [&](const QuantLib::MultiPath& path,
                                            std::vector<QuantLib::Real>& acc) {
            for (QuantLib::Size i = 0; i < n; ++i) {
                const Opt& o = opts[i];
                const double level = path[0][o.gridIndex];
                acc[i] = std::max(o.isCall ? level - o.strike : o.strike - level, 0.0);
            }
        };
        mc::PathEngineResult res;
        try {
            res = mc::RunPaths(procs, grid.grid, cfg, n, functor);
        } catch (const QuantraError&) {
            throw;
        } catch (const std::exception& ex) {
            QUANTRA_INVALID_ARGUMENT(
                "volatility_id '" + in.volatilityId +
                "': the local volatility derived from the surface could not be evaluated along "
                "the simulated paths (" +
                std::string(ex.what()) +
                "); the surface is not arbitrage-free where the paths went (see the arbitrage "
                "check)");
        }
        sd.paths = static_cast<unsigned>(res.paths);
        sd.timeSteps = static_cast<unsigned>(grid.size() - 1);
        sd.seed = mcd.seed;
        std::vector<BandPoint> points;
        std::size_t i = 0;
        for (const auto* e : work) {
            const double df = *e->discountFactor, fwd = *e->forward;
            for (std::size_t j = 0; j < e->used.size(); ++j, ++i) {
                QuoteSimulationData& q = sd.quotes[i];
                q.mcPrice = df * res.mean[i];
                q.standardError = df * res.standardError[i];
                q.insideSpread = q.mcPrice >= q.bid && q.mcPrice <= q.ask;
                q.withinNoise = q.mcPrice >= q.bid - kNoiseStdErrors * q.standardError &&
                                q.mcPrice <= q.ask + kNoiseStdErrors * q.standardError;
                q.priceError = q.mcPrice - q.mid;
                q.mcVol = impliedBlackVol(
                    q.isCall ? QuantLib::Option::Call : QuantLib::Option::Put, q.strike, fwd,
                    q.mcPrice, df, q.time);
                if (q.mcVol) q.volError = *q.mcVol - q.marketVol;
                ++sd.count;
                if (q.insideSpread) ++sd.inside;
                if (q.withinNoise) ++sd.withinNoise;
                sd.maxAbsPriceError = std::max(sd.maxAbsPriceError, std::fabs(q.priceError));
                sd.maxStandardError = std::max(sd.maxStandardError, q.standardError);
                if (q.volError) {
                    sd.maxAbsVolError =
                        std::max(sd.maxAbsVolError.value_or(0.0), std::fabs(*q.volError));
                }
                points.push_back({q.moneyness, q.time, q.insideSpread, q.volError});
            }
        }
        sd.bands = summarizeBands(points, out.bands);
        const double share = sd.count > 0 ? static_cast<double>(sd.withinNoise) / sd.count : 0.0;
        const std::string grade = share >= kSimPassWithin  ? "pass"
                                  : share < kSimFailWithin ? "fail"
                                                           : "warn";
        sd.verdict = grade + ": " + std::to_string(sd.withinNoise) + " of " +
                     std::to_string(sd.count) + " quotes (" + pct(sd.withinNoise, sd.count) +
                     ") priced within 3.5 standard errors of their bid/ask (" +
                     std::to_string(sd.inside) + " strictly inside) at " +
                     std::to_string(sd.paths) + " paths; largest standard error " +
                     fmt(sd.maxStandardError, 6) +
                     (sd.maxAbsVolError
                          ? ", largest vol error " + volPoints(*sd.maxAbsVolError) + " vol points"
                          : std::string()) +
                     " (pass: >= 95 % within the noise; fail: below 80 %)";
        out.simulation = sd;
        out.simulationMilliseconds = msSince(t0);
    }
    out.totalMilliseconds = msSince(tStart);
    return out;
}

} // namespace quantra
