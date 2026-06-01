#ifndef QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_MAPPER_H
#define QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_MAPPER_H

/**
 * YearOnYearInflationSwapMapper — the one place YYIIS flatbuffers live.
 * Decodes the PriceYearOnYearInflationSwapRequest into plain
 * YearOnYearInflationSwapInputs and serializes the
 * YearOnYearInflationSwapResult back into a PriceYearOnYearInflationSwapResponse.
 */

#include "flatbuffers/grpc.h"

#include "price_year_on_year_inflation_swap_request_generated.h"
#include "year_on_year_inflation_swap_evaluator.h"
#include "year_on_year_inflation_swap_response_generated.h"

namespace quantra {

class YearOnYearInflationSwapMapper {
public:
    YearOnYearInflationSwapInputs toInputs(
        const quantra::PriceYearOnYearInflationSwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceYearOnYearInflationSwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const YearOnYearInflationSwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_MAPPER_H
