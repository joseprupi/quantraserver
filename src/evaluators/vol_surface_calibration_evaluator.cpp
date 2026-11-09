#include "vol_surface_calibration_evaluator.h"

#include "basket_model.h"
#include "date_convert.h"
#include "error.h"
#include "smile_slice_vol_surface.h"
#include "ssvi_vol_surface.h"

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
#include <ql/methods/finitedifferences/meshers/concentrating1dmesher.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/andreasenhugevolatilityadapter.hpp>
#include <ql/termstructures/volatility/equityfx/andreasenhugevolatilityinterpl.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
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
// The eSSVI calibrator needs at least 3 used quotes per slice.
constexpr unsigned kMinQuotesSsvi = 3;
// Sequential SVI fit: calendar constraint points per slice, penalty weight
// (vol units, raised x10 per round), rounds, and the slack added to the
// target so the grid clip's floor (kCalendarFloor) is met as well.
constexpr unsigned kCalendarPoints = 401;
constexpr double kCalendarPenalty = 100.0;
constexpr unsigned kCalendarRounds = 3;
constexpr double kCalendarSlack = 2.0 * kCalendarFloor;
// Local vol check: strikes per scan time, offset of the scan times from the
// expiries, refit passes of the SVI payload, the margin multiplier per pass
// and the forward vol a raised margin starts from.
constexpr unsigned kLocalVolCheckRefits = 3;
constexpr double kMarginRaise = 4.0;
constexpr double kRaisedForwardVol = 0.01;
// Half-width of an expiry's region in at-the-money standard deviations.
constexpr double kReachStdDevs = 5.0;
constexpr double kNoBound = std::numeric_limits<double>::infinity();
// Andreasen-Huge grid inversion: the one-step finite-difference price carries
// noise of ~1e-7 of the spot; below this fraction of the spot that noise
// moves the implied vol by more than a tenth of a vol point.
constexpr double kGridInversionResolution = 1e-4;
constexpr unsigned kAndreasenHugeGridPoints = 500;

using UsedQuote = OutOfTheMoneyQuote;

struct ExpiryWork {
    Date expiry;
    double time = 0.0;
    double forward = 0.0;
    double df = 0.0;
    std::vector<UsedQuote> used; // sorted by strike
    std::optional<SviParams> svi;
    /// Strike region the surface is held to around this expiry (calendar
    /// constraint of the SVI fit, local vol check): the strikes quoted on
    /// both this expiry and the one before, extended to F e^(+/- 5 sigma
    /// sqrt(T)), sigma the larger at-the-money market vol of the two.
    double regionLo = 0.0, regionHi = 0.0;
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

// Roger Lee's moment bound on the wings of total variance: slope
// b (1 + |rho|) <= 2 (beyond it Durrleman's g is negative far out, a
// butterfly arbitrage). QuantLib's SviInterpolation admits up to 4; the
// refits below keep the slices within Lee's bound.
constexpr double kLeeSlope = 2.0;

SviParams sviDirect(const QuantLib::Array& x) {
    SviParams p;
    p.sigma = x[2] * x[2] + kEps1;
    p.rho = std::sin(x[3]) * kEps2;
    p.m = x[4];
    p.b = (std::atan(x[1]) + M_PI / 2.0) / M_PI * kEps2 * kLeeSlope / (1.0 + std::fabs(p.rho));
    p.a = kEps1 + x[0] * x[0] - p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
    return p;
}

QuantLib::Array sviInverse(const SviParams& p) {
    QuantLib::Array x(5);
    x[2] = std::sqrt(std::max(0.0, p.sigma - kEps1));
    x[3] = std::asin(std::max(-1.0, std::min(1.0, p.rho / kEps2)));
    x[4] = p.m;
    x[1] = std::tan(
        std::max(1e-12, std::min(1.0 - 1e-12, p.b / kLeeSlope * (1.0 + std::fabs(p.rho)) / kEps2)) *
            M_PI -
        M_PI / 2.0);
    x[0] = std::sqrt(std::max(0.0, p.a - kEps1 + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho)));
    return x;
}

/// A calendar constraint point of a slice: log-moneyness k (about the
/// slice's own forward) and the total variance the slice must reach there.
struct CalendarPoint {
    double k = 0.0;
    double lowerBound = 0.0;
    double upperBound = std::numeric_limits<double>::infinity();
    /// The bounded quantity is w(k) + slopeCoef dw/dk(k).
    double slopeCoef = 0.0;
};

/// dw/dk of a raw SVI slice.
double sviSlope(const SviParams& p, double k) {
    const double d = k - p.m;
    return p.b * (p.rho + d / std::sqrt(d * d + p.sigma * p.sigma));
}

double boundedValue(const SviParams& p, const CalendarPoint& c) {
    const double w = sviTotalVariance(p, c.k);
    return c.slopeCoef == 0.0 ? w : w + c.slopeCoef * sviSlope(p, c.k);
}

/// The calendar bounds of one slice against its neighbour.
struct CalendarBounds {
    std::vector<CalendarPoint> points; // bounds incl. margin + slack
    double margin = 0.0, requiredMargin = 0.0, dT = 0.0;
};

class SviPenalizedCost : public QuantLib::CostFunction {
public:
    SviPenalizedCost(
        std::vector<double> ks,
        std::vector<double> vols,
        std::vector<double> sqrtWeights,
        double T,
        std::vector<double> penaltyKs,
        double butterflyWeight = kButterflyPenalty,
        std::vector<CalendarPoint> calendar = {},
        double calendarWeight = 0.0)
        : ks_(std::move(ks)), vols_(std::move(vols)), sqrtWeights_(std::move(sqrtWeights)), T_(T),
          penaltyKs_(std::move(penaltyKs)), butterflyWeight_(butterflyWeight),
          calendar_(std::move(calendar)), calendarWeight_(calendarWeight) {}

