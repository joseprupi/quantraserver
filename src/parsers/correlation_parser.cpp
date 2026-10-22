#include "correlation_parser.h"

#include "error.h"

#include <cmath>
#include <sstream>
#include <unordered_set>

namespace quantra {

namespace {

constexpr double kTolerance = 1e-12;

std::string prefix(std::size_t index) {
    return "correlations[" + std::to_string(index) + "]";
}

std::string formatValue(double v) {
    std::ostringstream out;
    out.precision(15);
    out << v;
    return out.str();
}

} // namespace

CorrelationMatrix CorrelationParser::parse(
    const quantra::CorrelationMatrixSpec* spec,
    const QuoteRegistry* quotes,
    std::size_t index) const {
    const std::string where = prefix(index);
    if (!spec) {
        QUANTRA_INVALID_ARGUMENT(where + " is required");
    }
    if (!spec->id() || spec->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(where + ".id is required");
    }

    CorrelationMatrix out;
    out.id = spec->id()->str();
    const std::string suffix = " (matrix '" + out.id + "')";

    // --- Labels: at least two, non-empty, unique.
    if (!spec->labels() || spec->labels()->size() < 2) {
        QUANTRA_INVALID_ARGUMENT(where + ".labels must list at least two labels" + suffix);
    }
    std::unordered_set<std::string> seen;
    for (flatbuffers::uoffset_t i = 0; i < spec->labels()->size(); ++i) {
        const auto* label = spec->labels()->Get(i);
        if (!label || label->str().empty()) {
            QUANTRA_INVALID_ARGUMENT(
                where + ".labels[" + std::to_string(i) + "] must not be empty" + suffix);
        }
        if (!seen.insert(label->str()).second) {
            QUANTRA_INVALID_ARGUMENT(
                where + ".labels: duplicate label '" + label->str() + "'" + suffix);
        }
        out.labels.push_back(label->str());
    }
    out.n = out.labels.size();
    const std::size_t n = out.n;

    // --- Shape: n_rows == n_cols == len(labels), values row-major of length n*n.
    const auto* m = spec->values();
    if (!m) {
        QUANTRA_INVALID_ARGUMENT(where + ".values is required" + suffix);
    }
    if (m->n_rows() != static_cast<int>(n) || m->n_cols() != static_cast<int>(n)) {
        QUANTRA_INVALID_ARGUMENT(
            where +
            ".values: n_rows and n_cols must both equal len(labels) = " + std::to_string(n) +
            ", got " + std::to_string(m->n_rows()) + "x" + std::to_string(m->n_cols()) + suffix);
    }
    const std::size_t expected = n * n;
    if (!m->values() || m->values()->size() != expected) {
        QUANTRA_INVALID_ARGUMENT(
            where + ".values: values must hold n_rows*n_cols = " + std::to_string(expected) +
            " entries, got " + std::to_string(m->values() ? m->values()->size() : 0) + suffix);
    }
    if (m->quote_ids() && m->quote_ids()->size() != expected) {
        QUANTRA_INVALID_ARGUMENT(
            where +
            ".values: quote_ids must be empty or hold n_rows*n_cols = " + std::to_string(expected) +
            " entries, got " + std::to_string(m->quote_ids()->size()) + suffix);
    }

    // --- Entries: inline or through pricing.quotes; finite and in [-1, 1].
    out.values.resize(expected);
    for (std::size_t k = 0; k < expected; ++k) {
        double v = m->values()->Get(static_cast<flatbuffers::uoffset_t>(k));
        if (m->quote_ids()) {
            const auto* q = m->quote_ids()->Get(static_cast<flatbuffers::uoffset_t>(k));
            if (q && q->size() > 0) {
                if (!quotes) {
                    QUANTRA_ERROR(
                        where + ".values: quote_ids given but no QuoteRegistry is available" +
                        suffix);
                }
                v = quotes->getHandle(q->str())->value();
            }
        }
        const std::size_t row = k / n;
        const std::size_t col = k % n;
        const std::string pair = "(" + out.labels[row] + ", " + out.labels[col] + ")";
        if (!std::isfinite(v)) {
            QUANTRA_INVALID_ARGUMENT(where + ".values: entry " + pair + " must be finite" + suffix);
        }
        if (v < -1.0 || v > 1.0) {
            QUANTRA_INVALID_ARGUMENT(
                where + ".values: entry " + pair + " must be in [-1, 1], got " + formatValue(v) +
                suffix);
        }
        out.values[k] = v;
    }

    // --- Unit diagonal, then symmetry (first offending pair by label).
    for (std::size_t i = 0; i < n; ++i) {
        const double d = out.at(i, i);
        if (std::fabs(d - 1.0) > kTolerance) {
            QUANTRA_INVALID_ARGUMENT(
                where + ".values: diagonal entry (" + out.labels[i] + ", " + out.labels[i] +
                ") must be 1, got " + formatValue(d) + suffix);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double a = out.at(i, j);
            const double b = out.at(j, i);
            if (std::fabs(a - b) > kTolerance) {
                QUANTRA_INVALID_ARGUMENT(
                    where + ".values: not symmetric, (" + out.labels[i] + ", " + out.labels[j] +
                    ") = " + formatValue(a) + " but (" + out.labels[j] + ", " + out.labels[i] +
                    ") = " + formatValue(b) + suffix);
            }
        }
    }

    return out;
}

} // namespace quantra
