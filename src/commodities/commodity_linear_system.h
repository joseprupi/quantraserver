#ifndef QUANTRA_COMMODITIES_LINEAR_SYSTEM_H
#define QUANTRA_COMMODITIES_LINEAR_SYSTEM_H

/**
 * Small dense linear algebra for the commodity curve build: equations added
 * one at a time with their right-hand sides, an orthonormal basis of the
 * space they span (modified Gram-Schmidt, applied twice), the dependence of a
 * new equation on the earlier ones, the minimum-norm solution, the null
 * space, and a pivoted Gaussian solve.
 *
 * Rank decisions: an equation is dependent on the earlier ones when what is
 * left of it after removing their span is at most `tolerance` times its own
 * length (default 1e-9; the curve build's rows are in price units, so the
 * test is relative).
 */

#include <cstddef>
#include <vector>

namespace quantra { namespace commodities {

using Vector = std::vector<double>;

class EquationSpace {
public:
    explicit EquationSpace(std::size_t columns, double tolerance = 1e-9);

    struct Added {
        /// False when the equation lies in the span of the earlier ones.
        bool independent = true;
        /// Dependent equation: the right-hand side the earlier ones imply
        /// for it, and its coefficients on every equation added before it
        /// (indexes of add() calls; 0 for the ones not involved).
        double implied = 0.0;
        Vector combination;
    };

    /// Add equation `row` . x = `rhs` (row of length columns()). Its index is
    /// the number of add() calls before it.
    Added add(const Vector& row, double rhs);

    std::size_t columns() const { return n_; }
    std::size_t equations() const { return count_; }
    std::size_t rank() const { return q_.size(); }

    /// Minimum-norm solution of the independent equations for right-hand
    /// sides `rhs` (one per added equation; dependent ones are not read).
    Vector solve(const Vector& rhs) const;

    /// Whether `v` lies in the span of the equations (relative tolerance).
    bool spans(const Vector& v) const;

    /// Orthonormal basis of the vectors orthogonal to every equation
    /// (columns() - rank() vectors).
    std::vector<Vector> nullSpace() const;

private:
    Vector residual(const Vector& v, Vector& alpha) const;

    std::size_t n_;
    double tol_;
    std::size_t count_ = 0;
    std::vector<Vector> q_; // orthonormal rows
    std::vector<Vector> t_; // q_j = sum_k t_[j][k] * equation_k
    Vector beta_;           // q_j . x = beta_j for the added right-hand sides
};

double Dot(const Vector& a, const Vector& b);
double Norm(const Vector& a);

/// Solve the square system `a` z = `b` by Gaussian elimination with partial
/// pivoting; false when a pivot is at most `tolerance` times the largest
/// absolute entry of `a` (singular to that tolerance).
bool SolveSquare(std::vector<Vector> a, Vector b, Vector& z, double tolerance = 1e-12);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_LINEAR_SYSTEM_H
