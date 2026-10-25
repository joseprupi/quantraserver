#ifndef QUANTRA_SSVI_H
#define QUANTRA_SSVI_H

/**
 * eSSVI implied total variance, its arbitrage conditions and the analytic Dupire
 * local variance. Pure math: no QuantLib term structures (those live in
 * ssvi_vol_surface.h).
 *
 * Slice parametrization (Corbetta, Cohort, Laachir, Martini 2019, "Robust
 * calibration and arbitrage-free interpolation of SSVI slices",
 * arXiv:1804.04924; Hendriks-Martini 2019). Slice i has an expiry time T_i > 0
 * (year fraction), the ATM total implied variance theta_i > 0, the correlation
 * rho_i in (-1, 1) and psi_i = theta_i * phi_i > 0. With a = rho * psi and the
 * log-forward moneyness k = ln(K / F(T)) the total variance is
 *
 *     w(k) = 0.5 * ( theta + a k + sqrt( psi^2 k^2 + 2 a theta k + theta^2 ) )
 *
 * which is the Gatheral-Jacquier form theta/2 (1 + rho phi k + sqrt((phi k +
 * rho)^2 + 1 - rho^2)) with phi = psi / theta. Writing Q = psi^2 k^2 + 2 a theta
 * k + theta^2 and S = sqrt(Q) (Q > 0 because |a| < psi):
 *
 *     w_k  = 0.5 * ( a + (psi^2 k + a theta) / S )
 *     w_kk = 0.5 * theta^2 (psi^2 - a^2) / S^3
 *
 * w is homogeneous of degree 1 in (theta, psi, a).
 *
 * Time interpolation (Corbetta et al. Sec. 5, arbitrage free given arbitrage-free
 * slices). (theta, psi, a) are functions of t:
 *   * T_i <= t <= T_{i+1}: lambda = (t - T_i) / (T_{i+1} - T_i); theta, psi and
 *     a = rho psi are each linear in lambda (NOT rho). Their t-derivatives are
 *     the constant slopes of the interval.
 *   * 0 < t < T_1: lambda = t / T_1; theta = lambda theta_1, psi = lambda psi_1,
 *     rho = rho_1 (a = lambda a_1), so w(k, t) = (t / T_1) w_1(k) by
 *     homogeneity. Derivatives (theta_1, psi_1, a_1) / T_1.
 *   * t > T_N: psi, rho (hence a) constant; theta = theta_N + s (t - T_N) with
 *     s = (theta_N - theta_{N-1}) / (T_N - T_{N-1}) for N >= 2 and s = theta_1 /
 *     T_1 for N = 1 (continuation of the last forward variance).
 *   * At exactly a pillar the derivatives are those of the interval to the
 *     right (the last pillar uses the continuation). The local volatility is
 *     therefore discontinuous in t at the pillars, because dw/dt is piecewise
 *     constant in (theta, psi, a).
 * t is clamped to >= 1e-8 inside node(): at t = 0 the total variance is 0 and
 * the local variance is not defined; the clamp returns the limit t -> 0+ at the
 * first-interval slopes.
 *
 * Time derivative at fixed k, with ' = d/dt:
 *
 *     w_t = 0.5 * ( theta' + a' k
 *                   + ( psi psi' k^2 + (a' theta + a theta') k + theta theta' ) / S )
 *
 * Local variance (Dupire in total variance, Gatheral). In the plain case
 * the surface and the simulated process share the forward, y = k and
 *
 *     sigma_loc^2 = w_t / g(k, k),
 *     g(k, y) = (1 - y w_k / (2w))^2 - (w_k^2 / 4) (1 / w + 1 / 4) + w_kk / 2.
 *
 * Sticky-strike drift correction. The surface is defined in k = ln(K / F_ref(T))
 * for a frozen reference forward F_ref. When the simulated process has another
 * forward F_proc (bumped spot or rate), set D(T) = ln(F_proc(T) / F_ref(T)),
 * y = k - D(T) and dDrift = D'(T) = mu_proc(T) - mu_ref(T) with mu = r - q the
 * instantaneous forward rate. Then, at fixed strike moving with F_proc,
 *
 *     sigma_loc^2 = ( w_t(k) + w_k(k) * dDrift ) / g(k, y),
 *
 * all w terms at k. With D = 0 and dDrift = 0 this is the plain case.
 *
 * Arbitrage conditions (relative tolerance 1e-12 in the comparisons):
 *   * butterfly per slice (Gatheral-Jacquier, sufficient):
 *         psi (1 + |rho|) <= 4   and   psi^2 (1 + |rho|) <= 4 theta
 *   * calendar between consecutive slices (Hendriks-Martini Prop. 3.1,
 *     necessary and sufficient):
 *         theta_{i+1} >= theta_i, psi_{i+1} >= psi_i,
 *         |a_{i+1} - a_i| <= psi_{i+1} - psi_i
 *   * calendar, sufficient part (Hendriks-Martini Prop. 3.5, as restated in Mingone 2022,
 *     arXiv:2204.00312, Sec. 2.1; the necessary conditions above are not sufficient on their own):
 *     either (i) psi_{i+1} theta_i <= psi_i theta_{i+1} (phi = psi / theta non-increasing), or
 *     (ii) (rho_i - (psi_{i+1}/psi_i) rho_{i+1})^2 <=
 *          (theta_{i+1}/theta_i - 1)(psi_{i+1}^2 theta_i / (psi_i^2 theta_{i+1}) - 1).
 *     Reported as "calendar_sufficient" when neither holds; its margin is the violation
 *     amount, -max of the two (rhs - lhs). Under (i) the time interpolation above stays
 *     arbitrage free (theta, psi linear gives a monotone phi at every intermediate time, and
 *     phi is constant at the short end and decreasing at the long end).
 * `scan` is an independent numeric check (min g and min w_t on a grid).
 */

