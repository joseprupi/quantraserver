#ifndef JSON_TO_FBS_H
#define JSON_TO_FBS_H

#include <string>
#include <memory>

#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "flatbuffers/grpc.h"

#define FBS_INCLUDE_DIRECTORY "/workspace/flatbuffers/fbs"
#define FIXED_RATE_BOND_REQUEST_PATH "/workspace/flatbuffers/fbs/price_fixed_rate_bond_request.fbs"
#define FIXED_RATE_BOND_RESPONSE_PATH "/workspace/flatbuffers/fbs/fixed_rate_bond_response.fbs"
#define FLOATING_RATE_BOND_REQUEST_PATH "/workspace/flatbuffers/fbs/price_floating_rate_bond_request.fbs"
#define FLOATING_RATE_BOND_RESPONSE_PATH "/workspace/flatbuffers/fbs/floating_rate_bond_response.fbs"

struct json_response
{
    bool ok;
    std::shared_ptr<std::string> response_value;
};

class JSONParser
{
public:
    JSONParser();
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFixedRateBondRequestToFBS(std::string json_str);
    std::shared_ptr<std::string> PriceFixedRateBondResponseToJSON(const uint8_t *message);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFloatingRateBondRequestToFBS(std::string json_str);
    std::shared_ptr<std::string> PriceFloatingRateBondResponseToJSON(const uint8_t *message);

private:
    const char *fbs_include_directories[2] = {FBS_INCLUDE_DIRECTORY, nullptr};
    std::shared_ptr<flatbuffers::Parser> price_fixed_rate_bond_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> fixed_rate_bond_response_parser = std::make_shared<flatbuffers::Parser>();

    std::shared_ptr<flatbuffers::Parser> price_floating_rate_bond_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> floating_rate_bond_response_parser = std::make_shared<flatbuffers::Parser>();

    void LoadFBS();
};

#endif