    QuantLib::Array values(const QuantLib::Array& x) const override {
        const SviParams p = sviDirect(x);
        QuantLib::Array r(ks_.size() + penaltyKs_.size() + calendar_.size());
        for (std::size_t i = 0; i < ks_.size(); ++i) {
            const double w = std::max(0.0, sviTotalVariance(p, ks_[i]));
            r[i] = sqrtWeights_[i] * (std::sqrt(w / T_) - vols_[i]);
        }
        for (std::size_t j = 0; j < penaltyKs_.size(); ++j) {
            r[ks_.size() + j] = butterflyWeight_ * std::max(0.0, -sviDurrlemanG(p, penaltyKs_[j]));
        }
        // Calendar: the vol shortfall below the lower bound (vol units, so
        // the penalty weighs like the fit residuals whatever the expiry).
        const std::size_t base = ks_.size() + penaltyKs_.size();
        for (std::size_t j = 0; j < calendar_.size(); ++j) {
            const double w = boundedValue(p, calendar_[j]);
            const double lb = calendar_[j].lowerBound, ub = calendar_[j].upperBound;
            r[base + j] = w < lb   ? calendarWeight_ * (std::sqrt(std::max(0.0, lb) / T_) -
                                                        std::sqrt(std::max(0.0, w) / T_))
                          : w > ub ? calendarWeight_ * (std::sqrt(std::max(0.0, w) / T_) -
                                                        std::sqrt(std::max(0.0, ub) / T_))
                                   : 0.0;
        }
        return r;
    }

private:
    std::vector<double> ks_, vols_, sqrtWeights_;
    double T_;
    std::vector<double> penaltyKs_;
    double butterflyWeight_;
    std::vector<CalendarPoint> calendar_;
    double calendarWeight_;
};

std::vector<double> butterflyGrid(double kLo, double kHi) {
    std::vector<double> ks;
    ks.reserve(kButterflyPoints);
    for (unsigned j = 0; j < kButterflyPoints; ++j)
        ks.push_back(kLo + (kHi - kLo) * static_cast<double>(j) / (kButterflyPoints - 1));
    return ks;
}

double sviWingSlope(const SviParams& p) {
    return p.b * (1.0 + std::fabs(p.rho));
}

double minDurrleman(const SviParams& p, const std::vector<double>& ks) {
    double g = std::numeric_limits<double>::infinity();
    for (double k : ks)
        g = std::min(g, sviDurrlemanG(p, k));
    return g;
}

/// Per-slice settings the local vol check of the SVI payload raises when
/// the payload fails it: the calendar margin multiplier of the pair (this
/// expiry, the one before) and the slice's butterfly weight.
struct SviTuning {
    double marginScale = 1.0;
    double butterflyWeight = kButterflyPenalty;
};

/// The strikes of the expiry pair (j - 1, j): a log-spaced grid over the
/// region of expiry j, widened on both sides by the move of the forward
/// between the two expiries and QuantLib's strike stencil, so that the
/// calendar constraint covers every strike either slice is read at.
std::vector<double> pairStrikes(const std::vector<ExpiryWork>& work, std::size_t j) {
    const auto& w = work[j];
    const double widen = std::fabs(std::log(w.forward / work[j - 1].forward)) +
                         2.0 * mc::EdgeClampedLocalVol::kEdgeMargin;
    const double lo = std::log(w.regionLo) - widen, hi = std::log(w.regionHi) + widen;
    std::vector<double> strikes(kCalendarPoints);
    for (unsigned n = 0; n < kCalendarPoints; ++n)
        strikes[n] = std::exp(
            lo + (hi - lo) * static_cast<double>(n) / static_cast<double>(kCalendarPoints - 1));
    return strikes;
}

/**
 * The calendar constraint of the expiry pair (j - 1, j) as bounds on one of
 * its two slices (`fitted`, j - 1 or j), the other taken as final.
 *
 * Between the two expiries the surface interpolates total variance linearly
 * in time at fixed forward moneyness k = ln(K / F(t)) (SviTimeInterpolation
 * LinearTotalVarianceForwardMoneyness), and QuantLib's LocalVolSurface
 * differentiates it in time along the forward, where k is fixed: that
 * derivative times the interval length is w_j(k) - w_{j-1}(k) at every
 * time of the interval. It is required to be at least the margin
 * (sigma_min^2 (T_j - T_{j-1}) plus the slack) at every k of the pair's
 * grid (pairStrikes against the later forward; the grid is widened by the
 * forward's move, so it covers the moneyness of every strike of the region
 * at every time of the interval).
 */
std::vector<double> pairMoneyness(const std::vector<ExpiryWork>& work, std::size_t j) {
    std::vector<double> ks;
    for (double K : pairStrikes(work, j))
        ks.push_back(std::log(K / work[j].forward));
    return ks;
}

std::vector<CalendarPoint> pairBounds(
    const std::vector<ExpiryWork>& work, std::size_t j, std::size_t fitted, double margin) {
    const auto& later = work[j];
    const auto& earlier = work[j - 1];
    std::vector<CalendarPoint> pts;
    for (double k : pairMoneyness(work, j)) {
        if (fitted == j - 1) {
            // w_E(k) <= w_L(k) - margin.
            pts.push_back(
                CalendarPoint{k, -kNoBound, sviTotalVariance(*later.svi, k) - margin, 0.0});
        } else {
            // w_L(k) >= w_E(k) + margin.
            pts.push_back(
                CalendarPoint{k, sviTotalVariance(*earlier.svi, k) + margin, kNoBound, 0.0});
        }
    }
    return pts;
}

/// Smallest w_j(k) - w_{j-1}(k) over the pair's grid (total variance
/// units, before the margin), on the final slices.
double pairMinExcess(const std::vector<ExpiryWork>& work, std::size_t j) {
    double m = std::numeric_limits<double>::infinity();
    for (double k : pairMoneyness(work, j))
        m = std::min(m, sviTotalVariance(*work[j].svi, k) - sviTotalVariance(*work[j - 1].svi, k));
    return m;
}

