#ifndef QUANTRASERVER_QUANTRAREQUEST_H
#define QUANTRASERVER_QUANTRAREQUEST_H

#include "request_budget.h"

template <class Request, class Response>
class QuantraRequest
{
public:
    //virtual std::vector<std::shared_ptr<std::string>> verify(const Request *request) const = 0;
    // The budget bounds per-request CPU mid-computation; it is defaulted so 2-arg
    // callers (parity tests) compile unchanged and stay byte-identical.
    virtual flatbuffers::Offset<Response> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const Request *request,
        const quantra::RequestBudget &budget = quantra::RequestBudget::unlimited()) const = 0;
};

#endif //QUANTRASERVER_QUANTRAREQUEST_H