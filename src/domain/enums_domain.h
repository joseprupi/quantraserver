#ifndef QUANTRA_ENUMS_DOMAIN_H
#define QUANTRA_ENUMS_DOMAIN_H

/**
 * FB-free home for every plain C++ enum that mirrors a FlatBuffers enum.
 *
 * These enums let the pricer/domain layer carry wire-format choices without
 * including any *_generated.h header (the pricer boundary guard forbids it).
 * Each enum below is a 1:1 mirror of a named FlatBuffers enum; the integer
 * values are LOCKED to their FB counterparts by static_assert in
 * common/enums_domain.cpp, so the schema and these mirrors can never silently
 * drift. This header must stay FlatBuffers-free: no *_generated.h include, no
 * flatbuffers:: references.
 */

#include <cstdint>

namespace quantra {

// --- Model spec mirrors (see flatbuffers/fbs/model.fbs + enums.fbs) ---

/// Mirrors FB enums::IrModelType; value-locked in common/enums_domain.cpp.
enum class IrModelTypeKind : std::int8_t {
    Black = 0,
    ShiftedBlack = 1,
    Bachelier = 2,
    HullWhiteLattice = 3,
};

/// Mirrors FB enums::ModelParamMode; value-locked in common/enums_domain.cpp.
enum class ModelParamModeKind : std::int8_t {
    Explicit = 0,
    Calibrate = 1,
};

/// Mirrors FB enums::EquityModelType; value-locked in common/enums_domain.cpp.
enum class EquityModelTypeKind : std::int8_t {
    BlackScholesAnalytic = 0,
    BinomialCRR = 1,
};

/// Mirrors FB enums::CdsEngineType; value-locked in common/enums_domain.cpp.
enum class CdsEngineTypeKind : std::int8_t {
    MidPoint = 0,
    ISDA = 1,
};

/// Mirrors FB enums::CdsIsdaNumericalFix; value-locked in common/enums_domain.cpp.
enum class CdsIsdaNumericalFixKind : std::int8_t {
    None = 0,
    Taylor = 1,
};

/// Mirrors FB enums::CdsIsdaAccrualBias; value-locked in common/enums_domain.cpp.
enum class CdsIsdaAccrualBiasKind : std::int8_t {
    HalfDayBias = 0,
    NoBias = 1,
};

/// Mirrors FB enums::CdsIsdaForwardsInCouponPeriod; value-locked in common/enums_domain.cpp.
enum class CdsIsdaForwardsInCouponPeriodKind : std::int8_t {
    Flat = 0,
    Piecewise = 1,
};

// --- Credit curve spec mirrors (see flatbuffers/fbs/credit_curve.fbs + enums.fbs) ---

/// Mirrors FB enums::CdsHelperModel; value-locked in common/enums_domain.cpp.
enum class CdsHelperModelKind : std::int8_t {
    MidPoint = 0,
    ISDA = 1,
};

/// Mirrors FB enums::CdsQuoteType; value-locked in common/enums_domain.cpp.
enum class CdsQuoteTypeKind : std::int8_t {
    ParSpread = 0,
    Upfront = 1,
};

/// Mirrors FB enums::Interpolator; value-locked in common/enums_domain.cpp.
/// Credit curves only dispatch on Linear / BackwardFlat / LogLinear today;
/// the other interpolators stay here so the mirror tracks the FB schema.
enum class CreditCurveInterpolatorKind : std::int8_t {
    BackwardFlat = 0,
    ForwardFlat = 1,
    Linear = 2,
    LogCubic = 3,
    LogLinear = 4,
};

// --- Curve / inflation-curve sampling mirrors ---

/// Mirrors FB CurveMeasure (flatbuffers/fbs/curve_query.fbs); value-locked in
/// common/enums_domain.cpp. The mapper translates between the two so the
/// bootstrap-curves pricer stays free of any *_generated.h header.
enum class CurveSampleMeasure : std::int8_t {
    DiscountFactor = 0,
    ZeroRate = 1,
    ForwardRate = 2,
};

/// Mirrors FB enums::InflationCurveMeasure; value-locked in
/// common/enums_domain.cpp. The mapper translates between the two so the
/// inflation-curves pricer stays free of any *_generated.h header.
enum class InflationCurveSampleMeasure : std::int8_t {
    ZeroRate = 0,
    YoYRate = 1,
};

// --- Vol-surface sampling mirrors (see flatbuffers/fbs/vol_query.fbs) ---

/// Mirrors FB VolSurfaceType; value-locked in common/enums_domain.cpp.
enum class SampleSurfaceType : std::int8_t {
    Swaption = 0,
    Optionlet = 1,
    EquityBlack = 2,
};

/// Mirrors FB VolStrikeAxis; value-locked in common/enums_domain.cpp.
enum class SampleStrikeAxis : std::int8_t {
    AbsoluteStrike = 0,
    SpreadFromATM = 1,
};

/// Mirrors FB VolOutputMode; value-locked in common/enums_domain.cpp.
enum class SampleOutputMode : std::int8_t {
    Cube = 0,
    SmileSlice = 1,
    TermSlice = 2,
    ExpirySlice = 3,
};

/// Mirrors FB ExpiryKind; value-locked in common/enums_domain.cpp.
enum class SampleExpiryKind : std::int8_t {
    ExerciseDate = 0,
    GridDate = 1,
};

} // namespace quantra

#endif // QUANTRA_ENUMS_DOMAIN_H
