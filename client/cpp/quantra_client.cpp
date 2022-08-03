#include "quantra_client.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;

#include <string>
#include <fstream>
#include <streambuf>

QuantraClient::QuantraClient(std::string addr, bool secured) /* : fixed_rate_bond(new PriceFixedRateBondData(this)),*/
{
    grpc::ChannelArguments ch_args;
    ch_args.SetMaxReceiveMessageSize(INT_MAX);

    std::shared_ptr<grpc::ChannelCredentials> channel_creds;

    if (secured)
    {
        std::cout << "Secured" << std::endl;
        grpc::SslCredentialsOptions ssl_opts;

        char const *cert_path = getenv("QUANTRA_SERVER_CERT");
        if (cert_path == NULL)
        {
            throw std::runtime_error("QUANTRA_SERVER_CERT variable missing");
        }
        else
        {
            std::ifstream cert(cert_path);
            std::string cert_str((std::istreambuf_iterator<char>(cert)),
                                 std::istreambuf_iterator<char>());
            ssl_opts.pem_root_certs = cert_str;
        }

        channel_creds = grpc::SslCredentials(ssl_opts);
    }
    else
    {
        std::cout << "Not secured" << std::endl;
        channel_creds = grpc::InsecureChannelCredentials();
    }

    std::shared_ptr<grpc::Channel> channel = grpc::CreateCustomChannel(
        addr, channel_creds, ch_args);

    this->stub_ = std::make_shared<quantra::QuantraServer::Stub>(channel);
    this->init();
}

QuantraClient::QuantraClient(std::shared_ptr<quantra::QuantraServer::Stub> stub_)
{
    this->stub_ = stub_;
    this->init();
}

void QuantraClient::init()
{
    this->fixed_rate_bond = std::make_shared<PriceFixedRateBondData>(this->stub_);
    this->floating_rate_bond = std::make_shared<PriceFloatingRateBondData>(this->stub_);
}

std::vector<std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFixedRateBondValues>>>>
QuantraClient::PriceFixedRateBond(std::vector<std::shared_ptr<structs::PriceFixedRateBondRequest>> request)
{
    return this->fixed_rate_bond->RequestCall(request);
}
std::shared_ptr<json_response> QuantraClient::PriceFixedRateBondJSON(std::string json)
{
    return this->fixed_rate_bond->JSONRequest(json);
}

std::vector<std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFloatingRateBondValues>>>>
QuantraClient::PriceFloatingRateBond(std::vector<std::shared_ptr<structs::PriceFloatingRateBondRequest>> request)
{
    return this->floating_rate_bond->RequestCall(request);
}
std::shared_ptr<json_response> QuantraClient::PriceFloatingRateBondJSON(std::string json)
{
    return this->floating_rate_bond->JSONRequest(json);
}