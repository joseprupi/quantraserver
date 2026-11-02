#include "ssvi_calibrator.h"

#include "date_convert.h"
#include "error.h"

#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/pricingengines/blackformula.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace quantra { namespace ssvi {

namespace {

constexpr double kMargin = 1e-10;
constexpr double kRhoMax = 0.999;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string describeQuote(std::size_t i, const OptionQuote& q) {
    std::ostringstream os;
    os << "quote " << i << " (expiry " << DateToIso(q.expiry) << ", strike " << q.strike << ")";
    return os.str();
}

// Brent's minimisation (golden section + parabolic interpolation) of f on [a, b].
template <class F> double brentMinimise(const F& f, double a, double b, double relTol) {
    const double cgold = 0.3819660112501051;
    double x = a + cgold * (b - a);
    double w = x, v = x;
    double fx = f(x), fw = fx, fv = fx;
    double d = 0.0, e = 0.0;
    for (int iter = 0; iter < 300; ++iter) {
        const double xm = 0.5 * (a + b);
        const double tol1 = relTol * std::fabs(x) + 1e-300;
        const double tol2 = 2.0 * tol1;
        if (std::fabs(x - xm) <= tol2 - 0.5 * (b - a)) break;
        bool golden = true;
        if (std::fabs(e) > tol1) {
            double r = (x - w) * (fx - fv);
            double q = (x - v) * (fx - fw);
            double p = (x - v) * q - (x - w) * r;
            q = 2.0 * (q - r);
            if (q > 0.0) p = -p;
            q = std::fabs(q);
            const double etemp = e;
            e = d;
            if (!(std::fabs(p) >= std::fabs(0.5 * q * etemp) || p <= q * (a - x) ||
                  p >= q * (b - x))) {
                d = p / q;
                const double u = x + d;
                if (u - a < tol2 || b - u < tol2) d = (xm >= x) ? tol1 : -tol1;
                golden = false;
            }
        }
        if (golden) {
            e = (x >= xm) ? a - x : b - x;
            d = cgold * e;
        }
        const double u = (std::fabs(d) >= tol1) ? x + d : x + ((d >= 0.0) ? tol1 : -tol1);
        const double fu = f(u);
        if (fu <= fx) {
            if (u >= x)
                a = x;
            else
                b = x;
            v = w;
            w = x;
            x = u;
            fv = fw;
            fw = fx;
            fx = fu;
        } else {
            if (u < x)
                a = u;
            else
                b = u;
            if (fu <= fw || w == x) {
                v = w;
                w = u;
                fv = fw;
                fw = fu;
            } else if (fu <= fv || v == x || v == w) {
                v = u;
                fv = fu;
            }
        }
    }
    return x;
}

struct Point {
    double k;
    double sigma;
    double weight; // user weight x omega
};

struct PrevSlice {
    bool exists = false;
    double theta = 0.0, psi = 0.0, rho = 0.0;
};

struct Fit {
    double theta = 0.0, rho = 0.0, psi = 0.0;
    double objective = kInf;
};

// Slice variance at the anchor: the exact theta with w(k*; theta, rho, psi) = W (squaring
// 2W - theta - rho psi k = sqrt(psi^2 k^2 + 2 rho psi theta k + theta^2) gives a closed form with
// no extraneous root: 2W - theta - rho psi k = W + c psi^2 > 0).
double thetaAtAnchor(double W, double rho, double kStar, double psi) {
    const double c = (1.0 - rho * rho) * kStar * kStar / (4.0 * W);
    return W - rho * kStar * psi - c * psi * psi;
}

// Closed set {x : a x^2 + b x + g <= 0} for a >= 0 as [lo, hi] (infinite ends allowed); false when
// empty. Roots from the cancellation-free q = -(b + sign(b) sqrt(disc)) / 2 form.
bool quadraticLe(double a, double b, double g, double& lo, double& hi) {
    if (a <= 0.0) {
        if (b > 0.0) {
            lo = -kInf;
            hi = -g / b;
            return true;
        }
        if (b < 0.0) {
            lo = -g / b;
            hi = kInf;
            return true;
        }
        lo = -kInf;
        hi = kInf;
        return g <= 0.0;
    }
    const double disc = b * b - 4.0 * a * g;
    if (disc < 0.0) return false;
    const double q = -0.5 * (b + (b >= 0.0 ? 1.0 : -1.0) * std::sqrt(disc));
    const double r1 = q / a;
    const double r2 = q != 0.0 ? g / q : r1;
    lo = std::min(r1, r2);
    hi = std::max(r1, r2);
    return true;
}

// Feasible psi interval (margins included) for a given rho; false when empty. With
// theta(psi) = W - rho k* psi - c psi^2 (thetaAtAnchor) every constraint is a quadratic in psi
// with a nonnegative leading coefficient, so each feasible set is an interval:
//   theta > 0; butterfly psi (1 + |rho|) <= 4 and psi^2 (1 + |rho|) <= 4 theta;
//   calendar theta > theta_prev, psi >= psi_prev max((1 - rho_p) / (1 - rho), (1 + rho_p) /
//   (1 + rho)) and the sufficient condition (i) psi theta_prev <= psi_prev theta.
bool psiInterval(
    double rho, double kStar, double W, const PrevSlice& prev, double& lo, double& hi) {
    const double ar = 1.0 + std::fabs(rho);
    const double c = (1.0 - rho * rho) * kStar * kStar / (4.0 * W);
    const double rk = rho * kStar;
    double upper = 4.0 / ar;
    double lower = 0.0;
    auto intersect = [&](double a, double b, double g) {
        double l = 0.0, h = 0.0;
        if (!quadraticLe(a, b, g, l, h)) return false;
        upper = std::min(upper, h);
        if (l > 0.0) lower = std::max(lower, l);
        return true;
    };
    if (!intersect(c, rk, -W)) return false;                        // theta > 0
    if (!intersect(ar + 4.0 * c, 4.0 * rk, -4.0 * W)) return false; // butterfly
    if (!intersect(c, rk, prev.theta - W)) return false;            // calendar theta increasing
    if (prev.exists) {
        if (!intersect(prev.psi * c, prev.theta + prev.psi * rk, -prev.psi * W)) return false;
        lower = std::max(
            lower, std::max(
                       prev.psi * (1.0 - prev.rho) / (1.0 - rho),
                       prev.psi * (1.0 + prev.rho) / (1.0 + rho)));
    }
    if (!(upper > 0.0)) return false;
    hi = upper * (1.0 - kMargin);
    lo = lower > 0.0 ? lower * (1.0 + kMargin) : 0.0;
    lo = std::max(lo, 1e-9 * hi);
    return lo < hi;
}

double objective(
    const std::vector<Point>& pts,
    double T,
    double kStar,
    double thetaStar,
    double rho,
    double psi) {
    const double theta = thetaAtAnchor(thetaStar, rho, kStar, psi);
    if (!(theta > 0.0)) return kInf;
    const Node n{theta, psi, rho * psi, 0.0, 0.0, 0.0};
    double sum = 0.0;
    for (const Point& p : pts) {
        const double wv = w(n, p.k);
        const double d = std::sqrt(std::max(wv, 0.0) / T) - p.sigma;
        sum += p.weight * d * d;
    }
    return sum;
}

// Best psi for one rho, updating `best` when it improves.
void tryRho(
    double rho,
    const std::vector<Point>& pts,
    double T,
    double kStar,
    double thetaStar,
    const PrevSlice& prev,
    Fit& best) {
    double lo = 0.0, hi = 0.0;
    if (!psiInterval(rho, kStar, thetaStar, prev, lo, hi)) return;
    auto f = [&](double psi) { return objective(pts, T, kStar, thetaStar, rho, psi); };
    double psi = brentMinimise(f, lo, hi, 1e-12);
    double fv = f(psi);
    // the optimum may sit on a bound, which Brent only approaches
    for (double edge : {lo, hi}) {
        const double fe = f(edge);
        if (fe < fv) {
            fv = fe;
            psi = edge;
        }
    }
    if (fv < best.objective) {
        best.objective = fv;
        best.rho = rho;
        best.psi = psi;
        best.theta = thetaAtAnchor(thetaStar, rho, kStar, psi);
    }
}

bool searchSlice(
    const std::vector<Point>& pts,
    double T,
    double kStar,
    double thetaStar,
    const PrevSlice& prev,
    Fit& best) {
    best = Fit{};
    const int n0 = 41;
    const double step0 = 2.0 * kRhoMax / (n0 - 1);
    for (int i = 0; i < n0; ++i) {
        tryRho(-kRhoMax + i * step0, pts, T, kStar, thetaStar, prev, best);
    }
    if (prev.exists) tryRho(prev.rho, pts, T, kStar, thetaStar, prev, best);
    if (!std::isfinite(best.objective)) return false;
    double step = step0;
    for (int round = 0; round < 60 && step > 1e-10; ++round) {
        const double centre = best.rho;
        const double sub = step / 5.0;
        for (int i = -10; i <= 10; ++i) {
            const double rho = centre + i * sub;
            if (std::fabs(rho) <= kRhoMax) tryRho(rho, pts, T, kStar, thetaStar, prev, best);
        }
        step = sub;
    }
    return true;
}

struct QuoteVol {
    double stdDev = kNaN;     // sqrt(total variance)
    std::string error;        // empty when stdDev is valid
    bool outOfBounds = false; // price outside the no-arbitrage bounds at the slice forward
};

// Implied standard deviation of a quote. A price outside the no-arbitrage bounds at forward F
// (intrinsic, D F for a call or D K for a put) sets `outOfBounds`, a failed solve sets `error`;
// both leave the quote out of the fit instead of failing the request.
QuoteVol quoteStdDev(const OptionQuote& q, double T, double F, double D) {
    QuoteVol out;
    if (q.impliedVol) {
        out.stdDev = *q.impliedVol * std::sqrt(T);
        return out;
    }
    const double sign = q.type == QuantLib::Option::Call ? 1.0 : -1.0;
    const double intrinsic = D * std::max(sign * (F - q.strike), 0.0);
    const double upper = q.type == QuantLib::Option::Call ? D * F : D * q.strike;
    if (!(*q.price > intrinsic) || !(*q.price < upper)) {
        out.outOfBounds = true;
        return out;
    }
    try {
        out.stdDev = QuantLib::blackFormulaImpliedStdDev(
            q.type, q.strike, F, *q.price, D, 0.0, QuantLib::Null<double>(), 1e-14, 200);
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

// Black vega (price per unit of vol) at the market vol.
double blackVega(double K, double F, double D, double T, double vol) {
    const double sd = vol * std::sqrt(T);
    const double d1 = std::log(F / K) / sd + 0.5 * sd;
    return D * F * std::sqrt(T) * QuantLib::CumulativeNormalDistribution().derivative(d1);
}

struct ExpiryWork {
    QuantLib::Date expiry;
    double T = 0.0;
    double D = 1.0;
    double F = 0.0;
    double curveF = 0.0;
    std::vector<std::size_t> idx; // quote indices
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct ParityForward {
    double forward = kNaN; // NaN when fewer than 2 common-strike pairs
    std::size_t used = 0;
    std::size_t rejected = 0;
    std::set<std::size_t> rejectedQuotes; // both quotes of every rejected pair
};

constexpr std::size_t kMinPairsForRejection = 3;
constexpr double kMadScale = 5.0 * 1.4826;
constexpr double kMinParityTolerance = 1e-4; // relative to the median forward
constexpr std::size_t kMinUsedQuotes = 3;

// Parity forward from the common strikes of priced calls and puts. Per pair F_j = (C_j - P_j) / D
// + K_j; with >= 3 pairs the pairs further than max(5 * 1.4826 MAD, 1e-4 F_med) from the median
// are rejected, and F is the mean of the rest. Exact data has MAD = 0 and loses nothing.
ParityForward parityForward(
    const std::vector<OptionQuote>& quotes, const std::vector<std::size_t>& idx, double D) {
    struct Leg {
        double price;
        std::size_t quote;
    };
    std::map<double, Leg> calls, puts;
    for (std::size_t i : idx) {
        const OptionQuote& q = quotes[i];
        if (!q.price) continue;
        auto& m = q.type == QuantLib::Option::Call ? calls : puts;
        m.emplace(q.strike, Leg{*q.price, i}); // first quote of a strike/type wins
    }
    struct Pair {
        double f;
        std::size_t call, put;
    };
    std::vector<Pair> pairs;
    for (const auto& c : calls) {
        const auto p = puts.find(c.first);
        if (p == puts.end()) continue;
        pairs.push_back(
            Pair{
                (c.second.price - p->second.price) / D + c.first, c.second.quote, p->second.quote});
    }
    ParityForward out;
    if (pairs.size() < 2) return out;
    std::vector<double> fs;
    for (const Pair& p : pairs)
        fs.push_back(p.f);
    const double med = median(fs);
    double tol = kInf;
    if (pairs.size() >= kMinPairsForRejection) {
        std::vector<double> dev;
        for (double f : fs)
            dev.push_back(std::fabs(f - med));
        tol = std::max(kMadScale * median(dev), kMinParityTolerance * std::fabs(med));
    }
    double sum = 0.0;
    for (const Pair& p : pairs) {
        if (std::fabs(p.f - med) > tol) {
            ++out.rejected;
            out.rejectedQuotes.insert(p.call);
            out.rejectedQuotes.insert(p.put);
        } else {
            sum += p.f;
            ++out.used;
        }
    }
    out.forward = sum / static_cast<double>(out.used);
    return out;
}

// Reason a quote is left out before the pairing, moneyness and weighting stages, or "".
// An out-of-bounds price is reported as such whatever else would have dropped the quote.
std::string earlyExclusion(const QuoteVol& v, bool parityOutlier) {
    if (v.outOfBounds) return "price_outside_bounds";
    if (parityOutlier) return "parity_outlier";
    if (!v.error.empty()) return "implied_vol_not_solved";
    return "";
}

// 400 when fewer than kMinUsedQuotes quotes of the expiry stay in the fit.
void requireMinUsedQuotes(
    const std::vector<QuoteRow>& rows,
    const ExpiryWork& ex,
    std::size_t used,
    double maxAbsLogMoneyness) {
    if (used >= kMinUsedQuotes) return;
    std::map<std::string, std::size_t> byReason;
    for (std::size_t i : ex.idx) {
        if (!rows[i].used) ++byReason[rows[i].exclusionReason];
    }
    std::ostringstream os;
    os << "SSVI calibration: expiry " << DateToIso(ex.expiry) << " has " << used
       << " used quotes, at least " << kMinUsedQuotes
       << " are required (|k| <= " << maxAbsLogMoneyness << "); excluded:";
    for (const auto& r : byReason)
        os << " " << r.first << "=" << r.second;
    QUANTRA_INVALID_ARGUMENT(os.str());
}

} // namespace

double anchorTheta(double W, double rho, double k, double psi) {
    return thetaAtAnchor(W, rho, k, psi);
}

void validateInput(const CalibrationInput& in) {
    if (in.discount.empty()) {
        QUANTRA_INVALID_ARGUMENT("SSVI calibration: a discount curve is required");
    }
    if (!(in.spot > 0.0) || !std::isfinite(in.spot)) {
        QUANTRA_INVALID_ARGUMENT("SSVI calibration: spot must be finite and > 0");
    }
    if (!(in.maxAbsLogMoneyness > 0.0)) {
        QUANTRA_INVALID_ARGUMENT("SSVI calibration: maxAbsLogMoneyness must be > 0");
    }
    if (in.quotes.empty()) {
        QUANTRA_INVALID_ARGUMENT("SSVI calibration: at least one quote is required");
    }
    for (std::size_t i = 0; i < in.quotes.size(); ++i) {
        const OptionQuote& q = in.quotes[i];
        const std::string who = describeQuote(i, q);
        if (q.price.has_value() == q.impliedVol.has_value()) {
            QUANTRA_INVALID_ARGUMENT(who + ": exactly one of price / impliedVol is required");
        }
        if (!(q.strike > 0.0) || !std::isfinite(q.strike)) {
            QUANTRA_INVALID_ARGUMENT(who + ": strike must be finite and > 0");
        }
        if (q.impliedVol && !(*q.impliedVol > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(who + ": impliedVol must be > 0");
        }
        if (q.price && !(*q.price > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(who + ": price must be > 0");
        }
        if (!(q.weight > 0.0)) QUANTRA_INVALID_ARGUMENT(who + ": weight must be > 0");
    }
}

ScanResult defaultScan(const Surface& surface) {
    const std::vector<Slice>& sl = surface.slices();
    std::vector<double> times;
    times.push_back(0.5 * sl.front().t);
    for (std::size_t s = 0; s < sl.size(); ++s) {
        times.push_back(sl[s].t);
        if (s + 1 < sl.size()) times.push_back(0.5 * (sl[s].t + sl[s + 1].t));
    }
    times.push_back(1.5 * sl.back().t);
    return scan(surface, times, -1.5, 1.0, 201);
}

CalibrationResult calibrate(const CalibrationInput& in) {
    validateInput(in);
    const std::vector<OptionQuote>& quotes = in.quotes;

    std::map<QuantLib::Date, std::vector<std::size_t>> byExpiry;
    for (std::size_t i = 0; i < quotes.size(); ++i)
        byExpiry[quotes[i].expiry].push_back(i);

    CalibrationResult res;
    res.quotes.resize(quotes.size());
    std::vector<ssvi::Slice> slices;
    PrevSlice prev;

    for (const auto& entry : byExpiry) {
        ExpiryWork ex;
        ex.expiry = entry.first;
        ex.idx = entry.second;
        ex.T = in.dayCounter.yearFraction(in.referenceDate, ex.expiry);
        if (!(ex.T > 0.0)) {
            std::ostringstream os;
            os << "SSVI calibration: expiry " << DateToIso(ex.expiry)
               << " is not after the reference date";
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
        const double dRf = in.discount->discount(ex.T, true);
        ex.D = dRf;
        ex.curveF = in.dividend.empty() ? kNaN : in.spot * in.dividend->discount(ex.T, true) / dRf;
        const ParityForward parity = parityForward(quotes, ex.idx, ex.D);
        if (std::isnan(parity.forward) && std::isnan(ex.curveF)) {
            std::ostringstream os;
            os << "SSVI calibration: no forward at expiry " << DateToIso(ex.expiry)
               << ": add call and put quotes at >= 2 common strikes for put-call parity, or a "
                  "dividend curve";
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
        ex.F = std::isnan(parity.forward) ? ex.curveF : parity.forward;
        if (!(ex.F > 0.0)) {
            std::ostringstream os;
            os << "SSVI calibration: forward at expiry " << DateToIso(ex.expiry)
               << " is not positive (" << ex.F << ")";
            QUANTRA_INVALID_ARGUMENT(os.str());
        }

        // implied standard deviation of every quote
        std::vector<QuoteVol> vols;
        for (std::size_t i : ex.idx)
            vols.push_back(quoteStdDev(quotes[i], ex.T, ex.F, ex.D));

        // out-of-the-money choice where a strike has both a call and a put
        std::map<double, std::pair<bool, bool>> hasCallPut;
        for (std::size_t i : ex.idx) {
            auto& cp = hasCallPut[quotes[i].strike];
            (quotes[i].type == QuantLib::Option::Call ? cp.first : cp.second) = true;
        }

        std::vector<Point> pts;
        std::vector<std::size_t> usedIdx; // quote indices of pts
        std::vector<double> spreads;      // vol-unit spreads of the used quotes (NaN if none)
        for (std::size_t j = 0; j < ex.idx.size(); ++j) {
            const std::size_t i = ex.idx[j];
            const OptionQuote& q = quotes[i];
            QuoteRow& row = res.quotes[i];
            row.quoteIndex = i;
            row.expiry = ex.expiry;
            row.strike = q.strike;
            row.k = std::log(q.strike / ex.F);
            const bool solved = vols[j].error.empty() && !vols[j].outOfBounds;
            row.marketVol = solved ? vols[j].stdDev / std::sqrt(ex.T) : kNaN;
            const std::string early = earlyExclusion(vols[j], parity.rejectedQuotes.count(i) > 0);
            if (!early.empty()) {
                row.exclusionReason = early;
                continue;
            }
            const auto cp = hasCallPut[q.strike];
            bool chosen = true;
            if (cp.first && cp.second) {
                const bool wantCall = q.strike >= ex.F;
                chosen = (q.type == QuantLib::Option::Call) == wantCall;
            }
            if (!chosen) {
                row.exclusionReason = "in_the_money_side";
                continue;
            }
            if (std::fabs(row.k) > in.maxAbsLogMoneyness) {
                ++res.excludedByMoneyness;
                row.exclusionReason = "max_abs_log_moneyness";
                continue;
            }
            if (!(vols[j].stdDev > 0.0)) {
                row.exclusionReason = "implied_vol_not_positive";
                continue;
            }
            double omega = 1.0;
            double spread = kNaN;
            const double vega = blackVega(q.strike, ex.F, ex.D, ex.T, row.marketVol);
            if (in.weighting == QuoteWeighting::Vega) {
                omega = vega;
            } else if (in.weighting == QuoteWeighting::BidAsk && q.bid && q.ask) {
                spread = *q.ask - *q.bid;
                if (q.price) spread /= std::max(vega, 1e-300);
                if (!(spread > 0.0)) {
                    row.exclusionReason = "ask_not_above_bid";
                    continue;
                }
            }
            row.used = true;
            pts.push_back(Point{row.k, row.marketVol, q.weight * omega});
            usedIdx.push_back(i);
            spreads.push_back(spread);
        }
        requireMinUsedQuotes(res.quotes, ex, pts.size(), in.maxAbsLogMoneyness);
        if (in.weighting == QuoteWeighting::BidAsk) {
            std::vector<double> known;
            for (double s : spreads) {
                if (!std::isnan(s)) known.push_back(s);
            }
            if (known.empty()) {
                std::ostringstream os;
                os << "SSVI calibration: BidAsk weighting needs bid and ask on at least one "
                      "quote at expiry "
                   << DateToIso(ex.expiry);
                QUANTRA_INVALID_ARGUMENT(os.str());
            }
            const double med = median(known);
            for (std::size_t p = 0; p < pts.size(); ++p) {
                const double s = std::isnan(spreads[p]) ? med : spreads[p];
                pts[p].weight = quotes[usedIdx[p]].weight / (s * s);
            }
        }

        // anchor: used quote closest to the money
        std::size_t a = 0;
        for (std::size_t p = 1; p < pts.size(); ++p) {
            if (std::fabs(pts[p].k) < std::fabs(pts[a].k)) a = p;
        }
        const double kStar = pts[a].k;
        double thetaStar = pts[a].sigma * pts[a].sigma * ex.T;

        Fit fit;
        bool clamped = false;
        if (!searchSlice(pts, ex.T, kStar, thetaStar, prev, fit)) {
            const double floor = prev.theta * (1.0 + 1e-6) + 1e-12;
            if (prev.exists && thetaStar < floor) {
                thetaStar = floor;
                clamped = true;
                searchSlice(pts, ex.T, kStar, thetaStar, prev, fit);
            }
            if (!std::isfinite(fit.objective)) {
                std::ostringstream os;
                os << "SSVI calibration: no arbitrage-free slice exists at expiry "
                   << DateToIso(ex.expiry) << " (anchor k=" << kStar << ", theta=" << thetaStar
                   << ", previous theta=" << prev.theta << ")";
                QUANTRA_INVALID_ARGUMENT(os.str());
            }
        }

        SliceDiagnostics diag;
        diag.usedQuotes = pts.size();
        diag.anchorK = kStar;
        diag.anchorTheta = thetaStar;
        diag.anchorClamped = clamped;
        diag.parityPairsUsed = parity.used;
        diag.parityPairsRejected = parity.rejected;

        res.expiries.push_back(ex.expiry);
        res.params.push_back(SsviSliceParams{fit.theta, fit.rho, fit.psi});
        res.times.push_back(ex.T);
        res.fitForwards.push_back(ex.F);
        res.curveForwards.push_back(ex.curveF);
        res.impliedDividendYields.push_back(-std::log(ex.F * ex.D / in.spot) / ex.T);
        res.slices.push_back(diag);
        slices.push_back(ssvi::Slice{ex.T, fit.theta, fit.rho, fit.psi});
        prev = PrevSlice{true, fit.theta, fit.psi, fit.rho};
    }

    // residuals, margins
    const Surface surface(slices);
    double sumSq = 0.0, maxAbs = 0.0;
    std::size_t nUsed = 0;
    for (std::size_t s = 0; s < slices.size(); ++s) {
        const Slice& sl = slices[s];
        const Node n{sl.theta, sl.psi, sl.rho * sl.psi, 0.0, 0.0, 0.0};
        SliceDiagnostics& d = res.slices[s];
        double ss = 0.0, mx = 0.0;
        for (QuoteRow& row : res.quotes) {
            if (row.expiry != res.expiries[s]) continue;
            row.modelVol = std::sqrt(w(n, row.k) / sl.t);
            row.residualVolPts = (row.modelVol - row.marketVol) * 100.0;
            if (!row.used) continue;
            ss += row.residualVolPts * row.residualVolPts;
            mx = std::max(mx, std::fabs(row.residualVolPts));
        }
        d.rmseVolPts = std::sqrt(ss / static_cast<double>(d.usedQuotes));
        d.maxAbsVolPts = mx;
        sumSq += ss;
        maxAbs = std::max(maxAbs, mx);
        nUsed += d.usedQuotes;
        const double ar = 1.0 + std::fabs(sl.rho);
        d.marginButterflyPsi = 4.0 - sl.psi * ar;
        d.marginButterflyPsi2 = 4.0 * sl.theta - sl.psi * sl.psi * ar;
        if (s == 0) {
            d.marginCalendarTheta = d.marginCalendarPsi = d.marginCalendarA = kInf;
        } else {
            const Slice& p = slices[s - 1];
            d.marginCalendarTheta = sl.theta - p.theta;
            d.marginCalendarPsi = sl.psi - p.psi;
            d.marginCalendarA = (sl.psi - p.psi) - std::fabs(sl.rho * sl.psi - p.rho * p.psi);
        }
    }
    res.rmseVolPts = std::sqrt(sumSq / static_cast<double>(nUsed));
    res.maxAbsVolPts = maxAbs;

    res.violations = checkArbitrage(slices);
    if (!res.violations.empty()) {
        const Violation& v = res.violations.front();
        std::ostringstream os;
        os << "SSVI calibration produced an arbitrageable surface: " << v.condition << " at slice "
           << v.slice << " (margin " << v.margin << ")";
        QUANTRA_ERROR(os.str());
    }

    res.scan = defaultScan(surface);
    return res;
}

}} // namespace quantra::ssvi
