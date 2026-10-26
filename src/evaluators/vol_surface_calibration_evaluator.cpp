#include "vol_surface_calibration_evaluator.h"

#include "date_convert.h"
#include "error.h"
#include "smile_slice_vol_surface.h"

#include <ql/exercise.hpp>
#include <ql/experimental/volatility/sviinterpolation.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/array.hpp>
#include <ql/math/matrix.hpp>
#include <ql/math/optimization/constraint.hpp>
#include <ql/math/optimization/costfunction.hpp>
#include <ql/math/optimization/endcriteria.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/math/optimization/problem.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/andreasenhugevolatilityadapter.hpp>
#include <ql/termstructures/volatility/equityfx/andreasenhugevolatilityinterpl.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace quantra {

namespace {

using QuantLib::Date;
using QuantLib::Handle;
using QuantLib::Real;

constexpr double kCalendarFloor = 1e-8; // total variance
constexpr double kButterflyPenalty = 100.0;
constexpr unsigned kButterflyPoints = 201;
constexpr unsigned kMinQuotesSvi = 5;
// Andreasen-Huge grid inversion: the one-step finite-difference price carries
// noise of ~1e-7 of the spot; below this fraction of the spot that noise
// moves the implied vol by more than a tenth of a vol point.
constexpr double kGridInversionResolution = 1e-4;

using UsedQuote = OutOfTheMoneyQuote;

struct ExpiryWork {
    Date expiry;
    double time = 0.0;
    double forward = 0.0;
    double df = 0.0;
    std::vector<UsedQuote> used; // sorted by strike
    std::optional<SviParams> svi;
};

std::string fmt(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

std::string trimMessage(const std::string& s) {
    std::string t = s;
    for (auto& c : t)
        if (c == '\n') c = ' ';
    return t.size() > 160 ? t.substr(0, 160) + "..." : t;
}

std::vector<double> defaultMoneyness() {
    std::vector<double> m;
    for (int i = 0; i <= 40; ++i)
        m.push_back(std::round((0.50 + 0.025 * i) * 1e6) / 1e6);
    return m;
}

// ---------------------------------------------------------------------------
// SVI helpers: parameter transforms (QuantLib's SviSpecs, so the penalized
// refit keeps the same admissible set), weighted residuals and the penalty.
// ---------------------------------------------------------------------------

constexpr double kEps1 = 1e-6;
constexpr double kEps2 = 0.999999;

SviParams sviDirect(const QuantLib::Array& x) {
    SviParams p;
    p.sigma = x[2] * x[2] + kEps1;
    p.rho = std::sin(x[3]) * kEps2;
    p.m = x[4];
    p.b = (std::atan(x[1]) + M_PI / 2.0) / M_PI * kEps2 * 4.0 / (1.0 + std::fabs(p.rho));
    p.a = kEps1 + x[0] * x[0] - p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
    return p;
}

QuantLib::Array sviInverse(const SviParams& p) {
    QuantLib::Array x(5);
    x[2] = std::sqrt(std::max(0.0, p.sigma - kEps1));
    x[3] = std::asin(std::max(-1.0, std::min(1.0, p.rho / kEps2)));
    x[4] = p.m;
    x[1] = std::tan(
        std::max(1e-12, std::min(1.0 - 1e-12, p.b / 4.0 * (1.0 + std::fabs(p.rho)) / kEps2)) *
            M_PI -
        M_PI / 2.0);
    x[0] = std::sqrt(std::max(0.0, p.a - kEps1 + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho)));
    return x;
}

class SviPenalizedCost : public QuantLib::CostFunction {
public:
    SviPenalizedCost(
        std::vector<double> ks,
        std::vector<double> vols,
        std::vector<double> sqrtWeights,
        double T,
        std::vector<double> penaltyKs)
        : ks_(std::move(ks)), vols_(std::move(vols)), sqrtWeights_(std::move(sqrtWeights)), T_(T),
          penaltyKs_(std::move(penaltyKs)) {}

