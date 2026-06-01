// Calendar advance utility parity tests.
//
// New in refactor step 6e (fill zero/thin parity gaps). Query-shaped endpoint
// (no NPV): the advanced date is computed directly with QuantLib's
// Calendar::advance and the engine response is required to match it exactly.
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib binary.
#include "parity_fixture.h"

#include "date_convert.h"

namespace quantra { namespace testing {

namespace {

void runAndCompare(quantra::enums::Calendar calEnum,
                   const QuantLib::Calendar& cal, const std::string& date,
                   int tenorNumber, quantra::enums::TimeUnit fbUnit,
                   QuantLib::TimeUnit qlUnit,
                   quantra::enums::BusinessDayConvention fbConv,
                   QuantLib::BusinessDayConvention qlConv, bool endOfMonth) {
    const QuantLib::Date input = DateToQL(date);
    const QuantLib::Date expected = cal.advance(
        input, QuantLib::Period(tenorNumber, qlUnit), qlConv, endOfMonth);

    flatbuffers::grpc::MessageBuilder b;
    auto d = b.CreateString(date);
    quantra::CalendarAdvanceRequestBuilder rb(b);
    rb.add_calendar(calEnum);
    rb.add_date(d);
    rb.add_tenor_number(tenorNumber);
    rb.add_tenor_unit(fbUnit);
    rb.add_convention(fbConv);
    rb.add_end_of_month(endOfMonth);
    b.Finish(rb.Finish());

    CalendarAdvanceEndpoint req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalendarAdvanceRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::CalendarAdvanceResponse>(respB->GetBufferPointer());

    EXPECT_EQ(r->calendar(), calEnum);
    ASSERT_NE(r->input_date(), nullptr);
    ASSERT_NE(r->advanced_date(), nullptr);
    EXPECT_EQ(DateToQL(r->input_date()->str()), input);
    EXPECT_EQ(DateToQL(r->advanced_date()->str()), expected);
}

} // namespace

// Forward advance by business days under TARGET.
TEST_F(QuantraComparisonTest, CalendarAdvance_ForwardDays) {
    runAndCompare(quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-01-15", 5, quantra::enums::TimeUnit_Days, QuantLib::Days,
                  quantra::enums::BusinessDayConvention_Following, QuantLib::Following,
                  false);
}

// Negative tenor (back-shift) under TARGET.
TEST_F(QuantraComparisonTest, CalendarAdvance_BackwardDays) {
    runAndCompare(quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-01-15", -3, quantra::enums::TimeUnit_Days, QuantLib::Days,
                  quantra::enums::BusinessDayConvention_Preceding, QuantLib::Preceding,
                  false);
}

// Month advance from a month-end date with end_of_month set, ModifiedFollowing.
TEST_F(QuantraComparisonTest, CalendarAdvance_MonthsEndOfMonth) {
    runAndCompare(quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-01-31", 1, quantra::enums::TimeUnit_Months, QuantLib::Months,
                  quantra::enums::BusinessDayConvention_ModifiedFollowing,
                  QuantLib::ModifiedFollowing, true);
}

// Year advance under the US government-bond calendar.
TEST_F(QuantraComparisonTest, CalendarAdvance_UsGovBond_Years) {
    runAndCompare(quantra::enums::Calendar_UnitedStatesGovernmentBond,
                  QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
                  "2025-07-04", 1, quantra::enums::TimeUnit_Years, QuantLib::Years,
                  quantra::enums::BusinessDayConvention_Following, QuantLib::Following,
                  false);
}

}} // namespace quantra::testing
