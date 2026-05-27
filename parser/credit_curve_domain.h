#ifndef QUANTRA_CREDIT_CURVE_DOMAIN_H
#define QUANTRA_CREDIT_CURVE_DOMAIN_H

/**
 * Plain-domain (FB-free) representation of a CreditCurveSpec.
 *
 * Mirror of flatbuffers/fbs/credit_curve.fbs:CreditCurveSpec, expressed in
 * QL/std types so consumers (CDS pricer, future) need not include any
 * *_generated.h header. Populated by PricingRegistryBuilder alongside the
 * legacy FB pointer (kept until each consumer is cut over).
 *
 * Enum kinds (HelperModel / QuoteType / Interpolator) without a natural
 * QuantLib equivalent are mirrored here as scoped enums whose underlying
 * integer values match the FB enums exactly (single source of truth: the
 * *_generated.h header; this comment marks the invariant). Conversion from
 * FB->QL for the fields that do have QL counterparts (Calendar, DayCounter,
 * BusinessDayConvention, Frequency, DateGeneration::Rule) goes through
 * common/enums.* (D16).
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/dategenerationrule.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/date.hpp>
#include <ql/time/frequency.hpp>
#include <ql/time/period.hpp>

namespace quantra {

/// Mirrors enums::CdsHelperModel (values must match).
enum class CdsHelperModelKind : std::int8_t {
    MidPoint = 0,
    ISDA = 1,
};

/// Mirrors enums::CdsQuoteType (values must match).
enum class CdsQuoteTypeKind : std::int8_t {
    ParSpread = 0,
    Upfront = 1,
};

/// Mirrors enums::Interpolator (values must match). Credit curves only
/// dispatch on Linear / BackwardFlat / LogLinear today; other interpolators
/// remain in the enum so the mirror stays in lockstep with the FB schema.
enum class CreditCurveInterpolatorKind : std::int8_t {
    BackwardFlat = 0,
    ForwardFlat = 1,
    Linear = 2,
    LogCubic = 3,
    LogLinear = 4,
};

struct CdsHelperConventionsDomain {
    int settlement_days = 0;
    QuantLib::Frequency frequency = QuantLib::Quarterly;
    QuantLib::BusinessDayConvention business_day_convention = QuantLib::Following;
    QuantLib::DateGeneration::Rule date_generation_rule = QuantLib::DateGeneration::TwentiethIMM;
    QuantLib::DayCounter last_period_day_counter;
    bool settles_accrual = true;
    bool pays_at_default_time = true;
    bool rebates_accrual = true;
    CdsHelperModelKind helper_model = CdsHelperModelKind::MidPoint;
};

struct CdsQuoteDomain {
    QuantLib::Period tenor;
    CdsQuoteTypeKind quote_type = CdsQuoteTypeKind::ParSpread;
    std::string quote_id;
    double quoted_par_spread = 0.0;
    double quoted_upfront = 0.0;
    double running_coupon = 0.0;
};

struct CreditCurveDomain {
    std::string id;
    QuantLib::Date reference_date;
    /// As-loaded calendar enum mapped to QL. May be NullCalendar; callers
    /// validate (legacy parser rejects NullCalendar).
    QuantLib::Calendar calendar;
    QuantLib::DayCounter day_counter;
    double recovery_rate = 0.4;
    CreditCurveInterpolatorKind curve_interpolator = CreditCurveInterpolatorKind::LogLinear;
    std::optional<CdsHelperConventionsDomain> helper_conventions;
    double flat_hazard_rate = 0.0;
    std::vector<CdsQuoteDomain> quotes;
};

} // namespace quantra

#endif // QUANTRA_CREDIT_CURVE_DOMAIN_H
