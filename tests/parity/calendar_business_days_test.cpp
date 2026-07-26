// Calendar business-days utility parity tests.
//
// Query-shaped endpoint (no NPV): the expected business-day list is computed
// directly with QuantLib calendars and the engine response must match exactly (dates and
// counts are exact, not approximate). Shares QuantraComparisonTest from
// parity_fixture.h; built into the single test_quantra_vs_quantlib binary.
#include "parity_fixture.h"

#include "date_convert.h"

namespace quantra { namespace testing {

namespace {

// Independent reference: replicates the inclusive [start, end] business-day
// scan QuantLib performs, honouring the include_start/include_end toggles.
std::vector<QuantLib::Date> expectedBusinessDays(
    const QuantLib::Calendar& cal, const QuantLib::Date& start,
    const QuantLib::Date& end, bool includeStart, bool includeEnd) {
    std::vector<QuantLib::Date> out;
    for (QuantLib::Date d = start; d <= end; ++d) {
        if (!includeStart && d == start) continue;
        if (!includeEnd && d == end) continue;
        if (cal.isBusinessDay(d)) out.push_back(d);
    }
    return out;
}

void runAndCompare(QuantraComparisonTest& fixture,
                   quantra::enums::Calendar calEnum,
                   const QuantLib::Calendar& cal,
                   const std::string& start, const std::string& end,
                   bool includeStart, bool includeEnd) {
    (void)fixture;
    const auto expected = expectedBusinessDays(
        cal, DateToQL(start), DateToQL(end), includeStart, includeEnd);

    flatbuffers::grpc::MessageBuilder b;
    auto s = b.CreateString(start);
    auto e = b.CreateString(end);
    quantra::CalendarBusinessDaysRequestBuilder rb(b);
    rb.add_calendar(calEnum);
    rb.add_start_date(s);
    rb.add_end_date(e);
    rb.add_include_start(includeStart);
    rb.add_include_end(includeEnd);
    b.Finish(rb.Finish());

    CalendarBusinessDaysEndpoint req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalendarBusinessDaysRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::CalendarBusinessDaysResponse>(respB->GetBufferPointer());

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

// TARGET range spanning Good Friday (2025-04-18) and Labour Day (2025-05-01).
TEST_F(QuantraComparisonTest, CalendarBusinessDays_Target_WithHolidays) {
    runAndCompare(*this, quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-04-14", "2025-05-05", true, true);
}

// US government-bond calendar across US Independence Day (2025-07-04).
TEST_F(QuantraComparisonTest, CalendarBusinessDays_UsGovBond_July4) {
    runAndCompare(*this, quantra::enums::Calendar_UnitedStatesGovernmentBond,
                  QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
                  "2025-07-01", "2025-07-10", true, true);
}

// Boundary toggles: both endpoints are business days, excluded from the result.
TEST_F(QuantraComparisonTest, CalendarBusinessDays_ExcludeBoundaries) {
    runAndCompare(*this, quantra::enums::Calendar_TARGET, QuantLib::TARGET(),
                  "2025-01-20", "2025-01-24", false, false);
}

}} // namespace quantra::testing
