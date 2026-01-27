#ifndef FRA_REQUEST_FBS_H
#define FRA_REQUEST_FBS_H

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/grpc.h"
#include "quantraserver.grpc.fb.h"
#include "price_fra_request_generated.h"

flatbuffers::grpc::Message<quantra::PriceFRARequest> fra_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder);

#endif // FRA_REQUEST_FBS_H
