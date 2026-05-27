#ifndef QUANTRA_CDS_MAPPER_H
#define QUANTRA_CDS_MAPPER_H

/**
 * CdsMapper — the one place CDS flatbuffers live. Decodes a PriceCDSRequest
 * into plain CdsInputs and serializes a CdsResult back into a PriceCDSResponse.
 */

#include "flatbuffers/grpc.h"

#include "cds_pricer.h"
#include "cds_response_generated.h"
#include "price_cds_request_generated.h"

namespace quantra {

class CdsMapper {
public:
    CdsInputs toInputs(const quantra::PriceCDSRequest* req) const;

    flatbuffers::Offset<quantra::PriceCDSResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CdsResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CDS_MAPPER_H
