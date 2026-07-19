// Crafted FlatBuffers union with the type byte set but the value offset absent.
//
// FlatBuffers' verifier accepts a union whose type byte names a real member but
// whose value offset is missing (the point:Point, point:InflationPoint and
// payload:ModelPayload fields are not `(required)`). Downstream code then
// dereferences the null table returned by the `*_as_*()` accessors and
// SIGSEGVs the worker. These are hand-crafted buffers the JSON gateway cannot
// produce, so they are exercised here directly at the C++ boundary: the code
// must throw a QuantraError (mapped to a clean 400), never crash.
#include <gtest/gtest.h>

#include "curve_bootstrapper.h"
#include "curve_cache_key.h"
#include "error.h"

namespace quantra {
namespace testing {
namespace {

// Build a TermStructure whose single point carries a real helper TYPE byte
// (DepositHelper) but a value offset of 0 (absent). Structurally this is what
// the FlatBuffers verifier lets through.
const quantra::TermStructure* buildTypeSetValueAbsentCurve(
    flatbuffers::FlatBufferBuilder& fbb)
{
    auto pw = quantra::CreatePointsWrapper(
        fbb, quantra::Point_DepositHelper, /*value offset absent*/ 0);
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-absent-union",
        quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount,
        &pts, "2026-06-11");
    fbb.Finish(ts);
    return flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());
}

// The cache-key builder walks every point; serializePoint used to cast the
// absent union value and crash. It must now throw instead.
TEST(UnionAbsentValue, CurveCacheKeyRejectsAbsentUnionValue) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto* ts = buildTypeSetValueAbsentCurve(fbb);

    KeyContext ctx;
    EXPECT_THROW(CurveKeyBuilder::compute("2026-06-11", ts, ctx, {}),
                 QuantraError);
}

// The dependency collector (hit before the cache key on the bootstrap path)
// static_casts the absent union value; it must throw instead of dereferencing
// a null table.
TEST(UnionAbsentValue, CurveBootstrapperRejectsAbsentUnionValue) {
    flatbuffers::FlatBufferBuilder fbb;
    // Use SwapHelper: collectDeps dereferences its ->deps() unconditionally.
    auto pw = quantra::CreatePointsWrapper(
        fbb, quantra::Point_SwapHelper, /*value offset absent*/ 0);
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-absent-swap",
        quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount,
        &pts, "2026-06-11");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{ts};
    auto curvesVec = fbb.CreateVector(curves);
    fbb.Finish(curvesVec);
    const auto* curvesPtr =
        flatbuffers::GetRoot<flatbuffers::Vector<
            flatbuffers::Offset<quantra::TermStructure>>>(fbb.GetBufferPointer());

    CurveBootstrapper bootstrapper;
    EXPECT_THROW(bootstrapper.bootstrapAll(curvesPtr), QuantraError);
}

} // namespace
} // namespace testing
} // namespace quantra
