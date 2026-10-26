#ifndef QUANTRA_IMPLIED_FORWARDS_MAPPER_H
#define QUANTRA_IMPLIED_FORWARDS_MAPPER_H

/**
 * ImpliedForwardsMapper — the one place this product's flatbuffers live.
 * Decodes a CalibrateImpliedForwardsRequest into plain ImpliedForwardsInputs
 * (the quote lines with absent scalars kept absent, the filter with schema
 * defaults, the ids) and serializes an ImpliedForwardsResult back into a
 * CalibrateImpliedForwardsResponse, including the ready-to-paste
 * InterpolatedZero dividend TermStructure.
 */

#include "implied_forwards_evaluator.h"

#include "calibrate_implied_forwards_request_generated.h"
#include "calibrate_implied_forwards_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

/// An OptionQuoteSheet as decoded for any endpoint that takes one: ids,
/// style, the quote lines with absent scalars kept absent.
struct DecodedQuoteSheet {
    std::string sheetId;
    std::string underlyingId;
    OptionSheetStyle style = OptionSheetStyle::European;
    std::vector<OptionQuoteLine> quotes;
};

/// Decode a sheet; 400 naming `<path>.<field>` for a missing `id`,
/// `underlying_id`, an empty `quotes`, a missing `expiry` or an unknown
/// `option_style`. `path` is the sheet's own path ("sheet",
/// "diagnostics.vanilla_repricing_sheets[0]").
DecodedQuoteSheet decodeOptionQuoteSheet(
    const quantra::OptionQuoteSheet* sheet, const std::string& path);

/// Decode a QuoteFilter (schema defaults when absent).
QuoteFilterData decodeQuoteFilter(const quantra::QuoteFilter* filter);

class ImpliedForwardsMapper {
public:
    ImpliedForwardsInputs toInputs(const quantra::CalibrateImpliedForwardsRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateImpliedForwardsResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const ImpliedForwardsResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_IMPLIED_FORWARDS_MAPPER_H
