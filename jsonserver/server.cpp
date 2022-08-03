#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>

#include "crow_all.h"

#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "flatbuffers/grpc.h"

#include "../client/cpp/quantra_client.h"

int main(int argc, char **argv)
{
    crow::App<> app;

    std::string connection;
    connection = argv[1];

    int port = atoi(argv[2]);
    int secured = atoi(argv[3]);

    QuantraClient *base_client;

    if (secured == 1)
    {
        base_client = new QuantraClient(connection, true);
    }
    else
    {
        base_client = new QuantraClient(connection, false);
    }

    CROW_ROUTE(app, "/")
        .name("hello")([]
                       { return "hello"; });

    CROW_ROUTE(app, "/price-fixed-rate-bond")
        .methods("POST"_method)([&](const crow::request &req)
                                {
                                    try
                                    {
                                        auto client = std::make_shared<QuantraClient>(base_client->ReturnStub());
                                        auto response = client->PriceFixedRateBondJSON(req.body);
                                        if (response->ok)
                                        {
                                            std::ostringstream os;
                                            os << *response->response_value;
                                            return crow::response{os.str()};
                                        }
                                        else
                                        {
                                            std::ostringstream os;
                                            os << *response->response_value;
                                            return crow::response{500, os.str()};
                                        }
                                    }
                                    catch (const std::exception &e)
                                    {
                                        return crow::response{500, e.what()};
                                    } });

    CROW_ROUTE(app, "/price-floating-rate-bond")
        .methods("POST"_method)([&](const crow::request &req)
                                {
                                    try
                                    {
                                        auto client = std::make_shared<QuantraClient>(base_client->ReturnStub());
                                        auto response = client->PriceFloatingRateBondJSON(req.body);
                                        if (response->ok)
                                        {
                                            std::ostringstream os;
                                            os << *response->response_value;
                                            return crow::response{os.str()};
                                        }
                                        else
                                        {
                                            std::ostringstream os;
                                            os << *response->response_value;
                                            return crow::response{500, os.str()};
                                        }
                                    }
                                    catch (const std::exception &e)
                                    {
                                        return crow::response{500, e.what()};
                                    } });

    app.loglevel(crow::LogLevel::DEBUG);

    app.port(port)
        .multithreaded()
        .run();
}