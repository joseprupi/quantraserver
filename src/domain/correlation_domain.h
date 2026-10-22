#ifndef QUANTRA_CORRELATION_DOMAIN_H
#define QUANTRA_CORRELATION_DOMAIN_H

/**
 * Plain-domain mirror of flatbuffers/fbs/correlation.fbs:CorrelationMatrixSpec.
 *
 * Produced by CorrelationParser (parsers layer) from the wire table, turned
 * into a QuantLib::Matrix by CorrelationRegistryBuilder (market layer) and
 * consumed by basket evaluators through the registry. Holds plain values
 * only — no FlatBuffers pointers — so evaluators stay FlatBuffers-free.
 *
 * Invariants guaranteed by the parser: `n == labels.size() >= 2`, labels
 * unique and non-empty, `values.size() == n * n`, every entry finite and in
 * [-1, 1], unit diagonal and symmetry to 1e-12. Positive semi-definiteness
 * is NOT checked here: the registry builder does it on the QuantLib matrix.
 */

#include <cstddef>
#include <string>
#include <vector>

namespace quantra {

struct CorrelationMatrix {
    /// Identifier referenced by trades (`StructuredNote.correlation_id`).
    std::string id;
    /// Row/column labels in matrix order (underlying ids).
    std::vector<std::string> labels;
    /// Dense row-major entries, `n * n` of them.
    std::vector<double> values;
    /// Dimension (`labels.size()`).
    std::size_t n = 0;

    double at(std::size_t row, std::size_t col) const { return values[row * n + col]; }
};

} // namespace quantra

#endif // QUANTRA_CORRELATION_DOMAIN_H
