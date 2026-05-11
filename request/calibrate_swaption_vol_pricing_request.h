#ifndef QUANTRA_CALIBRATE_SWAPTION_VOL_PRICING_REQUEST_H
#define QUANTRA_CALIBRATE_SWAPTION_VOL_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"
#include "quantra_request.h"

#include "calibrate_swaption_vol_request_generated.h"
#include "calibrate_swaption_vol_response_generated.h"

class CalibrateSwaptionVolPricingRequest : QuantraRequest<
    quantra::CalibrateSwaptionVolRequest,
    quantra::CalibrateSwaptionVolResponse> {
public:
    flatbuffers::Offset<quantra::CalibrateSwaptionVolResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::CalibrateSwaptionVolRequest* request) const;
};

#endif // QUANTRA_CALIBRATE_SWAPTION_VOL_PRICING_REQUEST_H
