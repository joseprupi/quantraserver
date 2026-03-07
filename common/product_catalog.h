/**
 * @file product_catalog.h
 * @brief Shared product catalog and schema registry.
 *
 * QUANTRA_PRODUCT_LIST is the canonical source of truth for:
 * - ProductType enum values
 * - ProductTypeToString mapping
 * - Stable product keys used by tests/tooling
 * - HTTP route segments
 * - Request/response schema filenames
 */

#ifndef QUANTRA_PRODUCT_CATALOG_H
#define QUANTRA_PRODUCT_CATALOG_H

#include <map>
#include <string>
#include <vector>

namespace quantra {

struct ProductCatalogEntry {
    const char* product_key;
    const char* http_route;
    const char* request_file;
    const char* response_file;
};

#define QUANTRA_PRODUCT_LIST(X) \
    X(FixedRateBond, "fixed_rate_bond", "price-fixed-rate-bond", "price_fixed_rate_bond_request.fbs", "fixed_rate_bond_response.fbs") \
    X(FloatingRateBond, "floating_rate_bond", "price-floating-rate-bond", "price_floating_rate_bond_request.fbs", "floating_rate_bond_response.fbs") \
    X(VanillaSwap, "vanilla_swap", "price-vanilla-swap", "price_vanilla_swap_request.fbs", "vanilla_swap_response.fbs") \
    X(ZeroCouponInflationSwap, "zero_coupon_inflation_swap", "price-zero-coupon-inflation-swap", "price_zero_coupon_inflation_swap_request.fbs", "zero_coupon_inflation_swap_response.fbs") \
    X(YearOnYearInflationSwap, "year_on_year_inflation_swap", "price-year-on-year-inflation-swap", "price_year_on_year_inflation_swap_request.fbs", "year_on_year_inflation_swap_response.fbs") \
    X(OisSwap, "ois_swap", "price-ois-swap", "price_ois_swap_request.fbs", "ois_swap_response.fbs") \
    X(BasisSwap, "basis_swap", "price-basis-swap", "price_basis_swap_request.fbs", "basis_swap_response.fbs") \
    X(FRA, "fra", "price-fra", "price_fra_request.fbs", "fra_response.fbs") \
    X(CapFloor, "cap_floor", "price-cap-floor", "price_cap_floor_request.fbs", "cap_floor_response.fbs") \
    X(Swaption, "swaption", "price-swaption", "price_swaption_request.fbs", "swaption_response.fbs") \
    X(CDS, "cds", "price-cds", "price_cds_request.fbs", "cds_response.fbs") \
    X(BootstrapCurves, "bootstrap_curves", "bootstrap-curves", "bootstrap_curves_request.fbs", "bootstrap_curves_response.fbs") \
    X(BootstrapInflationCurves, "bootstrap_inflation_curves", "bootstrap-inflation-curves", "bootstrap_inflation_curves_request.fbs", "bootstrap_inflation_curves_response.fbs") \
    X(SampleVolSurfaces, "sample_vol_surfaces", "sample-vol-surfaces", "sample_vol_surfaces_request.fbs", "sample_vol_surfaces_response.fbs") \
    X(CalendarBusinessDays, "calendar_business_days", "calendar-business-days", "calendar_business_days_request.fbs", "calendar_business_days_response.fbs") \
    X(CalendarHolidays, "calendar_holidays", "calendar-holidays", "calendar_holidays_request.fbs", "calendar_holidays_response.fbs") \
    X(CalendarAdvance, "calendar_advance", "calendar-advance", "calendar_advance_request.fbs", "calendar_advance_response.fbs") \
    X(CalibrateSwaptionModel, "calibrate_swaption_model", "calibrate-swaption-model", "calibrate_swaption_model_request.fbs", "calibrate_swaption_model_response.fbs") \
    X(EquityOption, "equity_option", "price-equity-option", "price_equity_option_request.fbs", "equity_option_response.fbs")

enum class ProductType {
#define QUANTRA_ENUM_ENTRY(name, key, route, req, resp) name,
    QUANTRA_PRODUCT_LIST(QUANTRA_ENUM_ENTRY)
#undef QUANTRA_ENUM_ENTRY
};

inline const std::map<ProductType, ProductCatalogEntry>& GetProductCatalog() {
    static const std::map<ProductType, ProductCatalogEntry> catalog = {
#define QUANTRA_SCHEMA_ENTRY(name, key, route, req, resp) {ProductType::name, {key, route, req, resp}},
        QUANTRA_PRODUCT_LIST(QUANTRA_SCHEMA_ENTRY)
#undef QUANTRA_SCHEMA_ENTRY
    };
    return catalog;
}

inline const std::map<ProductType, ProductCatalogEntry>& GetProductSchemas() {
    return GetProductCatalog();
}

inline const char* ProductTypeToString(ProductType type) {
    switch (type) {
#define QUANTRA_NAME_CASE(name, key, route, req, resp) case ProductType::name: return #name;
        QUANTRA_PRODUCT_LIST(QUANTRA_NAME_CASE)
#undef QUANTRA_NAME_CASE
        default:                            return "Unknown";
    }
}

inline std::vector<std::string> GetProductEndpointList() {
    std::vector<std::string> endpoints;
    endpoints.reserve(GetProductCatalog().size());
    for (const auto& [_, entry] : GetProductCatalog()) {
        endpoints.emplace_back(std::string("POST /") + entry.http_route);
    }
    return endpoints;
}

} // namespace quantra

#endif // QUANTRA_PRODUCT_CATALOG_H
