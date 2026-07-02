// Day-counter enum-conversion regression tests.
//
// DayCounter_Actual365NoLeap used to be silently mapped to plain
// Actual365Fixed, which counts Feb 29 and therefore produces wrong
// year-fractions for accrual periods spanning a leap day. It must map to
// QuantLib's Actual365Fixed(NoLeap) convention instead. Built into the single
// test_quantra_vs_quantlib parity binary (Suite 1).
#include <gtest/gtest.h>

#include <ql/time/date.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include "enum_convert.h"

TEST(EnumConvertDayCounter, Actual365NoLeapMapsToNoLeapConvention) {
    // A period spanning 2024-02-29: 366 actual days, 365 NoLeap days.
    const QuantLib::Date start(1, QuantLib::January, 2024);
    const QuantLib::Date end(1, QuantLib::January, 2025);

    const QuantLib::DayCounter mapped =
        DayCounterToQL(quantra::enums::DayCounter_Actual365NoLeap);
    const QuantLib::DayCounter noLeap =
        QuantLib::Actual365Fixed(QuantLib::Actual365Fixed::NoLeap);
    const QuantLib::DayCounter plain = QuantLib::Actual365Fixed();

    EXPECT_EQ(mapped.name(), noLeap.name());
    EXPECT_EQ(mapped.dayCount(start, end), noLeap.dayCount(start, end));
    EXPECT_DOUBLE_EQ(mapped.yearFraction(start, end),
                     noLeap.yearFraction(start, end));

    // The NoLeap convention drops Feb 29: exactly 365/365 = 1.0, while plain
    // Actual365Fixed yields 366/365. The old fail-open mapping returned the
    // plain value.
    EXPECT_DOUBLE_EQ(mapped.yearFraction(start, end), 1.0);
    EXPECT_NE(mapped.yearFraction(start, end), plain.yearFraction(start, end));
}

TEST(EnumConvertDayCounter, Actual365FixedStillPlain) {
    // Guard the sibling mapping: DayCounter_Actual365Fixed must stay on the
    // standard convention (366/365 across a leap day).
    const QuantLib::Date start(1, QuantLib::January, 2024);
    const QuantLib::Date end(1, QuantLib::January, 2025);

    const QuantLib::DayCounter mapped =
        DayCounterToQL(quantra::enums::DayCounter_Actual365Fixed);
    EXPECT_EQ(mapped.name(), QuantLib::Actual365Fixed().name());
    EXPECT_DOUBLE_EQ(mapped.yearFraction(start, end), 366.0 / 365.0);
}
