#ifndef QUANTRA_CORRELATION_REGISTRY_H
#define QUANTRA_CORRELATION_REGISTRY_H

#include "correlation_domain.h"
#include "quote_registry.h"

#include "correlation_generated.h"

#include <ql/math/matrix.hpp>
#include <ql/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace quantra {

/**
 * A correlation matrix ready for a basket model: the QuantLib matrix in the
 * spec's label order plus the label -> row/column index map.
 */
struct CorrelationMatrixRuntime {
    std::string id;
    /// Row/column labels in matrix order (underlying ids), as on the wire.
    std::vector<std::string> labels;
    /// The matrix as supplied (symmetrised to the parser's 1e-12 tolerance).
    QuantLib::Matrix matrix;
    /// label -> row/column index into `matrix` and `labels`.
    std::unordered_map<std::string, QuantLib::Size> labelIndex;
    /// Smallest eigenvalue found by the positive semi-definite check.
    double smallestEigenvalue = 0.0;
};

/**
 * Build the runtime matrix from a parsed domain matrix and check that it is
 * positive semi-definite: the eigenvalues (QuantLib SymmetricSchurDecomposition,
 * Jacobi) must all be >= -1e-10, so a rank-deficient matrix such as rho = 1
 * on every pair (eigenvalue 0) is accepted and an inconsistent one (e.g.
 * {0.9, 0.9, -0.9} on three names, eigenvalue -0.8) is a 400
 * `correlations[<id>]: not positive semi-definite (smallest eigenvalue <x>)`.
 * No repair is applied.
 */
CorrelationMatrixRuntime MakeCorrelationRuntime(const CorrelationMatrix& domain);

/**
 * CorrelationRegistry - correlation matrices by id (QuantLib values only, no
 * FlatBuffers pointers). Built once per request from
 * pricing.correlations.matrices; basket products resolve their
 * `correlation_id` here.
 */
class CorrelationRegistry {
public:
    void put(const CorrelationMatrixRuntime& entry) { matrices_[entry.id] = entry; }

    bool has(const std::string& id) const { return matrices_.find(id) != matrices_.end(); }

    /// The matrix with the given id; QuantraNotFound (404) when absent.
    const CorrelationMatrixRuntime& get(const std::string& id) const;

    /**
     * The sub-matrix of `id` for `labels`, in the order given: entry (i, j)
     * of the result is the correlation between labels[i] and labels[j]. A
     * basket product names its underlyings in its own order and gets the
     * matrix permuted to match. A label absent from the matrix, or listed
     * twice, is a 400 naming it; an unknown id is a 404. A proper subset of
     * the labels is accepted (the principal sub-matrix of a PSD matrix is
     * PSD); the caller enforces "exactly these underlyings" when its
     * contract says so.
     */
    QuantLib::Matrix subMatrix(const std::string& id, const std::vector<std::string>& labels) const;

    std::size_t size() const { return matrices_.size(); }

private:
    std::unordered_map<std::string, CorrelationMatrixRuntime> matrices_;
};

/**
 * Build a CorrelationRegistry from the wire CorrelationMarketData. Every
 * matrix goes through CorrelationParser and MakeCorrelationRuntime (PSD
 * check); a duplicate id across pricing.correlations.matrices is a 400. A
 * null CorrelationMarketData yields an empty registry. Runs on every request
 * that carries the block, whether or not a product references it.
 */
class CorrelationRegistryBuilder {
public:
    CorrelationRegistry build(
        const quantra::CorrelationMarketData* correlations, const QuoteRegistry& quotes) const;
};

} // namespace quantra

#endif // QUANTRA_CORRELATION_REGISTRY_H
