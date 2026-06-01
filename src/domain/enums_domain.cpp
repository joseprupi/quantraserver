/**
 * Value locks for the FB-free domain enums declared in enums_domain.h.
 *
 * Every enumerator of every mirror is static_asserted equal to its
 * FlatBuffers counterpart. Reordering, renumbering, inserting or removing a
 * value on either side breaks this translation unit at compile time, so the
 * schema and its FB-free mirrors can never silently drift.
 *
 * This is NOT a pricer translation unit, so including the generated headers
 * here is allowed (and required) — it is the one place where the domain
 * mirrors meet the wire-format enums.
 */

#include "enums_domain.h"

#include "enums_generated.h"        // quantra::enums::*
#include "curve_query_generated.h"  // quantra::CurveMeasure
#include "vol_query_generated.h"    // quantra::VolSurfaceType / VolStrikeAxis / VolOutputMode / ExpiryKind

namespace {

template <typename A, typename B>
constexpr bool same(A a, B b) {
    return static_cast<int>(a) == static_cast<int>(b);
}

// --- IrModelTypeKind vs enums::IrModelType ---
static_assert(same(quantra::IrModelTypeKind::Black, quantra::enums::IrModelType_Black),
              "IrModelTypeKind drift vs FB IrModelType");
static_assert(same(quantra::IrModelTypeKind::ShiftedBlack, quantra::enums::IrModelType_ShiftedBlack),
              "IrModelTypeKind drift vs FB IrModelType");
static_assert(same(quantra::IrModelTypeKind::Bachelier, quantra::enums::IrModelType_Bachelier),
              "IrModelTypeKind drift vs FB IrModelType");
static_assert(same(quantra::IrModelTypeKind::HullWhiteLattice, quantra::enums::IrModelType_HullWhiteLattice),
              "IrModelTypeKind drift vs FB IrModelType");

// --- ModelParamModeKind vs enums::ModelParamMode ---
static_assert(same(quantra::ModelParamModeKind::Explicit, quantra::enums::ModelParamMode_Explicit),
              "ModelParamModeKind drift vs FB ModelParamMode");
static_assert(same(quantra::ModelParamModeKind::Calibrate, quantra::enums::ModelParamMode_Calibrate),
              "ModelParamModeKind drift vs FB ModelParamMode");

// --- EquityModelTypeKind vs enums::EquityModelType ---
static_assert(same(quantra::EquityModelTypeKind::BlackScholesAnalytic, quantra::enums::EquityModelType_BlackScholesAnalytic),
              "EquityModelTypeKind drift vs FB EquityModelType");
static_assert(same(quantra::EquityModelTypeKind::BinomialCRR, quantra::enums::EquityModelType_BinomialCRR),
              "EquityModelTypeKind drift vs FB EquityModelType");

// --- CdsEngineTypeKind vs enums::CdsEngineType ---
static_assert(same(quantra::CdsEngineTypeKind::MidPoint, quantra::enums::CdsEngineType_MidPoint),
              "CdsEngineTypeKind drift vs FB CdsEngineType");
static_assert(same(quantra::CdsEngineTypeKind::ISDA, quantra::enums::CdsEngineType_ISDA),
              "CdsEngineTypeKind drift vs FB CdsEngineType");

// --- CdsIsdaNumericalFixKind vs enums::CdsIsdaNumericalFix ---
static_assert(same(quantra::CdsIsdaNumericalFixKind::None, quantra::enums::CdsIsdaNumericalFix_None),
              "CdsIsdaNumericalFixKind drift vs FB CdsIsdaNumericalFix");
static_assert(same(quantra::CdsIsdaNumericalFixKind::Taylor, quantra::enums::CdsIsdaNumericalFix_Taylor),
              "CdsIsdaNumericalFixKind drift vs FB CdsIsdaNumericalFix");

// --- CdsIsdaAccrualBiasKind vs enums::CdsIsdaAccrualBias ---
static_assert(same(quantra::CdsIsdaAccrualBiasKind::HalfDayBias, quantra::enums::CdsIsdaAccrualBias_HalfDayBias),
              "CdsIsdaAccrualBiasKind drift vs FB CdsIsdaAccrualBias");
static_assert(same(quantra::CdsIsdaAccrualBiasKind::NoBias, quantra::enums::CdsIsdaAccrualBias_NoBias),
              "CdsIsdaAccrualBiasKind drift vs FB CdsIsdaAccrualBias");

// --- CdsIsdaForwardsInCouponPeriodKind vs enums::CdsIsdaForwardsInCouponPeriod ---
static_assert(same(quantra::CdsIsdaForwardsInCouponPeriodKind::Flat, quantra::enums::CdsIsdaForwardsInCouponPeriod_Flat),
              "CdsIsdaForwardsInCouponPeriodKind drift vs FB CdsIsdaForwardsInCouponPeriod");
static_assert(same(quantra::CdsIsdaForwardsInCouponPeriodKind::Piecewise, quantra::enums::CdsIsdaForwardsInCouponPeriod_Piecewise),
              "CdsIsdaForwardsInCouponPeriodKind drift vs FB CdsIsdaForwardsInCouponPeriod");

// --- CdsHelperModelKind vs enums::CdsHelperModel ---
static_assert(same(quantra::CdsHelperModelKind::MidPoint, quantra::enums::CdsHelperModel_MidPoint),
              "CdsHelperModelKind drift vs FB CdsHelperModel");
static_assert(same(quantra::CdsHelperModelKind::ISDA, quantra::enums::CdsHelperModel_ISDA),
              "CdsHelperModelKind drift vs FB CdsHelperModel");

// --- CdsQuoteTypeKind vs enums::CdsQuoteType ---
static_assert(same(quantra::CdsQuoteTypeKind::ParSpread, quantra::enums::CdsQuoteType_ParSpread),
              "CdsQuoteTypeKind drift vs FB CdsQuoteType");
static_assert(same(quantra::CdsQuoteTypeKind::Upfront, quantra::enums::CdsQuoteType_Upfront),
              "CdsQuoteTypeKind drift vs FB CdsQuoteType");

// --- CreditCurveInterpolatorKind vs enums::Interpolator ---
static_assert(same(quantra::CreditCurveInterpolatorKind::BackwardFlat, quantra::enums::Interpolator_BackwardFlat),
              "CreditCurveInterpolatorKind drift vs FB Interpolator");
static_assert(same(quantra::CreditCurveInterpolatorKind::ForwardFlat, quantra::enums::Interpolator_ForwardFlat),
              "CreditCurveInterpolatorKind drift vs FB Interpolator");
static_assert(same(quantra::CreditCurveInterpolatorKind::Linear, quantra::enums::Interpolator_Linear),
              "CreditCurveInterpolatorKind drift vs FB Interpolator");
static_assert(same(quantra::CreditCurveInterpolatorKind::LogCubic, quantra::enums::Interpolator_LogCubic),
              "CreditCurveInterpolatorKind drift vs FB Interpolator");
static_assert(same(quantra::CreditCurveInterpolatorKind::LogLinear, quantra::enums::Interpolator_LogLinear),
              "CreditCurveInterpolatorKind drift vs FB Interpolator");

// --- CurveSampleMeasure vs CurveMeasure ---
static_assert(same(quantra::CurveSampleMeasure::DiscountFactor, quantra::CurveMeasure_DF),
              "CurveSampleMeasure drift vs FB CurveMeasure");
static_assert(same(quantra::CurveSampleMeasure::ZeroRate, quantra::CurveMeasure_ZERO),
              "CurveSampleMeasure drift vs FB CurveMeasure");
static_assert(same(quantra::CurveSampleMeasure::ForwardRate, quantra::CurveMeasure_FWD),
              "CurveSampleMeasure drift vs FB CurveMeasure");

// --- InflationCurveSampleMeasure vs enums::InflationCurveMeasure ---
static_assert(same(quantra::InflationCurveSampleMeasure::ZeroRate, quantra::enums::InflationCurveMeasure_ZeroRate),
              "InflationCurveSampleMeasure drift vs FB InflationCurveMeasure");
static_assert(same(quantra::InflationCurveSampleMeasure::YoYRate, quantra::enums::InflationCurveMeasure_YoYRate),
              "InflationCurveSampleMeasure drift vs FB InflationCurveMeasure");

// --- SampleSurfaceType vs VolSurfaceType ---
static_assert(same(quantra::SampleSurfaceType::Swaption, quantra::VolSurfaceType_Swaption),
              "SampleSurfaceType drift vs FB VolSurfaceType");
static_assert(same(quantra::SampleSurfaceType::Optionlet, quantra::VolSurfaceType_Optionlet),
              "SampleSurfaceType drift vs FB VolSurfaceType");
static_assert(same(quantra::SampleSurfaceType::EquityBlack, quantra::VolSurfaceType_EquityBlack),
              "SampleSurfaceType drift vs FB VolSurfaceType");

// --- SampleStrikeAxis vs VolStrikeAxis ---
static_assert(same(quantra::SampleStrikeAxis::AbsoluteStrike, quantra::VolStrikeAxis_AbsoluteStrike),
              "SampleStrikeAxis drift vs FB VolStrikeAxis");
static_assert(same(quantra::SampleStrikeAxis::SpreadFromATM, quantra::VolStrikeAxis_SpreadFromATM),
              "SampleStrikeAxis drift vs FB VolStrikeAxis");

// --- SampleOutputMode vs VolOutputMode ---
static_assert(same(quantra::SampleOutputMode::Cube, quantra::VolOutputMode_Cube),
              "SampleOutputMode drift vs FB VolOutputMode");
static_assert(same(quantra::SampleOutputMode::SmileSlice, quantra::VolOutputMode_SmileSlice),
              "SampleOutputMode drift vs FB VolOutputMode");
static_assert(same(quantra::SampleOutputMode::TermSlice, quantra::VolOutputMode_TermSlice),
              "SampleOutputMode drift vs FB VolOutputMode");
static_assert(same(quantra::SampleOutputMode::ExpirySlice, quantra::VolOutputMode_ExpirySlice),
              "SampleOutputMode drift vs FB VolOutputMode");

// --- SampleExpiryKind vs ExpiryKind ---
static_assert(same(quantra::SampleExpiryKind::ExerciseDate, quantra::ExpiryKind_ExerciseDate),
              "SampleExpiryKind drift vs FB ExpiryKind");
static_assert(same(quantra::SampleExpiryKind::GridDate, quantra::ExpiryKind_GridDate),
              "SampleExpiryKind drift vs FB ExpiryKind");

} // namespace
