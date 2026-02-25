#ifndef QUANTRA_CALIBRATE_SWAPTION_MODEL_PRICING_REQUEST_H
#define QUANTRA_CALIBRATE_SWAPTION_MODEL_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"
#include "quantra_request.h"

#include "calibrate_swaption_model_request_generated.h"
#include "calibrate_swaption_model_response_generated.h"

class CalibrateSwaptionModelPricingRequest : QuantraRequest<
    quantra::CalibrateSwaptionModelRequest,
    quantra::CalibrateSwaptionModelResponse> {
public:
    flatbuffers::Offset<quantra::CalibrateSwaptionModelResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::CalibrateSwaptionModelRequest* request) const;
};

#endif // QUANTRA_CALIBRATE_SWAPTION_MODEL_PRICING_REQUEST_H
