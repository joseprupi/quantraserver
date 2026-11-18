#include "commodity_linear_system.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace quantra { namespace commodities {

double Dot(const Vector& a, const Vector& b) {
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        s += a[i] * b[i];
    }
    return s;
}

double Norm(const Vector& a) {
    return std::sqrt(Dot(a, a));
}

EquationSpace::EquationSpace(std::size_t columns, double tolerance)
    : n_(columns), tol_(tolerance) {}

Vector EquationSpace::residual(const Vector& v, Vector& alpha) const {
    Vector r = v;
    alpha.assign(q_.size(), 0.0);
    // Two passes of modified Gram-Schmidt keep the basis orthogonal to
    // working precision on nearly dependent equations.
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t j = 0; j < q_.size(); ++j) {
            const double a = Dot(q_[j], r);
            alpha[j] += a;
            for (std::size_t i = 0; i < n_; ++i) {
                r[i] -= a * q_[j][i];
            }
        }
    }
    return r;
}

EquationSpace::Added EquationSpace::add(const Vector& row, double rhs) {
    const std::size_t index = count_++;
    for (auto& t : t_) {
        t.push_back(0.0);
    }
    Vector alpha;
    Vector r = residual(row, alpha);
    const double length = Norm(row);
    const double left = Norm(r);
    Added out;
    // Coefficients of the projection on the earlier equations.
    Vector combination(count_, 0.0);
    double implied = 0.0;
    for (std::size_t j = 0; j < q_.size(); ++j) {
        implied += alpha[j] * beta_[j];
        for (std::size_t k = 0; k < count_; ++k) {
            combination[k] += alpha[j] * t_[j][k];
        }
    }
    if (left <= tol_ * length || length == 0.0) {
        out.independent = false;
        out.implied = implied;
        combination.resize(index);
        out.combination = std::move(combination);
        return out;
    }
    Vector q(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        q[i] = r[i] / left;
    }
    Vector t(count_, 0.0);
    t[index] = 1.0;
    for (std::size_t k = 0; k < count_; ++k) {
        t[k] = (t[k] - combination[k]) / left;
    }
    q_.push_back(std::move(q));
    t_.push_back(std::move(t));
    beta_.push_back((rhs - implied) / left);
    return out;
}

Vector EquationSpace::solve(const Vector& rhs) const {
    Vector x(n_, 0.0);
    for (std::size_t j = 0; j < q_.size(); ++j) {
        const double b = Dot(t_[j], rhs);
        for (std::size_t i = 0; i < n_; ++i) {
            x[i] += b * q_[j][i];
        }
    }
    return x;
}

bool EquationSpace::spans(const Vector& v) const {
    Vector alpha;
    const Vector r = residual(v, alpha);
    return Norm(r) <= tol_ * Norm(v);
}

std::vector<Vector> EquationSpace::nullSpace() const {
    std::vector<Vector> basis = q_;
    std::vector<Vector> out;
    for (std::size_t i = 0; i < n_ && basis.size() < n_; ++i) {
        Vector e(n_, 0.0);
        e[i] = 1.0;
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& b : basis) {
                const double a = Dot(b, e);
                for (std::size_t k = 0; k < n_; ++k) {
                    e[k] -= a * b[k];
                }
            }
        }
        const double length = Norm(e);
        if (length > 1e-8) {
            for (auto& v : e) {
                v /= length;
            }
            basis.push_back(e);
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool SolveSquare(std::vector<Vector> a, Vector b, Vector& z, double tolerance) {
    const std::size_t n = b.size();
    double scale = 0.0;
    for (const auto& row : a) {
        for (const double v : row) {
            scale = std::max(scale, std::fabs(v));
        }
    }
    if (scale == 0.0) {
        return n == 0;
    }
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t p = c;
        for (std::size_t r = c + 1; r < n; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) {
                p = r;
            }
        }
        if (std::fabs(a[p][c]) <= tolerance * scale) {
            return false;
        }
        std::swap(a[p], a[c]);
        std::swap(b[p], b[c]);
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = a[r][c] / a[c][c];
            for (std::size_t k = c; k < n; ++k) {
                a[r][k] -= f * a[c][k];
            }
            b[r] -= f * b[c];
        }
    }
    z.assign(n, 0.0);
    for (std::size_t c = n; c-- > 0;) {
        double s = b[c];
        for (std::size_t k = c + 1; k < n; ++k) {
            s -= a[c][k] * z[k];
        }
        z[c] = s / a[c][c];
    }
    return true;
}

}} // namespace quantra::commodities