bool calendarOnly(const std::vector<std::pair<double, bool>>& refused) {
    return std::all_of(refused.begin(), refused.end(), [](const auto& r) { return r.second; });
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
        base.filter.minQuotesPerExpiry, in.method == VolSurfaceFitMethod::SviPerExpiry
                                            ? kMinQuotesSvi
                                        : in.method == VolSurfaceFitMethod::Ssvi ? kMinQuotesSsvi
                                                                                 : 1u);

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
                 : in.method == VolSurfaceFitMethod::Ssvi
                     ? "; the eSSVI slice needs max(filter.min_quotes_per_expiry, 3)"
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
    const std::size_t nExp = gridDates.size();
    std::size_t nStr = out.gridStrikes.size();
    out.gridVols.assign(nExp * nStr, std::numeric_limits<double>::quiet_NaN());

    // The sheet's market: a risk-free curve on the implied discount factors
    // (log-linear) and a dividend curve with DF_q(T) = F DF / S, so the
    // forwards and the discounting are the sheet's (the Andreasen-Huge model
    // and the local vol check read them).
    Handle<QuantLib::YieldTermStructure> rTS, qTS;
    {
        std::vector<Date> dates;
        std::vector<double> fwds, dfs;
        for (const auto& w : work) {
            dates.push_back(w.expiry);
            fwds.push_back(w.forward);
            dfs.push_back(w.df);
        }
        const SheetForwardCurves sc = sheetForwardCurves(asOf, spot, dates, fwds, dfs);
        rTS = sc.riskFree;
        qTS = sc.dividend;
    }
    Handle<QuantLib::Quote> spotH(std::make_shared<QuantLib::SimpleQuote>(spot));
    // The forward of each grid expiry (the returned grid's expiry_forwards):
    // the fitted forward on a sheet expiry, spot D_q / D_r between them.
    for (const Date& d : gridDates) {
        const auto wIt = std::find_if(
            work.begin(), work.end(), [&](const ExpiryWork& w) { return w.expiry == d; });
        if (wIt != work.end()) {
            out.gridExpiryForwards.push_back(wIt->forward);
        } else {
            const double T = dc.yearFraction(asOf, d);
            out.gridExpiryForwards.push_back(spot * qTS->discount(T) / rTS->discount(T));
        }
    }
    std::vector<std::pair<double, double>> openRegions;
    // The region each expiry is held to: where a simulation goes around
    // that expiry. From the forward outwards on each side, the log-strike
    // distance covered by kReachStdDevs standard deviations when the vol
    // along the way is the market's implied vol (trapezoid of 1 / sigma
    // over the quoted strikes, the edge vol beyond them): the put wing's
    // higher vols carry a simulation further than the at-the-money vol
    // would, the call wing's lower vols less far. The strikes quoted on both
    // this expiry and the one before are always in.
    {
        double clampLo = out.gridStrikes.front(), clampHi = out.gridStrikes.back();
        for (const auto& w : work) {
            clampLo = std::min(clampLo, w.used.front().strike);
            clampHi = std::max(clampHi, w.used.back().strike);
        }
        std::vector<RegionQuotes> rq;
        for (const auto& w : work)
            rq.push_back({w.time, w.forward, &w.used});
        const auto regions = quoteRegions(rq, clampLo, clampHi);
        for (std::size_t i = 0; i < work.size(); ++i) {
            work[i].regionLo = regions[i].first;
            work[i].regionHi = regions[i].second;
        }
        // The unclamped eSSVI payload is scanned over the regions without
        // a strike limit, as /validate-vol-surface scans it.
        openRegions = quoteRegions(rq, 0.0, QL_MAX_REAL);
    }
    std::vector<double> workTimes;
    std::vector<std::pair<double, double>> workRegions;
    for (const auto& w : work) {
        workTimes.push_back(w.time);
        workRegions.emplace_back(w.regionLo, w.regionHi);
    }
    std::vector<std::optional<SviParams>> sviFits;

    std::vector<SviTuning> sviTuning(work.size());
    std::function<void(std::size_t)> fitSlicesDownFrom;

    // ------------------------------------------------------------------
    // 3a. Andreasen-Huge.
    // ------------------------------------------------------------------
    if (in.method == VolSurfaceFitMethod::AndreasenHuge) {
        QuantLib::AndreasenHugeVolatilityInterpl::CalibrationSet set;
        for (const auto& w : work) {
            auto exercise = std::make_shared<QuantLib::EuropeanExercise>(w.expiry);
            for (const auto& u : w.used) {
                auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(
                    u.isCall ? QuantLib::Option::Call : QuantLib::Option::Put, u.strike);
                set.emplace_back(
                    std::make_shared<QuantLib::VanillaOption>(payoff, exercise),
                    std::make_shared<QuantLib::SimpleQuote>(u.vol));
            }
        }
        std::shared_ptr<QuantLib::AndreasenHugeVolatilityInterpl> ah;
        std::shared_ptr<QuantLib::AndreasenHugeVolatilityAdapter> adapter;
        try {
            ah = std::make_shared<QuantLib::AndreasenHugeVolatilityInterpl>(
                set, spotH, rTS, qTS, QuantLib::AndreasenHugeVolatilityInterpl::CubicSpline,
                QuantLib::AndreasenHugeVolatilityInterpl::CallPut, kAndreasenHugeGridPoints);
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
        // The strike axis: the model's own finite-difference grid points
        // (the Concentrating1dMesher AndreasenHugeVolatilityInterpl builds:
        // 500 points in ln(K / S) between its min and max strike,
        // concentrated at the spot) inside the output grid's range and every
        // expiry's region, so that the cubic reading of the returned grid
        // follows the model between its own nodes instead of re-interpolating
        // a coarse sample.
        {
            double lo = out.gridStrikes.front(), hi = out.gridStrikes.back();
            for (const auto& w : work) {
                lo = std::min(lo, w.regionLo);
                hi = std::max(hi, w.regionHi);
            }
            const QuantLib::Concentrating1dMesher mesher(
                std::log(ah->minStrike() / spot), std::log(ah->maxStrike() / spot),
                kAndreasenHugeGridPoints, std::pair<Real, Real>(0.0, 0.025));
            std::vector<double> strikes;
            for (double x : mesher.locations()) {
                const double K = spot * std::exp(x);
                if (K >= lo * (1.0 - 1e-12) && K <= hi * (1.0 + 1e-12)) strikes.push_back(K);
            }
            if (strikes.size() >= 2) {
                out.gridStrikes = strikes;
                out.moneyness.clear();
                for (double K : strikes)
                    out.moneyness.push_back(K / spot);
                nStr = strikes.size();
                out.gridVols.assign(nExp * nStr, std::numeric_limits<double>::quiet_NaN());
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
    // 3b. SVI per expiry, fitted from the last expiry backwards, each slice
    //     held under the slice after it (calendar constraint).
    // ------------------------------------------------------------------
    const auto endCriteria = std::make_shared<QuantLib::EndCriteria>(500, 100, 1e-12, 1e-10, 1e-10);
    // (The helpers below live at function scope: step 3c refits with them.)
    sviFits.assign(work.size(), std::nullopt);

    // QuantLib's SviInterpolation on the expiry's quotes (cached: a refit
    // starts from it again).
    auto independentFit = [&](std::size_t i) {
        if (sviFits[i]) return *sviFits[i];
        const auto& w = work[i];
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
                QuantLib::Null<Real>(), QuantLib::Null<Real>(), false, false, false, false, false,
                true, endCriteria, lm);
            interp.update();
            p.a = interp.a();
            p.b = interp.b();
            p.sigma = interp.sigma();
            p.rho = interp.rho();
            p.m = interp.m();
        } catch (const std::exception& ex) {
            QUANTRA_INVALID_ARGUMENT(
                "SVI fit failed at expiry " + DateToIso(w.expiry) + ": " + trimMessage(ex.what()));
        }
        sviFits[i] = p;
        return p;
    };
    // Butterfly grid: the output strike range and the regions of the two
    // expiry pairs the slice belongs to (where the note pricer reads it).
    auto butterflyKs = [&](std::size_t i) {
        const auto& w = work[i];
        double lo = std::min(w.regionLo, out.gridStrikes.front());
        double hi = std::max(w.regionHi, out.gridStrikes.back());
        if (i + 1 < work.size()) {
            lo = std::min(lo, work[i + 1].regionLo);
            hi = std::max(hi, work[i + 1].regionHi);
        }
        return butterflyGrid(std::log(lo / w.forward), std::log(hi / w.forward));
    };
    // Upper bounds of slice i under the final slice i + 1: at every
    // forward moneyness of a grid over the pair's region (the later
    // expiry's), total variance at most the next slice's at the same
    // moneyness (the surface interpolates there and QuantLib's local vol
    // differentiates in time along the forward), minus the margin
    // sigma_min^2 (T_{i+1} - T_i).
    auto calendarBounds = [&](std::size_t i) {
        CalendarBounds cb;
        if (i + 1 >= work.size()) return cb;
        const auto& w = work[i];
        const ExpiryWork& next = work[i + 1];
        cb.dT = next.time - w.time;
        cb.requiredMargin = in.minForwardVol * in.minForwardVol * cb.dT;
        const double scale = sviTuning[i + 1].marginScale;
        const double sigmaMin =
            scale > 1.0 ? std::max(in.minForwardVol, kRaisedForwardVol) : in.minForwardVol;
        cb.margin = sigmaMin * sigmaMin * cb.dT * scale;
        cb.points = pairBounds(work, i + 1, i, cb.margin + kCalendarSlack);
        return cb;
    };
    auto excess = [](const SviParams& q, const std::vector<CalendarPoint>& pts) {
        double s = -std::numeric_limits<double>::infinity();
        for (const auto& c : pts)
            s = std::max(s, boundedValue(q, c) - c.upperBound);
        return s;
    };
    auto residualWeights = [&](std::size_t i, std::vector<double>& kq, std::vector<double>& vols,
                               std::vector<double>& sw) {
        const auto& w = work[i];
        double wsum = 0.0;
        for (const auto& u : w.used) {
            kq.push_back(std::log(u.strike / w.forward));
            vols.push_back(u.vol);
            const double vega = QuantLib::blackFormulaStdDevDerivative(
                u.strike, w.forward, u.vol * std::sqrt(w.time), 1.0);
            sw.push_back(vega);
            wsum += vega;
        }
        for (auto& x : sw)
            x = std::sqrt(x / wsum);
    };
    auto fitRmse = [&](std::size_t i, const SviParams& q, double* maxAbs = nullptr) {
        const auto& w = work[i];
        double ss = 0.0, mx = 0.0;
        for (const auto& u : w.used) {
            const double v = std::sqrt(
                std::max(0.0, sviTotalVariance(q, std::log(u.strike / w.forward))) / w.time);
            ss += (v - u.vol) * (v - u.vol);
            mx = std::max(mx, std::fabs(v - u.vol));
        }
        if (maxAbs) *maxAbs = mx;
        return std::sqrt(ss / w.used.size());
    };
    auto maxVolChange = [&](std::size_t i, const SviParams& a, const SviParams& b) {
        const auto& w = work[i];
        double mx = 0.0;
        for (const auto& u : w.used) {
            const double k = std::log(u.strike / w.forward);
            mx = std::max(
                mx, std::fabs(
                        std::sqrt(std::max(0.0, sviTotalVariance(a, k)) / w.time) -
                        std::sqrt(std::max(0.0, sviTotalVariance(b, k)) / w.time)));
        }
        return mx;
    };
    // Levenberg-Marquardt on the vega-weighted vol residuals, plus the
    // butterfly penalty on `bfKs` and the calendar penalty on `bounds`,
    // from each start, the best objective kept; the calendar weight rises
    // (x10 per round) until the remaining excess is a fraction of the
    // margin.
    auto penalizedRefit = [&](std::size_t i, const std::vector<SviParams>& starts,
                              const std::vector<double>& bfKs,
                              const std::vector<CalendarPoint>& bounds, double margin) {
        std::vector<double> kq, vols, sw;
        residualWeights(i, kq, vols, sw);
        double calW = kCalendarPenalty;
        SviParams best = starts.front();
        for (unsigned round = 0; round < kCalendarRounds; ++round) {
            double bestObj = std::numeric_limits<double>::infinity();
            for (const SviParams& start : starts) {
                SviPenalizedCost cost(
                    kq, vols, sw, work[i].time, bfKs, sviTuning[i].butterflyWeight, bounds, calW);
                QuantLib::NoConstraint none;
                QuantLib::Problem problem(cost, none, sviInverse(start));
                QuantLib::LevenbergMarquardt lm(1e-10, 1e-10, 1e-10);
                lm.minimize(problem, QuantLib::EndCriteria(2000, 200, 1e-12, 1e-10, 1e-10));
                const QuantLib::Array r = cost.values(problem.currentValue());
                const double obj = QuantLib::DotProduct(r, r);
                if (obj < bestObj) {
                    bestObj = obj;
                    best = sviDirect(problem.currentValue());
                }
            }
            if (bounds.empty() || excess(best, bounds) <= 0.1 * margin + kCalendarSlack) break;
            calW *= 10.0;
        }
        return best;
    };

    // Fit (or refit) slice i under the final slice i + 1.
    auto fitSlice = [&](std::size_t i) {
        auto& w = work[i];
        auto& e = reportOf(w.expiry);
        const SviParams p0 = independentFit(i);
        e.butterflyViolation = false;
        e.calendarRefit = false;
        e.calendarRefitMaxVolShift.reset();
        e.calendarFitAction.clear();
        const std::vector<double> ks = butterflyKs(i);
        const double minG0 = minDurrleman(p0, ks);
        const bool steepWing = sviWingSlope(p0) > kLeeSlope;
        e.butterflyViolation = minG0 < 0.0 || steepWing;
        const bool needBf = e.butterflyViolation && in.enforceButterfly;
        const std::string slopeNote =
            steepWing
                ? " (wing slope b (1 + |rho|) " + fmt(sviWingSlope(p0)) + " above Lee's bound 2)"
                : "";
        const CalendarBounds cb = calendarBounds(i);
        const double excess0 = cb.points.empty() ? -1.0 : excess(p0, cb.points);
        bool needCal = in.enforceCalendar && excess0 > 0.0;
        SviParams warm;
        if (i + 1 < work.size()) {
            // The next slice at the same moneyness lowered by the margin:
            // a start that meets the bounds.
            warm = *work[i + 1].svi;
            warm.a -= cb.margin + kCalendarSlack;
        }
        SviParams p = p0;
        std::optional<SviParams> butterflyRefit;
        if (needBf) {
            butterflyRefit = penalizedRefit(i, {p0}, ks, {}, 0.0);
            p = *butterflyRefit;
            // The butterfly refit alone moved the slice above the next
            // one: refit with both penalties instead.
            if (in.enforceCalendar && !cb.points.empty() &&
                excess(*butterflyRefit, cb.points) > 0.0)
                needCal = true;
        }
        double drop = 0.0;
        if (needCal) {
            // One refit with the butterfly and the calendar penalties
            // together, from the independent fit, the next slice moved
            // here and the butterfly refit; then the slice is lowered
            // (a - d) by whatever excess remains, so the constraint holds
            // on every point.
            const std::vector<SviParams> starts{butterflyRefit ? *butterflyRefit : p0, warm};
            p = penalizedRefit(
                i, starts, in.enforceButterfly ? ks : std::vector<double>{}, cb.points, cb.margin);
            drop = std::max(0.0, excess(p, cb.points));
            p.a -= drop;
        }
        const double minG = minDurrleman(p, ks);
        if (needCal) {
            const double shiftMax = maxVolChange(i, p, p0);
            e.calendarRefit = true;
            e.calendarRefitMaxVolShift = shiftMax;
            std::ostringstream os;
            os << "refit with calendar penalty under " << DateToIso(work[i + 1].expiry)
               << ": total variance above the next slice's minus margin by up to "
               << fmt(std::max(excess0, butterflyRefit ? excess(*butterflyRefit, cb.points) : 0.0) -
                      kCalendarSlack)
               << (excess0 > 0.0 ? "" : " (after the butterfly refit)") << ", max vol shift "
               << fmt(shiftMax);
            if (drop > 0.0) os << ", then lowered by " << fmt(drop) << " in total variance";
            if (sviTuning[i + 1].marginScale > 1.0) {
                os << "; margin raised to " << fmt(cb.margin)
                   << " by the local vol check of the SVI payload";
            }
            e.calendarFitAction = os.str();
        } else if (i + 1 == work.size()) {
            e.calendarFitAction = "last fitted expiry: fitted first, no calendar constraint";
        } else {
            e.calendarFitAction =
                excess0 > 0.0 ? "reported only (enforce_calendar false): total variance above the "
                                "next slice's minus margin by up to " +
                                    fmt(excess0 - kCalendarSlack)
                              : "none (under the next slice minus margin on the calendar grid)";
        }
        if (needCal) {
            if (e.butterflyViolation) {
                e.butterflyAction =
                    in.enforceButterfly
                        ? "refit with penalty (together with the calendar constraint): min "
                          "g " +
                              fmt(minG0) + slopeNote + " -> " + fmt(minG) +
                              (minG < 0.0 ? " (still negative)" : "")
                        : "reported only (enforce_butterfly false): min g " + fmt(minG0) +
                              slopeNote + " -> " + fmt(minG) + " after the calendar refit";
            } else {
                e.butterflyAction =
                    minG < 0.0 ? "none before the calendar refit; min g after it " + fmt(minG)
                               : "none";
            }
        } else if (butterflyRefit) {
            e.butterflyAction = "refit with penalty: min g " + fmt(minG0) + slopeNote + " -> " +
                                fmt(minG) + (minG < 0.0 ? " (still negative)" : "");
        } else if (e.butterflyViolation) {
            e.butterflyAction =
                "reported only (enforce_butterfly false): min g " + fmt(minG0) + slopeNote;
        } else {
            e.butterflyAction = "none";
        }
        // Report only: the raw SVI wing beyond the butterfly grid, out to
        // k = +/- 5 sigma_ATM sqrt(T). The note pricer holds the local
        // vol at the quoted strike edge, so a violation there does not
        // reach a price; it is reported.
        {
            const double kLo = ks.front(), kHi = ks.back();
            const double atmVol = std::sqrt(std::max(0.0, sviTotalVariance(p, 0.0)) / w.time);
            const double reach = 5.0 * atmVol * std::sqrt(w.time);
            double outMinG = std::numeric_limits<double>::infinity(), outK = 0.0;
            auto scanOutside = [&](double a, double b) {
                if (!(a < b)) return;
                for (double k : butterflyGrid(a, b)) {
                    const double g = sviDurrlemanG(p, k);
                    if (g < outMinG) {
                        outMinG = g;
                        outK = k;
                    }
                }
            };
            scanOutside(-reach, kLo);
            scanOutside(kHi, reach);
            if (outMinG < 0.0) {
                e.butterflyAction +=
                    "; beyond the checked strike range (reported only, up to k = +/-" + fmt(reach) +
                    "): min g " + fmt(outMinG) + " at k " + fmt(outK);
            }
        }
        w.svi = p;
        e.svi = p;
        e.strikeMin = w.regionLo;
        e.strikeMax = w.regionHi;
        e.butterflyMinG = minG;
        double mx = 0.0;
        e.rmseVol = fitRmse(i, p, &mx);
        e.maxAbsErrorVol = mx;
    };
    // Refit the slices from `last` down to the first.
    fitSlicesDownFrom = [&](std::size_t last) {
        for (std::size_t n = last + 1; n-- > 0;)
            fitSlice(n);
    };
    if (in.method == VolSurfaceFitMethod::SviPerExpiry) fitSlicesDownFrom(work.size() - 1);
    // The calendar margins of the final slices, every expiry after the first
    // against the one before (report).
    auto reportCalendarMargins = [&]() {
        for (std::size_t i = 1; i < work.size(); ++i) {
            const auto& w = work[i];
            const auto& prev = work[i - 1];
            auto& e = reportOf(w.expiry);
            const double dT = w.time - prev.time;
            const double minExcess = pairMinExcess(work, i);
            e.calendarMinMargin = minExcess - in.minForwardVol * in.minForwardVol * dT;
            e.calendarMinForwardVol =
                minExcess >= 0.0 ? std::sqrt(minExcess / dT) : -std::sqrt(-minExcess / dT);
        }
    };
    if (in.method == VolSurfaceFitMethod::SviPerExpiry) reportCalendarMargins();

    // ------------------------------------------------------------------
    // 3c. SviPerExpiry: the SVI payload's local vol, read as the note
    //     pricer reads it; a failing slice gets a larger calendar margin and
    //     butterfly weight and the slices from it on are refitted (bounded).
    // ------------------------------------------------------------------
    auto sviSurface = [&]() {
        std::vector<Date> dates;
        std::vector<SviSliceParams> params;
        for (const auto& w : work) {
            dates.push_back(w.expiry);
            SviSliceParams sp;
            sp.forward = w.forward;
            sp.a = w.svi->a;
            sp.b = w.svi->b;
            sp.sigma = w.svi->sigma;
            sp.rho = w.svi->rho;
            sp.m = w.svi->m;
            params.push_back(sp);
        }
        // As the returned payload reads it: fixed forward moneyness
        // against the slices' forwards and the spot.
        auto s = makeSviSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, dates, params,
            SviTimeReading::FixedForwardMoneyness, spot);
        s->enableExtrapolation();
        return s;
    };
    if (in.method == VolSurfaceFitMethod::SviPerExpiry) {
        // The payload's per-expiry strike ranges: each expiry's region.
        std::vector<mc::LocalVolStrikeBand> sviBands;
        for (const auto& w : work)
            sviBands.push_back({w.time, w.regionLo, w.regionHi});
        std::string action;
        LocalVolCheckData check;
        for (unsigned iter = 0;; ++iter) {
            std::vector<std::pair<double, bool>> refusedAt;
            check = checkLocalVol(
                Handle<QuantLib::BlackVolTermStructure>(sviSurface()), sviBands, std::nullopt,
                mc::LocalVolTimeExtrapolation::FlatLocalVol, spotH, rTS, qTS, workTimes, workTimes,
                workRegions, &refusedAt);
            if (check.arbitrageFree || iter == kLocalVolCheckRefits) break;
            // The expiry pair whose interval holds each refused time (a
            // time on a node's far side belongs to the next interval): a
            // calendar refusal raises the pair's margin (the earlier slice
            // is refitted under the later one); a negative local variance
            // raises the butterfly weight of both slices.
            std::set<std::size_t> pairs, slices;
            for (const auto& [t, calendarKind] : refusedAt) {
                std::size_t j = static_cast<std::size_t>(
                    std::lower_bound(workTimes.begin(), workTimes.end(), t) - workTimes.begin());
                if (j >= work.size()) j = work.size() - 1;
                if (calendarKind && j > 0) {
                    pairs.insert(j);
                    slices.insert(j - 1);
                } else {
                    slices.insert(j);
                    if (j > 0) slices.insert(j - 1);
                }
            }
            for (std::size_t j : pairs)
                sviTuning[j].marginScale *= kMarginRaise;
            if (!calendarOnly(refusedAt)) {
                for (std::size_t i : slices)
                    sviTuning[i].butterflyWeight *= 10.0;
            }
            const std::size_t from = *slices.rbegin();
            std::ostringstream os;
            os << (action.empty() ? "" : "; ") << "pass " << (iter + 1) << ": " << check.refused
               << " of " << check.points << " points refused (" << check.firstRefusal
               << "), refitted from " << DateToIso(work[from].expiry) << " down with a larger "
               << (pairs.empty()
                       ? ""
                       : "calendar margin on " + std::to_string(pairs.size()) + " expiry pair(s)")
               << (pairs.empty() || calendarOnly(refusedAt) ? "" : " and a larger ")
               << (calendarOnly(refusedAt)
                       ? ""
                       : "butterfly weight on " + std::to_string(slices.size()) + " slice(s)");
            action += os.str();
            fitSlicesDownFrom(from);
            reportCalendarMargins();
        }
        out.sviLocalVolRefitAction = action.empty() ? "none" : action;
        out.localVolCheckSvi = check;

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
    // 3d. Ssvi: eSSVI slices on the used quotes with the sheet's forwards,
    //     the grid sampled from the surface, the closed-form local vol
    //     checked as the note pricer reads it.
    // ------------------------------------------------------------------
    if (in.method == VolSurfaceFitMethod::Ssvi) {
        ssvi::CalibrationInput ci;
        ci.referenceDate = asOf;
        ci.dayCounter = dc;
        ci.spot = spot;
        ci.weighting = in.ssviWeighting;
        ci.maxAbsLogMoneyness = in.ssviMaxAbsLogMoneyness;
        // Quote index -> (expiry slot, used-quote slot).
        std::vector<std::pair<std::size_t, std::size_t>> origin;
        for (std::size_t i = 0; i < work.size(); ++i) {
            const auto& w = work[i];
            ci.expiryMarkets[w.expiry] = ssvi::ExpiryMarket{w.forward, w.df};
            const double sqrtT = std::sqrt(w.time);
            for (std::size_t j = 0; j < w.used.size(); ++j) {
                const auto& u = w.used[j];
                ssvi::OptionQuote q;
                q.expiry = w.expiry;
                q.strike = u.strike;
                q.type = u.isCall ? QuantLib::Option::Call : QuantLib::Option::Put;
                q.impliedVol = u.vol;
                // Bid / ask in vol units (BidAsk weighting): the price
                // half-spreads over the Black vega at the market vol.
                const double vega = QuantLib::blackFormulaStdDevDerivative(
                                        u.strike, w.forward, u.vol * sqrtT, w.df) *
                                    sqrtT;
                if (vega > 0.0 && std::isfinite(vega)) {
                    q.bid = u.vol - (u.mid - u.bid) / vega;
                    q.ask = u.vol + (u.ask - u.mid) / vega;
                }
                ci.quotes.push_back(q);
                origin.emplace_back(i, j);
            }
        }
        const ssvi::CalibrationResult fit = ssvi::calibrate(ci);
        // Quotes the fit left out: dropped with the calibrator's reason.
        std::vector<std::vector<bool>> keep(work.size());
        for (std::size_t i = 0; i < work.size(); ++i)
            keep[i].assign(work[i].used.size(), true);
        for (const auto& row : fit.quotes) {
            if (row.used) continue;
            const auto [i, j] = origin[row.quoteIndex];
            keep[i][j] = false;
            auto& e = reportOf(work[i].expiry);
            e.dropped.push_back(
                DroppedQuoteData{e.expiryIso, work[i].used[j].strike, row.exclusionReason});
        }
        std::vector<Date> dates;
        std::vector<SsviSliceParams> params;
        std::vector<double> forwards;
        for (std::size_t i = 0; i < work.size(); ++i) {
            auto& w = work[i];
            std::vector<UsedQuote> kept;
            for (std::size_t j = 0; j < w.used.size(); ++j)
                if (keep[i][j]) kept.push_back(w.used[j]);
            w.used = std::move(kept);
            auto& e = reportOf(w.expiry);
            e.quotesUsed = static_cast<unsigned>(w.used.size());
            // The calibrator returns one slice per quoted expiry, in date order.
            const auto& d = fit.slices[i];
            if (fit.expiries[i] != w.expiry) QUANTRA_ERROR("internal: SSVI slice order");
            e.ssvi = fit.params[i];
            e.ssviAnchorK = d.anchorK;
            e.ssviAnchorTotalVariance = d.anchorTheta;
            e.ssviAnchorClamped = d.anchorClamped;
            e.ssviMarginButterflyPsi = d.marginButterflyPsi;
            e.ssviMarginButterflyPsi2 = d.marginButterflyPsi2;
            if (i > 0) {
                e.ssviMarginCalendarTheta = d.marginCalendarTheta;
                e.ssviMarginCalendarPsi = d.marginCalendarPsi;
                e.ssviMarginCalendarA = d.marginCalendarA;
            }
            e.rmseVol = d.rmseVolPts / 100.0;
            e.maxAbsErrorVol = d.maxAbsVolPts / 100.0;
            // No strike range on an eSSVI slice: the formula is valid at
            // every strike and its conditions are checked when read, so the
            // payload's local vol is read unclamped.
            dates.push_back(w.expiry);
            params.push_back(fit.params[i]);
            forwards.push_back(w.forward);
        }
        SsviArbitrageCheckData arb;
        for (const auto& v : fit.violations) {
            std::ostringstream os;
            os << v.condition << " at slice " << v.slice;
            if (v.prevSlice != v.slice) os << " vs slice " << v.prevSlice;
            os << " (by " << fmt(v.margin) << ")";
            arb.violations.push_back(os.str());
        }
        arb.scanMinG = fit.scan.minG;
        arb.scanMinGTime = fit.scan.minGt;
        arb.scanMinGK = fit.scan.minGk;
        arb.scanMinDwDt = fit.scan.minWt;
        arb.scanMinDwDtTime = fit.scan.minWtT;
        arb.scanMinDwDtK = fit.scan.minWtK;
        out.ssviArbitrage = arb;

        // The payload's surface, exactly as the parser builds it.
        const auto ssviSurface = makeSsviSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, dates, params, forwards,
            spot);
        ssviSurface->enableExtrapolation();
        // Read as the payload is read: no strike range (unclamped).
        out.localVolCheckSsvi = checkLocalVol(
            Handle<QuantLib::BlackVolTermStructure>(ssviSurface), {}, std::nullopt,
            mc::LocalVolTimeExtrapolation::FlatImpliedVol, spotH, rTS, qTS, workTimes, workTimes,
            openRegions);

        for (std::size_t i = 0; i < nExp; ++i)
            for (std::size_t j = 0; j < nStr; ++j)
                out.gridVols[i * nStr + j] =
                    ssviSurface->blackVol(gridDates[i], out.gridStrikes[j], true);
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
        // At each grid node, total variance against the previous grid row's
        // at the same forward moneyness (the strike K F_{i-1} / F_i, read
        // on that row as the surface reads it: natural cubic spline of total
        // variance in ln K, constant beyond the edge strikes), which is what
        // Dupire's time derivative follows.
        std::vector<unsigned> viol(nExp, 0), beyond(nExp, 0);
        std::vector<double> maxClip(nExp, 0.0);
        const std::vector<double>& fwd = out.gridExpiryForwards;
        for (std::size_t i = 1; i < nExp; ++i) {
            std::vector<double> prevRow(nStr);
            for (std::size_t j = 0; j < nStr; ++j) {
                const double v = out.gridVols[(i - 1) * nStr + j];
                prevRow[j] = v * v * T[i - 1];
            }
            const CubicVarianceSlice prevSlice(out.gridStrikes, prevRow);
            // The region of the fitted expiry on this grid date (beyond it a
            // node is an extrapolation no simulation reaches at this date:
            // reported, not clipped, so the row keeps its smooth shape).
            double lo = 0.0, hi = QL_MAX_REAL;
            for (const auto& w : work) {
                if (w.expiry != gridDates[i]) continue;
                lo = w.regionLo;
                hi = w.regionHi;
            }
            for (std::size_t j = 0; j < nStr; ++j) {
                double& v = out.gridVols[i * nStr + j];
                const double tv = v * v * T[i];
                const double prev =
                    prevSlice.totalVariance(out.gridStrikes[j] * fwd[i - 1] / fwd[i]);
                const double K = out.gridStrikes[j];
                if (tv < prev + kCalendarFloor) {
                    if (K < lo || K > hi) {
                        ++beyond[i];
                        continue;
                    }
                    ++viol[i];
                    if (clip) {
                        const double nv = std::sqrt((prev + kCalendarFloor) / T[i]);
                        maxClip[i] = std::max(maxClip[i], std::fabs(nv - v));
                        v = nv;
                    }
                }
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
                    // Last resort: the sequential fit orders the slices on
                    // a grid finer than this one, so the clip fires only on
                    // a grid expiry list that skips fitted expiries less
                    // than the floor apart or after a fit that could not
                    // meet its constraint (calendar_fit_action says which).
                    e.calendarAction =
                        "clipped to the previous expiry's total variance + 1e-8 at " +
                        std::to_string(viol[i]) +
                        " grid strike(s), after the calendar-consistent fit (see "
                        "calendar_fit_action)";
                } else {
                    e.calendarAction =
                        in.method == VolSurfaceFitMethod::AndreasenHuge
                            ? "reported only (Andreasen-Huge: within the implied-vol inversion "
                              "tolerance)"
                        : in.method == VolSurfaceFitMethod::Ssvi
                            ? "reported only (Ssvi: the slices are calendar-free by "
                              "construction; this is the grid sampling)"
                            : "reported only (enforce_calendar false)";
                }
                if (beyond[i] > 0) {
                    e.calendarAction +=
                        "; below the previous row at " + std::to_string(beyond[i]) +
                        " grid strike(s) beyond the expiry's region (the quoted strikes and "
                        "+/- 5 at-the-money standard deviations): reported only, not clipped";
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
    // 5. The fit of the used quotes on the returned surface, read as the pricers
    //    read it (CubicStrikeLinearTimeForwardMoneyness, the output spec's
    //    interpolator, on its expiry_forwards and spot).
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
        std::vector<double> gridT;
        for (const Date& d : gridDates)
            gridT.push_back(dc.yearFraction(asOf, d));
        auto surface = makeCubicStrikeLinearTimeSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, gridDates, out.gridStrikes,
            rows, std::make_shared<NodeSurfaceForward>(gridT, out.gridExpiryForwards, spot));
        surface->enableExtrapolation();
        {
            std::vector<ValidationExpiry> ve;
            for (const auto& w : work) {
                ValidationExpiry v;
                v.expiryIso = DateToIso(w.expiry);
                v.expiry = w.expiry;
                v.time = w.time;
                v.usable = true;
                v.forward = w.forward;
                v.discountFactor = w.df;
                v.used = w.used;
                ve.push_back(std::move(v));
            }
            out.fit = fitQuotes(*surface, ve, defaultBandEdges());
        }
        // ------------------------------------------------------------------
        // 6. The returned grid's local vol, read as the note pricer reads it.
        // ------------------------------------------------------------------
        std::vector<double> gridTimes;
        for (const Date& d : gridDates)
            gridTimes.push_back(dc.yearFraction(asOf, d));
        // The grid's per-expiry strike ranges: the region of the fitted
        // expiry at or after the grid date, inside the second and the
        // second-to-last grid strike (the natural spline's end intervals,
        // where its curvature is forced to zero, are left out) pulled in by
        // the forward's move over the interval the expiry closes.
        const double inLo = out.gridStrikes[std::min<std::size_t>(1, nStr - 1)];
        const double inHi = out.gridStrikes[nStr >= 2 ? nStr - 2 : 0];
        std::vector<mc::LocalVolStrikeBand> gridBands;
        for (std::size_t i = 0; i < nExp; ++i) {
            const std::size_t r = std::min<std::size_t>(
                work.size() - 1,
                static_cast<std::size_t>(
                    std::lower_bound(workTimes.begin(), workTimes.end(), gridTimes[i] - 1e-12) -
                    workTimes.begin()));
            // Read at fixed forward moneyness, the two rows of the interval
            // this expiry closes are evaluated at K F_j / F(t), up to the
            // forward's move over the interval away from K: the range is
            // pulled in by that move so that both rows are read inside the
            // second and second-to-last grid strike at every time of it.
            const double fPrev = i == 0 ? spot : out.gridExpiryForwards[i - 1];
            const double move = std::exp(std::fabs(std::log(out.gridExpiryForwards[i] / fPrev)));
            double lo = std::max(work[r].regionLo, inLo * move);
            double hi = std::min(work[r].regionHi, inHi / move);
            if (!(lo < hi)) {
                lo = out.gridStrikes.front();
                hi = out.gridStrikes.back();
            }
            gridBands.push_back({gridTimes[i], lo, hi});
            out.gridLocalVolStrikeMin.push_back(lo);
            out.gridLocalVolStrikeMax.push_back(hi);
        }
        out.localVolCheck = checkLocalVol(
            Handle<QuantLib::BlackVolTermStructure>(surface), gridBands, std::nullopt,
            mc::LocalVolTimeExtrapolation::FlatLocalVol, spotH, rTS, qTS, gridTimes, workTimes,
            workRegions);
    }
    return out;
}

} // namespace quantra