    QuantLib::Array values(const QuantLib::Array& x) const override {
        const SviParams p = sviDirect(x);
        QuantLib::Array r(ks_.size() + penaltyKs_.size());
        for (std::size_t i = 0; i < ks_.size(); ++i) {
            const double w = std::max(0.0, sviTotalVariance(p, ks_[i]));
            r[i] = sqrtWeights_[i] * (std::sqrt(w / T_) - vols_[i]);
        }
        for (std::size_t j = 0; j < penaltyKs_.size(); ++j) {
            r[ks_.size() + j] = kButterflyPenalty * std::max(0.0, -sviDurrlemanG(p, penaltyKs_[j]));
        }
        return r;
    }

private:
    std::vector<double> ks_, vols_, sqrtWeights_;
    double T_;
    std::vector<double> penaltyKs_;
};

std::vector<double> butterflyGrid(double kLo, double kHi) {
    std::vector<double> ks;
    ks.reserve(kButterflyPoints);
    for (unsigned j = 0; j < kButterflyPoints; ++j)
        ks.push_back(kLo + (kHi - kLo) * static_cast<double>(j) / (kButterflyPoints - 1));
    return ks;
}

double minDurrleman(const SviParams& p, const std::vector<double>& ks) {
    double g = std::numeric_limits<double>::infinity();
    for (double k : ks)
        g = std::min(g, sviDurrlemanG(p, k));
    return g;
}

} // namespace

double sviTotalVariance(const SviParams& p, double k) {
    return sviRawTotalVariance(p.a, p.b, p.sigma, p.rho, p.m, k);
}

double sviDurrlemanG(const SviParams& p, double k) {
    const double d = k - p.m;
    const double R = std::sqrt(d * d + p.sigma * p.sigma);
    const double w = sviTotalVariance(p, k);
    if (!(w > 0.0)) return -std::numeric_limits<double>::infinity();
    const double w1 = p.b * (p.rho + d / R);
    const double w2 = p.b * p.sigma * p.sigma / (R * R * R);
    const double t1 = 1.0 - k * w1 / (2.0 * w);
    return t1 * t1 - (w1 * w1 / 4.0) * (1.0 / w + 0.25) + w2 / 2.0;
}

