#include <iostream>
#include <string>
#include <fstream>

#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

#include "quantraserver.grpc.fb.h"
#include "quantraserver_generated.h"
#include "fixed_rate_bond_pricing_request.h"

flatbuffers::grpc::Message<quantra::PriceFixedRateBondRequest> bond_request_fbs(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder);