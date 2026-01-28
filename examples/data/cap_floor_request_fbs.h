#ifndef CAP_FLOOR_REQUEST_FBS_H
#define CAP_FLOOR_REQUEST_FBS_H

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/grpc.h"
#include "quantraserver.grpc.fb.h"
#include "price_cap_floor_request_generated.h"

flatbuffers::grpc::Message<quantra::PriceCapFloorRequest> cap_floor_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder);

#endif // CAP_FLOOR_REQUEST_FBS_H
