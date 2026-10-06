// Per-request calendar holiday overrides: core apply/reset, the RAII guard, the
// canonical fingerprint, every rejection / accepted no-op, the wire parser and
// the endpoint wiring. Built into the single test_quantra_vs_quantlib binary.
//
// Overrides are process-global QuantLib state, so every test runs inside a
// fixture that resets them before and after.
#include "parity_fixture.h"

#include <algorithm>
#include <functional>
#include <memory>

#include "calendar_override_parser.h"
#include "calendar_overrides.h"
#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra { namespace testing {

namespace {

class CalendarOverridesTest : public ::testing::Test {
protected:
    void SetUp() override { resetCalendarOverrides(); }
    void TearDown() override { resetCalendarOverrides(); }
};

// Reference dates on TARGET: 2024-06-14 is a plain Friday, 2024-05-01 is
// Labour Day (a Wednesday holiday), 2024-06-15 is a Saturday.
const char* kPlainFriday = "2024-06-14";
const char* kLabourDay = "2024-05-01";
const char* kSaturday = "2024-06-15";

HolidayOverride make(const QuantLib::Calendar& cal,
                     std::vector<std::string> added,
                     std::vector<std::string> removed) {
    HolidayOverride o;
    o.calendar = cal;
    for (const auto& d : added) o.added.push_back(DateToQL(d));
    for (const auto& d : removed) o.removed.push_back(DateToQL(d));
    return o;
}

void expectInvalid(const std::function<void()>& fn, const std::string& needle) {
    try {
        fn();
        FAIL() << "expected QuantraInvalidArgument containing '" << needle << "'";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
            << "message was: " << e.what();
    }
}

void expectTargetClean() {
    EXPECT_TRUE(QuantLib::TARGET().isBusinessDay(DateToQL(kPlainFriday)));
    EXPECT_TRUE(QuantLib::TARGET().isHoliday(DateToQL(kLabourDay)));
    EXPECT_TRUE(QuantLib::TARGET().addedHolidays().empty());
    EXPECT_TRUE(QuantLib::TARGET().removedHolidays().empty());
    EXPECT_EQ(activeCalendarOverridesFingerprint(), "");
}

struct Entry {
    flatbuffers::Optional<quantra::enums::Calendar> calendar;
    std::vector<std::string> added;
    std::vector<std::string> removed;
};

// Builds a CalendarHolidaysRequest carrying the given override entries.
flatbuffers::DetachedBuffer holidaysRequest(const std::vector<Entry>& entries,
                                            const std::string& start = "2024-04-29",
                                            const std::string& end = "2024-06-14") {
    quantra::CalendarHolidaysRequestT t;
    t.calendar = quantra::enums::Calendar_TARGET;
    t.start_date = start;
    t.end_date = end;
    for (const auto& e : entries) {
        auto o = std::make_unique<quantra::CalendarOverrideT>();
        o->calendar = e.calendar;
        o->added_holidays = e.added;
        o->removed_holidays = e.removed;
        t.calendar_overrides.push_back(std::move(o));
    }
    flatbuffers::FlatBufferBuilder b;
    b.Finish(quantra::CalendarHolidaysRequest::Pack(b, &t));
    return b.Release();
}

std::vector<HolidayOverride> parse(const std::vector<Entry>& entries) {
    auto buf = holidaysRequest(entries);
    const auto* req = flatbuffers::GetRoot<quantra::CalendarHolidaysRequest>(buf.data());
    return parseCalendarOverrides(req->calendar_overrides(), "pricing.calendar_overrides");
}

std::vector<std::string> runHolidays(const flatbuffers::DetachedBuffer& buf) {
    CalendarHolidaysEndpoint endpoint;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = endpoint.request(
        respB, flatbuffers::GetRoot<quantra::CalendarHolidaysRequest>(buf.data()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::CalendarHolidaysResponse>(
        respB->GetBufferPointer());
    std::vector<std::string> out;
    for (const auto* d : *r->dates()) out.push_back(d->str());
    return out;
}

} // namespace

TEST_F(CalendarOverridesTest, ApplyThenResetRoundTrip) {
    applyCalendarOverrides({make(QuantLib::TARGET(), {kPlainFriday}, {kLabourDay})});
    // Every instance of the calendar sees the override.
    EXPECT_TRUE(QuantLib::TARGET().isHoliday(DateToQL(kPlainFriday)));
    EXPECT_TRUE(QuantLib::TARGET().isBusinessDay(DateToQL(kLabourDay)));
    EXPECT_NE(activeCalendarOverridesFingerprint(), "");

    resetCalendarOverrides();
    expectTargetClean();
}

TEST_F(CalendarOverridesTest, ApplyReplacesPreviousOverrides) {
    applyCalendarOverrides({make(QuantLib::TARGET(), {kPlainFriday}, {})});
    applyCalendarOverrides({make(QuantLib::Japan(), {kPlainFriday}, {})});
    EXPECT_TRUE(QuantLib::TARGET().isBusinessDay(DateToQL(kPlainFriday)));
    EXPECT_TRUE(QuantLib::Japan().isHoliday(DateToQL(kPlainFriday)));
    resetCalendarOverrides();
    EXPECT_TRUE(QuantLib::Japan().isBusinessDay(DateToQL(kPlainFriday)));
}

TEST_F(CalendarOverridesTest, GuardResetsOnScopeExitAndOnEntry) {
    {
        CalendarOverridesGuard guard;
        applyCalendarOverrides({make(QuantLib::TARGET(), {kPlainFriday}, {kLabourDay})});
        EXPECT_TRUE(QuantLib::TARGET().isHoliday(DateToQL(kPlainFriday)));
    }
    expectTargetClean();

    // Entry reset: overrides left behind by someone else are cleared.
    applyCalendarOverrides({make(QuantLib::TARGET(), {kPlainFriday}, {})});
    {
        CalendarOverridesGuard guard;
        expectTargetClean();
    }
}

TEST_F(CalendarOverridesTest, GuardResetsOnException) {
    try {
        CalendarOverridesGuard guard;
        applyCalendarOverrides({make(QuantLib::TARGET(), {kPlainFriday}, {kLabourDay})});
        throw std::runtime_error("unrelated failure");
    } catch (const std::runtime_error&) {
    }
    expectTargetClean();
}

TEST_F(CalendarOverridesTest, FingerprintIsCanonicalAndOrderIndependent) {
    EXPECT_EQ(activeCalendarOverridesFingerprint(), "");
    applyCalendarOverrides({});
    EXPECT_EQ(activeCalendarOverridesFingerprint(), "");

    applyCalendarOverrides({
        make(QuantLib::TARGET(), {"2024-06-14", "2024-03-05"}, {kLabourDay}),
        make(QuantLib::Japan(), {"2024-06-14"}, {}),
    });
    const std::string first = activeCalendarOverridesFingerprint();
    EXPECT_EQ(first, "Japan[+2024-06-14];TARGET[+2024-03-05,-2024-05-01,+2024-06-14]");

    applyCalendarOverrides({
        make(QuantLib::Japan(), {"2024-06-14"}, {}),
        make(QuantLib::TARGET(), {"2024-03-05", "2024-06-14"}, {kLabourDay}),
    });
    EXPECT_EQ(activeCalendarOverridesFingerprint(), first);

    // Different dates, a different calendar and add-vs-remove all change it.
    applyCalendarOverrides({make(QuantLib::TARGET(), {"2024-06-13"}, {})});
    const std::string a = activeCalendarOverridesFingerprint();
    applyCalendarOverrides({make(QuantLib::Japan(), {"2024-06-13"}, {})});
    const std::string b = activeCalendarOverridesFingerprint();
    applyCalendarOverrides({make(QuantLib::TARGET(), {}, {"2024-06-13"})});
    const std::string c = activeCalendarOverridesFingerprint();
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, first);

    resetCalendarOverrides();
    EXPECT_EQ(activeCalendarOverridesFingerprint(), "");
}

TEST_F(CalendarOverridesTest, AcceptedNoOps) {
    // Already a holiday, a weekend "holiday", already a business day, an entry
    // without dates and a calendar nothing else uses: all accepted, no effect.
    ASSERT_NO_THROW(applyCalendarOverrides({
        make(QuantLib::TARGET(), {kLabourDay, kSaturday}, {kPlainFriday}),
        make(QuantLib::Japan(), {}, {}),
    }));
    EXPECT_TRUE(QuantLib::TARGET().isHoliday(DateToQL(kLabourDay)));
    EXPECT_TRUE(QuantLib::TARGET().isHoliday(DateToQL(kSaturday)));
    EXPECT_TRUE(QuantLib::TARGET().isBusinessDay(DateToQL(kPlainFriday)));
    EXPECT_TRUE(QuantLib::TARGET().addedHolidays().empty());
    EXPECT_TRUE(QuantLib::TARGET().removedHolidays().empty());
    resetCalendarOverrides();
    expectTargetClean();

    // An entry without dates alone leaves the fingerprint empty.
    applyCalendarOverrides({make(QuantLib::TARGET(), {}, {})});
    EXPECT_EQ(activeCalendarOverridesFingerprint(), "");
}

TEST_F(CalendarOverridesTest, AliasesShareOverrides) {
    applyCalendarOverrides(
        {make(CalendarToQL(quantra::enums::Calendar_UnitedStates), {kPlainFriday}, {})});
    EXPECT_TRUE(CalendarToQL(quantra::enums::Calendar_UnitedStatesSettlement)
                    .isHoliday(DateToQL(kPlainFriday)));
    // Other United States markets are distinct calendars.
    EXPECT_TRUE(CalendarToQL(quantra::enums::Calendar_UnitedStatesNYSE)
                    .isBusinessDay(DateToQL(kPlainFriday)));
}

TEST_F(CalendarOverridesTest, RejectionsNameTheField) {
    const std::string p = "pricing.calendar_overrides";
    // Same date in both lists of one entry.
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::TARGET(), {kPlainFriday}, {kPlainFriday})}, p); },
        "pricing.calendar_overrides[0].removed_holidays[0]");
    // Duplicate date inside one list.
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::TARGET(), {"2024-06-13", kPlainFriday, kPlainFriday}, {})}, p); },
        "pricing.calendar_overrides[0].added_holidays[2]");
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::TARGET(), {}, {kLabourDay, kLabourDay})}, p); },
        "pricing.calendar_overrides[0].removed_holidays[1]");
    // Same calendar in two entries, directly and through an enum alias.
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::TARGET(), {kPlainFriday}, {}),
         make(QuantLib::TARGET(), {"2024-06-13"}, {})}, p); },
        "pricing.calendar_overrides[1].calendar");
    expectInvalid([&] { applyCalendarOverrides(
        {make(CalendarToQL(quantra::enums::Calendar_UnitedStates), {kPlainFriday}, {}),
         make(CalendarToQL(quantra::enums::Calendar_UnitedStatesSettlement), {}, {})}, p); },
        "pricing.calendar_overrides[1].calendar");
    // Calendars with per-instance state.
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::NullCalendar(), {kPlainFriday}, {})}, p); },
        "pricing.calendar_overrides[0].calendar");
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::BespokeCalendar(), {kPlainFriday}, {})}, p); },
        "pricing.calendar_overrides[0].calendar");
    // Weekend date in removed_holidays.
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::TARGET(), {}, {kLabourDay, kSaturday})}, p); },
        "pricing.calendar_overrides[0].removed_holidays[1]");
    expectTargetClean();
}

