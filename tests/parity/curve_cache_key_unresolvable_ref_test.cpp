// Curve cache key: unresolvable references must fail CLOSED.
//
// The cache key is built BEFORE the request is parsed, so it must be at least
// as strict as the parser about references. Two resolutions used to fall back
// silently instead:
//
//  - a helper `quote_id` that did not resolve fell back to the helper's
//    inline value. The key stores only the resolved double (never the id), so
//    a request the parser rejects (unknown quote id -> NOT_FOUND) hashed to
//    the SAME key as a valid inline-value request — a warm cache would serve
//    a price where the no-cache path serves the error;
//
//  - a referenced index id with no definition was skipped entirely, leaving
//    the definition out of the key (under-keyed): two curves differing only
//    in that index's conventions or fixings would collide.
//
// Both must throw instead, matching the unknown-point-type default in
// serializePoint. CurveBootstrapper::bootstrapAll catches the throw and
// degrades by skipping the cache for that curve (no L1/L2 get or put,
// bootstrap live, request still proceeds) — the parser then reports the real
// error to the client.
#include <gtest/gtest.h>

#include "curve_cache_key.h"
#include "error.h"

namespace quantra {
namespace testing {
namespace {

// Curve with a single DepositHelper carrying the given inline rate and an
// optional quote_id reference.
const quantra::TermStructure* buildDepositCurve(
    flatbuffers::FlatBufferBuilder& fbb, double rate, const char* quoteId)
{
    auto helper = quantra::CreateDepositHelperDirect(
        fbb, rate, /*tenor=*/0, /*fixing_days=*/2,
        quantra::enums::Calendar_TARGET,
        quantra::enums::BusinessDayConvention_ModifiedFollowing,
        quantra::enums::DayCounter_Actual360, quoteId);
    auto pw = quantra::CreatePointsWrapper(
        fbb, quantra::Point_DepositHelper, helper.Union());
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-quote-ref",
        quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount,
        &pts, "2026-06-11");
    fbb.Finish(ts);
    return flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());
}

// Curve with a single SwapHelper whose float leg references the given index
// id.
const quantra::TermStructure* buildSwapCurve(
    flatbuffers::FlatBufferBuilder& fbb, const char* indexId)
{
    auto idx = quantra::CreateIndexRefDirect(fbb, indexId);
    auto helper = quantra::CreateSwapHelperDirect(
        fbb, /*rate=*/0.02, /*tenor=*/0,
        quantra::enums::Calendar_TARGET,
        quantra::enums::Frequency_Annual,
        quantra::enums::BusinessDayConvention_ModifiedFollowing,
        quantra::enums::DayCounter_Actual360,
        idx);
    auto pw = quantra::CreatePointsWrapper(
        fbb, quantra::Point_SwapHelper, helper.Union());
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-index-ref",
        quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount,
        &pts, "2026-06-11");
    fbb.Finish(ts);
    return flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());
}

// A quote_id the request does not define must throw (fail closed -> curve
// left uncached), never fall back to the inline value: pre-fix, both curves
// below produced the SAME key as an inline-only curve with that rate, so a
// warm cache could answer a request the parser rejects.
TEST(CurveCacheKeyUnresolvableRef, UnknownQuoteIdFailsClosed) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto* ts = buildDepositCurve(fbb, 0.0096, "no-such-quote");

    KeyContext ctx;  // empty: the id resolves to nothing
    EXPECT_THROW(CurveKeyBuilder::compute("2026-06-11", ts, ctx, {}),
                 QuantraError);
}

// Same shape, but the quote resolves — key building must succeed, and the
// key must track the RESOLVED value (two different quote values, identical
// inline values, must not collide).
TEST(CurveCacheKeyUnresolvableRef, ResolvedQuoteIdStillKeyable) {
    flatbuffers::FlatBufferBuilder f1, f2;
    const auto* ts1 = buildDepositCurve(f1, 0.0096, "depo-3m");
    const auto* ts2 = buildDepositCurve(f2, 0.0096, "depo-3m");

    KeyContext lo, hi;
    lo.quoteValues["depo-3m"] = 0.01;
    hi.quoteValues["depo-3m"] = 0.05;

    std::string k1 = CurveKeyBuilder::compute("2026-06-11", ts1, lo, {});
    std::string k2 = CurveKeyBuilder::compute("2026-06-11", ts2, hi, {});
    EXPECT_EQ(k1.rfind("yc:v3:", 0), 0u);
    EXPECT_EQ(k2.rfind("yc:v3:", 0), 0u);
    EXPECT_NE(k1, k2);
}

// No quote_id at all: the inline value is authoritative, exactly as before.
TEST(CurveCacheKeyUnresolvableRef, InlineValueWithoutQuoteIdStillKeyable) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto* ts = buildDepositCurve(fbb, 0.0096, nullptr);

    KeyContext ctx;
    std::string key = CurveKeyBuilder::compute("2026-06-11", ts, ctx, {});
    EXPECT_EQ(key.rfind("yc:v3:", 0), 0u);
}

// A referenced index with no definition must throw (fail closed -> curve
// left uncached), never be silently dropped from the key.
TEST(CurveCacheKeyUnresolvableRef, MissingIndexDefinitionFailsClosed) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto* ts = buildSwapCurve(fbb, "no-such-index");

    KeyContext ctx;  // empty: the index id resolves to nothing
    EXPECT_THROW(CurveKeyBuilder::compute("2026-06-11", ts, ctx, {}),
                 QuantraError);
}

// Same shape, but the index definition exists — key building must succeed.
TEST(CurveCacheKeyUnresolvableRef, DefinedIndexStillKeyable) {
    flatbuffers::FlatBufferBuilder tsFbb, idxFbb;
    const auto* ts = buildSwapCurve(tsFbb, "euribor-6m");

    auto def = quantra::CreateIndexDefDirect(idxFbb, "euribor-6m", "Euribor6M");
    idxFbb.Finish(def);
    const auto* defRoot =
        flatbuffers::GetRoot<quantra::IndexDef>(idxFbb.GetBufferPointer());

    KeyContext ctx;
    ctx.indexDefs["euribor-6m"] = defRoot;

    std::string key = CurveKeyBuilder::compute("2026-06-11", ts, ctx, {});
    EXPECT_EQ(key.rfind("yc:v3:", 0), 0u);
}

} // namespace
} // namespace testing
} // namespace quantra
