#ifndef QUANTRA_REQUEST_BUDGET_H
#define QUANTRA_REQUEST_BUDGET_H

#include <algorithm>
#include <chrono>

#include "error.h"

namespace quantra {

/**
 * RequestBudget - a per-request CPU deadline threaded through the pricing path.
 *
 * The transport layer builds one budget per request from the client-propagated
 * gRPC deadline and an optional server-side ceiling, then hands it to the
 * evaluators and the curve bootstrapper. At natural checkpoints (the top of a
 * per-curve or per-trade loop) callers invoke check(); once the deadline has
 * passed it throws QuantraDeadlineExceeded, which CallDataGeneric maps to gRPC
 * DEADLINE_EXCEEDED (HTTP 504). This bounds a request's CPU MID-computation:
 * without it a request that has already blown its deadline still bootstraps
 * every curve and prices every trade to completion.
 *
 * This is plain std only (no gRPC / no FlatBuffers): evaluators hold it, and the
 * evaluator-boundary guard forbids those includes there.
 *
 * A default-constructed budget (deadline == time_point::max()) never triggers,
 * so every code path that does not thread a real budget stays byte-identical.
 */
struct RequestBudget {
    std::chrono::system_clock::time_point deadline{
        std::chrono::system_clock::time_point::max()};

    /// Throws QuantraDeadlineExceeded when the deadline has elapsed. Uses the
    /// same deadline-elapsed primitive as the transport front gate — NEVER
    /// ctx_.IsCancelled(), which is undefined on the raw async-CQ server.
    void check() const {
        if (std::chrono::system_clock::now() >= deadline) {
            QUANTRA_DEADLINE_EXCEEDED(
                "request deadline exceeded during computation");
        }
    }

    /// A budget that never triggers (the default for every call site that does
    /// not opt in). Keeps existing suites bit-identical.
    static RequestBudget unlimited() { return RequestBudget{}; }

    /// Compose the effective deadline from the client-propagated deadline and an
    /// optional server-side ceiling (milliseconds; <= 0 means "no ceiling"). The
    /// result is the EARLIER of the two. Passing `now` explicitly keeps the
    /// min-logic unit-testable without a real clock. Overflow-safe: a client
    /// with no deadline yields time_point::max(), and the ceiling is only
    /// computed as now + ceilingMs when ceilingMs > 0.
    static RequestBudget fromDeadlineAndCeiling(
        std::chrono::system_clock::time_point clientDeadline,
        std::chrono::system_clock::time_point now,
        long ceilingMs) {
        RequestBudget b;
        std::chrono::system_clock::time_point ceiling =
            std::chrono::system_clock::time_point::max();
        if (ceilingMs > 0) {
            ceiling = now + std::chrono::milliseconds(ceilingMs);
        }
        b.deadline = std::min(clientDeadline, ceiling);
        return b;
    }
};

} // namespace quantra

#endif // QUANTRA_REQUEST_BUDGET_H