TEST_F(CalendarOverridesTest, NothingAppliedWhenAnyEntryIsInvalid) {
    expectInvalid([&] { applyCalendarOverrides(
        {make(QuantLib::TARGET(), {kPlainFriday}, {kLabourDay}),
         make(QuantLib::Japan(), {}, {kSaturday})}); },
        "calendar_overrides[1].removed_holidays[0]");
    expectTargetClean();
}

// Proof of which wire calendars cannot be overridden: for every enum value,
// two independently built instances either share added holidays (overridable)
// or do not (inert), and calendarSupportsOverrides agrees.
TEST_F(CalendarOverridesTest, EveryEnumCalendarIsSharedOrRejected) {
    std::vector<std::string> inert;
    for (int v = quantra::enums::Calendar_MIN; v <= quantra::enums::Calendar_MAX; ++v) {
        const auto e = static_cast<quantra::enums::Calendar>(v);
        QuantLib::Calendar a = CalendarToQL(e);
        const QuantLib::Calendar b = CalendarToQL(e);
        QuantLib::Date d = DateToQL(kPlainFriday);
        while (!a.isBusinessDay(d)) ++d;
        a.addHoliday(d);
        const bool shared = b.isHoliday(d);
        a.resetAddedAndRemovedHolidays();
        EXPECT_EQ(calendarSupportsOverrides(b), shared)
            << quantra::enums::EnumNameCalendar(e);
        if (!shared) inert.push_back(quantra::enums::EnumNameCalendar(e));
    }
    EXPECT_EQ(inert, (std::vector<std::string>{"BespokeCalendar", "NullCalendar"}));
}

