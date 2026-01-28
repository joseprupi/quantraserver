#ifndef SWAPTION_REQUEST_FBS_H
#define SWAPTION_REQUEST_FBS_H

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/grpc.h"
#include "quantraserver.grpc.fb.h"
#include "price_swaption_request_generated.h"

flatbuffers::grpc::Message<quantra::PriceSwaptionRequest> swaption_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder);

#endif // SWAPTION_REQUEST_FBS_H
