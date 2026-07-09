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
 * QuantLib equivalent live in common/enums_domain.h, where they are
 * value-locked to their FlatBuffers counterparts. Conversion from FB->QL for
 * the fields that do have QL counterparts (Calendar, DayCounter,
 * BusinessDayConvention, Frequency, DateGeneration::Rule) goes through
 * common/enums.*.
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

#include "enums_domain.h"

namespace quantra {

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
    /// Present -> use it (including a genuine 0). Absent on an upfront quote is
    /// an error; the evaluator rejects it rather than defaulting.
    std::optional<double> running_coupon;
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
    /// Present -> flat-hazard curve. Absent -> bootstrap from `quotes`; absent
    /// with no quotes is an error (no invented default hazard rate).
    std::optional<double> flat_hazard_rate;
    std::vector<CdsQuoteDomain> quotes;
};

} // namespace quantra

#endif // QUANTRA_CREDIT_CURVE_DOMAIN_H
