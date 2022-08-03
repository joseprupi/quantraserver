#include <iostream>
#include <string>

#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "flatbuffers/grpc.h"

#include "fixed_rate_bond_pricing_request.h"
#include "data/fixed_rate_bond_request_fbs.h"

std::string bond_to_json(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder)
{
    std::string schemafile;
    auto root = flatbuffers::GetRoot<PriceFixedRateBondRequest>(builder->GetBufferPointer());
    bool ok = flatbuffers::LoadFile("../flatbuffers/fbs/price_fixed_rate_bond_request.fbs", false, &schemafile);
    if (!ok)
    {
        printf("couldn't load files!\n");
        return "";
    }

    flatbuffers::Parser parser;
    parser.opts.strict_json = true;
    const char *include_directories[] = {"../flatbuffers/fbs", nullptr};

    ok = parser.Parse(schemafile.c_str(), include_directories);
    assert(ok);

    std::string jsongen;
    if (!GenerateText(parser, builder->GetBufferPointer(), &jsongen))
    {
        printf("Couldn't serialize parsed data to JSON!\n");
        return "";
    }

    return jsongen;
}

int main(int argc, char **argv)
{
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    bond_request_fbs(builder);

    auto json_string = bond_to_json(builder);

    std::cout << json_string << std::endl;

    return 0;
}