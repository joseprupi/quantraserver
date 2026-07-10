#ifndef QUANTRA_ERROR_H
#define QUANTRA_ERROR_H

// =============================================================================
// Quantra error-type policy
// =============================================================================
//
// Every handler throw site picks exactly one of the four macros below. The
// async gRPC layer (CallDataGeneric) catches them and maps each to a gRPC
// status code (and therefore an HTTP status when fronted by the JSON gateway):
//
//   QUANTRA_NOT_FOUND         -> gRPC NOT_FOUND          (HTTP 404)
//   QUANTRA_INVALID_ARGUMENT  -> gRPC INVALID_ARGUMENT   (HTTP 400)
//   QUANTRA_NOT_IMPLEMENTED   -> gRPC UNIMPLEMENTED       (HTTP 501)
//   QUANTRA_ERROR             -> gRPC ABORTED            (HTTP 500)
//   QUANTRA_DEADLINE_EXCEEDED -> gRPC DEADLINE_EXCEEDED   (HTTP 504)
//
// QuantLib's own exceptions are caught separately and also mapped to ABORTED.
//
// Choose the macro by what the failure means to the caller:
//
// QUANTRA_NOT_FOUND (404) — a referenced id is absent from the registry or the
//   request. The id is well-formed and of the right shape; it simply does not
//   resolve to anything. Covers curves, indices, swap indices, vol surfaces,
//   models, credit curves, coupon pricers, equity underlyings, inflation
//   curves/indices, etc. Canonical message shape: "<thing> not found: <id>".
//
// QUANTRA_INVALID_ARGUMENT (400) — the request is malformed or semantically
//   invalid independent of pricing, i.e. it is the client's fault. This is by
//   far the most common client error and includes:
//     * a null required FlatBuffer table, a missing required field, or an empty
//       list where at least one element is required (note: in the *_parser.cpp
//       layer a message like "X not found" usually means a null FB table, which
//       is a missing required field -> 400, NOT a registry miss);
//     * a WRONG KIND / WRONG VARIANT of a thing that WAS found — the id resolved
//       but is the wrong type ("Model 'X' is not a swaption model", "Index 'X'
//       is not an OvernightIndex", "Inflation curve is not zero inflation"). The
//       lookup succeeded, so this is a bad request, not a not-found;
//     * an out-of-range or otherwise invalid parameter ("must be non-negative",
//       "upper_limit must exceed lower_limit", "displacement must be > 0",
//       "must be strictly increasing", a value outside a curve/surface's
//       support);
//     * a config inconsistency in the request's own data (a strict-mode
//       as_of_date that does not match a curve/surface reference date, a swap
//       index whose conventions do not match the trade);
//     * a configuration the client deliberately chose that the server rejects
//       by design — a wrong variant or an intentionally-unsupported combination
//       ("Collar not yet supported - use separate Cap and Floor", "Normal SABR
//       is intentionally not supported for v1", "CMS leg cap/floor is not
//       supported in v1"). The request is well-formed but asks for a
//       combination the server will not honor. Contrast with the
//       NotImplemented bucket below, which is for features that ARE intended to
//       work but are not built yet.
//
// QUANTRA_NOT_IMPLEMENTED (501) — a valid, well-formed request for an
//   unsupported/unimplemented feature. The request would be perfectly legal once
//   the feature exists; it is simply not built yet in this server (or not present
//   in this QuantLib build). This is NOT the client's fault and is NOT a bad
//   variant — it is a genuine feature gap. Examples: the OIS-shaped SABR
//   calibrate path (schema-ready but the OIS-aware cube construction is deferred),
//   runtime bumping of a SABR swaption vol surface, schema-ready-but-unbuilt term
//   structure helpers (TenorBasisSwap/FxSwap/CrossCcyBasis), and an engine that
//   is unavailable in the linked QuantLib version. Maps to gRPC UNIMPLEMENTED
//   (HTTP 501), which correctly tells the client the request was understood but
//   the feature is not available, rather than that they sent something malformed.
//
// QUANTRA_ERROR (500) — a genuine server-side or internal invariant violation
//   that is NOT the client's fault: a "should never happen" reached only if
//   upstream validation or server wiring is broken. Examples: a matrix/grid
//   dimension mismatch detected while BUILDING, BUMPING, or FINALIZING a surface
//   (the dims were already validated at parse time), a server-computed value
//   coming back non-positive, a required server-supplied registry pointer being
//   null, or a built handle being unexpectedly empty. These should be rare.
// =============================================================================

#include <exception>
#include <sstream>
#include <string>
#include <memory>

class QuantraError : public std::exception
{
public:
    // Add more detail to the exception
    QuantraError(const std::string &message = "");
    ~QuantraError() throw() {}
    const char *what() const throw() { return message_->c_str(); }

private:
    std::shared_ptr<std::string> message_;
};

// Client-error subclasses. Handlers throw these (via the macros below) when
// the request itself is malformed or asks for something that doesn't exist;
// CallDataGeneric translates them to gRPC INVALID_ARGUMENT (HTTP 400) and
// NOT_FOUND (HTTP 404) respectively. Existing handlers that throw the base
// QuantraError continue to surface as ABORTED (HTTP 500), unchanged.
class QuantraInvalidArgument : public QuantraError
{
public:
    explicit QuantraInvalidArgument(const std::string &message = "")
        : QuantraError(message) {}
};

class QuantraNotFound : public QuantraError
{
public:
    explicit QuantraNotFound(const std::string &message = "")
        : QuantraError(message) {}
};

// Thrown when a valid request asks for an unsupported/unimplemented feature
// (a genuine feature gap, not a malformed or bad-variant request).
// CallDataGeneric translates it to gRPC UNIMPLEMENTED (HTTP 501).
class QuantraNotImplemented : public QuantraError
{
public:
    explicit QuantraNotImplemented(const std::string &message = "")
        : QuantraError(message) {}
};

// Thrown at a mid-computation checkpoint when the per-request budget (the
// client-propagated deadline and/or the server-side ceiling) has been
// exhausted. It is NOT a client-input error: the request was well-formed but
// the caller ran out of time. CallDataGeneric translates it to gRPC
// DEADLINE_EXCEEDED (HTTP 504) — slow is not malformed.
class QuantraDeadlineExceeded : public QuantraError
{
public:
    explicit QuantraDeadlineExceeded(const std::string &message = "")
        : QuantraError(message) {}
};

inline void QUANTRA_ERROR(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraError(msg_stream.str());
};

inline void QUANTRA_INVALID_ARGUMENT(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraInvalidArgument(msg_stream.str());
};

inline void QUANTRA_NOT_FOUND(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraNotFound(msg_stream.str());
};

inline void QUANTRA_NOT_IMPLEMENTED(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraNotImplemented(msg_stream.str());
};

inline void QUANTRA_DEADLINE_EXCEEDED(std::string message)
{
    std::ostringstream msg_stream;
    msg_stream << message;
    throw QuantraDeadlineExceeded(msg_stream.str());
};

#endif //QUANTRA_ERROR_H