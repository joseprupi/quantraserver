#ifndef QUANTRA_EQUITY_OPTION_MAPPER_H
#define QUANTRA_EQUITY_OPTION_MAPPER_H

/**
 * EquityOptionMapper — the one place equity-option flatbuffers live. Decodes a
 * PriceEquityOptionRequest into plain EquityOptionInputs and serializes an
 * EquityOptionResult back into a PriceEquityOptionResponse.
 */

#include "flatbuffers/grpc.h"

#include "equity_option_pricer.h"
#include "equity_option_response_generated.h"
#include "price_equity_option_request_generated.h"

namespace quantra {

class EquityOptionMapper {
public:
    EquityOptionInputs toInputs(const quantra::PriceEquityOptionRequest* req) const;

    flatbuffers::Offset<quantra::PriceEquityOptionResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const EquityOptionResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_OPTION_MAPPER_H