VolSurfaceCalibrationResult VolSurfaceCalibrationEvaluator::evaluate(
    const VolSurfaceCalibrationInputs& in,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    const ImpliedForwardsInputs& base = in.base;
    if (base.style != OptionSheetStyle::European) {
        QUANTRA_INVALID_ARGUMENT(
            "sheet.option_style must be European: the implied vols are Black-76 on the forward, "
            "which does not hold for American or Bermudan options");
    }
    validateQuoteFilter(base.filter);
    if (base.quotes.empty()) {
        QUANTRA_INVALID_ARGUMENT("sheet.quotes must contain at least one quote");
    }
    if (in.volSurfaceId.empty()) {
        QUANTRA_INVALID_ARGUMENT("vol_surface_id is required");
    }
    const SheetMarket market = resolveSheetMarket(reg, base.underlyingId, base.discountingCurveId);
    const double spot = market.spot;
    const Date asOf = ctx.asOf;
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();

    // Output moneyness.
    std::vector<double> moneyness = in.moneyness.empty() ? defaultMoneyness() : in.moneyness;
    for (std::size_t j = 0; j < moneyness.size(); ++j) {
        if (!std::isfinite(moneyness[j]) || !(moneyness[j] > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(
                "output_grid.strikes_as_moneyness[" + std::to_string(j) +
                "] must be finite and > 0");
        }
        if (j > 0 && !(moneyness[j] > moneyness[j - 1])) {
            QUANTRA_INVALID_ARGUMENT(
                "output_grid.strikes_as_moneyness must be strictly increasing");
        }
    }

    // ------------------------------------------------------------------
    // 1. Forwards: from the request or from the implied-forward fit.
    // ------------------------------------------------------------------
    VolSurfaceCalibrationResult out;
    out.sheetId = base.sheetId;
    out.underlyingId = base.underlyingId;
    out.spot = spot;
    out.method = in.method;
    out.filter = base.filter;
    out.asOfIso = DateToIso(asOf);
    out.volSurfaceId = in.volSurfaceId;

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
        out.forwardsSource = "fitted";
        ImpliedForwardsInputs fitIn = base;
        fitIn.dividendCurveId = "unused";
        const ImpliedForwardsResult fit = ImpliedForwardsEvaluator().evaluate(fitIn, reg, ctx);
        for (const auto& e : fit.expiries) {
            if (!e.usable) continue;
            VolSurfaceForwardInput f;
            f.expiryIso = e.expiryIso;
            f.expiry = e.expiry;
            f.forward = *e.forward;
            f.impliedDiscountFactor = *e.impliedDiscountFactor;
            forwardByDate.emplace(f.expiry, f);
        }
    }

    // ------------------------------------------------------------------
    // 2. Per expiry: filters, out-of-the-money mids, implied vols.
    // ------------------------------------------------------------------
    std::map<Date, std::vector<const OptionQuoteLine*>> byExpiry;
    for (const auto& q : base.quotes)
        byExpiry[q.expiry].push_back(&q);

    const unsigned minUsed = std::max(
        base.filter.minQuotesPerExpiry,
        in.method == VolSurfaceFitMethod::SviPerExpiry ? kMinQuotesSvi : 1u);

    std::vector<ExpiryWork> work; // usable expiries, date order
    for (const auto& [expiry, lines] : byExpiry) {
        VolSurfaceExpiryReportData e;
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
        auto fIt = forwardByDate.find(expiry);
        if (fIt == forwardByDate.end()) {
            dropExpiry(
                in.forwards ? "no forward for this expiry in forwards"
                            : "no forward: the implied-forward fit dropped this expiry");
            continue;
        }
        const double F = fIt->second.forward, DF = fIt->second.impliedDiscountFactor;
        const double T = e.time;
        e.forward = F;
        e.impliedDiscountFactor = DF;

        ExpiryWork w;
        w.expiry = expiry;
        w.time = T;
        w.forward = F;
        w.df = DF;
        w.used =
            selectOutOfTheMoneyQuotes(lines, base.filter, e.expiryIso, spot, F, DF, T, e.dropped);
        if (w.used.size() < minUsed) {
            dropExpiry(
                "fewer than " + std::to_string(minUsed) + " quotes with an implied vol (" +
                std::to_string(w.used.size()) + ")" +
                (in.method == VolSurfaceFitMethod::SviPerExpiry
                     ? "; the SVI slice needs max(filter.min_quotes_per_expiry, 5)"
                     : "; filter.min_quotes_per_expiry"));
            continue;
        }
        e.usable = true;
        e.quotesUsed = static_cast<unsigned>(w.used.size());
        out.expiries.push_back(e);
        work.push_back(std::move(w));
        out.forwards.push_back(fIt->second);
        out.forwards.back().time = T;
    }
    if (work.empty()) {
        std::ostringstream os;
        os << "No expiry of sheet '" << base.sheetId << "' is usable for the vol-surface fit:";
        for (const auto& e : out.expiries)
            os << " [" << e.expiryIso << ": " << e.droppedReason << "]";
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
    auto reportOf = [&](const Date& d) -> VolSurfaceExpiryReportData& {
        for (auto& e : out.expiries)
            if (e.expiry == d) return e;
        QUANTRA_ERROR("internal: expiry report missing");
        return out.expiries.front(); // unreachable
    };

    // ------------------------------------------------------------------
    // Output grid.
    // ------------------------------------------------------------------
    out.moneyness = moneyness;
    for (double m : moneyness)
        out.gridStrikes.push_back(m * spot);
    std::vector<Date> gridDates;
    if (in.gridExpiries.empty()) {
        for (const auto& w : work)
            gridDates.push_back(w.expiry);
    } else {
        const QuantLib::NullCalendar cal;
        for (std::size_t i = 0; i < in.gridExpiries.size(); ++i) {
            const Date d = cal.advance(asOf, in.gridExpiries[i], QuantLib::Unadjusted);
            if (d <= asOf) {
                QUANTRA_INVALID_ARGUMENT(
                    "output_grid.expiries[" + std::to_string(i) + "] must be after as_of_date");
            }
            if (!gridDates.empty() && d <= gridDates.back()) {
                QUANTRA_INVALID_ARGUMENT("output_grid.expiries must be strictly increasing");
            }
            if (d > work.back().expiry) {
                QUANTRA_INVALID_ARGUMENT(
                    "output_grid.expiries[" + std::to_string(i) + "] (" + DateToIso(d) +
                    ") is after the last fitted sheet expiry " + DateToIso(work.back().expiry));
            }
            if (in.method == VolSurfaceFitMethod::SviPerExpiry) {
                const bool onSheet = std::any_of(
                    work.begin(), work.end(), [&](const ExpiryWork& w) { return w.expiry == d; });
                if (!onSheet) {
                    QUANTRA_INVALID_ARGUMENT(
                        "output_grid.expiries[" + std::to_string(i) + "] (" + DateToIso(d) +
                        ") is not a fitted sheet expiry; SviPerExpiry samples the fitted slices "
                        "only");
                }
            }
            gridDates.push_back(d);
        }
    }
    for (const Date& d : gridDates) {
        out.gridExpiryIso.push_back(DateToIso(d));
        out.gridExpiryDays.push_back(static_cast<int>(d - asOf));
    }
    const std::size_t nExp = gridDates.size(), nStr = out.gridStrikes.size();
    out.gridVols.assign(nExp * nStr, std::numeric_limits<double>::quiet_NaN());

    // ------------------------------------------------------------------
    // 3a. Andreasen-Huge.
    // ------------------------------------------------------------------
    if (in.method == VolSurfaceFitMethod::AndreasenHuge) {
        std::vector<Date> curveDates{asOf};
        std::vector<QuantLib::DiscountFactor> rDfs{1.0}, qDfs{1.0};
        QuantLib::AndreasenHugeVolatilityInterpl::CalibrationSet set;
        for (const auto& w : work) {
            curveDates.push_back(w.expiry);
            rDfs.push_back(w.df);
            qDfs.push_back(w.forward * w.df / spot);
            auto exercise = std::make_shared<QuantLib::EuropeanExercise>(w.expiry);
            for (const auto& u : w.used) {
                auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(
                    u.isCall ? QuantLib::Option::Call : QuantLib::Option::Put, u.strike);
                set.emplace_back(
                    std::make_shared<QuantLib::VanillaOption>(payoff, exercise),
                    std::make_shared<QuantLib::SimpleQuote>(u.vol));
            }
        }
        Handle<QuantLib::YieldTermStructure> rTS(
            std::make_shared<QuantLib::DiscountCurve>(curveDates, rDfs, dc));
        Handle<QuantLib::YieldTermStructure> qTS(
            std::make_shared<QuantLib::DiscountCurve>(curveDates, qDfs, dc));
        Handle<QuantLib::Quote> spotH(std::make_shared<QuantLib::SimpleQuote>(spot));
        std::shared_ptr<QuantLib::AndreasenHugeVolatilityInterpl> ah;
        std::shared_ptr<QuantLib::AndreasenHugeVolatilityAdapter> adapter;
        try {
            ah = std::make_shared<QuantLib::AndreasenHugeVolatilityInterpl>(
                set, spotH, rTS, qTS, QuantLib::AndreasenHugeVolatilityInterpl::CubicSpline,
                QuantLib::AndreasenHugeVolatilityInterpl::CallPut, 500);
            adapter = std::make_shared<QuantLib::AndreasenHugeVolatilityAdapter>(ah);
            const auto err = ah->calibrationError();
            AndreasenHugeErrorData ed;
            ed.minVol = std::get<0>(err);
            ed.maxVol = std::get<1>(err);
            ed.avgVol = std::get<2>(err);
            out.andreasenHugeError = ed;
        } catch (const std::exception& ex) {
            QUANTRA_INVALID_ARGUMENT(
                std::string("Andreasen-Huge calibration failed: ") + trimMessage(ex.what()));
        }
        // Fit error per expiry against the market vols.
        for (const auto& w : work) {
            auto& e = reportOf(w.expiry);
            double ss = 0.0, mx = 0.0;
            unsigned n = 0;
            for (const auto& u : w.used) {
                try {
                    const double v = adapter->blackVol(w.expiry, u.strike, true);
                    const double d = v - u.vol;
                    ss += d * d;
                    mx = std::max(mx, std::fabs(d));
                    ++n;
                } catch (...) {
                }
            }
            if (n > 0) {
                e.rmseVol = std::sqrt(ss / n);
                e.maxAbsErrorVol = mx;
            }
        }
        // Sample the grid. A point is inverted only where the model's
        // out-of-the-money price is at least 1e-4 of the spot; the rest (deep
        // wings, where the finite-difference price is noise of ~1e-7 of the
        // spot) is filled flat from the nearest inverted strike.
        for (std::size_t i = 0; i < nExp; ++i) {
            const double T = dc.yearFraction(asOf, gridDates[i]);
            const double F = ah->fwd(T);
            std::vector<double> row(nStr, std::numeric_limits<double>::quiet_NaN());
            for (std::size_t j = 0; j < nStr; ++j) {
                const double K = out.gridStrikes[j];
                try {
                    const double price = ah->optionPrice(
                        T, K, K >= F ? QuantLib::Option::Call : QuantLib::Option::Put);
                    if (!(price >= kGridInversionResolution * spot)) continue;
                    const double v = adapter->blackVol(gridDates[i], K, true);
                    if (std::isfinite(v) && v > 0.0) row[j] = v;
                } catch (...) {
                }
            }
            unsigned filled = 0;
            for (std::size_t j = 0; j < nStr; ++j) {
                if (std::isfinite(row[j])) continue;
                std::size_t best = nStr;
                std::size_t bestDist = nStr + 1;
                for (std::size_t l = 0; l < nStr; ++l) {
                    if (!std::isfinite(row[l])) continue;
                    const std::size_t dist = l > j ? l - j : j - l;
                    if (dist < bestDist) {
                        bestDist = dist;
                        best = l;
                    }
                }
                if (best == nStr) {
                    QUANTRA_INVALID_ARGUMENT(
                        "Andreasen-Huge surface could not be inverted to an implied vol at any "
                        "grid strike of expiry " +
                        out.gridExpiryIso[i]);
                }
                out.gridVols[i * nStr + j] = row[best];
                ++filled;
            }
            for (std::size_t j = 0; j < nStr; ++j)
                if (std::isfinite(row[j])) out.gridVols[i * nStr + j] = row[j];
            // Attribute the fill count to the sheet expiry on that date, if any.
            for (auto& e : out.expiries)
                if (e.usable && e.expiry == gridDates[i]) e.gridPointsFilled = filled;
        }
    }

    // ------------------------------------------------------------------
    // 3b. SVI per expiry.
    // ------------------------------------------------------------------
    if (in.method == VolSurfaceFitMethod::SviPerExpiry) {
        const auto endCriteria =
            std::make_shared<QuantLib::EndCriteria>(500, 100, 1e-12, 1e-10, 1e-10);
        for (auto& w : work) {
            auto& e = reportOf(w.expiry);
            std::vector<double> strikes, vols;
            for (const auto& u : w.used) {
                strikes.push_back(u.strike);
                vols.push_back(u.vol);
            }
            SviParams p;
            try {
                const auto lm = std::make_shared<QuantLib::LevenbergMarquardt>(1e-10, 1e-10, 1e-10);
                QuantLib::SviInterpolation interp(
                    strikes.begin(), strikes.end(), vols.begin(), w.time, w.forward,
                    QuantLib::Null<Real>(), QuantLib::Null<Real>(), QuantLib::Null<Real>(),
                    QuantLib::Null<Real>(), QuantLib::Null<Real>(), false, false, false, false,
                    false, true, endCriteria, lm);
                interp.update();
                p.a = interp.a();
                p.b = interp.b();
                p.sigma = interp.sigma();
                p.rho = interp.rho();
                p.m = interp.m();
            } catch (const std::exception& ex) {
                QUANTRA_INVALID_ARGUMENT(
                    "SVI fit failed at expiry " + e.expiryIso + ": " + trimMessage(ex.what()));
            }
            // Butterfly check on the output strike range.
            const double kLo = std::log(out.gridStrikes.front() / w.forward);
            const double kHi = std::log(out.gridStrikes.back() / w.forward);
            const std::vector<double> ks = butterflyGrid(kLo, kHi);
            double minG = minDurrleman(p, ks);
            if (minG < 0.0) {
                e.butterflyViolation = true;
                if (in.enforceButterfly) {
                    std::vector<double> kq, sw;
                    double wsum = 0.0;
                    for (const auto& u : w.used) {
                        kq.push_back(std::log(u.strike / w.forward));
                        const double vega = QuantLib::blackFormulaStdDevDerivative(
                            u.strike, w.forward, u.vol * std::sqrt(w.time), 1.0);
                        sw.push_back(vega);
                        wsum += vega;
                    }
                    for (auto& x : sw)
                        x = std::sqrt(x / wsum);
                    SviPenalizedCost cost(kq, vols, sw, w.time, ks);
                    QuantLib::NoConstraint none;
                    QuantLib::Problem problem(cost, none, sviInverse(p));
                    QuantLib::LevenbergMarquardt lm(1e-10, 1e-10, 1e-10);
                    lm.minimize(problem, QuantLib::EndCriteria(2000, 200, 1e-12, 1e-10, 1e-10));
                    const SviParams refit = sviDirect(problem.currentValue());
                    const double minGAfter = minDurrleman(refit, ks);
                    e.butterflyAction = "refit with penalty: min g " + fmt(minG) + " -> " +
                                        fmt(minGAfter) +
                                        (minGAfter < 0.0 ? " (still negative)" : "");
                    p = refit;
                    minG = minGAfter;
                } else {
                    e.butterflyAction =
                        "reported only (enforce_butterfly false): min g " + fmt(minG);
                }
            } else {
                e.butterflyAction = "none";
            }
            e.butterflyMinG = minG;
            e.svi = p;
            w.svi = p;
            // Fit error against the market vols.
            double ss = 0.0, mx = 0.0;
            for (const auto& u : w.used) {
                const double v = std::sqrt(
                    std::max(0.0, sviTotalVariance(p, std::log(u.strike / w.forward))) / w.time);
                const double d = v - u.vol;
                ss += d * d;
                mx = std::max(mx, std::fabs(d));
            }
            e.rmseVol = std::sqrt(ss / w.used.size());
            e.maxAbsErrorVol = mx;
        }
        for (std::size_t i = 0; i < nExp; ++i) {
            const auto wIt = std::find_if(work.begin(), work.end(), [&](const ExpiryWork& w) {
                return w.expiry == gridDates[i];
            });
            for (std::size_t j = 0; j < nStr; ++j) {
                const double k = std::log(out.gridStrikes[j] / wIt->forward);
                const double tv = std::max(0.0, sviTotalVariance(*wIt->svi, k));
                out.gridVols[i * nStr + j] = std::sqrt(tv / wIt->time);
            }
        }
    }

    // ------------------------------------------------------------------
    // 4. Calendar check on the grid (clip for SVI when enforced).
    // ------------------------------------------------------------------
    {
        const bool clip =
            in.method == VolSurfaceFitMethod::SviPerExpiry ? in.enforceCalendar : false;
        std::vector<double> T(nExp);
        for (std::size_t i = 0; i < nExp; ++i)
            T[i] = dc.yearFraction(asOf, gridDates[i]);
        std::vector<unsigned> viol(nExp, 0);
        std::vector<double> maxClip(nExp, 0.0);
        for (std::size_t j = 0; j < nStr; ++j) {
            double prev = -1.0;
            for (std::size_t i = 0; i < nExp; ++i) {
                double& v = out.gridVols[i * nStr + j];
                double tv = v * v * T[i];
                if (i > 0 && tv < prev + kCalendarFloor) {
                    ++viol[i];
                    if (clip) {
                        const double nv = std::sqrt((prev + kCalendarFloor) / T[i]);
                        maxClip[i] = std::max(maxClip[i], std::fabs(nv - v));
                        v = nv;
                        tv = prev + kCalendarFloor;
                    }
                }
                prev = tv;
            }
        }
        for (std::size_t i = 0; i < nExp; ++i) {
            for (auto& e : out.expiries) {
                if (!e.usable || e.expiry != gridDates[i]) continue;
                e.calendarViolations = viol[i];
                if (viol[i] == 0) {
                    e.calendarAction = "none";
                } else if (clip) {
                    e.calendarMaxClipVol = maxClip[i];
                    e.calendarAction =
                        "clipped to the previous expiry's total variance + 1e-8 at " +
                        std::to_string(viol[i]) + " grid strike(s)";
                } else {
                    e.calendarAction =
                        in.method == VolSurfaceFitMethod::AndreasenHuge
                            ? "reported only (Andreasen-Huge: within the implied-vol inversion "
                              "tolerance)"
                            : "reported only (enforce_calendar false)";
                }
            }
        }
    }
    for (double v : out.gridVols) {
        if (!std::isfinite(v) || !(v > 0.0)) {
            QUANTRA_ERROR("vol-surface calibration produced a non-positive grid vol");
        }
    }

    // ------------------------------------------------------------------
    // 5. Vanilla repricing on the returned surface, read as the pricers
    //    read it (CubicStrikeLinearTime, the output spec's interpolator).
    // ------------------------------------------------------------------
    {
        if (nStr < 2) {
            QUANTRA_INVALID_ARGUMENT(
                "output_grid.strikes_as_moneyness needs at least 2 strikes (the returned surface "
                "is a cubic spline in strike)");
        }
        std::vector<std::vector<double>> rows(nExp, std::vector<double>(nStr));
        for (std::size_t i = 0; i < nExp; ++i)
            for (std::size_t j = 0; j < nStr; ++j)
                rows[i][j] = out.gridVols[i * nStr + j];
        auto surface = makeCubicStrikeLinearTimeSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, gridDates, out.gridStrikes,
            rows);
        surface->enableExtrapolation();
        auto& rp = out.repricing;
        for (const auto& w : work) {
            for (const auto& u : w.used) {
                VanillaRepricePointData r;
                r.expiryIso = DateToIso(w.expiry);
                r.strike = u.strike;
                r.isCall = u.isCall;
                r.bid = u.bid;
                r.ask = u.ask;
                r.mid = u.mid;
                r.marketVol = u.vol;
                r.modelVol = surface->blackVol(w.expiry, u.strike, true);
                r.modelPrice = QuantLib::blackFormula(
                    u.isCall ? QuantLib::Option::Call : QuantLib::Option::Put, u.strike, w.forward,
                    r.modelVol * std::sqrt(w.time), w.df);
                r.insideBidAsk = r.modelPrice >= u.bid && r.modelPrice <= u.ask;
                r.priceError = r.modelPrice - u.mid;
                r.volError = r.modelVol - u.vol;
                ++rp.count;
                if (r.insideBidAsk) ++rp.insideBidAsk;
                rp.maxAbsPriceError = std::max(rp.maxAbsPriceError, std::fabs(r.priceError));
                rp.maxAbsVolError = std::max(rp.maxAbsVolError, std::fabs(r.volError));
                rp.quotes.push_back(r);
            }
        }
    }
    return out;
}

} // namespace quantra
