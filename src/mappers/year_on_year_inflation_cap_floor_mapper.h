#ifndef QUANTRA_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_MAPPER_H
#define QUANTRA_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_MAPPER_H

/**
 * YearOnYearInflationCapFloorMapper — the one place YoY inflation cap/floor
 * flatbuffers live. Decodes a PriceYearOnYearInflationCapFloorRequest into
 * plain YoYInflationCapFloorInputs and serializes the YoYInflationCapFloorResult
 * back into a PriceYearOnYearInflationCapFloorResponse.
 */

#include "flatbuffers/grpc.h"

#include "price_year_on_year_inflation_cap_floor_request_generated.h"
#include "year_on_year_inflation_cap_floor_evaluator.h"
#include "year_on_year_inflation_cap_floor_response_generated.h"

namespace quantra {

class YearOnYearInflationCapFloorMapper {
public:
    YoYInflationCapFloorInputs toInputs(
        const quantra::PriceYearOnYearInflationCapFloorRequest* req) const;

    flatbuffers::Offset<quantra::PriceYearOnYearInflationCapFloorResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const YoYInflationCapFloorResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_MAPPER_H
