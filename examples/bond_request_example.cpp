#include <vector>

#include "quantra_client.h"
#include "data/fixed_rate_bond_request_quantra.h"

int main(int argc, char **argv)
{

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    int n = 1;

    if (argc > 1)
    {
        std::istringstream iss(argv[1]);
        iss >> n;
    }

    QuantraClient client("localhost:50051", false);

    std::shared_ptr<structs::PriceFixedRateBondRequest> bond_pricing_request = request_bond();

    std::vector<std::shared_ptr<structs::PriceFixedRateBondRequest>> requests;

    for (int i = 0; i < n; i++)
        requests.push_back(bond_pricing_request);

    // PriceFixedRateBondData request_data;
    // request_data.RequestCall(requests);
    client.PriceFixedRateBond(requests);

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[s]" << std::endl;

    return 0;
}