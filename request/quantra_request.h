#ifndef QUANTRASERVER_QUANTRAREQUEST_H
#define QUANTRASERVER_QUANTRAREQUEST_H

template <class Request, class Response>
class QuantraRequest
{
public:
    //virtual std::vector<std::shared_ptr<std::string>> verify(const Request *request) const = 0;
    virtual flatbuffers::Offset<Response> request(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
                                                  const Request *request) const = 0;
};

#endif //QUANTRASERVER_QUANTRAREQUEST_H