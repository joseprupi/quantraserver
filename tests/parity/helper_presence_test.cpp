// F4: presence-based field selection for BondHelper / FutureHelper.
//
// BondHelper used to encode "price vs rate" via a magic zero ("if price==0
// use rate") and FutureHelper via "if futures_price!=0, rate ignored". The
// schema now uses optional scalars (price:double = null) and the parser
// selects by presence. These tests pin the new contract:
//   - a BondHelper carrying the quote in `price` and one carrying the same
//     value in `rate` must hand QuantLib the identical quote;
//   - FutureHelper with futures_price present hands QuantLib the futures
//     PRICE as its quote (presence wins over rate), identical to an explicit
//     rate-only helper quoting the equivalent level via price = 100*(1-rate);
//   - a helper with neither field nor quote_id must fail with a clear
//     INVALID_ARGUMENT, not silently bootstrap from zero.
#include <gtest/gtest.h>

#include <ql/termstructures/yield/ratehelpers.hpp>

#include "term_structure_generated.h"
#include "term_structure_point_parser.h"
#include "error.h"

namespace quantra {
namespace testing {
namespace {

flatbuffers::Offset<quantra::Schedule> makeBondSchedule(
    flatbuffers::FlatBufferBuilder& fbb)
{
    auto eff = fbb.CreateString("2005/03/15");
    auto term = fbb.CreateString("2010/08/31");
    quantra::ScheduleBuilder sb(fbb);
    sb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_frequency(quantra::enums::Frequency_Semiannual);
    sb.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    return sb.Finish();
}

enum class BondQuoteSlot { Price, Rate, None };

const quantra::BondHelper* makeBondHelper(
    flatbuffers::FlatBufferBuilder& fbb, BondQuoteSlot slot, double value)
{
    auto schedule = makeBondSchedule(fbb);
    auto issue = fbb.CreateString("2005/03/15");
    quantra::BondHelperBuilder bb(fbb);
    if (slot == BondQuoteSlot::Price) bb.add_price(value);
    if (slot == BondQuoteSlot::Rate) bb.add_rate(value);
    bb.add_settlement_days(3);
    bb.add_face_amount(100.0);
    bb.add_schedule(schedule);
    bb.add_coupon_rate(0.02375);
    bb.add_day_counter(quantra::enums::DayCounter_ActualActualBond);
    bb.add_business_day_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    bb.add_redemption(100.0);
    bb.add_issue_date(issue);
    fbb.Finish(bb.Finish());
    return flatbuffers::GetRoot<quantra::BondHelper>(fbb.GetBufferPointer());
}

enum class FutureQuoteSlot { FuturesPrice, Rate, Both, None };

const quantra::FutureHelper* makeFutureHelper(
    flatbuffers::FlatBufferBuilder& fbb, FutureQuoteSlot slot,
    double price, double rate)
{
    auto start = fbb.CreateString("2026/09/16");
    quantra::FutureHelperBuilder fb(fbb);
    if (slot == FutureQuoteSlot::FuturesPrice || slot == FutureQuoteSlot::Both)
        fb.add_futures_price(price);
    if (slot == FutureQuoteSlot::Rate || slot == FutureQuoteSlot::Both)
        fb.add_rate(rate);
    fb.add_future_start_date(start);
    fb.add_future_months(3);
    fb.add_calendar(quantra::enums::Calendar_TARGET);
    fb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fb.add_day_counter(quantra::enums::DayCounter_Actual360);
    fbb.Finish(fb.Finish());
    return flatbuffers::GetRoot<quantra::FutureHelper>(fbb.GetBufferPointer());
}

// A BondHelper quoting 100.390625 in `price` and one quoting it in `rate`
// must produce RateHelpers carrying the identical quote value.
TEST(BondHelperPresence, PriceAndRateSlotsPriceIdentically) {
    const double quote = 100.390625;
    TermStructurePointParser parser;

    flatbuffers::FlatBufferBuilder f1, f2;
    auto viaPrice = parser.parse(
        quantra::Point_BondHelper, makeBondHelper(f1, BondQuoteSlot::Price, quote));
    auto viaRate = parser.parse(
        quantra::Point_BondHelper, makeBondHelper(f2, BondQuoteSlot::Rate, quote));

    ASSERT_NE(viaPrice, nullptr);
    ASSERT_NE(viaRate, nullptr);
    EXPECT_DOUBLE_EQ(viaPrice->quote()->value(), quote);
    EXPECT_DOUBLE_EQ(viaPrice->quote()->value(), viaRate->quote()->value());
}

// Neither price nor rate nor quote_id: must be a clear request error, not a
// silent zero-price helper.
TEST(BondHelperPresence, AbsentBothIsClearError) {
    TermStructurePointParser parser;
    flatbuffers::FlatBufferBuilder fbb;
    const auto* helper = makeBondHelper(fbb, BondQuoteSlot::None, 0.0);

    try {
        parser.parse(quantra::Point_BondHelper, helper);
        FAIL() << "expected QuantraError for BondHelper without price/rate/quote_id";
    } catch (const QuantraError& e) {
        EXPECT_NE(std::string(e.what()).find("price, rate or quote_id"),
                  std::string::npos)
            << "actual message: " << e.what();
    }
}

// futures_price present wins over rate: the helper's quote is the futures
// PRICE itself, the same as an explicit rate-only helper quoting the
// equivalent level via the identity price = 100*(1-rate).
TEST(FutureHelperPresence, FuturesPricePresenceWinsOverRate) {
    const double price = 95.25;
    const double equivalentRate = 1.0 - price / 100.0; // 100*(1-rate) == price
    TermStructurePointParser parser;

    flatbuffers::FlatBufferBuilder f1, f2;
    // rate also present but at a decoy level — presence of futures_price wins
    auto viaPrice = parser.parse(
        quantra::Point_FutureHelper,
        makeFutureHelper(f1, FutureQuoteSlot::Both, price, 0.99));
    auto viaRate = parser.parse(
        quantra::Point_FutureHelper,
        makeFutureHelper(f2, FutureQuoteSlot::Rate, 0.0, equivalentRate));

    ASSERT_NE(viaPrice, nullptr);
    ASSERT_NE(viaRate, nullptr);
    EXPECT_DOUBLE_EQ(viaPrice->quote()->value(), price);
    EXPECT_DOUBLE_EQ(viaPrice->quote()->value(), viaRate->quote()->value());
}

// An explicit futures_price of 0.0 is honored as a real price (quote 0.0)
// instead of being mistaken for "absent" — the sentinel is dead.
TEST(FutureHelperPresence, ExplicitZeroPriceIsAPrice) {
    TermStructurePointParser parser;
    flatbuffers::FlatBufferBuilder fbb;
    auto h = parser.parse(
        quantra::Point_FutureHelper,
        makeFutureHelper(fbb, FutureQuoteSlot::FuturesPrice, 0.0, 0.0));
    ASSERT_NE(h, nullptr);
    EXPECT_DOUBLE_EQ(h->quote()->value(), 0.0);
}

// Neither futures_price nor rate nor quote_id: clear request error.
TEST(FutureHelperPresence, AbsentBothIsClearError) {
    TermStructurePointParser parser;
    flatbuffers::FlatBufferBuilder fbb;
    const auto* helper = makeFutureHelper(fbb, FutureQuoteSlot::None, 0.0, 0.0);

    try {
        parser.parse(quantra::Point_FutureHelper, helper);
        FAIL() << "expected QuantraError for FutureHelper without futures_price/rate/quote_id";
    } catch (const QuantraError& e) {
        EXPECT_NE(std::string(e.what()).find("futures_price, rate or quote_id"),
                  std::string::npos)
            << "actual message: " << e.what();
    }
}

} // namespace
} // namespace testing
} // namespace quantra
