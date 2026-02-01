/**
 * @file product_registry.h
 * @brief Product definitions - ADD NEW PRODUCTS HERE
 * 
 * To add a new product:
 * 1. Add entry to PRODUCT_SCHEMAS below
 * 2. Add case to ProductTypeToString()
 * 3. Add JSON API method in quantra_client.cpp
 * 4. Add Native API method in quantra_client.cpp
 */

#ifndef QUANTRA_PRODUCT_REGISTRY_H
#define QUANTRA_PRODUCT_REGISTRY_H

#include "quantra_client.h"
#include <map>
#include <string>

namespace quantra {

// =============================================================================
// Schema Definitions - ADD NEW PRODUCTS HERE
// =============================================================================

struct ProductSchema {
    const char* request_file;
    const char* response_file;
};

// This is the ONLY place you need to add schema paths for new products
inline const std::map<ProductType, ProductSchema>& GetProductSchemas() {
    static const std::map<ProductType, ProductSchema> schemas = {
        {ProductType::FixedRateBond, {
            "price_fixed_rate_bond_request.fbs",
            "fixed_rate_bond_response.fbs"
        }},
        {ProductType::FloatingRateBond, {
            "price_floating_rate_bond_request.fbs",
            "floating_rate_bond_response.fbs"
        }},
        {ProductType::VanillaSwap, {
            "price_vanilla_swap_request.fbs",
            "vanilla_swap_response.fbs"
        }},
        {ProductType::FRA, {
            "price_fra_request.fbs",
            "fra_response.fbs"
        }},
        {ProductType::CapFloor, {
            "price_cap_floor_request.fbs",
            "cap_floor_response.fbs"
        }},
        {ProductType::Swaption, {
            "price_swaption_request.fbs",
            "swaption_response.fbs"
        }},
        {ProductType::CDS, {
            "price_cds_request.fbs",
            "cds_response.fbs"
        }}
        // ADD NEW PRODUCTS HERE:
        // {ProductType::ExoticOption, {
        //     "price_exotic_option_request.fbs",
        //     "exotic_option_response.fbs"
        // }},
    };
    return schemas;
}

// =============================================================================
// Product Type Utilities
// =============================================================================

inline const char* ProductTypeToString(ProductType type) {
    switch (type) {
        case ProductType::FixedRateBond:    return "FixedRateBond";
        case ProductType::FloatingRateBond: return "FloatingRateBond";
        case ProductType::VanillaSwap:      return "VanillaSwap";
        case ProductType::FRA:              return "FRA";
        case ProductType::CapFloor:         return "CapFloor";
        case ProductType::Swaption:         return "Swaption";
        case ProductType::CDS:              return "CDS";
        // ADD NEW PRODUCTS HERE:
        // case ProductType::ExoticOption:  return "ExoticOption";
        default:                            return "Unknown";
    }
}

} // namespace quantra

#endif // QUANTRA_PRODUCT_REGISTRY_H