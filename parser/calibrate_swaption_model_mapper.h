#ifndef QUANTRA_CALIBRATE_SWAPTION_MODEL_MAPPER_H
#define QUANTRA_CALIBRATE_SWAPTION_MODEL_MAPPER_H

/**
 * CalibrateSwaptionModelMapper — the one place this product's flatbuffers live.
 * Decodes a CalibrateSwaptionModelRequest into plain CalibrateSwaptionModelInputs
 * (resolving the calibration source up-front: request override, else the
 * referenced model's hw_calibration block) and serializes a
 * CalibrateSwaptionModelResult back into a CalibrateSwaptionModelResponse.
 */

#include "flatbuffers/grpc.h"

#include "calibrate_swaption_model_pricer.h"
#include "calibrate_swaption_model_request_generated.h"
#include "calibrate_swaption_model_response_generated.h"

namespace quantra {

class CalibrateSwaptionModelMapper {
public:
    CalibrateSwaptionModelInputs toInputs(
        const quantra::CalibrateSwaptionModelRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateSwaptionModelResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CalibrateSwaptionModelResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALIBRATE_SWAPTION_MODEL_MAPPER_H