#include <cstddef>
#include <string>
#include <vector>

namespace quantra { namespace ssvi {

struct Slice {
    double t;     // expiry, year fraction, > 0
    double theta; // ATM total implied variance, > 0
    double rho;   // correlation, in (-1, 1)
    double psi;   // theta * phi, > 0
};

// Interpolated parameters at one time with their t-derivatives.
struct Node {
    double theta;
    double psi;
    double a; // rho * psi
    double dTheta;
    double dPsi;
    double dA;
};

class Surface {
public:
    // Slices sorted by t; throws QUANTRA_INVALID_ARGUMENT naming the slice index.
    explicit Surface(std::vector<Slice> slices);

    const std::vector<Slice>& slices() const { return slices_; }

    // Interpolated parameters at t (clamped to >= 1e-8), see the file comment.
    Node node(double t) const;

private:
    std::vector<Slice> slices_;
};

double w(const Node& n, double k);
double wk(const Node& n, double k);
double wkk(const Node& n, double k);
double wt(const Node& n, double k);
// Durrleman function with the moneyness y of the first term (see above).
double g(const Node& n, double k, double y);
// Local variance (w_t + w_k dDrift) / g(k, y); negative or infinite when the
// surface admits arbitrage.
double localVariance(const Node& n, double k, double y, double dDrift);

struct Violation {
    std::size_t slice;     // slice index (the later slice for calendar conditions)
    std::size_t prevSlice; // earlier slice for calendar conditions, == slice otherwise
    std::string condition; // "butterfly_psi", "butterfly_psi2", "calendar_theta", ...
    double margin;         // amount by which the condition is violated (> 0)
};

std::vector<Violation> checkArbitrage(const std::vector<Slice>& slices);

struct ScanResult {
    double minG = 0.0;
    double minGt = 0.0;
    double minGk = 0.0;
    double minWt = 0.0;
    double minWtT = 0.0;
    double minWtK = 0.0;
};

// Minimum of g(k, k) and w_t over times x (nk points of [kMin, kMax]).
ScanResult scan(
    const Surface& surface,
    const std::vector<double>& times,
    double kMin,
    double kMax,
    std::size_t nk);

}} // namespace quantra::ssvi

#endif // QUANTRA_SSVI_H
