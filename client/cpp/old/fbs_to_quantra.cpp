#include "fbs_to_quantra.h"

std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFixedRateBondValues>>> price_fixed_rate_bond_response_to_quantra(const quantra::PriceFixedRateBondResponse *response)
{
    auto price = std::make_shared<std::vector<std::shared_ptr<structs::PriceFixedRateBondValues>>>();
    int size = response->bonds()->size();
    price.get()->reserve(response->bonds()->size());

    for (auto it = response->bonds()->begin(); it != response->bonds()->end(); it++)
    {
        auto npv_response = std::make_shared<structs::PriceFixedRateBondValues>();
        npv_response->npv = it->npv();
        price->push_back(npv_response);
    }

    return price;
}

std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFloatingRateBondValues>>> price_floating_rate_bond_response_to_quantra(const quantra::PriceFloatingRateBondResponse *response)
{
    auto price = std::make_shared<std::vector<std::shared_ptr<structs::PriceFloatingRateBondValues>>>();
    int size = response->bonds()->size();
    price.get()->reserve(response->bonds()->size());

    for (auto it = response->bonds()->begin(); it != response->bonds()->end(); it++)
    {
        auto npv_response = std::make_shared<structs::PriceFloatingRateBondValues>();
        npv_response->npv = it->npv();
        price->push_back(npv_response);
    }

    return price;
}