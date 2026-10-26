#ifndef QUANTRA_CORRELATION_PARSER_H
#define QUANTRA_CORRELATION_PARSER_H

#include "correlation_domain.h"
#include "quote_registry.h"

#include "correlation_generated.h"

#include <cstddef>

namespace quantra {

/**
 * CorrelationParser - CorrelationMatrixSpec (wire) -> CorrelationMatrix (domain).
 *
 * Every structural rule of the fbs contract is checked here and a violation
 * is a 400 whose message starts with `correlations[<index>]` (the position in
 * pricing.correlations.matrices) and names the offending labels:
 *   - id present and non-empty;
 *   - at least two labels, none empty, no duplicates;
 *   - values.n_rows == values.n_cols == len(labels), values row-major of
 *     length n*n (quote_ids, when given, the same length; a non-empty
 *     quote_id replaces the inline value through pricing.quotes, any quote
 *     type, 404 when unknown);
 *   - every entry finite and in [-1, 1];
 *   - unit diagonal (tolerance 1e-12);
 *   - symmetric (tolerance 1e-12), the first offending pair named by label.
 * Positive semi-definiteness and id uniqueness across matrices are checked
 * by CorrelationRegistryBuilder, which owns the QuantLib matrix.
 */
class CorrelationParser {
public:
    CorrelationMatrix parse(
        const quantra::CorrelationMatrixSpec* spec,
        const QuoteRegistry* quotes,
        std::size_t index) const;
};

} // namespace quantra

#endif // QUANTRA_CORRELATION_PARSER_H
