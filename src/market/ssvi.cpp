#include "ssvi.h"

#include "error.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace quantra { namespace ssvi {

namespace {

constexpr double kTimeFloor = 1e-8;
constexpr double kTol = 1e-12;
constexpr double kInf = std::numeric_limits<double>::infinity();

double sqrtQ(const Node& n, double k) {
    const double q = n.psi * n.psi * k * k + 2.0 * n.a * n.theta * k + n.theta * n.theta;
    return std::sqrt(q);
}

void requireFinitePositive(double v, const char* what, std::size_t i) {
    if (!(v > 0.0) || !std::isfinite(v)) {
        std::ostringstream os;
        os << "SSVI slice " << i << ": " << what << " must be finite and > 0, got " << v;
        QUANTRA_INVALID_ARGUMENT(os.str());
    }
}

} // namespace

Surface::Surface(std::vector<Slice> slices) : slices_(std::move(slices)) {
    if (slices_.empty()) {
        QUANTRA_INVALID_ARGUMENT("SSVI surface needs at least one slice");
    }
    for (std::size_t i = 0; i < slices_.size(); ++i) {
        const Slice& s = slices_[i];
        requireFinitePositive(s.t, "t", i);
        requireFinitePositive(s.theta, "theta", i);
        requireFinitePositive(s.psi, "psi", i);
        if (!(std::fabs(s.rho) < 1.0)) {
            std::ostringstream os;
            os << "SSVI slice " << i << ": |rho| must be < 1, got " << s.rho;
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
        if (i > 0 && !(s.t > slices_[i - 1].t)) {
            std::ostringstream os;
            os << "SSVI slice " << i << ": t must be strictly increasing, got " << s.t << " after "
               << slices_[i - 1].t;
            QUANTRA_INVALID_ARGUMENT(os.str());
        }
    }
}

Node Surface::node(double tIn) const {
    const double t = std::max(tIn, kTimeFloor);
    const std::vector<Slice>& s = slices_;
    const std::size_t n = s.size();

    // Short end: homogeneous scaling of the first slice.
    if (t < s[0].t) {
        const double lam = t / s[0].t;
        const double a1 = s[0].rho * s[0].psi;
        return Node{lam * s[0].theta,    lam * s[0].psi,    lam * a1,
                    s[0].theta / s[0].t, s[0].psi / s[0].t, a1 / s[0].t};
    }

    // First pillar with T_j > t, so T_{j-1} <= t < T_j: a pillar belongs to the interval on
    // its right.
    const auto it =
        std::upper_bound(s.begin(), s.end(), t, [](double v, const Slice& sl) { return v < sl.t; });
    if (it != s.end()) {
        const std::size_t j = static_cast<std::size_t>(it - s.begin());
        const Slice& lo = s[j - 1];
        const Slice& hi = s[j];
        const double dt = hi.t - lo.t;
        const double lam = (t - lo.t) / dt;
        const double aLo = lo.rho * lo.psi;
        const double aHi = hi.rho * hi.psi;
        const double dTheta = (hi.theta - lo.theta) / dt;
        const double dPsi = (hi.psi - lo.psi) / dt;
        const double dA = (aHi - aLo) / dt;
        return Node{
            lo.theta + lam * (hi.theta - lo.theta),
            lo.psi + lam * (hi.psi - lo.psi),
            aLo + lam * (aHi - aLo),
            dTheta,
            dPsi,
            dA};
    }

    // Long end: psi, rho constant, theta continues the last forward variance.
    const Slice& last = s[n - 1];
    const double slope =
        (n >= 2) ? (last.theta - s[n - 2].theta) / (last.t - s[n - 2].t) : s[0].theta / s[0].t;
    return Node{last.theta + slope * (t - last.t), last.psi, last.rho * last.psi, slope, 0.0, 0.0};
}

double w(const Node& n, double k) {
    return 0.5 * (n.theta + n.a * k + sqrtQ(n, k));
}

double wk(const Node& n, double k) {
    return 0.5 * (n.a + (n.psi * n.psi * k + n.a * n.theta) / sqrtQ(n, k));
}

double wkk(const Node& n, double k) {
    const double s = sqrtQ(n, k);
    return 0.5 * n.theta * n.theta * (n.psi * n.psi - n.a * n.a) / (s * s * s);
}

double wt(const Node& n, double k) {
    const double s = sqrtQ(n, k);
    return 0.5 *
           (n.dTheta + n.dA * k +
            (n.psi * n.dPsi * k * k + (n.dA * n.theta + n.a * n.dTheta) * k + n.theta * n.dTheta) /
                s);
}

double g(const Node& n, double k, double y) {
    const double wv = w(n, k);
    const double d1 = wk(n, k);
    const double d2 = wkk(n, k);
    const double u = 1.0 - y * d1 / (2.0 * wv);
    return u * u - 0.25 * d1 * d1 * (1.0 / wv + 0.25) + 0.5 * d2;
}

double localVariance(const Node& n, double k, double y, double dDrift) {
    return (wt(n, k) + wk(n, k) * dDrift) / g(n, k, y);
}

std::vector<Violation> checkArbitrage(const std::vector<Slice>& slices) {
    std::vector<Violation> out;
    // Violated when lhs > rhs beyond the relative tolerance.
    auto add = [&out](std::size_t i, std::size_t prev, const char* name, double lhs, double rhs) {
        const double scale = std::max({std::fabs(lhs), std::fabs(rhs), 1e-300});
        if (lhs - rhs > kTol * scale) {
            out.push_back(Violation{i, prev, name, lhs - rhs});
        }
    };
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const Slice& s = slices[i];
        const double ar = 1.0 + std::fabs(s.rho);
        add(i, i, "butterfly_psi", s.psi * ar, 4.0);
        add(i, i, "butterfly_psi2", s.psi * s.psi * ar, 4.0 * s.theta);
        if (i == 0) continue;
        const Slice& p = slices[i - 1];
        add(i, i - 1, "calendar_theta", p.theta, s.theta);
        add(i, i - 1, "calendar_psi", p.psi, s.psi);
        add(i, i - 1, "calendar_a", std::fabs(s.rho * s.psi - p.rho * p.psi), s.psi - p.psi);

        // Sufficient part (Hendriks-Martini Prop. 3.5, Mingone 2022 Sec. 2.1): (i) phi = psi / theta
        // non-increasing, or (ii) the quadratic condition. Margins are rhs - lhs.
        const double m1 = p.psi * s.theta - s.psi * p.theta;
        const double scale1 = std::max(std::fabs(p.psi * s.theta), std::fabs(s.psi * p.theta));
        const bool ok1 = m1 >= -kTol * scale1;
        const double f1 = s.theta / p.theta - 1.0;
        const double f2 = s.psi * s.psi * p.theta / (p.psi * p.psi * s.theta) - 1.0;
        const double d = p.rho - (s.psi / p.psi) * s.rho;
        const double m2 = (f1 >= 0.0 && f2 >= 0.0) ? f1 * f2 - d * d : -kInf;
        const bool ok2 =
            f1 >= 0.0 && f2 >= 0.0 && m2 >= -kTol * std::max(d * d, std::fabs(f1 * f2));
        if (!ok1 && !ok2) {
            out.push_back(Violation{i, i - 1, "calendar_sufficient", -std::max(m1, m2)});
        }
    }
    return out;
}

ScanResult scan(
    const Surface& surface,
    const std::vector<double>& times,
    double kMin,
    double kMax,
    std::size_t nk) {
    if (times.empty() || nk < 2 || !(kMax > kMin)) {
        QUANTRA_INVALID_ARGUMENT("SSVI scan needs times, nk >= 2 and kMax > kMin");
    }
    ScanResult r;
    r.minG = r.minWt = std::numeric_limits<double>::infinity();
    for (double t : times) {
        const Node n = surface.node(t);
        for (std::size_t j = 0; j < nk; ++j) {
            const double k =
                kMin + (kMax - kMin) * static_cast<double>(j) / static_cast<double>(nk - 1);
            const double gv = g(n, k, k);
            if (gv < r.minG) {
                r.minG = gv;
                r.minGt = t;
                r.minGk = k;
            }
            const double wv = wt(n, k);
            if (wv < r.minWt) {
                r.minWt = wv;
                r.minWtT = t;
                r.minWtK = k;
            }
        }
    }
    return r;
}

}} // namespace quantra::ssvi
