#ifndef VANILLA_SWAP_REQUEST_FBS_H
#define VANILLA_SWAP_REQUEST_FBS_H

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/grpc.h"
#include "quantraserver.grpc.fb.h"
#include "price_vanilla_swap_request_generated.h"

flatbuffers::grpc::Message<quantra::PriceVanillaSwapRequest> swap_request_fbs(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder);

#endif // VANILLA_SWAP_REQUEST_FBS_H