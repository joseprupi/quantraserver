/**
 * @file non_finite_scan.h
 * @brief Detects bare non-finite number tokens in generated JSON text.
 *
 * FlatBuffers' text generator prints a non-finite double verbatim (`inf`,
 * `-inf`, `nan`), which is not JSON. The gateway runs this scan over every
 * generated response body before handing it to the HTTP layer, so a
 * non-finite value becomes an HTTP 500 that names the offending field instead
 * of a body strict JSON parsers reject.
 */

#ifndef QUANTRA_NON_FINITE_SCAN_H
#define QUANTRA_NON_FINITE_SCAN_H

#include <optional>
#include <string>

namespace quantra {

/**
 * Returns the path of the first bare non-finite number token in `json`
 * (e.g. "swaps[0].legs[1].flows[3].amount"), or std::nullopt when every scalar
 * is a finite number, a boolean, null or a string.
 *
 * Tokenizer-level: it walks objects/arrays to keep the path, skips string
 * contents (so "inf" inside a string value or key is never flagged), and flags
 * any bare token that is not `true`/`false`/`null` and cannot be a JSON number
 * (`inf`, `-inf`, `nan`, `-nan`, `Infinity`, `NaN`, ...). A top-level
 * non-finite scalar is reported with the path "<root>".
 */
std::optional<std::string> FindNonFiniteValue(const std::string& json);

} // namespace quantra

#endif // QUANTRA_NON_FINITE_SCAN_H
