// Calendar holidays utility parity tests.
//
// New in refactor step 6e (fill zero/thin parity gaps). Query-shaped endpoint
// (no NPV): the expected holiday list is computed directly with QuantLib
// calendars and the engine response is required to match it exactly. Shares
// QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib binary.
#include "parity_fixture.h"

#include "common.h"

namespace quantra { namespace testing {

namespace {

// Independent reference: replicates the engine's [start, end] holiday scan —
// a date is a "holiday" if it is not a business day, with weekends filtered out
// unless include_weekends is set.
std::vector<QuantLib::Date> expectedHolidays(
    const QuantLib::Calendar& cal, const QuantLib::Date& start,
    const QuantLib::Date& end, bool includeWeekends) {
    std::vector<QuantLib::Date> out;
    for (QuantLib::Date d = start; d <= end; ++d) {
        if (cal.isBusinessDay(d)) continue;
        if (!includeWeekends && cal.isWeekend(d.weekday())) continue;
        out.push_back(d);
    }
    return out;
}

void runAndCompare(quantra::enums::Calendar calEnum,
                   const QuantLib::Calendar& cal,
                   const std::string& start, const std::string& end,
                   bool includeWeekends) {
    const auto expected = expectedHolidays(
        cal, DateToQL(start), DateToQL(end), includeWeekends);

    flatbuffers::grpc::MessageBuilder b;
    auto s = b.CreateString(start);
    auto e = b.CreateString(end);
    quantra::CalendarHolidaysRequestBuilder rb(b);
    rb.add_calendar(calEnum);
    rb.add_start_date(s);
    rb.add_end_date(e);
    rb.add_include_weekends(includeWeekends);
    b.Finish(rb.Finish());

    CalendarHolidaysEndpoint req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalendarHolidaysRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::CalendarHolidaysResponse>(respB->GetBufferPointer());

    ASSERT_NE(r->dates(), nullptr);
    EXPECT_EQ(r->calendar(), calEnum);
    ASSERT_EQ(r->count(), expected.size());
    ASSERT_EQ(r->dates()->size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(DateToQL(r->dates()->Get(static_cast<flatbuffers::uoffset_t>(i))->str()), expected[i])
            << "mismatch at index " << i;
    }
}

} // namespace

// TARGET holidays (no weekends) over Easter 2025: Good Friday (Apr 18) and
// Easter Monday (Apr 21).
TEST_F(QuantraComparisonTest, CalendarHolidays_Target_Easter_NoWeekends) {
    runAndCompare(quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-04-14", "2025-04-25", false);
}

// Same window including weekends: the result also lists the intervening
// Saturdays/Sundays.
TEST_F(QuantraComparisonTest, CalendarHolidays_Target_Easter_WithWeekends) {
    runAndCompare(quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-04-14", "2025-04-25", true);
}

// US government-bond holidays around Independence Day (2025-07-04), no weekends.
TEST_F(QuantraComparisonTest, CalendarHolidays_UsGovBond_July4) {
    runAndCompare(quantra::enums::Calendar_UnitedStatesGovernmentBond,
                  QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
                  "2025-06-28", "2025-07-07", false);
}

}} // namespace quantra::testing