TEST_F(CalendarOverridesTest, ParserBuildsDomainOverrides) {
    const auto parsed = parse({
        {quantra::enums::Calendar_TARGET, {kPlainFriday}, {kLabourDay}},
        {quantra::enums::Calendar_Japan, {}, {}},
    });
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0].calendar.name(), QuantLib::TARGET().name());
    ASSERT_EQ(parsed[0].added.size(), 1u);
    EXPECT_EQ(parsed[0].added[0], DateToQL(kPlainFriday));
    ASSERT_EQ(parsed[0].removed.size(), 1u);
    EXPECT_EQ(parsed[0].removed[0], DateToQL(kLabourDay));
    EXPECT_TRUE(parsed[1].added.empty());
    EXPECT_TRUE(parsed[1].removed.empty());
    EXPECT_TRUE(parseCalendarOverrides(nullptr, "calendar_overrides").empty());
}

TEST_F(CalendarOverridesTest, ParserRejectionsNameTheField) {
    // Entry without calendar.
    expectInvalid([&] { parse({{quantra::enums::Calendar_TARGET, {}, {}},
                               {flatbuffers::nullopt, {kPlainFriday}, {}}}); },
        "pricing.calendar_overrides[1].calendar is required");
    // Unparseable and out-of-range dates.
    expectInvalid([&] { parse({{quantra::enums::Calendar_TARGET,
                                {kPlainFriday, "14/06/2024"}, {}}}); },
        "pricing.calendar_overrides[0].added_holidays[1]");
    expectInvalid([&] { parse({{quantra::enums::Calendar_TARGET, {}, {"2024-02-30"}}}); },
        "pricing.calendar_overrides[0].removed_holidays[0]");
    expectInvalid([&] { parse({{quantra::enums::Calendar_TARGET, {"1800-01-01"}, {}}}); },
        "pricing.calendar_overrides[0].added_holidays[0]");
}

