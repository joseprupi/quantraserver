// Curve cache key: unknown Point union types must fail CLOSED.
//
// The cache key is a content hash over every helper point. If serializePoint
// hits a Point union type it does not recognize (a future schema addition
// whose author forgot the key builder), writing only the type byte under-keys
// the curve: two different curves hash to the same key and the cache serves
// wrong prices. The key builder must throw instead; the bootstrapper catches
// the throw and degrades by skipping the cache for that curve and its
// dependents (bootstrap live, no L1/L2 get or put, request succeeds).
//
// NOTE: this is a fail-closed behavior test, not a red->green bug
// reproduction — the Point union is currently fully covered, so the bad path
// is only reachable by hand-crafting an out-of-range type byte, as done here.
// Pre-fix, this scenario demonstrated the bug directly: computing keys for
// the two curves below (identical except for the rate inside the
// unknown-typed point) returned the IDENTICAL key
//   yc:v1:55f7073bdf41687c212b9ef1139c01db9529d74a89faf6e49186f017fd5d124f
// for both. Post-fix, compute() throws QuantraError for each.
//
// The call-site degradation in CurveBootstrapper::bootstrapAll is not
// exercised here: TermStructurePointParser rejects unknown point types
// before bootstrapping, so the live scenario (parser taught a new type, key
// builder forgotten) cannot be simulated without also extending the parser.
#include <gtest/gtest.h>

#include "curve_cache_key.h"
#include "error.h"

namespace quantra {
namespace testing {
namespace {

// Out-of-range union type byte simulating an unhandled schema addition.
constexpr uint8_t kUnknownPointType = 0xAA;

// Build a TermStructure whose single point carries an unrecognized union
// type byte. The payload is a real DepositHelper table so the wrapper is
// structurally valid; `rate` differentiates the point content.
const quantra::TermStructure* buildUnknownPointCurve(
    flatbuffers::FlatBufferBuilder& fbb, double rate)
{
    auto helper = quantra::CreateDepositHelper(fbb, rate);
    auto pw = quantra::CreatePointsWrapper(
        fbb, static_cast<quantra::Point>(kUnknownPointType), helper.Union());
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-unknown-point",
        quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount,
        &pts, "2026-06-11");
    fbb.Finish(ts);
    return flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());
}

TEST(CurveCacheKeyUnknownPoint, UnknownPointTypeFailsClosed) {
    flatbuffers::FlatBufferBuilder f1, f2;
    const auto* ts1 = buildUnknownPointCurve(f1, 0.01);
    const auto* ts2 = buildUnknownPointCurve(f2, 0.05);

    KeyContext ctx;
    EXPECT_THROW(CurveKeyBuilder::compute("2026-06-11", ts1, ctx, {}),
                 QuantraError);
    EXPECT_THROW(CurveKeyBuilder::compute("2026-06-11", ts2, ctx, {}),
                 QuantraError);
}

// Point_NONE carries no payload table, so the type byte alone IS the full
// content — it must keep producing a key, not throw.
TEST(CurveCacheKeyUnknownPoint, NonePointTypeStillKeyable) {
    flatbuffers::FlatBufferBuilder fbb;
    auto pw = quantra::CreatePointsWrapper(fbb, quantra::Point_NONE, 0);
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-none-point",
        quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount,
        &pts, "2026-06-11");
    fbb.Finish(ts);
    const auto* root =
        flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());

    KeyContext ctx;
    std::string key = CurveKeyBuilder::compute("2026-06-11", root, ctx, {});
    EXPECT_EQ(key.rfind("yc:v1:", 0), 0u);
}

} // namespace
} // namespace testing
} // namespace quantra
