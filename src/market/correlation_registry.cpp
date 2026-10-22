#include "correlation_registry.h"

#include "correlation_parser.h"
#include "error.h"

#include <ql/math/matrixutilities/symmetricschurdecomposition.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace quantra {

namespace {

// Eigenvalues this far below zero are rounding on a singular matrix (every
// pair at rho = 1 has eigenvalue exactly 0 in exact arithmetic), not an
// inconsistent correlation structure.
constexpr double kPsdTolerance = 1e-10;

std::string formatValue(double v) {
    std::ostringstream out;
    out.precision(15);
    out << v;
    return out.str();
}

std::string joinLabels(const std::vector<std::string>& labels) {
    std::string out;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) out += ", ";
        out += labels[i];
    }
    return out;
}

} // namespace

CorrelationMatrixRuntime MakeCorrelationRuntime(const CorrelationMatrix& domain) {
    CorrelationMatrixRuntime out;
    out.id = domain.id;
    out.labels = domain.labels;
    const QuantLib::Size n = domain.n;
    out.matrix = QuantLib::Matrix(n, n);
    for (QuantLib::Size i = 0; i < n; ++i) {
        out.labelIndex[domain.labels[i]] = i;
        out.matrix[i][i] = 1.0;
        for (QuantLib::Size j = i + 1; j < n; ++j) {
            // The parser allows an asymmetry of 1e-12; the Jacobi
            // decomposition and every consumer want an exactly symmetric
            // matrix.
            const double v = 0.5 * (domain.at(i, j) + domain.at(j, i));
            out.matrix[i][j] = v;
            out.matrix[j][i] = v;
        }
    }

    const QuantLib::SymmetricSchurDecomposition schur(out.matrix);
    const QuantLib::Array& eigenvalues = schur.eigenvalues();
    out.smallestEigenvalue = *std::min_element(eigenvalues.begin(), eigenvalues.end());
    if (out.smallestEigenvalue < -kPsdTolerance) {
        QUANTRA_INVALID_ARGUMENT(
            "correlations[" + out.id + "]: not positive semi-definite (smallest eigenvalue " +
            formatValue(out.smallestEigenvalue) + ")");
    }
    return out;
}

const CorrelationMatrixRuntime& CorrelationRegistry::get(const std::string& id) const {
    auto it = matrices_.find(id);
    if (it == matrices_.end()) {
        QUANTRA_NOT_FOUND("Correlation matrix not found: " + id);
    }
    return it->second;
}

QuantLib::Matrix CorrelationRegistry::subMatrix(
    const std::string& id, const std::vector<std::string>& labels) const {
    const CorrelationMatrixRuntime& entry = get(id);
    std::vector<QuantLib::Size> rows;
    rows.reserve(labels.size());
    std::unordered_set<std::string> seen;
    for (const std::string& label : labels) {
        auto it = entry.labelIndex.find(label);
        if (it == entry.labelIndex.end()) {
            QUANTRA_INVALID_ARGUMENT(
                "correlation matrix '" + id + "' has no label '" + label +
                "' (labels: " + joinLabels(entry.labels) + ")");
        }
        if (!seen.insert(label).second) {
            QUANTRA_INVALID_ARGUMENT(
                "correlation matrix '" + id + "': label '" + label + "' requested twice");
        }
        rows.push_back(it->second);
    }
    const QuantLib::Size n = rows.size();
    QuantLib::Matrix out(n, n);
    for (QuantLib::Size i = 0; i < n; ++i) {
        for (QuantLib::Size j = 0; j < n; ++j) {
            out[i][j] = entry.matrix[rows[i]][rows[j]];
        }
    }
    return out;
}

CorrelationRegistry CorrelationRegistryBuilder::build(
    const quantra::CorrelationMarketData* correlations, const QuoteRegistry& quotes) const {
    CorrelationRegistry out;
    if (!correlations || !correlations->matrices()) {
        return out;
    }
    CorrelationParser parser;
    std::size_t index = 0;
    for (const auto* spec : *correlations->matrices()) {
        CorrelationMatrix domain = parser.parse(spec, &quotes, index);
        if (out.has(domain.id)) {
            QUANTRA_INVALID_ARGUMENT(
                "duplicate correlation matrix id in pricing.correlations.matrices: " + domain.id);
        }
        out.put(MakeCorrelationRuntime(domain));
        ++index;
    }
    return out;
}

} // namespace quantra