TEST_F(CalendarOverridesTest, EndpointAppliesAndResets) {
    const auto plain = runHolidays(holidaysRequest({}));
    const auto with = runHolidays(holidaysRequest(
        {{quantra::enums::Calendar_TARGET, {kPlainFriday}, {kLabourDay}}}));
    auto has = [](const std::vector<std::string>& v, const std::string& d) {
        return std::find(v.begin(), v.end(), d) != v.end();
    };
    EXPECT_TRUE(has(plain, kLabourDay));
    EXPECT_FALSE(has(plain, kPlainFriday));
    EXPECT_FALSE(has(with, kLabourDay));
    EXPECT_TRUE(has(with, kPlainFriday));
    expectTargetClean();
    EXPECT_EQ(runHolidays(holidaysRequest({})), plain);
}

TEST_F(CalendarOverridesTest, EndpointResetsWhenRequestFailsLater) {
    // Valid overrides, then the request fails on an unrelated field.
    const auto bad = holidaysRequest(
        {{quantra::enums::Calendar_TARGET, {kPlainFriday}, {kLabourDay}}},
        "not-a-date");
    EXPECT_ANY_THROW(runHolidays(bad));
    expectTargetClean();
}

TEST_F(CalendarOverridesTest, EndpointRejectsMalformedOverrides) {
    expectInvalid([&] { runHolidays(holidaysRequest(
        {{quantra::enums::Calendar_NullCalendar, {kPlainFriday}, {}}})); },
        "calendar_overrides[0].calendar");
    expectTargetClean();
}

} } // namespace quantra::testing
