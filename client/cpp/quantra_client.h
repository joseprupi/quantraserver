#ifndef QUANTRA_CLIENT_H
#define QUANTRA_CLIENT_H

#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "quantra_call.h"
#include "quantra_to_fbs.h"
#include "json_to_fbs.h"
#include "fbs_to_quantra.h"
#include "quantraserver.grpc.fb.h"
#include "fixed_rate_bond_response_generated.h"
#include "floating_rate_bond_response_generated.h"

class PriceFixedRateBondData;
class PriceFloatingRateBondData;

class QuantraClient
{
public:
    explicit QuantraClient(){};
    explicit QuantraClient(std::string addr, bool secured);
    explicit QuantraClient(std::shared_ptr<quantra::QuantraServer::Stub> stub_);

    void init();

    std::shared_ptr<quantra::QuantraServer::Stub> ReturnStub() { return this->stub_; }

    std::shared_ptr<quantra::QuantraServer::Stub> stub_;

    std::shared_ptr<PriceFixedRateBondData> fixed_rate_bond;
    std::vector<std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFixedRateBondValues>>>> PriceFixedRateBond(std::vector<std::shared_ptr<structs::PriceFixedRateBondRequest>> request);
    std::shared_ptr<json_response> PriceFixedRateBondJSON(std::string json);

    std::shared_ptr<PriceFloatingRateBondData> floating_rate_bond;
    std::vector<std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFloatingRateBondValues>>>> PriceFloatingRateBond(std::vector<std::shared_ptr<structs::PriceFloatingRateBondRequest>> request);
    std::shared_ptr<json_response> PriceFloatingRateBondJSON(std::string json);
};

#endif