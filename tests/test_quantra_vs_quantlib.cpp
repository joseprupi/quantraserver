/**
 * Quantra vs QuantLib Comparison Tests
 * 
 * Tests that Quantra FlatBuffers serialization produces identical results to raw QuantLib.
 * Uses identical bootstrapped curves for exact matching.
 *
 * Updated for IndexDef/IndexRef redesign:
 *   - No more Ibor/OvernightIndex enums
 *   - SwapHelper uses IndexRef (float_index)
 *   - Instruments use IndexRef (index)
 *   - IndexDef objects registered in Pricing.indices
 */

#include <gtest/gtest.h>
#include <ql/quantlib.hpp>
#include <iostream>
#include <iomanip>

#include "fixed_rate_bond_handler.h"
#include "vanilla_swap_handler.h"
#include "fra_pricing_request.h"
#include "cap_floor_pricing_request.h"
#include "swaption_pricing_request.h"
#include "cds_handler.h"
#include "bootstrap_curves_request.h"
#include "sample_vol_surfaces_request.h"
#include "bootstrap_inflation_curves_request.h"
#include "zero_coupon_inflation_swap_handler.h"
#include "year_on_year_inflation_swap_handler.h"
#include "equity_option_pricing_request.h"
#include "vol_surface_parsers.h"

#include "price_fixed_rate_bond_request_generated.h"
#include "fixed_rate_bond_response_generated.h"
#include "price_vanilla_swap_request_generated.h"
#include "vanilla_swap_response_generated.h"
#include "price_fra_request_generated.h"
#include "fra_response_generated.h"
#include "price_cap_floor_request_generated.h"
#include "cap_floor_response_generated.h"
#include "price_swaption_request_generated.h"
#include "swaption_response_generated.h"
#include "calibrate_swaption_model_request_generated.h"
#include "calibrate_swaption_model_response_generated.h"
#include "ois_swap_generated.h"
#include "price_cds_request_generated.h"
#include "cds_response_generated.h"
#include "volatility_generated.h"
#include "model_generated.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"
#include "price_zero_coupon_inflation_swap_request_generated.h"
#include "zero_coupon_inflation_swap_response_generated.h"
#include "price_year_on_year_inflation_swap_request_generated.h"
#include "year_on_year_inflation_swap_response_generated.h"
#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"
#include "price_equity_option_request_generated.h"
#include "equity_option_response_generated.h"
#include "inflation_generated.h"
#include "inflation_curve_query_generated.h"
#include "vol_query_generated.h"
#include "index_generated.h"
#include "swap_index_generated.h"
#include "quotes_generated.h"
#include "calibrate_swaption_model_pricing_request.h"
#include "swaption_model_calibration.h"
#include "pricing_registry.h"
#include "cms_leg_parser.h"
#include "calibrate_swaption_vol_pricing_request.h"
#include "calibrate_swaption_vol_request_generated.h"
#include "calibrate_swaption_vol_response_generated.h"
#include "diagnostics_generated.h"
#include "sabr_calibrate_cache.h"
#include "sabr_calibrate_cache_key.h"
#include "swaption_vol_runtime.h"

namespace quantra { namespace testing {

class QuantraComparisonTest : public ::testing::Test {
protected:
    void SetUp() override {
        evaluationDate_ = QuantLib::Date(15, QuantLib::January, 2025);
        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        flatRate_ = 0.03;
        dividendFlatRate_ = 0.01;
        buildBootstrappedCurve();
        dividendCurve_ = std::make_shared<QuantLib::FlatForward>(
            evaluationDate_, dividendFlatRate_, QuantLib::Actual365Fixed());
        dividendHandle_ = QuantLib::Handle<QuantLib::YieldTermStructure>(dividendCurve_);
    }
    
    void buildBootstrappedCurve() {
        std::vector<std::shared_ptr<QuantLib::RateHelper>> instruments;
        
        instruments.push_back(std::make_shared<QuantLib::DepositRateHelper>(
            flatRate_, 3 * QuantLib::Months, 2, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, true, QuantLib::Actual365Fixed()));
        
        instruments.push_back(std::make_shared<QuantLib::DepositRateHelper>(
            flatRate_, 6 * QuantLib::Months, 2, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, true, QuantLib::Actual365Fixed()));
        
        instruments.push_back(std::make_shared<QuantLib::DepositRateHelper>(
            flatRate_, 1 * QuantLib::Years, 2, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, true, QuantLib::Actual365Fixed()));
        
        auto euribor6m = std::make_shared<QuantLib::Euribor6M>();
        instruments.push_back(std::make_shared<QuantLib::SwapRateHelper>(
            flatRate_, 5 * QuantLib::Years, QuantLib::TARGET(), QuantLib::Annual,
            QuantLib::ModifiedFollowing, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), euribor6m));
        
        instruments.push_back(std::make_shared<QuantLib::SwapRateHelper>(
            flatRate_, 10 * QuantLib::Years, QuantLib::TARGET(), QuantLib::Annual,
            QuantLib::ModifiedFollowing, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), euribor6m));
        
        bootstrappedCurve_ = std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
            evaluationDate_, instruments, QuantLib::Actual365Fixed());
        
        discountHandle_ = QuantLib::Handle<QuantLib::YieldTermStructure>(bootstrappedCurve_);
        forwardHandle_ = discountHandle_;
    }

    // =========================================================================
    // IndexDef builders — define the index once, reference by id everywhere
    // =========================================================================

    /// Build an IndexDef for EUR 3M (Euribor-like conventions)
    flatbuffers::Offset<quantra::IndexDef> buildIndexDef_EUR3M(
        flatbuffers::grpc::MessageBuilder& b) {
        auto id = b.CreateString("EUR_3M");
        auto name = b.CreateString("Euribor");
        auto ccy = b.CreateString("EUR");
        auto tenor = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Ibor);
        idb.add_tenor(tenor);
        idb.add_fixing_days(2);
        idb.add_calendar(quantra::enums::Calendar_TARGET);
        idb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        idb.add_day_counter(quantra::enums::DayCounter_Actual360);
        idb.add_end_of_month(false);
        idb.add_currency(ccy);
        return idb.Finish();
    }

    /// Build an IndexDef for EUR 6M (Euribor-like conventions)
    flatbuffers::Offset<quantra::IndexDef> buildIndexDef_EUR6M(
        flatbuffers::grpc::MessageBuilder& b) {
        auto id = b.CreateString("EUR_6M");
        auto name = b.CreateString("Euribor");
        auto ccy = b.CreateString("EUR");
        auto tenor = buildPeriod(b, 6, quantra::enums::TimeUnit_Months);
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Ibor);
        idb.add_tenor(tenor);
        idb.add_fixing_days(2);
        idb.add_calendar(quantra::enums::Calendar_TARGET);
        idb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        idb.add_day_counter(quantra::enums::DayCounter_Actual360);
        idb.add_end_of_month(false);
        idb.add_currency(ccy);
        return idb.Finish();
    }

    /// Build an IndexDef for USD SOFR (overnight conventions)
    flatbuffers::Offset<quantra::IndexDef> buildIndexDef_USD_SOFR(
        flatbuffers::grpc::MessageBuilder& b) {
        auto id = b.CreateString("USD_SOFR");
        auto name = b.CreateString("SOFR");
        auto ccy = b.CreateString("USD");
        auto tenor = buildPeriod(b, 0, quantra::enums::TimeUnit_Days);
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Overnight);
        idb.add_tenor(tenor);
        idb.add_fixing_days(0);
        idb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
        idb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
        idb.add_day_counter(quantra::enums::DayCounter_Actual360);
        idb.add_currency(ccy);
        return idb.Finish();
    }

    // =========================================================================
    // IndexRef builders — lightweight reference by id
    // =========================================================================

    flatbuffers::Offset<quantra::IndexRef> buildIndexRef(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id) {
        auto sid = b.CreateString(id);
        quantra::IndexRefBuilder irb(b);
        irb.add_id(sid);
        return irb.Finish();
    }

    quantra::enums::TimeUnit toFbTimeUnit(QuantLib::TimeUnit u) {
        switch (u) {
            case QuantLib::Days: return quantra::enums::TimeUnit_Days;
            case QuantLib::Weeks: return quantra::enums::TimeUnit_Weeks;
            case QuantLib::Months: return quantra::enums::TimeUnit_Months;
            case QuantLib::Years: return quantra::enums::TimeUnit_Years;
            default: return quantra::enums::TimeUnit_Days;
        }
    }

    flatbuffers::Offset<quantra::Period> buildPeriod(
        flatbuffers::grpc::MessageBuilder& b, int n, quantra::enums::TimeUnit unit) {
        quantra::PeriodBuilder pb(b);
        pb.add_n(n);
        pb.add_unit(unit);
        return pb.Finish();
    }

    // =========================================================================
    // Yield curve builder for Quantra (with IndexRef for SwapHelpers)
    // =========================================================================

    flatbuffers::Offset<quantra::TermStructure> buildCurve(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        double flatRate = std::numeric_limits<double>::quiet_NaN()) {
        const double curveRate = std::isfinite(flatRate) ? flatRate : flatRate_;
        
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;
        
        // 3M deposit
        auto dep3mTenor = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);
        quantra::DepositHelperBuilder dep3m(b);
        dep3m.add_rate(curveRate);
        dep3m.add_tenor(dep3mTenor);
        dep3m.add_fixing_days(2);
        dep3m.add_calendar(quantra::enums::Calendar_TARGET);
        dep3m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep3m.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep3m_off = dep3m.Finish();
        quantra::PointsWrapperBuilder pw3m(b);
        pw3m.add_point_type(quantra::Point_DepositHelper);
        pw3m.add_point(dep3m_off.Union());
        points_vector.push_back(pw3m.Finish());
        
        // 6M deposit
        auto dep6mTenor = buildPeriod(b, 6, quantra::enums::TimeUnit_Months);
        quantra::DepositHelperBuilder dep6m(b);
        dep6m.add_rate(curveRate);
        dep6m.add_tenor(dep6mTenor);
        dep6m.add_fixing_days(2);
        dep6m.add_calendar(quantra::enums::Calendar_TARGET);
        dep6m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep6m.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep6m_off = dep6m.Finish();
        quantra::PointsWrapperBuilder pw6m(b);
        pw6m.add_point_type(quantra::Point_DepositHelper);
        pw6m.add_point(dep6m_off.Union());
        points_vector.push_back(pw6m.Finish());
        
        // 1Y deposit
        auto dep1yTenor = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
        quantra::DepositHelperBuilder dep1y(b);
        dep1y.add_rate(curveRate);
        dep1y.add_tenor(dep1yTenor);
        dep1y.add_fixing_days(2);
        dep1y.add_calendar(quantra::enums::Calendar_TARGET);
        dep1y.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep1y.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep1y_off = dep1y.Finish();
        quantra::PointsWrapperBuilder pw1y(b);
        pw1y.add_point_type(quantra::Point_DepositHelper);
        pw1y.add_point(dep1y_off.Union());
        points_vector.push_back(pw1y.Finish());
        
        // 5Y swap — uses IndexRef instead of Ibor enum
        auto float_idx_5y = buildIndexRef(b, "EUR_6M");
        auto sw5yTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
        quantra::SwapHelperBuilder sw5y(b);
        sw5y.add_rate(curveRate);
        sw5y.add_tenor(sw5yTenor);
        sw5y.add_calendar(quantra::enums::Calendar_TARGET);
        sw5y.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        sw5y.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sw5y.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        sw5y.add_float_index(float_idx_5y);
        sw5y.add_spread(0.0);
        sw5y.add_fwd_start_days(0);
        auto sw5y_off = sw5y.Finish();
        quantra::PointsWrapperBuilder pw5y(b);
        pw5y.add_point_type(quantra::Point_SwapHelper);
        pw5y.add_point(sw5y_off.Union());
        points_vector.push_back(pw5y.Finish());
        
        // 10Y swap
        auto float_idx_10y = buildIndexRef(b, "EUR_6M");
        auto sw10yTenor = buildPeriod(b, 10, quantra::enums::TimeUnit_Years);
        quantra::SwapHelperBuilder sw10y(b);
        sw10y.add_rate(curveRate);
        sw10y.add_tenor(sw10yTenor);
        sw10y.add_calendar(quantra::enums::Calendar_TARGET);
        sw10y.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        sw10y.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sw10y.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        sw10y.add_float_index(float_idx_10y);
        sw10y.add_spread(0.0);
        sw10y.add_fwd_start_days(0);
        auto sw10y_off = sw10y.Finish();
        quantra::PointsWrapperBuilder pw10y(b);
        pw10y.add_point_type(quantra::Point_SwapHelper);
        pw10y.add_point(sw10y_off.Union());
        points_vector.push_back(pw10y.Finish());
        
        auto points = b.CreateVector(points_vector);
        auto cid = b.CreateString(id);
        auto ref_date = b.CreateString("2025-01-15");
        
        quantra::TermStructureBuilder tsb(b);
        tsb.add_id(cid);
        tsb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        tsb.add_interpolator(quantra::enums::Interpolator_LogLinear);
        tsb.add_bootstrap_trait(quantra::enums::BootstrapTrait_Discount);
        tsb.add_reference_date(ref_date);
        tsb.add_points(points);
        return tsb.Finish();
    }

    // Like buildCurve(), but extends past 12Y so SABR (2Y exp, 10Y tenor)
    // forward swaps fit comfortably inside the curve range.
    flatbuffers::Offset<quantra::TermStructure> buildLongCurve(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        double flatRate = std::numeric_limits<double>::quiet_NaN()) {
        const double curveRate = std::isfinite(flatRate) ? flatRate : flatRate_;
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;

        auto addDeposit = [&](int n, quantra::enums::TimeUnit unit) {
            auto tenor = buildPeriod(b, n, unit);
            quantra::DepositHelperBuilder dep(b);
            dep.add_rate(curveRate);
            dep.add_tenor(tenor);
            dep.add_fixing_days(2);
            dep.add_calendar(quantra::enums::Calendar_TARGET);
            dep.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            dep.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
            auto off = dep.Finish();
            quantra::PointsWrapperBuilder pw(b);
            pw.add_point_type(quantra::Point_DepositHelper);
            pw.add_point(off.Union());
            points_vector.push_back(pw.Finish());
        };
        auto addSwap = [&](int years) {
            auto floatIdx = buildIndexRef(b, "EUR_6M");
            auto tenor = buildPeriod(b, years, quantra::enums::TimeUnit_Years);
            quantra::SwapHelperBuilder sw(b);
            sw.add_rate(curveRate);
            sw.add_tenor(tenor);
            sw.add_calendar(quantra::enums::Calendar_TARGET);
            sw.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
            sw.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            sw.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
            sw.add_float_index(floatIdx);
            sw.add_spread(0.0);
            sw.add_fwd_start_days(0);
            auto off = sw.Finish();
            quantra::PointsWrapperBuilder pw(b);
            pw.add_point_type(quantra::Point_SwapHelper);
            pw.add_point(off.Union());
            points_vector.push_back(pw.Finish());
        };

        addDeposit(3, quantra::enums::TimeUnit_Months);
        addDeposit(6, quantra::enums::TimeUnit_Months);
        addDeposit(1, quantra::enums::TimeUnit_Years);
        addSwap(2);
        addSwap(5);
        addSwap(10);
        addSwap(15);
        addSwap(20);
        addSwap(30);

        auto points = b.CreateVector(points_vector);
        auto cid = b.CreateString(id);
        auto ref_date = b.CreateString("2025-01-15");

        quantra::TermStructureBuilder tsb(b);
        tsb.add_id(cid);
        tsb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        tsb.add_interpolator(quantra::enums::Interpolator_LogLinear);
        tsb.add_bootstrap_trait(quantra::enums::BootstrapTrait_Discount);
        tsb.add_reference_date(ref_date);
        tsb.add_points(points);
        return tsb.Finish();
    }

    struct OisTenorRate {
        int tenor_number;
        quantra::enums::TimeUnit tenor_unit;
        double rate;
    };

    // Build OIS curve using OISHelpers (overnight index)
    flatbuffers::Offset<quantra::TermStructure> buildOisCurve(
        flatbuffers::grpc::MessageBuilder& b,
        const std::string& id,
        const std::string& overnightIndexId,
        const std::vector<OisTenorRate>& rates,
        quantra::enums::Calendar calendar) {
        
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;
        auto idxRef = buildIndexRef(b, overnightIndexId);

        for (const auto& tr : rates) {
            auto oisTenor = buildPeriod(b, tr.tenor_number, tr.tenor_unit);
            quantra::OISHelperBuilder ois(b);
            ois.add_rate(tr.rate);
            ois.add_tenor(oisTenor);
            ois.add_overnight_index(idxRef);
            ois.add_settlement_days(2);
            ois.add_calendar(calendar);
            ois.add_fixed_leg_frequency(quantra::enums::Frequency_Annual);
            ois.add_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            ois.add_fixed_leg_day_counter(quantra::enums::DayCounter_Actual360);
            auto ois_off = ois.Finish();

            quantra::PointsWrapperBuilder pw(b);
            pw.add_point_type(quantra::Point_OISHelper);
            pw.add_point(ois_off.Union());
            points_vector.push_back(pw.Finish());
        }

        auto points = b.CreateVector(points_vector);
        auto cid = b.CreateString(id);
        auto ref_date = b.CreateString("2024-08-14");

        quantra::TermStructureBuilder tsb(b);
        tsb.add_id(cid);
        tsb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        tsb.add_interpolator(quantra::enums::Interpolator_LogLinear);
        tsb.add_bootstrap_trait(quantra::enums::BootstrapTrait_Discount);
        tsb.add_reference_date(ref_date);
        tsb.add_points(points);
        return tsb.Finish();
    }
    
    /// Build indices vector for Pricing-based requests
    /// Contains all IndexDefs needed by the curve helpers and instruments
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>>
    buildIndicesVector(flatbuffers::grpc::MessageBuilder& b, bool include3M = false) {
        std::vector<flatbuffers::Offset<quantra::IndexDef>> defs;
        defs.push_back(buildIndexDef_EUR6M(b));
        if (include3M) defs.push_back(buildIndexDef_EUR3M(b));
        return b.CreateVector(defs);
    }

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapIndexDef>>>
    buildSwapIndicesVector(
        flatbuffers::grpc::MessageBuilder& b,
        bool includeEur6m = true,
        bool includeOis = false,
        bool includeEur3m = false) {
        std::vector<flatbuffers::Offset<quantra::SwapIndexDef>> defs;

        quantra::SwapIndexFixedLegSpecBuilder fx(b);
        fx.add_fixed_frequency(quantra::enums::Frequency_Annual);
        fx.add_fixed_day_counter(quantra::enums::DayCounter_Thirty360);
        fx.add_fixed_calendar(quantra::enums::Calendar_TARGET);
        fx.add_fixed_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fx.add_fixed_term_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fx.add_fixed_date_rule(quantra::enums::DateGenerationRule_Forward);
        fx.add_fixed_eom(false);
        auto fixedLeg = fx.Finish();

        auto flTenor = buildPeriod(b, 6, quantra::enums::TimeUnit_Months);
        quantra::SwapIndexFloatLegSpecBuilder fl(b);
        fl.add_float_tenor(flTenor);
        fl.add_float_calendar(quantra::enums::Calendar_TARGET);
        fl.add_float_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fl.add_float_term_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fl.add_float_date_rule(quantra::enums::DateGenerationRule_Forward);
        fl.add_float_eom(false);
        auto floatLeg = fl.Finish();

        if (includeEur6m) {
            auto eurSwap6mId = b.CreateString("EUR_SWAP_6M");
            auto eur6mFloatId = b.CreateString("EUR_6M");
            quantra::SwapIndexDefBuilder sw(b);
            sw.add_id(eurSwap6mId);
            sw.add_kind(quantra::SwapIndexKind_IborSwapIndex);
            sw.add_spot_days(2);
            sw.add_calendar(quantra::enums::Calendar_TARGET);
            sw.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            sw.add_end_of_month(false);
            sw.add_fixed_leg(fixedLeg);
            sw.add_float_index_id(eur6mFloatId);
            sw.add_float_leg(floatLeg);
            defs.push_back(sw.Finish());
        }

        if (includeEur3m) {
            auto eurSwap3mId = b.CreateString("EUR_SWAP_3M");
            auto eur3mFloatId = b.CreateString("EUR_3M");
            quantra::SwapIndexDefBuilder sw3m(b);
            sw3m.add_id(eurSwap3mId);
            sw3m.add_kind(quantra::SwapIndexKind_IborSwapIndex);
            sw3m.add_spot_days(2);
            sw3m.add_calendar(quantra::enums::Calendar_TARGET);
            sw3m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            sw3m.add_end_of_month(false);
            sw3m.add_fixed_leg(fixedLeg);
            sw3m.add_float_index_id(eur3mFloatId);
            sw3m.add_float_leg(floatLeg);
            defs.push_back(sw3m.Finish());
        }

        if (includeOis) {
            quantra::SwapIndexFixedLegSpecBuilder fxo(b);
            fxo.add_fixed_frequency(quantra::enums::Frequency_Annual);
            fxo.add_fixed_day_counter(quantra::enums::DayCounter_Actual360);
            fxo.add_fixed_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
            fxo.add_fixed_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            fxo.add_fixed_term_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            fxo.add_fixed_date_rule(quantra::enums::DateGenerationRule_Forward);
            fxo.add_fixed_eom(false);
            auto fixedOis = fxo.Finish();

            auto floTenor = buildPeriod(b, 1, quantra::enums::TimeUnit_Days);
            quantra::SwapIndexFloatLegSpecBuilder flo(b);
            flo.add_float_tenor(floTenor);
            flo.add_float_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
            flo.add_float_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            flo.add_float_term_bdc(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            flo.add_float_date_rule(quantra::enums::DateGenerationRule_Forward);
            flo.add_float_eom(false);
            auto floatOis = flo.Finish();

            auto usdOisSwapId = b.CreateString("USD_SOFR_OIS");
            auto usdSofrFloatId = b.CreateString("USD_SOFR");
            quantra::SwapIndexDefBuilder swo(b);
            swo.add_id(usdOisSwapId);
            swo.add_kind(quantra::SwapIndexKind_OisSwapIndex);
            swo.add_spot_days(1);
            swo.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
            swo.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            swo.add_end_of_month(false);
            swo.add_fixed_leg(fixedOis);
            swo.add_float_index_id(usdSofrFloatId);
            swo.add_float_leg(floatOis);
            defs.push_back(swo.Finish());
        }

        return b.CreateVector(defs);
    }

    flatbuffers::Offset<quantra::Pricing> buildPricing(
        flatbuffers::grpc::MessageBuilder& b,
        flatbuffers::Offset<flatbuffers::String> asOfDate,
        flatbuffers::Offset<flatbuffers::String> settlementDate = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>> quotes = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>> indices = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapIndexDef>>> swapIndices = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>> curves = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CouponPricer>>> couponPricers = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CreditCurveSpec>>> creditCurves = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>> volSurfaces = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::ModelSpec>>> models = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>> equityUnderlyings = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>> inflationIndices = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::InflationCurveSpec>>> inflationCurves = 0,
        bool bondPricingDetails = false,
        bool bondPricingFlows = false,
        bool swaptionPricingDetails = false,
        bool swaptionPricingRebump = false) {
        auto rates = (indices.o != 0 || swapIndices.o != 0 || curves.o != 0 || couponPricers.o != 0)
            ? quantra::CreateRatesMarketData(b, indices, swapIndices, curves, couponPricers)
            : 0;
        auto credit = creditCurves.o != 0 ? quantra::CreateCreditMarketData(b, creditCurves) : 0;
        auto volatility = (volSurfaces.o != 0 || models.o != 0)
            ? quantra::CreateVolatilityMarketData(b, volSurfaces, models)
            : 0;
        auto equity = equityUnderlyings.o != 0 ? quantra::CreateEquityMarketData(b, equityUnderlyings) : 0;
        auto inflation = (inflationIndices.o != 0 || inflationCurves.o != 0)
            ? quantra::CreateInflationMarketData(b, inflationIndices, inflationCurves)
            : 0;
        auto options = (bondPricingDetails || bondPricingFlows || swaptionPricingDetails || swaptionPricingRebump)
            ? quantra::CreatePricingOptions(
                  b,
                  bondPricingDetails,
                  bondPricingFlows,
                  swaptionPricingDetails,
                  swaptionPricingRebump)
            : 0;

        return quantra::CreatePricing(
            b,
            asOfDate,
            settlementDate,
            quotes,
            rates,
            credit,
            volatility,
            equity,
            inflation,
            options);
    }
    
    flatbuffers::Offset<quantra::Yield> buildYield(flatbuffers::grpc::MessageBuilder& b) {
        quantra::YieldBuilder yb(b);
        yb.add_day_counter(quantra::enums::DayCounter_Actual360);
        yb.add_compounding(quantra::enums::Compounding_Compounded);
        yb.add_frequency(quantra::enums::Frequency_Annual);
        return yb.Finish();
    }
    
    // Build OptionletVolSurface for Caps/Floors (using new union-based schema)
    flatbuffers::Offset<quantra::VolSurfaceSpec> buildOptionletVolSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id, double vol,
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0) {
        
        auto ref_date = b.CreateString("2025-01-15");
        
        quantra::IrVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_Constant);
        baseBuilder.add_volatility_type(volType);
        baseBuilder.add_displacement(displacement);
        baseBuilder.add_constant_vol(vol);
        auto base = baseBuilder.Finish();
        
        quantra::OptionletVolSpecBuilder optBuilder(b);
        optBuilder.add_base(base);
        auto optPayload = optBuilder.Finish();
        
        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_OptionletVolSpec);
        vsBuilder.add_payload(optPayload.Union());
        return vsBuilder.Finish();
    }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildBlackVolSurface(
        flatbuffers::grpc::MessageBuilder& b,
        const std::string& id,
        double vol,
        quantra::enums::VolSurfaceShape shape = quantra::enums::VolSurfaceShape_Constant,
        const std::string& refDate = "2025-01-15") {
        auto ref_date = b.CreateString(refDate);

        quantra::BlackVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(shape);
        baseBuilder.add_constant_vol(vol);
        auto base = baseBuilder.Finish();

        quantra::BlackVolSpecBuilder blackBuilder(b);
        blackBuilder.add_base(base);
        auto blackPayload = blackBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_BlackVolSpec);
        vsBuilder.add_payload(blackPayload.Union());
        return vsBuilder.Finish();
    }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildBlackVolTermStructure(
        flatbuffers::grpc::MessageBuilder& b,
        const std::string& id,
        const std::vector<QuantLib::Period>& expiries,
        const std::vector<double>& vols,
        const std::string& refDate = "2025-01-15") {
        auto ref_date = b.CreateString(refDate);
        quantra::BlackVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_AtmMatrix2D);
        baseBuilder.add_constant_vol(0.20);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<quantra::Period>> expOffsets;
        expOffsets.reserve(expiries.size());
        for (const auto& p : expiries) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        auto expVec = b.CreateVector(expOffsets);
        auto termVals = b.CreateVector(vols);
        quantra::QuoteMatrix2DBuilder mb(b);
        mb.add_n_rows(static_cast<int>(expiries.size()));
        mb.add_n_cols(1);
        mb.add_values(termVals);
        auto termMatrix = mb.Finish();

        quantra::BlackVolSpecBuilder blackBuilder(b);
        blackBuilder.add_base(base);
        blackBuilder.add_expiries(expVec);
        blackBuilder.add_term_vols(termMatrix);
        auto blackPayload = blackBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_BlackVolSpec);
        vsBuilder.add_payload(blackPayload.Union());
        return vsBuilder.Finish();
    }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildBlackVolSurfaceGrid(
        flatbuffers::grpc::MessageBuilder& b,
        const std::string& id,
        const std::vector<QuantLib::Period>& expiries,
        const std::vector<double>& strikes,
        const std::vector<double>& volsFlat,
        const std::string& refDate = "2025-01-15") {
        auto ref_date = b.CreateString(refDate);
        quantra::BlackVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_SmileCube3D);
        baseBuilder.add_constant_vol(0.20);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<quantra::Period>> expOffsets;
        expOffsets.reserve(expiries.size());
        for (const auto& p : expiries) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        auto expVec = b.CreateVector(expOffsets);
        auto strikeVec = b.CreateVector(strikes);
        auto surfVals = b.CreateVector(volsFlat);
        quantra::QuoteMatrix2DBuilder mb(b);
        mb.add_n_rows(static_cast<int>(expiries.size()));
        mb.add_n_cols(static_cast<int>(strikes.size()));
        mb.add_values(surfVals);
        auto surfaceMatrix = mb.Finish();

        quantra::BlackVolSpecBuilder blackBuilder(b);
        blackBuilder.add_base(base);
        blackBuilder.add_expiries(expVec);
        blackBuilder.add_strikes(strikeVec);
        blackBuilder.add_surface_vols(surfaceMatrix);
        auto blackPayload = blackBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_BlackVolSpec);
        vsBuilder.add_payload(blackPayload.Union());
        return vsBuilder.Finish();
    }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildBlackVolSurfaceFromPrices(
        flatbuffers::grpc::MessageBuilder& b,
        const std::string& id,
        const std::vector<std::string>& expiryDatesIso,
        const std::vector<double>& strikes,
        const std::vector<double>& pricesFlat,
        const std::string& spotQuoteId,
        const std::string& discountCurveId,
        const std::string& dividendCurveId,
        quantra::enums::SurfaceInterpolator2D surfaceInterp =
            quantra::enums::SurfaceInterpolator2D_Bilinear,
        const std::string& refDate = "2025-01-15") {
        auto ref_date = b.CreateString(refDate);
        quantra::BlackVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_SurfaceFromPrices);
        baseBuilder.add_constant_vol(0.20);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<flatbuffers::String>> expiryOffsets;
        expiryOffsets.reserve(expiryDatesIso.size());
        for (const auto& s : expiryDatesIso) {
            expiryOffsets.push_back(b.CreateString(s));
        }
        auto expiryVec = b.CreateVector(expiryOffsets);
        auto strikeVec = b.CreateVector(strikes);
        auto priceVals = b.CreateVector(pricesFlat);
        quantra::QuoteMatrix2DBuilder mb(b);
        mb.add_n_rows(static_cast<int>(expiryDatesIso.size()));
        mb.add_n_cols(static_cast<int>(strikes.size()));
        mb.add_values(priceVals);
        auto priceMatrix = mb.Finish();

        auto spotQ = b.CreateString(spotQuoteId);
        auto discCurve = b.CreateString(discountCurveId);
        auto divCurve = b.CreateString(dividendCurveId);
        quantra::BlackVolSpecBuilder blackBuilder(b);
        blackBuilder.add_base(base);
        blackBuilder.add_surface_prices(priceMatrix);
        blackBuilder.add_price_expiries(expiryVec);
        blackBuilder.add_price_strikes(strikeVec);
        blackBuilder.add_price_option_type(quantra::enums::EquityOptionType_Call);
        blackBuilder.add_spot_quote_id(spotQ);
        blackBuilder.add_discount_curve_id(discCurve);
        blackBuilder.add_dividend_curve_id(divCurve);
        blackBuilder.add_surface_interpolator(surfaceInterp);
        auto blackPayload = blackBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_BlackVolSpec);
        vsBuilder.add_payload(blackPayload.Union());
        return vsBuilder.Finish();
    }
    
    // Build SwaptionVolSurface for Swaptions (using new union-based schema)
    flatbuffers::Offset<quantra::VolSurfaceSpec> buildSwaptionVolSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id, double vol,
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0,
        const std::string& quoteId = "",
        const std::string& refDate = "2025-01-15",
        const std::string& swapIndexId = "EUR_SWAP_6M") {
        
        auto ref_date = b.CreateString(refDate);
        flatbuffers::Offset<flatbuffers::String> qid;
        if (!quoteId.empty()) {
            qid = b.CreateString(quoteId);
        }
        
        quantra::IrVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_Constant);
        baseBuilder.add_volatility_type(volType);
        baseBuilder.add_displacement(displacement);
        baseBuilder.add_constant_vol(vol);
        if (!quoteId.empty()) {
            baseBuilder.add_quote_id(qid);
        }
        auto base = baseBuilder.Finish();
        
        quantra::SwaptionVolConstantSpecBuilder constBuilder(b);
        constBuilder.add_base(base);
        auto constPayload = constBuilder.Finish();

        auto swapIndexIdOff = b.CreateString(swapIndexId);
        quantra::SwaptionVolSpecBuilder swpBuilder(b);
        swpBuilder.add_swap_index_id(swapIndexIdOff);
        swpBuilder.add_payload_type(quantra::SwaptionVolPayload_SwaptionVolConstantSpec);
        swpBuilder.add_payload(constPayload.Union());
        auto swpPayload = swpBuilder.Finish();
        
        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_SwaptionVolSpec);
        vsBuilder.add_payload(swpPayload.Union());
        return vsBuilder.Finish();
    }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildSwaptionVolAtmMatrixSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        const std::vector<QuantLib::Period>& expiries,
        const std::vector<QuantLib::Period>& tenors,
        const std::vector<double>& volsFlat,
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0,
        const std::string& refDate = "2025-01-15",
        const std::string& swapIndexId = "EUR_SWAP_6M") {

        auto ref_date = b.CreateString(refDate);

        quantra::IrVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_AtmMatrix2D);
        baseBuilder.add_volatility_type(volType);
        baseBuilder.add_displacement(displacement);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<quantra::Period>> expOffsets;
        for (const auto& p : expiries) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::Period>> tenOffsets;
        for (const auto& p : tenors) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            tenOffsets.push_back(pb.Finish());
        }

        int nExp = static_cast<int>(expiries.size());
        int nTen = static_cast<int>(tenors.size());
        auto values = b.CreateVector(volsFlat);
        quantra::QuoteMatrix2DBuilder mBuilder(b);
        mBuilder.add_n_rows(nExp);
        mBuilder.add_n_cols(nTen);
        mBuilder.add_values(values);
        auto matrix = mBuilder.Finish();

        auto expVec = b.CreateVector(expOffsets);
        auto tenVec = b.CreateVector(tenOffsets);
        quantra::SwaptionVolAtmMatrixSpecBuilder atmBuilder(b);
        atmBuilder.add_base(base);
        atmBuilder.add_expiries(expVec);
        atmBuilder.add_tenors(tenVec);
        atmBuilder.add_vols(matrix);
        auto atmPayload = atmBuilder.Finish();

        auto swapIndexIdOff = b.CreateString(swapIndexId);
        quantra::SwaptionVolSpecBuilder swpBuilder(b);
        swpBuilder.add_swap_index_id(swapIndexIdOff);
        swpBuilder.add_payload_type(quantra::SwaptionVolPayload_SwaptionVolAtmMatrixSpec);
        swpBuilder.add_payload(atmPayload.Union());
        auto swpPayload = swpBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_SwaptionVolSpec);
        vsBuilder.add_payload(swpPayload.Union());
        return vsBuilder.Finish();
    }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildSwaptionVolSmileCubeSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        const std::vector<QuantLib::Period>& expiries,
        const std::vector<QuantLib::Period>& tenors,
        const std::vector<double>& strikes,
        const std::vector<double>& volsFlat,
        quantra::enums::SwaptionStrikeKind strikeKind = quantra::enums::SwaptionStrikeKind_Absolute,
        const std::string& swapIndexId = "",
        const std::vector<double>& atmForwardsFlat = {},
        bool allowExternalAtm = false,
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0,
        const std::string& refDate = "2025-01-15") {

        auto ref_date = b.CreateString(refDate);
        quantra::IrVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_SmileCube3D);
        baseBuilder.add_volatility_type(volType);
        baseBuilder.add_displacement(displacement);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<quantra::Period>> expOffsets;
        for (const auto& p : expiries) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::Period>> tenOffsets;
        for (const auto& p : tenors) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            tenOffsets.push_back(pb.Finish());
        }

        int nExp = static_cast<int>(expiries.size());
        int nTen = static_cast<int>(tenors.size());
        int nStr = static_cast<int>(strikes.size());
        auto values = b.CreateVector(volsFlat);
        flatbuffers::Offset<quantra::QuoteMatrix2D> atmMatrix;
        quantra::QuoteTensor3DBuilder tBuilder(b);
        tBuilder.add_n_1(nExp);
        tBuilder.add_n_2(nTen);
        tBuilder.add_n_3(nStr);
        tBuilder.add_values(values);
        auto tensor = tBuilder.Finish();
        if (!atmForwardsFlat.empty()) {
            auto atmValues = b.CreateVector(atmForwardsFlat);
            quantra::QuoteMatrix2DBuilder aBuilder(b);
            aBuilder.add_n_rows(nExp);
            aBuilder.add_n_cols(nTen);
            aBuilder.add_values(atmValues);
            atmMatrix = aBuilder.Finish();
        }

        auto expVec = b.CreateVector(expOffsets);
        auto tenVec = b.CreateVector(tenOffsets);
        auto strikeVec = b.CreateVector(strikes);
        quantra::SwaptionVolSmileCubeSpecBuilder cubeBuilder(b);
        cubeBuilder.add_base(base);
        cubeBuilder.add_expiries(expVec);
        cubeBuilder.add_tenors(tenVec);
        cubeBuilder.add_strikes(strikeVec);
        cubeBuilder.add_strike_kind(strikeKind);
        cubeBuilder.add_allow_external_atm(allowExternalAtm);
        if (!atmForwardsFlat.empty()) {
            cubeBuilder.add_atm_forwards(atmMatrix);
        }
        cubeBuilder.add_vols(tensor);
        auto cubePayload = cubeBuilder.Finish();

        auto swapIndexIdOff = b.CreateString(swapIndexId);
        quantra::SwaptionVolSpecBuilder swpBuilder(b);
        swpBuilder.add_swap_index_id(swapIndexIdOff);
        swpBuilder.add_payload_type(quantra::SwaptionVolPayload_SwaptionVolSmileCubeSpec);
        swpBuilder.add_payload(cubePayload.Union());
        auto swpPayload = swpBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_SwaptionVolSpec);
        vsBuilder.add_payload(swpPayload.Union());
        return vsBuilder.Finish();
    }

    // Build a SwaptionSabrParamsSpec surface. nRowsExp/nColsTen describe the
    // (alpha,beta,rho,nu) matrix dimensions actually written into the buffer;
    // they default to the natural sizes but can be overridden to exercise the
    // dimension-mismatch validation path.
    flatbuffers::Offset<quantra::VolSurfaceSpec> buildSwaptionSabrParamsSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        const std::vector<QuantLib::Period>& expiries,
        const std::vector<QuantLib::Period>& tenors,
        const std::vector<double>& alphaFlat,
        const std::vector<double>& betaFlat,
        const std::vector<double>& rhoFlat,
        const std::vector<double>& nuFlat,
        const std::string& swapIndexId = "EUR_SWAP_6M",
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0,
        const std::string& refDate = "2025-01-15",
        int matrixRowsOverride = -1,
        int matrixColsOverride = -1) {

        auto ref_date = b.CreateString(refDate);
        quantra::IrVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_SabrParams);
        baseBuilder.add_volatility_type(volType);
        baseBuilder.add_displacement(displacement);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<quantra::Period>> expOffsets;
        for (const auto& p : expiries) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::Period>> tenOffsets;
        for (const auto& p : tenors) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            tenOffsets.push_back(pb.Finish());
        }

        const int nExp = static_cast<int>(expiries.size());
        const int nTen = static_cast<int>(tenors.size());
        const int rows = matrixRowsOverride > 0 ? matrixRowsOverride : nExp;
        const int cols = matrixColsOverride > 0 ? matrixColsOverride : nTen;

        auto buildMatrix = [&](const std::vector<double>& flat) {
            auto values = b.CreateVector(flat);
            quantra::QuoteMatrix2DBuilder mb(b);
            mb.add_n_rows(rows);
            mb.add_n_cols(cols);
            mb.add_values(values);
            return mb.Finish();
        };
        auto alphaM = buildMatrix(alphaFlat);
        auto betaM = buildMatrix(betaFlat);
        auto rhoM = buildMatrix(rhoFlat);
        auto nuM = buildMatrix(nuFlat);

        auto expVec = b.CreateVector(expOffsets);
        auto tenVec = b.CreateVector(tenOffsets);
        quantra::SwaptionSabrParamsSpecBuilder sb(b);
        sb.add_base(base);
        sb.add_expiries(expVec);
        sb.add_tenors(tenVec);
        sb.add_alpha(alphaM);
        sb.add_beta(betaM);
        sb.add_rho(rhoM);
        sb.add_nu(nuM);
        auto sabrPayload = sb.Finish();

        auto swapIndexIdOff = b.CreateString(swapIndexId);
        quantra::SwaptionVolSpecBuilder swpBuilder(b);
        swpBuilder.add_swap_index_id(swapIndexIdOff);
        swpBuilder.add_payload_type(quantra::SwaptionVolPayload_SwaptionSabrParamsSpec);
        swpBuilder.add_payload(sabrPayload.Union());
        auto swpPayload = swpBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_SwaptionVolSpec);
        vsBuilder.add_payload(swpPayload.Union());
        return vsBuilder.Finish();
    }

    // Build a SwaptionSabrCalibrateSpec surface. Mirrors buildSwaptionSabrParamsSurface
    // for the calibrate path. `marketVolsFlat` is row-major (nExp * nTen * nStrikes).
    // The dimension overrides drive the dim-mismatch validation tests; pass -1 to
    // use the natural sizes from the periods/strike vectors. `addNonEmptyWeights`
    // emits a non-empty weights tensor to exercise the v1 weights-rejection path.
    flatbuffers::Offset<quantra::VolSurfaceSpec> buildSwaptionSabrCalibrateSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        const std::vector<QuantLib::Period>& expiries,
        const std::vector<QuantLib::Period>& tenors,
        const std::vector<double>& strikeSpreads,
        const std::vector<double>& marketVolsFlat,
        bool betaFixed = true,
        double betaValue = 0.5,
        bool vegaWeightedSmileFit = false,
        const std::string& swapIndexId = "EUR_SWAP_6M",
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0,
        const std::string& refDate = "2025-01-15",
        int n1Override = -1,
        int n2Override = -1,
        int n3Override = -1,
        bool addNonEmptyWeights = false) {

        auto ref_date = b.CreateString(refDate);
        quantra::IrVolBaseSpecBuilder baseBuilder(b);
        baseBuilder.add_reference_date(ref_date);
        baseBuilder.add_calendar(quantra::enums::Calendar_TARGET);
        baseBuilder.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        baseBuilder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        baseBuilder.add_shape(quantra::enums::VolSurfaceShape_SabrCalibrate);
        baseBuilder.add_volatility_type(volType);
        baseBuilder.add_displacement(displacement);
        auto base = baseBuilder.Finish();

        std::vector<flatbuffers::Offset<quantra::Period>> expOffsets;
        for (const auto& p : expiries) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::Period>> tenOffsets;
        for (const auto& p : tenors) {
            quantra::PeriodBuilder pb(b);
            pb.add_n(p.length());
            pb.add_unit(toFbTimeUnit(p.units()));
            tenOffsets.push_back(pb.Finish());
        }

        const int nExp = static_cast<int>(expiries.size());
        const int nTen = static_cast<int>(tenors.size());
        const int nStr = static_cast<int>(strikeSpreads.size());
        const int n1 = n1Override > 0 ? n1Override : nExp;
        const int n2 = n2Override > 0 ? n2Override : nTen;
        const int n3 = n3Override > 0 ? n3Override : nStr;

        auto strikesVec = b.CreateVector(strikeSpreads);
        auto volsVec = b.CreateVector(marketVolsFlat);
        quantra::QuoteTensor3DBuilder tBuilder(b);
        tBuilder.add_n_1(n1);
        tBuilder.add_n_2(n2);
        tBuilder.add_n_3(n3);
        tBuilder.add_values(volsVec);
        auto tensor = tBuilder.Finish();

        flatbuffers::Offset<quantra::QuoteTensor3D> weightsTensor = 0;
        if (addNonEmptyWeights) {
            std::vector<double> w(static_cast<size_t>(nExp * nTen * nStr), 1.0);
            auto wVec = b.CreateVector(w);
            quantra::QuoteTensor3DBuilder wb(b);
            wb.add_n_1(nExp);
            wb.add_n_2(nTen);
            wb.add_n_3(nStr);
            wb.add_values(wVec);
            weightsTensor = wb.Finish();
        }

        auto expVec = b.CreateVector(expOffsets);
        auto tenVec = b.CreateVector(tenOffsets);
        quantra::SwaptionSabrCalibrateSpecBuilder sb(b);
        sb.add_base(base);
        sb.add_expiries(expVec);
        sb.add_tenors(tenVec);
        sb.add_strikes(strikesVec);
        sb.add_vols(tensor);
        sb.add_beta_fixed(betaFixed);
        sb.add_beta_value(betaValue);
        if (weightsTensor.o != 0) sb.add_weights(weightsTensor);
        sb.add_vega_weighted_smile_fit(vegaWeightedSmileFit);
        auto sabrPayload = sb.Finish();

        auto swapIndexIdOff = b.CreateString(swapIndexId);
        quantra::SwaptionVolSpecBuilder swpBuilder(b);
        swpBuilder.add_swap_index_id(swapIndexIdOff);
        swpBuilder.add_payload_type(quantra::SwaptionVolPayload_SwaptionSabrCalibrateSpec);
        swpBuilder.add_payload(sabrPayload.Union());
        auto swpPayload = swpBuilder.Finish();

        auto vol_id = b.CreateString(id);
        quantra::VolSurfaceSpecBuilder vsBuilder(b);
        vsBuilder.add_id(vol_id);
        vsBuilder.add_payload_type(quantra::VolPayload_SwaptionVolSpec);
        vsBuilder.add_payload(swpPayload.Union());
        return vsBuilder.Finish();
    }

    // Build CapFloorModelSpec
    flatbuffers::Offset<quantra::ModelSpec> buildCapFloorModel(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        quantra::enums::IrModelType modelType = quantra::enums::IrModelType_Black) {
        
        quantra::CapFloorModelSpecBuilder cfmBuilder(b);
        cfmBuilder.add_model_type(modelType);
        auto cfmPayload = cfmBuilder.Finish();
        
        auto model_id = b.CreateString(id);
        quantra::ModelSpecBuilder msBuilder(b);
        msBuilder.add_id(model_id);
        msBuilder.add_payload_type(quantra::ModelPayload_CapFloorModelSpec);
        msBuilder.add_payload(cfmPayload.Union());
        return msBuilder.Finish();
    }
    
    // Build SwaptionModelSpec
    flatbuffers::Offset<quantra::ModelSpec> buildSwaptionModel(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id,
        quantra::enums::IrModelType modelType = quantra::enums::IrModelType_Black,
        double hwA = 0.03,
        double hwSigma = 0.01,
        int latticeSteps = 50,
        quantra::enums::ModelParamMode paramMode = quantra::enums::ModelParamMode_Explicit,
        const std::string& calibVolId = "",
        const std::string& calibCurveId = "",
        const std::string& calibSwapIndexId = "",
        const std::string& calibForwardCurveId = "") {
        flatbuffers::Offset<quantra::SwaptionHwCalibrationSpec> calibSpec;
        if (!calibVolId.empty() && !calibCurveId.empty() && !calibSwapIndexId.empty() &&
            !calibForwardCurveId.empty()) {
            auto volId = b.CreateString(calibVolId);
            auto curveId = b.CreateString(calibCurveId);
            auto swapIndexId = b.CreateString(calibSwapIndexId);
            auto fwdCurveId = b.CreateString(calibForwardCurveId);
            quantra::SwaptionHwCalibrationSpecBuilder cb(b);
            cb.add_swaption_vol_id(volId);
            cb.add_discount_curve_id(curveId);
            cb.add_forwarding_curve_id(fwdCurveId);
            cb.add_swap_index_id(swapIndexId);
            calibSpec = cb.Finish();
        }

        quantra::SwaptionModelSpecBuilder smBuilder(b);
        smBuilder.add_model_type(modelType);
        smBuilder.add_hw_a(hwA);
        smBuilder.add_hw_sigma(hwSigma);
        smBuilder.add_lattice_steps(latticeSteps);
        smBuilder.add_param_mode(paramMode);
        if (calibSpec.o != 0) {
            smBuilder.add_hw_calibration(calibSpec);
        }
        auto smPayload = smBuilder.Finish();
        
        auto model_id = b.CreateString(id);
        quantra::ModelSpecBuilder msBuilder(b);
        msBuilder.add_id(model_id);
        msBuilder.add_payload_type(quantra::ModelPayload_SwaptionModelSpec);
        msBuilder.add_payload(smPayload.Union());
        return msBuilder.Finish();
    }

    QuantLib::Date evaluationDate_;
    double flatRate_;
    double dividendFlatRate_;
    std::shared_ptr<QuantLib::YieldTermStructure> bootstrappedCurve_;
    std::shared_ptr<QuantLib::YieldTermStructure> dividendCurve_;
    QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle_, forwardHandle_, dividendHandle_;
};

// ======================== FIXED RATE BOND ========================
TEST_F(QuantraComparisonTest, FixedRateBond_NPVMatches) {
    std::cout << "\n=== Fixed Rate Bond ===" << std::endl;
    double face = 100.0, coupon = 0.05;
    QuantLib::Date issue(15,QuantLib::January,2024), mat(15,QuantLib::January,2029);
    
    QuantLib::Schedule sch(issue, mat, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::Unadjusted, QuantLib::Unadjusted, QuantLib::DateGeneration::Backward, false);
    auto qlBond = std::make_shared<QuantLib::FixedRateBond>(2, face, sch,
        std::vector<QuantLib::Rate>(1, coupon), QuantLib::ActualActual(QuantLib::ActualActual::ISDA));
    qlBond->setPricingEngine(std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle_));
    double qlNPV = qlBond->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    // IndexDefs needed by SwapHelpers in the curve
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, 0, 0, 0, 0, 0, 0, true);
    
    auto eff = b.CreateString("2024-01-15");
    auto term = b.CreateString("2029-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Annual);
    sb.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    sb.add_end_of_month(false);
    auto schedule = sb.Finish();
    
    auto idate = b.CreateString("2024-01-15");
    quantra::FixedRateBondBuilder bb(b);
    bb.add_settlement_days(2);
    bb.add_face_amount(face);
    bb.add_schedule(schedule);
    bb.add_rate(coupon);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_ActualActual);
    bb.add_issue_date(idate);
    bb.add_redemption(100.0);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    auto bond = bb.Finish();
    
    auto yield = buildYield(b);
    auto dc = b.CreateString("discount");
    
    quantra::PriceFixedRateBondBuilder pfb(b);
    pfb.add_fixed_rate_bond(bond);
    pfb.add_discounting_curve(dc);
    pfb.add_yield(yield);
    auto pfbOff = pfb.Finish();
    
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfbOff});
    
    quantra::PriceFixedRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_bonds(bonds);
    b.Finish(rb.Finish());
    
    FixedRateBondPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFixedRateBondRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceFixedRateBondResponse>(respB->GetBufferPointer())->bonds()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// ======================== VANILLA SWAP ========================
TEST_F(QuantraComparisonTest, VanillaSwap_NPVMatches) {
    std::cout << "\n=== Vanilla Swap ===" << std::endl;
    double notional = 1000000.0, fixedRate = 0.035;
    QuantLib::Date start = evaluationDate_ + 2, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule fixSch(start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto qlSwap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, fixedRate, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    double qlNPV = qlSwap->NPV();
    double qlFairRate = qlSwap->fairRate();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);
    
    // Fixed leg
    auto feff = b.CreateString("2025-01-17");
    auto fterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(fixedRate);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();
    
    // Float leg — uses IndexRef
    auto fleff = b.CreateString("2025-01-17");
    auto flterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();
    
    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto swap = vsb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvsbOff = pvsb.Finish();
    
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvsbOff});
    
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());
    
    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceVanillaSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);
    double qNPV = r->npv();
    double qFairRate = r->fair_rate();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fair: " << qlFairRate*100 << "% | Quantra: " << qFairRate*100 << "%" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFairRate, qFairRate, 1e-6);
    EXPECT_DOUBLE_EQ(-1.0, r->used_cms_mean_reversion());
}

TEST_F(QuantraComparisonTest, VanillaSwap_CMS_NPVMatches) {
    const double notional = 1000000.0;
    const double fixedRate = 0.035;
    const double cmsVol = 0.20;

    QuantLib::Date start = evaluationDate_ + 2;
    QuantLib::Date end = start + QuantLib::Period(5, QuantLib::Years);
    QuantLib::Schedule fixSch(
        start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule cmsSch(
        start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);

    auto ibor6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swapIndex = std::make_shared<QuantLib::SwapIndex>(
        "EUR_SWAP_10Y",
        QuantLib::Period(5, QuantLib::Years),
        2,
        ibor6m->currency(),
        QuantLib::TARGET(),
        QuantLib::Period(QuantLib::Annual),
        QuantLib::ModifiedFollowing,
        QuantLib::Thirty360(QuantLib::Thirty360::BondBasis),
        ibor6m,
        discountHandle_);

    QuantLib::Leg qlFixedLeg = QuantLib::FixedRateLeg(fixSch)
        .withNotionals(notional)
        .withCouponRates(fixedRate, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis))
        .withPaymentAdjustment(QuantLib::ModifiedFollowing);
    QuantLib::Leg qlCmsLeg = QuantLib::CmsLeg(cmsSch, swapIndex)
        .withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360())
        .withPaymentAdjustment(QuantLib::ModifiedFollowing)
        .withFixingDays(2)
        .withGearings(1.0)
        .withSpreads(0.0);

    auto qlCmsVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
        evaluationDate_,
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        cmsVol,
        QuantLib::Actual365Fixed(),
        QuantLib::ShiftedLognormal,
        0.0);
    auto meanReversion = QuantLib::Handle<QuantLib::Quote>(
        QuantLib::ext::make_shared<QuantLib::SimpleQuote>(0.03));
    auto cmsPricer = QuantLib::ext::make_shared<QuantLib::LinearTsrPricer>(
        QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlCmsVol),
        meanReversion,
        discountHandle_);
    QuantLib::setCouponPricer(qlCmsLeg, cmsPricer);

    std::vector<QuantLib::Leg> qlLegs{qlFixedLeg, qlCmsLeg};
    std::vector<bool> payer{true, false}; // Payer: pay fixed, receive CMS.
    auto qlSwap = std::make_shared<QuantLib::Swap>(qlLegs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto vol = buildSwaptionVolSurface(
        b, "cms_swaption_vol", cmsVol, quantra::enums::VolatilityType_Lognormal, 0.0, "", "2025-01-15", "EUR_SWAP_6M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, asof, 0, indices, swapIndices, curves, 0, 0, vols);

    auto feff = b.CreateString("2025-01-17");
    auto fterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(fixedRate);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto cleff = b.CreateString("2025-01-17");
    auto clterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder cmsSb(b);
    cmsSb.add_effective_date(cleff);
    cmsSb.add_termination_date(clterm);
    cmsSb.add_calendar(quantra::enums::Calendar_TARGET);
    cmsSb.add_frequency(quantra::enums::Frequency_Semiannual);
    cmsSb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsSb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsSb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto cmsSchOff = cmsSb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto cmsSwapIndexId = b.CreateString("EUR_SWAP_6M");
    auto cmsVolId = b.CreateString("cms_swaption_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(notional);
    cmsLb.add_schedule(cmsSchOff);
    cmsLb.add_swap_index_id(cmsSwapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(cmsVolId);
    cmsLb.add_fixing_days(2);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsLb.add_gear(1.0);
    cmsLb.add_spread(0.0);
    auto cmsLeg = cmsLb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_cms_leg(cmsLeg);
    auto swap = vsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvsbOff = pvsb.Finish();

    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvsbOff});
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto out = flatbuffers::GetRoot<quantra::PriceVanillaSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);
    const double qNPV = out->npv();

    EXPECT_TRUE(std::isfinite(qNPV));
    EXPECT_NEAR(qlNPV, qNPV, 0.5);
}

TEST_F(QuantraComparisonTest, VanillaSwap_CMS_MissingVolThrows) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves);

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Semiannual);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto sch = sb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(1000000.0);
    flb.add_schedule(sch);
    flb.add_rate(0.03);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto swapIndexId = b.CreateString("EUR_SWAP_6M");
    auto missingVolId = b.CreateString("missing_cms_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(1000000.0);
    cmsLb.add_schedule(sch);
    cmsLb.add_swap_index_id(swapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(missingVolId);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cmsLeg = cmsLb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_cms_leg(cmsLeg);
    auto swap = vsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvs = pvsb.Finish();
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvs});
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, VanillaSwap_CMS_UnknownSwapIndexThrows) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto vol = buildSwaptionVolSurface(
        b, "cms_swaption_vol", 0.20, quantra::enums::VolatilityType_Lognormal, 0.0, "", "2025-01-15", "EUR_SWAP_6M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Semiannual);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto sch = sb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(1000000.0);
    flb.add_schedule(sch);
    flb.add_rate(0.03);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto swapIndexId = b.CreateString("UNKNOWN_SWAP_INDEX");
    auto volId = b.CreateString("cms_swaption_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(1000000.0);
    cmsLb.add_schedule(sch);
    cmsLb.add_swap_index_id(swapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(volId);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cmsLeg = cmsLb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_cms_leg(cmsLeg);
    auto swap = vsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvs = pvsb.Finish();
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvs});
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, CMSCoupons_PricerAttached) {
    flatbuffers::grpc::MessageBuilder pbuilder;
    auto ts = buildCurve(pbuilder, "discount");
    auto curves = pbuilder.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(pbuilder);
    auto swapIndices = buildSwapIndicesVector(pbuilder);
    auto vol = buildSwaptionVolSurface(
        pbuilder, "cms_swaption_vol", 0.20, quantra::enums::VolatilityType_Lognormal, 0.0, "", "2025-01-15", "EUR_SWAP_6M");
    auto vols = pbuilder.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
    auto asof = pbuilder.CreateString("2025-01-15");

    auto pricing = buildPricing(pbuilder, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
    pbuilder.Finish(pricing);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(
        flatbuffers::GetRoot<quantra::Pricing>(pbuilder.GetBufferPointer()));

    flatbuffers::grpc::MessageBuilder b;
    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Semiannual);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto sch = sb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto swapIndexId = b.CreateString("EUR_SWAP_6M");
    auto volId = b.CreateString("cms_swaption_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(1000000.0);
    cmsLb.add_schedule(sch);
    cmsLb.add_swap_index_id(swapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(volId);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsLb.add_fixing_days(2);
    auto cmsLegFb = cmsLb.Finish();

    b.Finish(cmsLegFb);
    auto cmsLegRoot = flatbuffers::GetRoot<quantra::SwapCmsLeg>(b.GetBufferPointer());

    auto dIt = reg.rates.curves.find("discount");
    ASSERT_TRUE(dIt != reg.rates.curves.end());
    Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());

    CmsLegParser parser;
    auto cmsLeg = parser.parse(
        cmsLegRoot, reg.rates.indices, reg.rates.swapIndices, discountCurve, discountCurve);
    auto vIt = reg.volatility.swaptionVols.find("cms_swaption_vol");
    ASSERT_TRUE(vIt != reg.volatility.swaptionVols.end());
    auto pricerBuild = parser.makeCouponPricer(cmsLegRoot, vIt->second, discountCurve);
    QuantLib::setCouponPricer(cmsLeg, pricerBuild.pricer);

    int cmsCoupons = 0;
    for (const auto& cf : cmsLeg) {
        auto cpn = QuantLib::ext::dynamic_pointer_cast<QuantLib::CmsCoupon>(cf);
        if (cpn) {
            ++cmsCoupons;
            EXPECT_TRUE(static_cast<bool>(cpn->pricer()));
        }
    }
    EXPECT_GT(cmsCoupons, 0);
}

// ======================== FRA ========================
TEST_F(QuantraComparisonTest, FRA_NPVMatches) {
    std::cout << "\n=== FRA ===" << std::endl;
    double notional = 1000000.0, strike = 0.032;
    QuantLib::Date valDate = evaluationDate_ + QuantLib::Period(3, QuantLib::Months);
    QuantLib::Date matDate = evaluationDate_ + QuantLib::Period(6, QuantLib::Months);
    
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    auto qlFRA = std::make_shared<QuantLib::ForwardRateAgreement>(
        idx, valDate, matDate, QuantLib::Position::Long, strike, notional, discountHandle_);
    double qlNPV = qlFRA->NPV();
    double qlFwd = qlFRA->forwardRate();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b, true);  // include EUR_3M
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);
    
    auto vd = b.CreateString("2025-04-15");
    auto md = b.CreateString("2025-07-15");
    auto idx3m = buildIndexRef(b, "EUR_3M");
    
    quantra::FRABuilder fb(b);
    fb.add_start_date(vd);
    fb.add_maturity_date(md);
    fb.add_fra_type(quantra::enums::FRAType_Long);
    fb.add_strike(strike);
    fb.add_notional(notional);
    fb.add_index(idx3m);
    fb.add_day_counter(quantra::enums::DayCounter_Actual360);
    fb.add_calendar(quantra::enums::Calendar_TARGET);
    fb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fra = fb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceFRABuilder pfb(b);
    pfb.add_fra(fra);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    auto pfbOff = pfb.Finish();
    
    auto fras = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFRA>>{pfbOff});
    
    quantra::PriceFRARequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_fras(fras);
    b.Finish(rb.Finish());
    
    FRAPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFRARequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceFRAResponse>(respB->GetBufferPointer())->fras()->Get(0);
    double qNPV = r->npv();
    double qFwd = r->forward_rate();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fwd: " << qlFwd*100 << "% | Quantra: " << qFwd*100 << "%" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFwd, qFwd, 1e-6);
}

// ======================== CAP ========================
TEST_F(QuantraComparisonTest, Cap_NPVMatches) {
    std::cout << "\n=== Cap ===" << std::endl;
    double notional = 1000000.0, strike = 0.04, vol = 0.20;
    QuantLib::Date start = evaluationDate_ + 2, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule sch(start, end, QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    QuantLib::Leg leg = QuantLib::IborLeg(sch, idx).withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360()).withPaymentAdjustment(QuantLib::ModifiedFollowing).withFixingDays(2);
    auto qlCap = std::make_shared<QuantLib::Cap>(leg, std::vector<QuantLib::Rate>(1, strike));
    auto volH = QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(
        std::make_shared<QuantLib::ConstantOptionletVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlCap->setPricingEngine(std::make_shared<QuantLib::BlackCapFloorEngine>(discountHandle_, volH));
    double qlNPV = qlCap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    
    auto volSurface = buildOptionletVolSurface(b, "vol_20pct", vol);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    
    auto model = buildCapFloorModel(b, "black_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    
    auto indices = buildIndicesVector(b, true);  // include EUR_3M for cap
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models, 0, 0, 0, false, false, false, true);
    
    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto schedule = sb.Finish();
    
    auto idx3m = buildIndexRef(b, "EUR_3M");
    quantra::CapFloorBuilder cb(b);
    cb.add_cap_floor_type(quantra::enums::CapFloorType_Cap);
    cb.add_notional(notional);
    cb.add_schedule(schedule);
    cb.add_strike(strike);
    cb.add_index(idx3m);
    cb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cap = cb.Finish();
    
    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("vol_20pct");
    auto model_id = b.CreateString("black_model");
    
    quantra::PriceCapFloorBuilder pcb(b);
    pcb.add_cap_floor(cap);
    pcb.add_discounting_curve(dc);
    pcb.add_forwarding_curve(dc);
    pcb.add_volatility(vol_id);
    pcb.add_model(model_id);
    auto pcbOff = pcb.Finish();
    
    auto caps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCapFloor>>{pcbOff});
    
    quantra::PriceCapFloorRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cap_floors(caps);
    b.Finish(rb.Finish());
    
    CapFloorPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCapFloorRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceCapFloorResponse>(respB->GetBufferPointer())->cap_floors()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// ======================== SWAPTION ========================
TEST_F(QuantraComparisonTest, Swaption_NPVMatches) {
    std::cout << "\n=== Swaption ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.20;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    
    auto volSurface = buildSwaptionVolSurface(b, "swaption_vol", vol);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models);
    
    // Fixed leg schedule
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();
    
    // Float leg schedule
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();
    
    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();
    
    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();
    
    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_vol");
    auto model_id = b.CreateString("black_swaption_model");
    
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});
    
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());
    
    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

TEST_F(QuantraComparisonTest, Swaption_Bermudan_HullWhiteLattice_NPVMatches) {
    std::cout << "\n=== Swaption Bermudan (Hull-White Lattice) ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.20;
    const double hwA = 0.03;
    const double hwSigma = 0.01;
    const int latticeSteps = 50;
    QuantLib::Date swapStart(17, QuantLib::January, 2026);
    QuantLib::Date swapEnd(17, QuantLib::January, 2031);

    std::vector<QuantLib::Date> exerciseDates = {
        QuantLib::Date(15, QuantLib::January, 2026),
        QuantLib::Date(15, QuantLib::January, 2027),
        QuantLib::Date(15, QuantLib::January, 2028)
    };

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::BermudanExercise>(exerciseDates);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto hwModel = std::make_shared<QuantLib::HullWhite>(discountHandle_, hwA, hwSigma);
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::TreeSwaptionEngine>(hwModel, latticeSteps));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    auto volSurface = buildSwaptionVolSurface(b, "swaption_vol", vol);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_lattice_model", quantra::enums::IrModelType_HullWhiteLattice, hwA, hwSigma, latticeSteps);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models);

    // Fixed leg schedule
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    // Float leg schedule
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();

    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    std::vector<flatbuffers::Offset<flatbuffers::String>> exDateStrs = {
        b.CreateString("2026-01-15"),
        b.CreateString("2027-01-15"),
        b.CreateString("2028-01-15")
    };
    auto exDatesVec = b.CreateVector(exDateStrs);

    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_type(quantra::enums::ExerciseType_Bermudan);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    swb.add_exercise_dates(exDatesVec);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_vol");
    auto model_id = b.CreateString("hw_lattice_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.05);
}

TEST_F(QuantraComparisonTest, CalibrateSwaptionModel_ReturnsReasonableParams) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years) };
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_cal_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto modelId = b.CreateString("hw_cal_model");
    quantra::CalibrateSwaptionModelRequestBuilder cb(b);
    cb.add_pricing(pricing);
    cb.add_model_id(modelId);
    b.Finish(cb.Finish());

    CalibrateSwaptionModelPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto out = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());

    EXPECT_GT(out->hw_a(), 0.0);
    EXPECT_GT(out->hw_sigma(), 0.0);
    EXPECT_TRUE(std::isfinite(out->rmse()));
    EXPECT_GT(out->num_helpers(), 0);
}

TEST_F(QuantraComparisonTest, CalibrateSwaptionModel_MatchesDirectQuantLibCalibration) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    std::vector<QuantLib::Period> expiries = {
        QuantLib::Period(1, QuantLib::Years),
        QuantLib::Period(2, QuantLib::Years)
    };
    std::vector<QuantLib::Period> tenors = {
        QuantLib::Period(5, QuantLib::Years),
        QuantLib::Period(7, QuantLib::Years)
    };
    std::vector<double> volsFlat = {0.20, 0.21, 0.22, 0.23};
    auto volSurface = buildSwaptionVolAtmMatrixSurface(b, "swaption_atm", expiries, tenors, volsFlat);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_cal_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto modelId = b.CreateString("hw_cal_model");
    quantra::CalibrateSwaptionModelRequestBuilder cb(b);
    cb.add_pricing(pricing);
    cb.add_model_id(modelId);
    b.Finish(cb.Finish());

    CalibrateSwaptionModelPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto out = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());

    auto reqRoot = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer());
    quantra::PricingRegistryBuilder regBuilder;
    quantra::PricingRegistry reg = regBuilder.build(reqRoot->pricing());
    auto dIt = reg.rates.curves.find("discount");
    auto fIt = reg.rates.curves.find("discount");
    ASSERT_TRUE(dIt != reg.rates.curves.end() && fIt != reg.rates.curves.end());
    QuantLib::Handle<QuantLib::YieldTermStructure> discountCurve(dIt->second->currentLink());
    QuantLib::Handle<QuantLib::YieldTermStructure> forwardingCurve(fIt->second->currentLink());

    auto vIt = reg.volatility.swaptionVols.find("swaption_atm");
    ASSERT_TRUE(vIt != reg.volatility.swaptionVols.end());
    const auto& volEntry = vIt->second;
    ASSERT_TRUE(reg.rates.swapIndices.has("EUR_SWAP_6M"));
    const auto& sidx = reg.rates.swapIndices.get("EUR_SWAP_6M");
    auto ibor = reg.rates.indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);

    const QuantLib::Period fixedLegTenor(sidx.fixedFrequency);
    const QuantLib::DayCounter fixedDc = sidx.fixedDayCounter;
    const QuantLib::DayCounter floatDc = ibor->dayCounter();
    const auto errorType =
        (volEntry.qlVolType == QuantLib::Normal)
            ? QuantLib::BlackCalibrationHelper::PriceError
            : QuantLib::BlackCalibrationHelper::ImpliedVolError;
    const QuantLib::Natural settlementDays = static_cast<QuantLib::Natural>(sidx.spotDays);

    std::vector<QuantLib::ext::shared_ptr<QuantLib::CalibrationHelper>> helpers;
    helpers.reserve(expiries.size() * tenors.size());
    for (const auto& exp : expiries) {
        for (const auto& ten : tenors) {
            const double marketVol = volEntry.handle->volatility(exp, ten, 0.0, true);
            auto volQuote = QuantLib::Handle<QuantLib::Quote>(
                QuantLib::ext::make_shared<QuantLib::SimpleQuote>(marketVol));
            auto helper = QuantLib::ext::make_shared<QuantLib::SwaptionHelper>(
                exp,
                ten,
                volQuote,
                ibor,
                fixedLegTenor,
                fixedDc,
                floatDc,
                discountCurve,
                errorType,
                QuantLib::Null<QuantLib::Real>(),
                1.0,
                volEntry.qlVolType,
                volEntry.displacement,
                settlementDays);
            helpers.push_back(helper);
        }
    }

    auto hwModel = QuantLib::ext::make_shared<QuantLib::HullWhite>(discountCurve, 0.03, 0.01);
    auto engine = QuantLib::ext::make_shared<QuantLib::JamshidianSwaptionEngine>(hwModel);
    for (auto& h : helpers) {
        auto blackHelper = QuantLib::ext::dynamic_pointer_cast<QuantLib::BlackCalibrationHelper>(h);
        ASSERT_TRUE(static_cast<bool>(blackHelper));
        blackHelper->setPricingEngine(engine);
    }

    QuantLib::LevenbergMarquardt lm;
    QuantLib::EndCriteria endCriteria(1000, 200, 1.0e-8, 1.0e-8, 1.0e-8);
    std::vector<bool> fixParams = {false, false};
    hwModel->calibrate(helpers, lm, endCriteria, QuantLib::NoConstraint(), std::vector<double>(), fixParams);
    const auto params = hwModel->params();
    ASSERT_GE(params.size(), 2u);

    double err2 = 0.0;
    for (const auto& h : helpers) {
        const double e = h->calibrationError();
        err2 += e * e;
    }
    const double qlRmse = std::sqrt(err2 / static_cast<double>(helpers.size()));

    EXPECT_NEAR(out->hw_a(), params[0], 1.0e-10);
    EXPECT_NEAR(out->hw_sigma(), params[1], 1.0e-10);
    EXPECT_NEAR(out->rmse(), qlRmse, 1.0e-10);
    EXPECT_EQ(out->num_helpers(), static_cast<int>(helpers.size()));
    EXPECT_EQ(out->grid_rows(), static_cast<int>(expiries.size()));
    EXPECT_EQ(out->grid_cols(), static_cast<int>(tenors.size()));
    EXPECT_EQ(out->grid_points(), static_cast<int>(expiries.size() * tenors.size()));
}

TEST_F(QuantraComparisonTest, PriceSwaption_InlineCalibrate_MatchesEndpoint) {
    const double notional = 1000000.0;
    const double strike = 0.035;

    auto buildSwaptionPricing = [&](flatbuffers::grpc::MessageBuilder& b, bool calibrateMode, double hwA, double hwSigma, const std::string& modelId) {
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
        std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years) };
        auto volSurface = buildSwaptionVolAtmMatrixSurface(
            b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
        auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
        auto model = buildSwaptionModel(
            b, modelId, quantra::enums::IrModelType_HullWhiteLattice, hwA, hwSigma, 50,
            calibrateMode ? quantra::enums::ModelParamMode_Calibrate : quantra::enums::ModelParamMode_Explicit,
            "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
        auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        return buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);
    };

    auto buildBermudanTrade = [&](flatbuffers::grpc::MessageBuilder& b, const std::string& modelId) {
        auto feff = b.CreateString("2026-01-17");
        auto fterm = b.CreateString("2031-01-17");
        quantra::ScheduleBuilder fsb(b);
        fsb.add_effective_date(feff);
        fsb.add_termination_date(fterm);
        fsb.add_calendar(quantra::enums::Calendar_TARGET);
        fsb.add_frequency(quantra::enums::Frequency_Annual);
        fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        auto fixedSch = fsb.Finish();

        quantra::SwapFixedLegBuilder flb(b);
        flb.add_notional(notional);
        flb.add_schedule(fixedSch);
        flb.add_rate(strike);
        flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
        flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto fixedLeg = flb.Finish();

        auto fleff = b.CreateString("2026-01-17");
        auto flterm = b.CreateString("2031-01-17");
        quantra::ScheduleBuilder flsb(b);
        flsb.add_effective_date(fleff);
        flsb.add_termination_date(flterm);
        flsb.add_calendar(quantra::enums::Calendar_TARGET);
        flsb.add_frequency(quantra::enums::Frequency_Semiannual);
        flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        auto floatSch = flsb.Finish();

        auto idx6m = buildIndexRef(b, "EUR_6M");
        quantra::SwapFloatingLegBuilder flgb(b);
        flgb.add_notional(notional);
        flgb.add_schedule(floatSch);
        flgb.add_index(idx6m);
        flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
        flgb.add_spread(0.0);
        flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto floatLeg = flgb.Finish();

        quantra::VanillaSwapBuilder vsb(b);
        vsb.add_swap_type(quantra::enums::SwapType_Payer);
        vsb.add_fixed_leg(fixedLeg);
        vsb.add_floating_leg(floatLeg);
        auto uswap = vsb.Finish();

        std::vector<flatbuffers::Offset<flatbuffers::String>> exDateStrs = {
            b.CreateString("2026-01-15"),
            b.CreateString("2027-01-15"),
            b.CreateString("2028-01-15")
        };
        auto exDatesVec = b.CreateVector(exDateStrs);

        quantra::SwaptionBuilder swb(b);
        swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
        swb.add_underlying(uswap.Union());
        swb.add_exercise_type(quantra::enums::ExerciseType_Bermudan);
        swb.add_settlement_type(quantra::enums::SettlementType_Physical);
        swb.add_exercise_dates(exDatesVec);
        auto swaption = swb.Finish();

        auto dc = b.CreateString("discount");
        auto vol_id = b.CreateString("swaption_atm");
        auto model_id = b.CreateString(modelId);
        quantra::PriceSwaptionBuilder psb(b);
        psb.add_swaption(swaption);
        psb.add_discounting_curve(dc);
        psb.add_forwarding_curve(dc);
        psb.add_volatility(vol_id);
        psb.add_model(model_id);
        return psb.Finish();
    };

    // Endpoint calibration first
    double aStar = 0.0;
    double sigmaStar = 0.0;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildSwaptionPricing(b, true, 0.03, 0.01, "hw_inline_model");
        auto modelId = b.CreateString("hw_inline_model");
        quantra::CalibrateSwaptionModelRequestBuilder cb(b);
        cb.add_pricing(pricing);
        cb.add_model_id(modelId);
        b.Finish(cb.Finish());

        CalibrateSwaptionModelPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        auto out = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());
        aStar = out->hw_a();
        sigmaStar = out->hw_sigma();
    }

    // Inline calibration pricing
    double npvInline = 0.0;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildSwaptionPricing(b, true, 0.03, 0.01, "hw_inline_model");
        auto ps = buildBermudanTrade(b, "hw_inline_model");
        auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{ps});
        quantra::PriceSwaptionRequestBuilder rb(b);
        rb.add_pricing(pricing);
        rb.add_swaptions(swaptions);
        b.Finish(rb.Finish());

        SwaptionPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        npvInline = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();
    }

    // Explicit pricing with calibrated params
    double npvExplicit = 0.0;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildSwaptionPricing(b, false, aStar, sigmaStar, "hw_explicit_model");
        auto ps = buildBermudanTrade(b, "hw_explicit_model");
        auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{ps});
        quantra::PriceSwaptionRequestBuilder rb(b);
        rb.add_pricing(pricing);
        rb.add_swaptions(swaptions);
        b.Finish(rb.Finish());

        SwaptionPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        npvExplicit = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();
    }

    EXPECT_NEAR(npvInline, npvExplicit, 1.0e-8);
}

TEST_F(QuantraComparisonTest, PriceSwaption_InlineCalibrate_CachesPerModel) {
    quantra::resetHwCalibrationCallCount();

    const double notional = 1000000.0;
    const double strike = 0.035;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years) };
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(
        b, "hw_inline_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();

    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    std::vector<flatbuffers::Offset<flatbuffers::String>> exDateStrs = {
        b.CreateString("2026-01-15"),
        b.CreateString("2027-01-15")
    };
    auto exDatesVec = b.CreateVector(exDateStrs);

    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_type(quantra::enums::ExerciseType_Bermudan);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    swb.add_exercise_dates(exDatesVec);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_atm");
    auto model_id = b.CreateString("hw_inline_model");
    quantra::PriceSwaptionBuilder psb1(b);
    psb1.add_swaption(swaption);
    psb1.add_discounting_curve(dc);
    psb1.add_forwarding_curve(dc);
    psb1.add_volatility(vol_id);
    psb1.add_model(model_id);
    auto ps1 = psb1.Finish();

    quantra::PriceSwaptionBuilder psb2(b);
    psb2.add_swaption(swaption);
    psb2.add_discounting_curve(dc);
    psb2.add_forwarding_curve(dc);
    psb2.add_volatility(vol_id);
    psb2.add_model(model_id);
    auto ps2 = psb2.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{ps1, ps2});
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);

    EXPECT_EQ(quantra::getHwCalibrationCallCount(), 1);
}

TEST_F(QuantraComparisonTest, Swaption_ATMMatrix_NPVMatches) {
    std::cout << "\n=== Swaption (ATM Matrix) ===" << std::endl;
    double notional = 1000000.0, strike = 0.035;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(10, QuantLib::Years) };
    QuantLib::Matrix qlVols(2, 2);
    qlVols[0][0] = 0.20; qlVols[0][1] = 0.22;
    qlVols[1][0] = 0.24; qlVols[1][1] = 0.25;
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::SwaptionVolatilityMatrix>(
            evaluationDate_, QuantLib::TARGET(), QuantLib::ModifiedFollowing,
            expiries, tenors, qlVols, QuantLib::Actual365Fixed(), false, QuantLib::ShiftedLognormal, QuantLib::Matrix()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.22, 0.24, 0.25});
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();

    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_atm");
    auto model_id = b.CreateString("black_swaption_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);
    double qNPV = res->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_AtmMatrix2D);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_ConstantMatches) {
    std::cout << "\n=== Swaption (Smile Cube) ===" << std::endl;
    double notional = 1000000.0, strike = 0.02, vol = 0.20;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), vol);

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();

    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_smile");
    auto model_id = b.CreateString("black_swaption_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);
    double qNPV = res->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_SmileCube3D);
    EXPECT_GT(res->used_atm_forward(), 0.0);
    EXPECT_EQ(res->used_strike_kind(), quantra::enums::SwaptionStrikeKind_SpreadFromATM);
    EXPECT_NEAR(
        res->used_strike(),
        res->used_cube_node_atm() + res->used_spread_from_atm(),
        1.0e-12);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_IndexMismatchThrows) {
    double notional = 1000000.0, strike = 0.02, vol = 0.20;
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), vol);

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_3M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b, true);
    auto swapIndices = buildSwapIndicesVector(b, true, true, true);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();

    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_smile");
    auto model_id = b.CreateString("black_swaption_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_ExternalAtmRequiresFlag) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.20);
    std::vector<double> atmForwards = { 0.02 };

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M", atmForwards, false);
    b.Finish(volSurface);

    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()),
            nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_SpreadRequiresAtmSourceAtParseTime) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.20);

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "", {}, false);
    b.Finish(volSurface);

    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()),
            nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_ExternalAtmEqualsInjectedServerAtm) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat = {0.20, 0.21, 0.22};
    std::vector<double> atmForwards = {0.02};

    flatbuffers::grpc::MessageBuilder b1;
    auto volSurface1 = buildSwaptionVolSmileCubeSurface(
        b1, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    b1.Finish(volSurface1);
    auto entryNoAtm = quantra::parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b1.GetBufferPointer()),
        nullptr);
    auto injected = quantra::withSwaptionSmileCubeAtm(entryNoAtm, atmForwards);

    flatbuffers::grpc::MessageBuilder b2;
    auto volSurface2 = buildSwaptionVolSmileCubeSurface(
        b2, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M", atmForwards, true);
    b2.Finish(volSurface2);
    auto external = quantra::parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()),
        nullptr);

    const QuantLib::Date ref(15, QuantLib::January, 2025);
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const double optionTime = dc.yearFraction(ref, cal.advance(ref, expiries[0], bdc));
    const double swapLength = dc.yearFraction(ref, cal.advance(ref, tenors[0], bdc));
    const double absStrike = atmForwards[0] + strikes[1];

    double vInjected = injected.handle->volatility(optionTime, swapLength, absStrike);
    double vExternal = external.handle->volatility(optionTime, swapLength, absStrike);
    EXPECT_NEAR(vInjected, vExternal, 1.0e-12);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_AbsoluteRejectsAtmForwards) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { 0.01, 0.02, 0.03 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.20);
    std::vector<double> atmForwards = { 0.02 };

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M", atmForwards, true);
    b.Finish(volSurface);

    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()),
            nullptr),
        QuantraError);
}

// ======================== SWAPTION (SABR Params) ========================
//
// Synthetic 2x2 grid from the implementation plan:
//   expiries = [1Y, 2Y]
//   tenors   = [5Y, 10Y]
//   beta     = 0.5
//   alpha    = [[0.020, 0.022], [0.023, 0.025]]
//   rho      = [[-0.30, -0.25], [-0.28, -0.22]]
//   nu       = [[0.40, 0.35], [0.38, 0.33]]
namespace {

struct SabrSyntheticGrid {
    std::vector<QuantLib::Period> expiries{
        QuantLib::Period(1, QuantLib::Years),
        QuantLib::Period(2, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors{
        QuantLib::Period(5, QuantLib::Years),
        QuantLib::Period(10, QuantLib::Years)};
    std::vector<double> alpha{0.020, 0.022, 0.023, 0.025};
    std::vector<double> beta{0.5, 0.5, 0.5, 0.5};
    std::vector<double> rho{-0.30, -0.25, -0.28, -0.22};
    std::vector<double> nu{0.40, 0.35, 0.38, 0.33};
};

} // namespace

TEST_F(QuantraComparisonTest, Swaption_SabrParams_ParsePopulatesEntry) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_params", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    b.Finish(vs);

    auto entry = quantra::parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr);

    EXPECT_EQ(entry.volKind, quantra::enums::SwaptionVolKind_SabrParams);
    EXPECT_EQ(entry.strikeKind, quantra::enums::SwaptionStrikeKind_Absolute);
    EXPECT_EQ(entry.swapIndexId, "EUR_SWAP_6M");
    EXPECT_EQ(entry.nExp, 2);
    EXPECT_EQ(entry.nTen, 2);
    ASSERT_EQ(entry.expiries.size(), 2u);
    ASSERT_EQ(entry.tenors.size(), 2u);
    ASSERT_EQ(entry.sabrAlpha.size(), 4u);
    ASSERT_EQ(entry.sabrBeta.size(), 4u);
    ASSERT_EQ(entry.sabrRho.size(), 4u);
    ASSERT_EQ(entry.sabrNu.size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(entry.sabrAlpha[i], g.alpha[i]);
        EXPECT_DOUBLE_EQ(entry.sabrBeta[i], g.beta[i]);
        EXPECT_DOUBLE_EQ(entry.sabrRho[i], g.rho[i]);
        EXPECT_DOUBLE_EQ(entry.sabrNu[i], g.nu[i]);
    }
    // Handle is intentionally deferred until finalize injects forwards.
    EXPECT_TRUE(entry.handle.empty());
    EXPECT_TRUE(entry.atmForwardsFlat.empty());
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_RejectsDimensionMismatch) {
    SabrSyntheticGrid g;
    // Alpha grid claims 2x2 but actually holds only 3 values.
    std::vector<double> badAlpha{0.020, 0.022, 0.023};

    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_params_bad", g.expiries, g.tenors, badAlpha, g.beta, g.rho, g.nu);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);

    // Matrix dimension override mismatches the periods grid.
    flatbuffers::grpc::MessageBuilder b2;
    auto vs2 = buildSwaptionSabrParamsSurface(
        b2, "sabr_params_bad2", g.expiries, g.tenors,
        g.alpha, g.beta, g.rho, g.nu, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Lognormal, 0.0, "2025-01-15",
        /*matrixRowsOverride=*/3, /*matrixColsOverride=*/2);
    b2.Finish(vs2);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_RejectsInvalidParameterRanges) {
    SabrSyntheticGrid g;

    auto expectThrow = [&](const std::vector<double>& alpha,
                           const std::vector<double>& beta,
                           const std::vector<double>& rho,
                           const std::vector<double>& nu,
                           const std::string& tag) {
        flatbuffers::grpc::MessageBuilder b;
        auto vs = buildSwaptionSabrParamsSurface(
            b, "sabr_invalid_" + tag, g.expiries, g.tenors, alpha, beta, rho, nu);
        b.Finish(vs);
        EXPECT_THROW(
            quantra::parseSwaptionVol(
                flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
            QuantraError) << tag;
    };

    auto badAlpha = g.alpha; badAlpha[0] = 0.0;       // alpha must be > 0
    expectThrow(badAlpha, g.beta, g.rho, g.nu, "alpha_zero");

    auto badAlpha2 = g.alpha; badAlpha2[1] = -0.01;
    expectThrow(badAlpha2, g.beta, g.rho, g.nu, "alpha_negative");

    auto badBeta = g.beta; badBeta[2] = 1.5;          // beta must be in [0, 1]
    expectThrow(g.alpha, badBeta, g.rho, g.nu, "beta_above");

    auto badBeta2 = g.beta; badBeta2[3] = -0.01;
    expectThrow(g.alpha, badBeta2, g.rho, g.nu, "beta_below");

    auto badRhoLow = g.rho; badRhoLow[0] = -1.0;      // rho must be in (-1, 1)
    expectThrow(g.alpha, g.beta, badRhoLow, g.nu, "rho_low");

    auto badRhoHigh = g.rho; badRhoHigh[1] = 1.0;
    expectThrow(g.alpha, g.beta, badRhoHigh, g.nu, "rho_high");

    auto badNu = g.nu; badNu[2] = 0.0;                // nu must be > 0
    expectThrow(g.alpha, g.beta, g.rho, badNu, "nu_zero");

    auto badNu2 = g.nu; badNu2[3] = -0.01;
    expectThrow(g.alpha, g.beta, g.rho, badNu2, "nu_negative");
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_RejectsNormalVolType) {
    SabrSyntheticGrid g;
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_normal", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu,
        "EUR_SWAP_6M", quantra::enums::VolatilityType_Normal, 0.0);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrParamsReturnsFinitePositiveVols) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.02, 0.03, 0.04, 0.05});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("sabr_vol");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    if (r->error() != nullptr) {
        FAIL() << "sample failed: "
               << (r->error()->error_message() ? r->error()->error_message()->c_str() : "(no msg)");
    }
    // 2 expiries x 2 tenors x 4 strikes
    ASSERT_EQ(r->vols()->size(), 16u);
    for (flatbuffers::uoffset_t i = 0; i < r->vols()->size(); ++i) {
        double v = r->vols()->Get(i);
        EXPECT_TRUE(std::isfinite(v)) << "non-finite vol at index " << i;
        EXPECT_GT(v, 0.0) << "non-positive vol at index " << i;
    }
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_PriceWithBlackProducesFinitePositiveNpv) {
    SabrSyntheticGrid g;
    const double notional = 1000000.0;
    const double strike = 0.04;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("sabr_vol");
    auto model_id = b.CreateString("black_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);
    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_SabrParams);
    EXPECT_TRUE(std::isfinite(res->npv()));
    EXPECT_GT(res->npv(), 0.0);
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_BachelierEnginePairingRejected) {
    SabrSyntheticGrid g;
    const double notional = 1000000.0;
    const double strike = 0.04;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("sabr_vol");
    auto model_id = b.CreateString("bachelier_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer())),
        QuantraError);
}

// ======================== SWAPTION (SABR Calibrate) ========================
//
// Step 6 — formal test suite for the calibrate path.
//
// All cases reuse the synthetic 2x2 SabrSyntheticGrid declared above. The
// round-trip case computes synthetic market vols from the known SABR
// parameter grid using QuantLib::SabrSmileSection (the same Hagan formula
// the calibrator fits against), drives them through the standalone
// /calibrate-swaption-vol handler, and confirms the recovered (alpha, beta,
// rho, nu) match the originals to within tight tolerance on noiseless data.

namespace {

// Compute synthetic market vols for the calibrate-path round-trip test.
// `forwards` is row-major nExp*nTen, `spreads` is per-strike. Returns a
// row-major nExp*nTen*nStrikes vector ready to feed into a
// SwaptionSabrCalibrateSpec.
std::vector<double> sabrSyntheticMarketVols(
    const SabrSyntheticGrid& g,
    const std::vector<double>& forwards,
    const std::vector<QuantLib::Real>& timesToExpiry,
    const std::vector<double>& spreads,
    double displacement = 0.0) {
    const int nExp = static_cast<int>(g.expiries.size());
    const int nTen = static_cast<int>(g.tenors.size());
    const int nStr = static_cast<int>(spreads.size());
    std::vector<double> vols(static_cast<size_t>(nExp * nTen * nStr), 0.0);
    for (int i = 0; i < nExp; ++i) {
        for (int j = 0; j < nTen; ++j) {
            const int k = i * nTen + j;
            // SabrSmileSection parameter order: alpha, beta, nu, rho.
            std::vector<QuantLib::Real> params{g.alpha[k], g.beta[k], g.nu[k], g.rho[k]};
            QuantLib::SabrSmileSection section(
                timesToExpiry[k], forwards[k], params, displacement,
                QuantLib::ShiftedLognormal);
            for (int s = 0; s < nStr; ++s) {
                vols[k * nStr + s] = section.volatility(forwards[k] + spreads[s]);
            }
        }
    }
    return vols;
}

} // namespace

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RoundTripRecoversParameters) {
    // Correctness anchor: feed exact SABR-formula vols into the calibrate
    // path; LM should recover the original parameters near-exactly.
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};

    // ---- Phase 1: finalize a SabrParams surface to obtain per-node ATM
    // forwards from the production code path. The same forwards will be
    // used by the calibrate finalize, guaranteeing the synthetic vols line
    // up with the calibrator's strike axis.
    std::vector<double> forwards;
    std::vector<QuantLib::Real> tte;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto curve = buildLongCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto paramsSurface = buildSwaptionSabrParamsSurface(
            b, "sabr_params_seed", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
        auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{paramsSurface});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
        b.Finish(pricing);

        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        auto reg = quantra::PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
        auto& entry = reg.volatility.swaptionVols.at("sabr_params_seed");
        auto dCurve = reg.rates.curves.at("discount");
        QuantLib::Handle<QuantLib::YieldTermStructure> dHandle(dCurve->currentLink());
        auto finalEntry = quantra::finalizeSwaptionVolEntryForPricing(
            entry, nullptr, reg, dHandle, dHandle, false, "discount", "discount");
        forwards = finalEntry.atmForwardsFlat;
        ASSERT_EQ(forwards.size(), 4u);
        tte.resize(4);
        for (int i = 0; i < 2; ++i) {
            QuantLib::Date exercise = finalEntry.calendar.advance(
                finalEntry.referenceDate, finalEntry.expiries[i],
                finalEntry.businessDayConvention);
            double t = finalEntry.dayCounter.yearFraction(finalEntry.referenceDate, exercise);
            tte[i * 2 + 0] = t;
            tte[i * 2 + 1] = t;
        }
    }

    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    // ---- Phase 2: drive the synthetic vols through /calibrate-swaption-vol.
    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto calibSurface = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{calibSurface});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto volIdOff = b.CreateString("sabr_calibrate");
    auto discIdOff = b.CreateString("discount");
    auto fwdIdOff = b.CreateString("discount");
    quantra::CalibrateSwaptionVolRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_vol_id(volIdOff);
    rb.add_discounting_curve_id(discIdOff);
    rb.add_forwarding_curve_id(fwdIdOff);
    b.Finish(rb.Finish());

    CalibrateSwaptionVolPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto respOff = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(b.GetBufferPointer()));
    outBuilder->Finish(respOff);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateSwaptionVolResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp, nullptr);
    ASSERT_NE(resp->diagnostics(), nullptr);
    auto* diag = resp->diagnostics();
    ASSERT_NE(diag->alpha_per_node(), nullptr);
    ASSERT_EQ(diag->alpha_per_node()->size(), 4u);

    // Tolerances: alpha/rho/nu within 1e-4 absolute on noiseless Hagan data;
    // beta exactly equal because beta_fixed=true clamps it during the fit.
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(diag->alpha_per_node()->Get(k), g.alpha[k], 1e-4) << "alpha node " << k;
        EXPECT_DOUBLE_EQ(diag->beta_per_node()->Get(k), g.beta[k]) << "beta node " << k;
        EXPECT_NEAR(diag->rho_per_node()->Get(k), g.rho[k], 1e-4) << "rho node " << k;
        EXPECT_NEAR(diag->nu_per_node()->Get(k), g.nu[k], 1e-4) << "nu node " << k;
    }

    auto* calib = diag->calibration();
    ASSERT_NE(calib, nullptr);
    EXPECT_TRUE(calib->converged());
    EXPECT_LT(calib->overall_rmse(), 1e-6);
    ASSERT_EQ(calib->per_node_rmse()->size(), 4u);
    for (int k = 0; k < 4; ++k) {
        EXPECT_LT(calib->per_node_rmse()->Get(k), 1e-6) << "rmse node " << k;
    }

    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsMismatchedDimensions) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> okVols(2 * 2 * 5, 0.20);

    // Tensor n_1/n_2/n_3 dims declared incorrectly relative to expiries/tenors/strikes.
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_dim_bad", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Lognormal, 0.0, "2025-01-15",
        /*n1Override=*/3, /*n2Override=*/2, /*n3Override=*/5);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);

    // Strike count too small for beta_fixed=true (needs >=3) — also rejected.
    flatbuffers::grpc::MessageBuilder b2;
    const std::vector<double> shortSpreads{0.0, 0.01};
    const std::vector<double> shortVols(2 * 2 * 2, 0.20);
    auto vs2 = buildSwaptionSabrCalibrateSurface(
        b2, "sabr_calibrate_strikes_short", g.expiries, g.tenors, shortSpreads, shortVols);
    b2.Finish(vs2);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsNonPositiveVols) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> badVols(2 * 2 * 5, 0.20);
    badVols[7] = 0.0;
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_zero_vol", g.expiries, g.tenors, spreads, badVols);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);

    std::vector<double> negVols(2 * 2 * 5, 0.20);
    negVols[3] = -0.01;
    flatbuffers::grpc::MessageBuilder b2;
    auto vs2 = buildSwaptionSabrCalibrateSurface(
        b2, "sabr_calibrate_neg_vol", g.expiries, g.tenors, spreads, negVols);
    b2.Finish(vs2);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsNormalVolType) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> okVols(2 * 2 * 5, 0.01); // normal-scale, but rejection happens before vol shape check
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_normal", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Normal, 0.0);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsOisSwapIndex) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> okVols(2 * 2 * 5, 0.20);

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    // Indices vector includes EUR_6M plus USD_SOFR (the OIS underlying).
    std::vector<flatbuffers::Offset<quantra::IndexDef>> idxDefs;
    idxDefs.push_back(buildIndexDef_EUR6M(b));
    idxDefs.push_back(buildIndexDef_USD_SOFR(b));
    auto indices = b.CreateVector(idxDefs);
    auto swapIndices = buildSwapIndicesVector(b, /*includeEur6m=*/true, /*includeOis=*/true);
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_ois", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, /*swapIndexId=*/"USD_SOFR_OIS");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
    b.Finish(pricing);

    // Rejection lives at registry-build time (vol-kind cross-check against
    // swap_indices), not at parseSwaptionVol time.
    EXPECT_THROW(
        quantra::PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsTooSmallGrid) {
    // Single expiry violates the >= 2x2 minimum required by QuantLib's cube
    // interpolators.
    std::vector<QuantLib::Period> oneExp{QuantLib::Period(1, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors{
        QuantLib::Period(5, QuantLib::Years), QuantLib::Period(10, QuantLib::Years)};
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> vols(1 * 2 * 5, 0.20);
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_thin", oneExp, tenors, spreads, vols);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsNonEmptyWeights) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> okVols(2 * 2 * 5, 0.20);
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_weights", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Lognormal, 0.0, "2025-01-15",
        -1, -1, -1, /*addNonEmptyWeights=*/true);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_CacheBehavior) {
    // Direct cache-state observability: clear, populate via finalize with a
    // synthetic key, observe size() transitions, confirm key sensitivity.
    SabrCalibrateCache::instance().clear();
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 0u);

    // Build a SwaptionVolEntry and a swap-index base via the production path,
    // then call withSwaptionSabrCalibrateAtm twice with the same explicit key.
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    // Use synthetic vols so the calibrator has a clean target.
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_cache", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
    b.Finish(pricing);

    QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
    auto reg = quantra::PricingRegistryBuilder().build(
        flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
    auto entry = reg.volatility.swaptionVols.at("sabr_calibrate_cache");
    auto dCurve = reg.rates.curves.at("discount");
    QuantLib::Handle<QuantLib::YieldTermStructure> dHandle(dCurve->currentLink());

    // Compute the same forwards the production path would, by hand.
    auto& sidx = reg.rates.swapIndices.get(entry.swapIndexId);
    auto computedForwards = quantra::computeServerAtmForwards(
        entry, sidx, reg.rates.indices, dHandle, dHandle);
    auto swapIndexBase = reg.rates.swapIndices.getIborSwapIndexWithCurves(
        entry.swapIndexId, entry.tenors.front(), reg.rates.indices, dHandle, dHandle);

    // First call: MISS + PUT, size 0 -> 1.
    auto out1 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "test-key-A");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 1u);
    EXPECT_EQ(out1.sabrAlpha.size(), 4u);

    // Second call same key: HIT, size still 1.
    auto out2 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "test-key-A");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 1u);
    // Cache HIT must return the same calibrated parameters.
    ASSERT_EQ(out2.sabrAlpha.size(), out1.sabrAlpha.size());
    for (size_t k = 0; k < out1.sabrAlpha.size(); ++k) {
        EXPECT_DOUBLE_EQ(out2.sabrAlpha[k], out1.sabrAlpha[k]);
        EXPECT_DOUBLE_EQ(out2.sabrRho[k], out1.sabrRho[k]);
        EXPECT_DOUBLE_EQ(out2.sabrNu[k], out1.sabrNu[k]);
    }

    // Third call different key: MISS + PUT, size grows to 2.
    auto out3 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "test-key-B");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 2u);
    EXPECT_EQ(out3.sabrAlpha.size(), 4u);

    // Empty cache key bypasses cache entirely: size unchanged.
    auto out4 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 2u);
    EXPECT_EQ(out4.sabrAlpha.size(), 4u);

    // Key sensitivity: perturb one market-vol cell by 1bp and confirm the
    // canonical cache key changes. Same for a different curve cache key.
    auto entryPerturbed = entry;
    entryPerturbed.sabrMarketVolsFlat[2] += 0.0001;
    const std::string keyA = quantra::buildSabrCalibrateCacheKey(
        entry, computedForwards, "disc-key-1", "fwd-key-1");
    const std::string keyAPert = quantra::buildSabrCalibrateCacheKey(
        entryPerturbed, computedForwards, "disc-key-1", "fwd-key-1");
    EXPECT_NE(keyA, keyAPert);
    const std::string keyB = quantra::buildSabrCalibrateCacheKey(
        entry, computedForwards, "disc-key-2", "fwd-key-1");
    EXPECT_NE(keyA, keyB);

    // Bypass-when-curve-cache-disabled: finalize via the production path
    // with curve cache disabled (default in tests). Cache size stays put
    // because cube key is empty.
    const size_t sizeBefore = SabrCalibrateCache::instance().size();
    auto finalEntry = quantra::finalizeSwaptionVolEntryForPricing(
        entry, nullptr, reg, dHandle, dHandle, false, "discount", "discount");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), sizeBefore);
    EXPECT_EQ(finalEntry.sabrAlpha.size(), 4u);

    SabrCalibrateCache::instance().clear();
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrCalibrateReturnsFiniteVols) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_sample", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.02, 0.03, 0.04});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("sabr_calibrate_sample");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    if (r->error() != nullptr) {
        FAIL() << "sample failed: "
               << (r->error()->error_message() ? r->error()->error_message()->c_str() : "(no msg)");
    }
    ASSERT_EQ(r->vols()->size(), 12u);
    for (flatbuffers::uoffset_t i = 0; i < r->vols()->size(); ++i) {
        double v = r->vols()->Get(i);
        EXPECT_TRUE(std::isfinite(v)) << "non-finite vol at index " << i;
        EXPECT_GT(v, 0.0) << "non-positive vol at index " << i;
    }
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_PriceWithBlackEqualsParamsPath) {
    // Architectural invariant: SabrCalibrate and SabrParams converge into the
    // same SwaptionVolEntry post-finalize, so pricing the same swaption
    // through either surface (with the same parameters) must produce the
    // same NPV within tolerance.
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const double notional = 1000000.0;
    const double strike = 0.04;

    // ---- Pre-finalize SabrParams to get authoritative forwards.
    std::vector<double> forwards;
    std::vector<QuantLib::Real> tte;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto curve = buildLongCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto paramsSurface = buildSwaptionSabrParamsSurface(
            b, "sabr_params_seed", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
        auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{paramsSurface});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
        b.Finish(pricing);

        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        auto reg = quantra::PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
        auto& entry = reg.volatility.swaptionVols.at("sabr_params_seed");
        auto dCurve = reg.rates.curves.at("discount");
        QuantLib::Handle<QuantLib::YieldTermStructure> dHandle(dCurve->currentLink());
        auto finalEntry = quantra::finalizeSwaptionVolEntryForPricing(
            entry, nullptr, reg, dHandle, dHandle, false, "discount", "discount");
        forwards = finalEntry.atmForwardsFlat;
        tte.resize(4);
        for (int i = 0; i < 2; ++i) {
            QuantLib::Date exercise = finalEntry.calendar.advance(
                finalEntry.referenceDate, finalEntry.expiries[i],
                finalEntry.businessDayConvention);
            double t = finalEntry.dayCounter.yearFraction(finalEntry.referenceDate, exercise);
            tte[i * 2 + 0] = t;
            tte[i * 2 + 1] = t;
        }
    }

    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    auto buildPriceRequest = [&](bool useCalibrate) {
        auto b = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto ts = buildLongCurve(*b, "discount");
        auto curves = b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        flatbuffers::Offset<quantra::VolSurfaceSpec> volSurface;
        if (useCalibrate) {
            volSurface = buildSwaptionSabrCalibrateSurface(
                *b, "sabr_vol", g.expiries, g.tenors, spreads, syntheticVols);
        } else {
            volSurface = buildSwaptionSabrParamsSurface(
                *b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
        }
        auto vols = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
        auto model = buildSwaptionModel(*b, "black_model", quantra::enums::IrModelType_Black);
        auto models = b->CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

        auto feff = b->CreateString("2026-01-17");
        auto fterm = b->CreateString("2031-01-17");
        quantra::ScheduleBuilder fsb(*b);
        fsb.add_effective_date(feff);
        fsb.add_termination_date(fterm);
        fsb.add_calendar(quantra::enums::Calendar_TARGET);
        fsb.add_frequency(quantra::enums::Frequency_Annual);
        fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        auto fixedSch = fsb.Finish();
        quantra::SwapFixedLegBuilder flb(*b);
        flb.add_notional(notional);
        flb.add_schedule(fixedSch);
        flb.add_rate(strike);
        flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
        flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto fixedLeg = flb.Finish();

        auto fleff = b->CreateString("2026-01-17");
        auto flterm = b->CreateString("2031-01-17");
        quantra::ScheduleBuilder flsb(*b);
        flsb.add_effective_date(fleff);
        flsb.add_termination_date(flterm);
        flsb.add_calendar(quantra::enums::Calendar_TARGET);
        flsb.add_frequency(quantra::enums::Frequency_Semiannual);
        flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        auto floatSch = flsb.Finish();
        auto idx6m = buildIndexRef(*b, "EUR_6M");
        quantra::SwapFloatingLegBuilder flgb(*b);
        flgb.add_notional(notional);
        flgb.add_schedule(floatSch);
        flgb.add_index(idx6m);
        flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
        flgb.add_spread(0.0);
        flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto floatLeg = flgb.Finish();

        quantra::VanillaSwapBuilder vsb(*b);
        vsb.add_swap_type(quantra::enums::SwapType_Payer);
        vsb.add_fixed_leg(fixedLeg);
        vsb.add_floating_leg(floatLeg);
        auto uswap = vsb.Finish();

        auto exd = b->CreateString("2026-01-15");
        quantra::SwaptionBuilder swb(*b);
        swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
        swb.add_underlying(uswap.Union());
        swb.add_exercise_date(exd);
        swb.add_exercise_type(quantra::enums::ExerciseType_European);
        swb.add_settlement_type(quantra::enums::SettlementType_Physical);
        auto swaption = swb.Finish();

        auto dc = b->CreateString("discount");
        auto vol_id = b->CreateString("sabr_vol");
        auto model_id = b->CreateString("black_model");
        quantra::PriceSwaptionBuilder psb(*b);
        psb.add_swaption(swaption);
        psb.add_discounting_curve(dc);
        psb.add_forwarding_curve(dc);
        psb.add_volatility(vol_id);
        psb.add_model(model_id);
        auto psbOff = psb.Finish();
        auto swaptions = b->CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

        quantra::PriceSwaptionRequestBuilder rb(*b);
        rb.add_pricing(pricing);
        rb.add_swaptions(swaptions);
        b->Finish(rb.Finish());
        return b;
    };

    auto runPrice = [&](bool useCalibrate) {
        auto b = buildPriceRequest(useCalibrate);
        SwaptionPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(
            respB,
            flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b->GetBufferPointer()));
        respB->Finish(resp);
        return flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(
                   respB->GetBufferPointer())->swaptions()->Get(0)->npv();
    };

    double npvParams = runPrice(/*useCalibrate=*/false);
    double npvCalibrate = runPrice(/*useCalibrate=*/true);
    EXPECT_TRUE(std::isfinite(npvParams));
    EXPECT_GT(npvParams, 0.0);
    EXPECT_TRUE(std::isfinite(npvCalibrate));
    EXPECT_GT(npvCalibrate, 0.0);
    // Tolerance reflects calibrator rounding and Hagan-vs-cube interpolation
    // residual; 5bp of NPV is comfortable for noiseless data.
    EXPECT_NEAR(npvCalibrate, npvParams, std::max(1.0, npvParams * 1e-4));
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_BachelierEnginePairingRejected) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);
    const double notional = 1000000.0;
    const double strike = 0.04;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionSabrCalibrateSurface(
        b, "sabr_vol", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("sabr_vol");
    auto model_id = b.CreateString("bachelier_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer())),
        QuantraError);
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrCalibrateDiagnosticsPopulated) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    auto buildSampleRequest = [&](bool includeDiagnostics) {
        auto b = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto curve = buildLongCurve(*b, "discount");
        auto curves = b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto vs = buildSwaptionSabrCalibrateSurface(
            *b, "sabr_calibrate_diag", g.expiries, g.tenors, spreads, syntheticVols);
        auto vols = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

        quantra::PeriodBuilder e1(*b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
        auto e1Off = e1.Finish();
        quantra::PeriodBuilder e2(*b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
        auto e2Off = e2.Finish();
        auto expVec = b->CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
        quantra::TenorGridBuilder expGridB(*b); expGridB.add_tenors(expVec);
        auto expGrid = expGridB.Finish();
        quantra::DateGridSpecBuilder expSpecB(*b);
        expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
        expSpecB.add_grid(expGrid.Union());
        auto expSpec = expSpecB.Finish();

        quantra::PeriodBuilder t5(*b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
        auto t5Off = t5.Finish();
        quantra::PeriodBuilder t10(*b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
        auto t10Off = t10.Finish();
        auto tenVec = b->CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
        quantra::TenorGridBuilder tenGridB(*b); tenGridB.add_tenors(tenVec);
        auto tenGrid = tenGridB.Finish();
        quantra::DateGridSpecBuilder tenSpecB(*b);
        tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
        tenSpecB.add_grid(tenGrid.Union());
        auto tenSpec = tenSpecB.Finish();

        auto strikeVals = b->CreateVector(std::vector<double>{0.02, 0.03, 0.04});
        quantra::StrikeGridBuilder sgb(*b);
        sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
        sgb.add_strikes(strikeVals);
        auto strikeGrid = sgb.Finish();

        auto disc = b->CreateString("discount");
        auto fwd = b->CreateString("discount");
        auto volId = b->CreateString("sabr_calibrate_diag");
        auto swapIdx = b->CreateString("EUR_SWAP_6M");
        quantra::VolQuerySpecBuilder qsb(*b);
        qsb.add_vol_id(volId);
        qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
        qsb.add_expiry_grid(expSpec);
        qsb.add_tenor_grid(tenSpec);
        qsb.add_strike_grid(strikeGrid);
        qsb.add_swap_index_id(swapIdx);
        qsb.add_discounting_curve_id(disc);
        qsb.add_forwarding_curve_id(fwd);
        auto query = qsb.Finish();
        auto queries = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
        quantra::SampleVolSurfacesRequestBuilder rb(*b);
        rb.add_pricing(pricing);
        rb.add_queries(queries);
        rb.add_include_diagnostics(includeDiagnostics);
        b->Finish(rb.Finish());
        return b;
    };

    auto runSample = [&](bool includeDiagnostics) {
        auto b = buildSampleRequest(includeDiagnostics);
        SampleVolSurfacesRequestHandler handler;
        auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto out = handler.request(
            outBuilder,
            flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b->GetBufferPointer()));
        outBuilder->Finish(out);
        return std::make_pair(
            outBuilder,
            flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer()));
    };

    // include_diagnostics=false: response carries no diagnostics vector.
    auto noDiag = runSample(false);
    auto* respNo = noDiag.second;
    EXPECT_TRUE(respNo->diagnostics() == nullptr || respNo->diagnostics()->size() == 0u)
        << "diagnostics array unexpectedly present when include_diagnostics=false";

    // include_diagnostics=true: one populated SwaptionVolDiagnostics with full
    // surface block + calibration sub-block + converged=true.
    auto withDiag = runSample(true);
    auto* respYes = withDiag.second;
    ASSERT_NE(respYes->diagnostics(), nullptr);
    ASSERT_EQ(respYes->diagnostics()->size(), 1u);
    auto* d = respYes->diagnostics()->Get(0);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind(), quantra::enums::SwaptionVolKind_SabrCalibrate);
    ASSERT_NE(d->alpha_per_node(), nullptr);
    EXPECT_EQ(d->alpha_per_node()->size(), 4u);
    ASSERT_NE(d->beta_per_node(), nullptr);
    ASSERT_NE(d->rho_per_node(), nullptr);
    ASSERT_NE(d->nu_per_node(), nullptr);
    ASSERT_NE(d->forward_per_node(), nullptr);
    ASSERT_NE(d->atm_vol_per_node(), nullptr);
    auto* calib = d->calibration();
    ASSERT_NE(calib, nullptr) << "calibration sub-block missing on SabrCalibrate";
    EXPECT_TRUE(calib->converged());
    EXPECT_TRUE(std::isfinite(calib->overall_rmse()));
    ASSERT_NE(calib->per_node_rmse(), nullptr);
    EXPECT_EQ(calib->per_node_rmse()->size(), 4u);
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrParamsDiagnosticsHasNoCalibrationBlock) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_params_diag", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.03});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("sabr_params_diag");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    rb.add_include_diagnostics(true);
    b.Finish(rb.Finish());

    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer()));
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp->diagnostics(), nullptr);
    ASSERT_EQ(resp->diagnostics()->size(), 1u);
    auto* d = resp->diagnostics()->Get(0);
    EXPECT_EQ(d->kind(), quantra::enums::SwaptionVolKind_SabrParams);
    // SABR-params surface: surface block populated, calibration sub-block null.
    ASSERT_NE(d->alpha_per_node(), nullptr);
    EXPECT_EQ(d->alpha_per_node()->size(), 4u);
    ASSERT_NE(d->forward_per_node(), nullptr);
    ASSERT_NE(d->atm_vol_per_node(), nullptr);
    EXPECT_EQ(d->calibration(), nullptr)
        << "params-path diagnostics must not carry a calibration sub-block";
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_DiagnosticsEmitsHighRmseWarnings) {
    // Construct a deliberately non-SABR-shaped market vol cube (vol levels far
    // apart at neighbouring strikes — Hagan can't fit cleanly) and expect at
    // least one warning naming high RMSE on a node. Threshold (50bp RMSE) lives
    // in swaption_vol_diagnostics.cpp.
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    // Per-node strike-vol pattern is a symmetric W around a 20% ATM with
    // ±100 bp amplitude. SABR (Hagan, beta_fixed=0.5) cannot fit a W shape
    // — best fit is a flat/skewed smile, leaving residuals around 70-90 bp
    // per strike. That lands above the 50 bp diagnostics warning threshold
    // but below QuantLib's 1% rmsError hard throw inside
    // XabrSwaptionVolatilityCube::performCalculations, so calibration
    // completes and the warnings array is populated.
    std::vector<double> badVols;
    badVols.reserve(2 * 2 * 5);
    for (int n = 0; n < 4; ++n) {
        badVols.insert(badVols.end(), {0.21, 0.19, 0.21, 0.19, 0.21});
    }

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_warn", g.expiries, g.tenors, spreads, badVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto volIdOff = b.CreateString("sabr_calibrate_warn");
    auto discIdOff = b.CreateString("discount");
    auto fwdIdOff = b.CreateString("discount");
    quantra::CalibrateSwaptionVolRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_vol_id(volIdOff);
    rb.add_discounting_curve_id(discIdOff);
    rb.add_forwarding_curve_id(fwdIdOff);
    b.Finish(rb.Finish());

    CalibrateSwaptionVolPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto respOff = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(b.GetBufferPointer()));
    outBuilder->Finish(respOff);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateSwaptionVolResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp, nullptr);
    auto* d = resp->diagnostics();
    ASSERT_NE(d, nullptr);
    ASSERT_NE(d->warnings(), nullptr);
    EXPECT_GT(d->warnings()->size(), 0u)
        << "expected at least one warning on a deliberately bad SABR fit";
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, CalibrateSwaptionVolEndpoint_HappyPath) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_endpoint", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto volIdOff = b.CreateString("sabr_calibrate_endpoint");
    auto discIdOff = b.CreateString("discount");
    auto fwdIdOff = b.CreateString("discount");
    quantra::CalibrateSwaptionVolRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_vol_id(volIdOff);
    rb.add_discounting_curve_id(discIdOff);
    rb.add_forwarding_curve_id(fwdIdOff);
    b.Finish(rb.Finish());

    CalibrateSwaptionVolPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto respOff = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(b.GetBufferPointer()));
    outBuilder->Finish(respOff);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateSwaptionVolResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp, nullptr);
    ASSERT_NE(resp->vol_id(), nullptr);
    EXPECT_EQ(resp->vol_id()->str(), "sabr_calibrate_endpoint");
    auto* d = resp->diagnostics();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind(), quantra::enums::SwaptionVolKind_SabrCalibrate);
    ASSERT_NE(d->alpha_per_node(), nullptr);
    EXPECT_EQ(d->alpha_per_node()->size(), 4u);
    auto* calib = d->calibration();
    ASSERT_NE(calib, nullptr);
    EXPECT_TRUE(calib->converged());
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, CalibrateSwaptionVolEndpoint_RejectsBadInputs) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    auto buildBaseContext = [&](
            std::function<void(flatbuffers::grpc::MessageBuilder&,
                               std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>&)> volSurfaceFn,
            const std::string& volId,
            const std::string& discId,
            const std::string& fwdId) {
        auto b = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto curve = buildLongCurve(*b, "discount");
        auto curves = b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>> volSurfaces;
        volSurfaceFn(*b, volSurfaces);
        auto vols = b->CreateVector(volSurfaces);
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

        auto volIdOff = b->CreateString(volId);
        auto discIdOff = b->CreateString(discId);
        auto fwdIdOff = b->CreateString(fwdId);
        quantra::CalibrateSwaptionVolRequestBuilder rb(*b);
        rb.add_pricing(pricing);
        rb.add_vol_id(volIdOff);
        rb.add_discounting_curve_id(discIdOff);
        rb.add_forwarding_curve_id(fwdIdOff);
        b->Finish(rb.Finish());
        return b;
    };

    auto callHandler = [](flatbuffers::grpc::MessageBuilder& msgB) {
        CalibrateSwaptionVolPricingRequest handler;
        auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        return handler.request(
            outBuilder,
            flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(msgB.GetBufferPointer()));
    };

    // (a) vol_id absent from the registry -> NOT_FOUND.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrCalibrateSurface(
                    b, "sabr_calibrate_present", g.expiries, g.tenors, spreads, syntheticVols));
            },
            /*volId=*/"sabr_calibrate_missing",
            /*discId=*/"discount",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraNotFound);
    }

    // (b) vol_id resolves to AtmMatrix2D (non-SabrCalibrate) -> INVALID_ARGUMENT.
    {
        std::vector<double> atmVols{0.20, 0.21, 0.22, 0.23};
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionVolAtmMatrixSurface(
                    b, "atm_matrix", g.expiries, g.tenors, atmVols));
            },
            /*volId=*/"atm_matrix",
            /*discId=*/"discount",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }

    // (c) vol_id resolves to SabrParams (also non-SabrCalibrate) -> INVALID_ARGUMENT.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrParamsSurface(
                    b, "sabr_params_only", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu));
            },
            /*volId=*/"sabr_params_only",
            /*discId=*/"discount",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }

    // (d) discounting_curve_id missing from registry -> INVALID_ARGUMENT.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrCalibrateSurface(
                    b, "sabr_calibrate_present", g.expiries, g.tenors, spreads, syntheticVols));
            },
            /*volId=*/"sabr_calibrate_present",
            /*discId=*/"missing_curve",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }

    // (e) forwarding_curve_id missing from registry -> INVALID_ARGUMENT.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrCalibrateSurface(
                    b, "sabr_calibrate_present", g.expiries, g.tenors, spreads, syntheticVols));
            },
            /*volId=*/"sabr_calibrate_present",
            /*discId=*/"discount",
            /*fwdId=*/"missing_fwd_curve");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }
    SabrCalibrateCache::instance().clear();
}

// ======================== SWAPTION (Bachelier / Normal vols) ========================
TEST_F(QuantraComparisonTest, Swaption_Bachelier_NPVMatches) {
    std::cout << "\n=== Swaption (Bachelier) ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.01; // Normal vol
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed(), QuantLib::Normal));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BachelierSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    auto volSurface = buildSwaptionVolSurface(
        b, "swaption_vol_norm", vol, quantra::enums::VolatilityType_Normal, 0.0, "swaption_vol_quote");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(b, "bachelier_swaption_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto quote_id = b.CreateString("swaption_vol_quote");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quote_id);
    qb.add_kind(quantra::QuoteKind_Rate);
    qb.add_value(vol);
    qb.add_quote_type(quantra::QuoteType_Volatility);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, vols, models);

    // Fixed leg schedule
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    // Float leg schedule
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();

    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_vol_norm");
    auto model_id = b.CreateString("bachelier_swaption_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// ======================== SWAPTION (OIS / SOFR, Bachelier) ===================
TEST_F(QuantraComparisonTest, Swaption_OIS_Bachelier_NPVMatches) {
    std::cout << "\n=== Swaption (OIS SOFR, Bachelier) ===" << std::endl;

    QuantLib::Date prevEval = QuantLib::Settings::instance().evaluationDate();
    QuantLib::Date evalDate(14, QuantLib::August, 2024);
    QuantLib::Settings::instance().evaluationDate() = evalDate;

    double notional = 1000000.0;
    double strike = 0.03367463;
    double vol = 0.010267; // Normal vol

    QuantLib::Date exerciseDate(16, QuantLib::September, 2024);
    QuantLib::Date swapStart(18, QuantLib::September, 2024);
    QuantLib::Date swapEnd(18, QuantLib::September, 2034);

    QuantLib::Calendar usGov = QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond);

    struct OisRate {
        QuantLib::Period tenor;
        double rate;
    };

    std::vector<OisRate> oisRates = {
        {1 * QuantLib::Weeks, 0.0533410},
        {2 * QuantLib::Weeks, 0.0533585},
        {3 * QuantLib::Weeks, 0.0533814},
        {1 * QuantLib::Months, 0.0534261},
        {2 * QuantLib::Months, 0.0520565},
        {3 * QuantLib::Months, 0.0511385},
        {4 * QuantLib::Months, 0.0502265},
        {5 * QuantLib::Months, 0.0490300},
        {6 * QuantLib::Months, 0.0478850},
        {7 * QuantLib::Months, 0.0470850},
        {8 * QuantLib::Months, 0.0460968},
        {9 * QuantLib::Months, 0.0452458},
        {10 * QuantLib::Months, 0.0444082},
        {11 * QuantLib::Months, 0.0436380},
        {12 * QuantLib::Months, 0.0428710},
        {18 * QuantLib::Months, 0.0392930},
        {2 * QuantLib::Years, 0.0373480},
        {3 * QuantLib::Years, 0.0351270},
        {4 * QuantLib::Years, 0.0340905},
        {5 * QuantLib::Years, 0.0336448},
        {6 * QuantLib::Years, 0.0334900},
        {7 * QuantLib::Years, 0.0334540},
        {8 * QuantLib::Years, 0.0335100},
        {9 * QuantLib::Years, 0.0336048},
        {10 * QuantLib::Years, 0.0337219},
        {12 * QuantLib::Years, 0.0340177},
        {15 * QuantLib::Years, 0.0343655},
        {20 * QuantLib::Years, 0.0343820},
        {25 * QuantLib::Years, 0.0337260},
        {30 * QuantLib::Years, 0.0329430},
        {40 * QuantLib::Years, 0.0310050},
        {50 * QuantLib::Years, 0.0290915}
    };

    std::vector<std::shared_ptr<QuantLib::RateHelper>> helpers;
    auto sofr = std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(), usGov, QuantLib::Actual360());
    for (const auto& tr : oisRates) {
        helpers.push_back(std::make_shared<QuantLib::OISRateHelper>(
            2, tr.tenor, tr.rate, sofr));
    }

    auto oisCurve = std::make_shared<
        QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
        evalDate, helpers, QuantLib::Actual365Fixed());
    oisCurve->enableExtrapolation();

    QuantLib::Handle<QuantLib::YieldTermStructure> oisHandle(oisCurve);
    auto sofrWithCurve = std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(), usGov, QuantLib::Actual360(), oisHandle);

    QuantLib::Schedule fixedSchedule(
        swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), usGov,
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule floatSchedule(
        swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), usGov,
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);

    auto oisSwap = std::make_shared<QuantLib::OvernightIndexedSwap>(
        QuantLib::OvernightIndexedSwap::Payer,
        notional,
        fixedSchedule,
        strike,
        QuantLib::Actual360(),
        floatSchedule,
        sofrWithCurve,
        0.0,
        2,
        QuantLib::ModifiedFollowing,
        usGov,
        false,
        QuantLib::RateAveraging::Compound);

    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(
        oisSwap, ex, QuantLib::Settlement::Cash, QuantLib::Settlement::ParYieldCurve);

    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(
            evalDate, usGov, QuantLib::ModifiedFollowing, vol,
            QuantLib::Actual365Fixed(), QuantLib::Normal));
    qlSwaption->setPricingEngine(
        std::make_shared<QuantLib::BachelierSwaptionEngine>(oisHandle, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;

    std::vector<OisTenorRate> oisRatesFb = {
        {1, quantra::enums::TimeUnit_Weeks, 0.0533410},
        {2, quantra::enums::TimeUnit_Weeks, 0.0533585},
        {3, quantra::enums::TimeUnit_Weeks, 0.0533814},
        {1, quantra::enums::TimeUnit_Months, 0.0534261},
        {2, quantra::enums::TimeUnit_Months, 0.0520565},
        {3, quantra::enums::TimeUnit_Months, 0.0511385},
        {4, quantra::enums::TimeUnit_Months, 0.0502265},
        {5, quantra::enums::TimeUnit_Months, 0.0490300},
        {6, quantra::enums::TimeUnit_Months, 0.0478850},
        {7, quantra::enums::TimeUnit_Months, 0.0470850},
        {8, quantra::enums::TimeUnit_Months, 0.0460968},
        {9, quantra::enums::TimeUnit_Months, 0.0452458},
        {10, quantra::enums::TimeUnit_Months, 0.0444082},
        {11, quantra::enums::TimeUnit_Months, 0.0436380},
        {12, quantra::enums::TimeUnit_Months, 0.0428710},
        {18, quantra::enums::TimeUnit_Months, 0.0392930},
        {2, quantra::enums::TimeUnit_Years, 0.0373480},
        {3, quantra::enums::TimeUnit_Years, 0.0351270},
        {4, quantra::enums::TimeUnit_Years, 0.0340905},
        {5, quantra::enums::TimeUnit_Years, 0.0336448},
        {6, quantra::enums::TimeUnit_Years, 0.0334900},
        {7, quantra::enums::TimeUnit_Years, 0.0334540},
        {8, quantra::enums::TimeUnit_Years, 0.0335100},
        {9, quantra::enums::TimeUnit_Years, 0.0336048},
        {10, quantra::enums::TimeUnit_Years, 0.0337219},
        {12, quantra::enums::TimeUnit_Years, 0.0340177},
        {15, quantra::enums::TimeUnit_Years, 0.0343655},
        {20, quantra::enums::TimeUnit_Years, 0.0343820},
        {25, quantra::enums::TimeUnit_Years, 0.0337260},
        {30, quantra::enums::TimeUnit_Years, 0.0329430},
        {40, quantra::enums::TimeUnit_Years, 0.0310050},
        {50, quantra::enums::TimeUnit_Years, 0.0290915}
    };

    auto ts = buildOisCurve(
        b, "USD_SOFR", "USD_SOFR", oisRatesFb,
        quantra::enums::Calendar_UnitedStatesGovernmentBond);
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    auto volSurface = buildSwaptionVolSurface(
        b, "usd_sofr_vol", vol, quantra::enums::VolatilityType_Normal, 0.0, "", "2024-08-14");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto indices = b.CreateVector(
        std::vector<flatbuffers::Offset<quantra::IndexDef>>{buildIndexDef_USD_SOFR(b)});
    auto asof = b.CreateString("2024-08-14");

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models);

    // Fixed leg schedule
    auto feff = b.CreateString("2024-09-18");
    auto fterm = b.CreateString("2034-09-18");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    // Overnight leg schedule
    auto oeff = b.CreateString("2024-09-18");
    auto oterm = b.CreateString("2034-09-18");
    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(oeff);
    osb.add_termination_date(oterm);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Annual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto overnightSch = osb.Finish();

    auto idxRef = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder olb(b);
    olb.add_notional(notional);
    olb.add_schedule(overnightSch);
    olb.add_index(idxRef);
    olb.add_spread(0.0);
    olb.add_day_counter(quantra::enums::DayCounter_Actual360);
    olb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    olb.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    olb.add_payment_lag(2);
    olb.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    olb.add_lookback_days(-1);
    olb.add_lockout_days(0);
    olb.add_apply_observation_shift(false);
    olb.add_telescopic_value_dates(false);
    auto overnightLeg = olb.Finish();

    quantra::OisSwapBuilder oisBuilder(b);
    oisBuilder.add_swap_type(quantra::enums::SwapType_Payer);
    oisBuilder.add_fixed_leg(fixedLeg);
    oisBuilder.add_overnight_leg(overnightLeg);
    auto oisSwapFb = oisBuilder.Finish();

    auto exd = b.CreateString("2024-09-16");
    quantra::SwaptionBuilder swb(b);
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Cash);
    swb.add_settlement_method(quantra::enums::SettlementMethod_ParYieldCurve);
    swb.add_underlying_type(quantra::SwaptionUnderlying_OisSwap);
    swb.add_underlying(oisSwapFb.Union());
    auto swaption = swb.Finish();

    auto dc = b.CreateString("USD_SOFR");
    auto vol_id = b.CreateString("usd_sofr_vol");
    auto model_id = b.CreateString("bachelier_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.05);

    QuantLib::Settings::instance().evaluationDate() = prevEval;
}

TEST_F(QuantraComparisonTest, Swaption_OIS_SmileCubeSpreadFromATM_UsesSwapIndexRegistry) {
    flatbuffers::grpc::MessageBuilder b;

    std::vector<OisTenorRate> oisRatesFb = {
        {1, quantra::enums::TimeUnit_Months, 0.02},
        {6, quantra::enums::TimeUnit_Months, 0.021},
        {1, quantra::enums::TimeUnit_Years, 0.022},
        {2, quantra::enums::TimeUnit_Years, 0.023},
        {5, quantra::enums::TimeUnit_Years, 0.024},
        {10, quantra::enums::TimeUnit_Years, 0.025}
    };

    auto ts = buildOisCurve(
        b, "USD_SOFR", "USD_SOFR", oisRatesFb,
        quantra::enums::Calendar_UnitedStatesGovernmentBond);
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.01);
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "usd_sofr_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "USD_SOFR_OIS",
        {}, false, quantra::enums::VolatilityType_Normal, 0.0, "2024-08-14");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = b.CreateVector(
        std::vector<flatbuffers::Offset<quantra::IndexDef>>{buildIndexDef_USD_SOFR(b)});
    auto swapIndices = buildSwapIndicesVector(b, false, true, false);
    auto asof = b.CreateString("2024-08-14");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models, 0, 0, 0, false, false, false, true);

    auto feff = b.CreateString("2025-08-18");
    auto fterm = b.CreateString("2030-08-18");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(1000000.0);
    flb.add_schedule(fixedSch);
    flb.add_rate(0.025);
    flb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto oeff = b.CreateString("2025-08-18");
    auto oterm = b.CreateString("2030-08-18");
    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(oeff);
    osb.add_termination_date(oterm);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Annual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto overnightSch = osb.Finish();

    auto idxRef = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder olb(b);
    olb.add_notional(1000000.0);
    olb.add_schedule(overnightSch);
    olb.add_index(idxRef);
    olb.add_spread(0.0);
    olb.add_day_counter(quantra::enums::DayCounter_Actual360);
    olb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    olb.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    olb.add_payment_lag(2);
    olb.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    olb.add_lookback_days(-1);
    olb.add_lockout_days(0);
    olb.add_apply_observation_shift(false);
    olb.add_telescopic_value_dates(false);
    auto overnightLeg = olb.Finish();

    quantra::OisSwapBuilder oisBuilder(b);
    oisBuilder.add_swap_type(quantra::enums::SwapType_Payer);
    oisBuilder.add_fixed_leg(fixedLeg);
    oisBuilder.add_overnight_leg(overnightLeg);
    auto oisSwapFb = oisBuilder.Finish();

    auto exd = b.CreateString("2025-08-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Cash);
    swb.add_settlement_method(quantra::enums::SettlementMethod_ParYieldCurve);
    swb.add_underlying_type(quantra::SwaptionUnderlying_OisSwap);
    swb.add_underlying(oisSwapFb.Union());
    auto swaption = swb.Finish();

    auto dc = b.CreateString("USD_SOFR");
    auto vol_id = b.CreateString("usd_sofr_smile");
    auto model_id = b.CreateString("bachelier_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);

    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_SmileCube3D);
    EXPECT_EQ(res->used_strike_kind(), quantra::enums::SwaptionStrikeKind_SpreadFromATM);
    EXPECT_GT(res->used_atm_forward(), 0.0);
    EXPECT_GT(res->used_cube_node_atm(), 0.0);
}

// ======================== CDS ========================
TEST_F(QuantraComparisonTest, CDS_NPVMatches) {
    std::cout << "\n=== CDS ===" << std::endl;
    double notional = 10000000.0, spread = 0.01, recovery = 0.40, hazard = 0.02;
    QuantLib::Date start = evaluationDate_, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule sch(start, end, QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::Following, QuantLib::Unadjusted, QuantLib::DateGeneration::TwentiethIMM, false);
    auto qlCDS = std::make_shared<QuantLib::CreditDefaultSwap>(QuantLib::Protection::Buyer,
        notional, spread, sch, QuantLib::Following, QuantLib::Actual360());
    auto defCurve = std::make_shared<QuantLib::FlatHazardRate>(evaluationDate_, hazard, QuantLib::Actual365Fixed());
    qlCDS->setPricingEngine(std::make_shared<QuantLib::MidPointCdsEngine>(
        QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure>(defCurve), recovery, discountHandle_));
    double qlNPV = qlCDS->NPV();
    double qlFair = qlCDS->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);  // needed by SwapHelpers in the curve
    auto asof = b.CreateString("2025-01-15");
    auto credit_id = b.CreateString("credit");
    auto discount_id = b.CreateString("discount");
    
    quantra::CdsHelperConventionsBuilder hcb(b);
    hcb.add_settlement_days(0);
    hcb.add_frequency(quantra::enums::Frequency_Quarterly);
    hcb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    hcb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    hcb.add_last_period_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    hcb.add_settles_accrual(true);
    hcb.add_pays_at_default_time(true);
    hcb.add_rebates_accrual(true);
    hcb.add_helper_model(quantra::enums::CdsHelperModel_MidPoint);
    auto helper_conv = hcb.Finish();
    auto empty_quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CdsQuote>>{});

    quantra::CreditCurveSpecBuilder ccb(b);
    ccb.add_id(credit_id);
    ccb.add_reference_date(asof);
    ccb.add_calendar(quantra::enums::Calendar_TARGET);
    ccb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    ccb.add_recovery_rate(recovery);
    ccb.add_curve_interpolator(quantra::enums::Interpolator_LogLinear);
    ccb.add_helper_conventions(helper_conv);
    ccb.add_quotes(empty_quotes);
    ccb.add_flat_hazard_rate(hazard);
    auto cc = ccb.Finish();
    auto credit_curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CreditCurveSpec>>{cc});

    quantra::CdsModelSpecBuilder cmsb(b);
    cmsb.add_engine_type(quantra::enums::CdsEngineType_MidPoint);
    auto cds_payload = cmsb.Finish();
    auto model_id = b.CreateString("cds_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(model_id);
    msb.add_payload_type(quantra::ModelPayload_CdsModelSpec);
    msb.add_payload(cds_payload.Union());
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, credit_curves, 0, models);
    
    auto eff = b.CreateString("2025-01-15");
    auto term = b.CreateString("2030-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    auto schedule = sb.Finish();
    
    quantra::CDSBuilder cdsb(b);
    cdsb.add_side(quantra::enums::ProtectionSide_Buyer);
    cdsb.add_notional(notional);
    cdsb.add_running_coupon(spread);
    cdsb.add_schedule(schedule);
    cdsb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    auto cds = cdsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceCDSBuilder pcdsb(b);
    pcdsb.add_cds(cds);
    pcdsb.add_discounting_curve(dc);
    pcdsb.add_credit_curve_id(credit_id);
    pcdsb.add_model(model_id);
    auto pcdsbOff = pcdsb.Finish();
    
    auto cdss = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCDS>>{pcdsbOff});
    
    quantra::PriceCDSRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cds_list(cdss);
    b.Finish(rb.Finish());
    
    CDSPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCDSRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceCDSResponse>(respB->GetBufferPointer())->cds_list()->Get(0);
    double qNPV = r->npv();
    double qFair = r->fair_spread();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fair: " << qlFair*10000 << "bps | Quantra: " << qFair*10000 << "bps" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFair, qFair, 1e-6);
}

// =============================================================================
// BootstrapCurves Tests
// =============================================================================

TEST_F(QuantraComparisonTest, BootstrapCurves_DiscountFactors) {
    std::cout << "\n--- Test: BootstrapCurves Discount Factors ---\n";
    
    flatbuffers::grpc::MessageBuilder b;
    
    // Build TenorGrid with 1Y, 5Y, 10Y
    std::vector<flatbuffers::Offset<quantra::Period>> tenors;
    
    quantra::PeriodBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    
    quantra::PeriodBuilder t5y(b);
    t5y.add_n(5); t5y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t5y.Finish());
    
    quantra::PeriodBuilder t10y(b);
    t10y.add_n(10); t10y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t10y.Finish());
    
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::DateGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
    auto measures = b.CreateVector(measures_vec);
    
    auto curve_id = b.CreateString("test_curve");
    quantra::CurveQuerySpecBuilder cqb(b);
    cqb.add_curve_id(curve_id);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries_vec;
    queries_vec.push_back(query);
    auto queries = b.CreateVector(queries_vec);

    auto curve = buildCurve(b, "test_curve");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vec;
    curves_vec.push_back(curve);
    auto curves = b.CreateVector(curves_vec);
    
    // IndexDefs needed by SwapHelpers
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, as_of, 0, 0, indices, 0, curves);

    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());
    
    auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
    BootstrapCurvesRequestHandler handler;
    auto response_builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto response_offset = handler.request(response_builder, request);
    response_builder->Finish(response_offset);
    
    auto response = flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(response_builder->GetBufferPointer());
    
    ASSERT_EQ(response->results()->size(), 1u);
    auto result = response->results()->Get(0);
    ASSERT_EQ(result->error(), nullptr);
    ASSERT_EQ(result->series()->size(), 1u);
    
    auto df_series = result->series()->Get(0);
    ASSERT_EQ(df_series->measure(), quantra::CurveMeasure_DF);
    ASSERT_EQ(df_series->values()->size(), 3u);
    
    QuantLib::Date refDate = bootstrappedCurve_->referenceDate();
    std::vector<QuantLib::Date> testDates = {
        refDate + 1 * QuantLib::Years,
        refDate + 5 * QuantLib::Years,
        refDate + 10 * QuantLib::Years
    };
    
    for (size_t i = 0; i < testDates.size(); i++) {
        double quantra_df = df_series->values()->Get(i);
        double ql_df = bootstrappedCurve_->discount(testDates[i]);
        std::cout << "  " << testDates[i] << ": QL=" << std::fixed << std::setprecision(8) 
                  << ql_df << ", Quantra=" << quantra_df 
                  << ", Diff=" << std::scientific << std::abs(ql_df - quantra_df) << std::endl;
        EXPECT_NEAR(quantra_df, ql_df, 1e-10);
    }
}

TEST_F(QuantraComparisonTest, BootstrapCurves_ZeroRates) {
    std::cout << "\n--- Test: BootstrapCurves Zero Rates ---\n";
    
    flatbuffers::grpc::MessageBuilder b;
    
    std::vector<flatbuffers::Offset<quantra::Period>> tenors;
    
    quantra::PeriodBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    
    quantra::PeriodBuilder t5y(b);
    t5y.add_n(5); t5y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t5y.Finish());
    
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::DateGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_ZERO)};
    auto measures = b.CreateVector(measures_vec);
    
    auto curve_id = b.CreateString("test_curve");
    quantra::CurveQuerySpecBuilder cqb(b);
    cqb.add_curve_id(curve_id);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries_vec;
    queries_vec.push_back(query);
    auto queries = b.CreateVector(queries_vec);

    auto curve = buildCurve(b, "test_curve");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vec;
    curves_vec.push_back(curve);
    auto curves = b.CreateVector(curves_vec);
    
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, as_of, 0, 0, indices, 0, curves);

    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());
    
    auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
    BootstrapCurvesRequestHandler handler;
    auto response_builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto response_offset = handler.request(response_builder, request);
    response_builder->Finish(response_offset);
    
    auto response = flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(response_builder->GetBufferPointer());
    
    auto result = response->results()->Get(0);
    ASSERT_EQ(result->error(), nullptr);
    
    auto zero_series = result->series()->Get(0);
    ASSERT_EQ(zero_series->measure(), quantra::CurveMeasure_ZERO);
    
    QuantLib::Date refDate = bootstrappedCurve_->referenceDate();
    std::vector<QuantLib::Date> testDates = {
        refDate + 1 * QuantLib::Years,
        refDate + 5 * QuantLib::Years
    };
    
    for (size_t i = 0; i < testDates.size(); i++) {
        double quantra_zero = zero_series->values()->Get(i);
        double ql_zero = bootstrappedCurve_->zeroRate(testDates[i], QuantLib::Actual365Fixed(), 
                                                       QuantLib::Continuous).rate();
        std::cout << "  " << testDates[i] << ": QL=" << std::fixed << std::setprecision(8) 
                  << ql_zero << ", Quantra=" << quantra_zero 
                  << ", Diff=" << std::scientific << std::abs(ql_zero - quantra_zero) << std::endl;
        EXPECT_NEAR(quantra_zero, ql_zero, 1e-10);
    }
}

TEST_F(QuantraComparisonTest, BootstrapCurves_PillarDates) {
    std::cout << "\n--- Test: BootstrapCurves Pillar Dates ---\n";
    
    flatbuffers::grpc::MessageBuilder b;
    
    std::vector<flatbuffers::Offset<quantra::Period>> tenors;
    quantra::PeriodBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::DateGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
    auto measures = b.CreateVector(measures_vec);
    
    auto curve_id = b.CreateString("test_curve");
    quantra::CurveQuerySpecBuilder cqb(b);
    cqb.add_curve_id(curve_id);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries_vec;
    queries_vec.push_back(query);
    auto queries = b.CreateVector(queries_vec);

    auto curve = buildCurve(b, "test_curve");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vec;
    curves_vec.push_back(curve);
    auto curves = b.CreateVector(curves_vec);
    
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, as_of, 0, 0, indices, 0, curves);

    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());
    
    auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
    BootstrapCurvesRequestHandler handler;
    auto response_builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto response_offset = handler.request(response_builder, request);
    response_builder->Finish(response_offset);
    
    auto response = flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(response_builder->GetBufferPointer());
    
    auto result = response->results()->Get(0);
    ASSERT_NE(result->pillar_dates(), nullptr);
    
    std::cout << "  Pillar dates count: " << result->pillar_dates()->size() << std::endl;
    for (size_t i = 0; i < result->pillar_dates()->size(); i++) {
        std::cout << "    " << result->pillar_dates()->Get(i)->c_str() << std::endl;
    }
    EXPECT_GE(result->pillar_dates()->size(), 6u);
}

// =============================================================================
// SampleVolSurfaces Tests
// =============================================================================

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionConstantCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSurface(b, "swp_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikes = b.CreateVector(std::vector<double>{0.01, 0.02});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikes);
    auto strikeGrid = sgb.Finish();

    auto volId = b.CreateString("swp_const");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 8u);
    EXPECT_EQ(r->atm_levels()->size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSpreadFromAtmReturnsAtmGrid) {
    flatbuffers::grpc::MessageBuilder b;
    std::vector<QuantLib::Period> expiries = {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors = {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<double> strikes = {-0.0025, 0.0, 0.0025};
    std::vector<double> vols(expiries.size() * tenors.size() * strikes.size(), 0.01);
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swp_spread", expiries, tenors, strikes, vols,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(1); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(2); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{-0.001, 0.0, 0.001});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_SpreadFromATM); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("swp_spread");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 12u);
    EXPECT_EQ(r->atm_levels()->size(), 4u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_OptionletCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildOptionletVolSurface(b, "opt_const", 0.25, quantra::enums::VolatilityType_Normal);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.01, 0.02, 0.03});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("opt_const");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Optionlet);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 6u);
    EXPECT_EQ(r->tenors()->size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "EQVOL_CONST", 0.25, quantra::enums::VolSurfaceShape_Constant, "2026-02-27");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2026-02-27");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(6); e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b); e3.add_n(1); e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("EQVOL_CONST");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->n_expiries(), 3);
    EXPECT_EQ(r->n_tenors(), 0);
    EXPECT_EQ(r->n_strikes(), 3);
    ASSERT_EQ(r->vols()->size(), 9u);
    for (flatbuffers::uoffset_t i = 0; i < r->vols()->size(); ++i) {
        EXPECT_NEAR(r->vols()->Get(i), 0.25, 1.0e-12);
    }
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackTermStructureCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolTermStructure(
        b, "eq_black_term",
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(6, QuantLib::Months), QuantLib::Period(1, QuantLib::Years)},
        {0.20, 0.22, 0.25});
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(6); e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b); e3.add_n(1); e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_term");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->vols()->size(), 9u);
    EXPECT_NEAR(r->vols()->Get(0), 0.20, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(3), 0.22, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(6), 0.25, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackSurfaceCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceGrid(
        b, "eq_black_surface",
        {QuantLib::Period(6, QuantLib::Months), QuantLib::Period(1, QuantLib::Years)},
        {80.0, 100.0, 120.0},
        {0.30, 0.25, 0.22, 0.32, 0.27, 0.24});
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(6); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(1); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_surface");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->vols()->size(), 6u);
    EXPECT_NEAR(r->vols()->Get(0), 0.30, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(1), 0.25, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(2), 0.22, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(3), 0.32, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(4), 0.27, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(5), 0.24, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackSurfaceFromPricesMatchesDirectQuantLib) {
    const QuantLib::Date refDate(15, QuantLib::January, 2025);
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    const double spot = 100.0;
    const std::vector<QuantLib::Date> pillarDates = {
        QuantLib::Date(15, QuantLib::July, 2025),
        QuantLib::Date(15, QuantLib::January, 2026)
    };
    const std::vector<double> strikes = {80.0, 100.0, 120.0};
    const std::vector<double> inputVolsFlat = {
        0.30, 0.25, 0.22,  // 2025-07-15
        0.32, 0.27, 0.24   // 2026-01-15
    };

    // Build a direct QuantLib surface to generate option prices at the input nodes.
    QuantLib::Matrix inputVolMatrix(static_cast<int>(strikes.size()), static_cast<int>(pillarDates.size()));
    for (size_t i = 0; i < pillarDates.size(); ++i) {
        for (size_t j = 0; j < strikes.size(); ++j) {
            inputVolMatrix[static_cast<int>(j)][static_cast<int>(i)] =
                inputVolsFlat[i * strikes.size() + j];
        }
    }
    auto sourceSurface = std::make_shared<QuantLib::BlackVarianceSurface>(
        refDate, cal, pillarDates, strikes, inputVolMatrix, dc);
    sourceSurface->setInterpolation<QuantLib::Bilinear>();

    auto spotHandle = QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
    auto sourceProcess = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spotHandle,
        dividendHandle_,
        discountHandle_,
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(sourceSurface));
    std::vector<double> priceMatrixFlat;
    priceMatrixFlat.reserve(pillarDates.size() * strikes.size());
    for (const auto& expiry : pillarDates) {
        auto ex = std::make_shared<QuantLib::EuropeanExercise>(expiry);
        for (double k : strikes) {
            auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, k);
            QuantLib::VanillaOption opt(payoff, ex);
            opt.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(sourceProcess));
            priceMatrixFlat.push_back(opt.NPV());
        }
    }

    // Reconstruct implied vol nodes directly in QuantLib from the generated prices.
    auto seedVol = std::make_shared<QuantLib::BlackConstantVol>(refDate, cal, 0.20, dc);
    auto inversionProcess = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spotHandle,
        dividendHandle_,
        discountHandle_,
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(seedVol));
    QuantLib::Matrix impliedNodeVols(static_cast<int>(strikes.size()), static_cast<int>(pillarDates.size()));
    for (size_t i = 0; i < pillarDates.size(); ++i) {
        auto ex = std::make_shared<QuantLib::EuropeanExercise>(pillarDates[i]);
        for (size_t j = 0; j < strikes.size(); ++j) {
            auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, strikes[j]);
            QuantLib::VanillaOption opt(payoff, ex);
            const double mktPrice = priceMatrixFlat[i * strikes.size() + j];
            impliedNodeVols[static_cast<int>(j)][static_cast<int>(i)] =
                opt.impliedVolatility(mktPrice, inversionProcess, 1.0e-8, 500, 1.0e-8, 10.0);
        }
    }
    auto expectedSurface = std::make_shared<QuantLib::BlackVarianceSurface>(
        refDate, cal, pillarDates, strikes, impliedNodeVols, dc);
    expectedSurface->setInterpolation<QuantLib::Bilinear>();

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceFromPrices(
        b,
        "eq_black_prices",
        {"2025-07-15", "2026-01-15"},
        strikes,
        priceMatrixFlat,
        "EQ_SPOT",
        "discount",
        "div");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curveDiscount = buildCurve(b, "discount", flatRate_);
    auto curveDividend = buildCurve(b, "div", dividendFlatRate_);
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        curveDiscount, curveDividend});
    auto indices = buildIndicesVector(b);

    auto quoteId = b.CreateString("EQ_SPOT");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quoteId);
    qb.add_kind(quantra::QuoteKind_Price);
    qb.add_value(spot);
    qb.add_quote_type(quantra::QuoteType_Curve);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(6); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(9); e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b); e3.add_n(1); e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    expGridB.add_calendar(quantra::enums::Calendar_TARGET);
    expGridB.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(strikes);
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_prices");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->n_expiries(), 3);
    ASSERT_EQ(r->n_strikes(), 3);
    ASSERT_EQ(r->vols()->size(), 9u);

    const std::vector<QuantLib::Date> queryDates = {
        cal.advance(refDate, QuantLib::Period(6, QuantLib::Months), bdc),
        cal.advance(refDate, QuantLib::Period(9, QuantLib::Months), bdc),
        cal.advance(refDate, QuantLib::Period(1, QuantLib::Years), bdc)
    };
    const std::vector<double> queryStrikes = strikes;
    for (size_t i = 0; i < queryDates.size(); ++i) {
        for (size_t j = 0; j < queryStrikes.size(); ++j) {
            const double expected = expectedSurface->blackVol(queryDates[i], queryStrikes[j]);
            const double actual = r->vols()->Get(static_cast<flatbuffers::uoffset_t>(i * queryStrikes.size() + j));
            EXPECT_NEAR(actual, expected, 2.0e-4);
        }
    }

    // Also verify exact node recovery at original pillar grid from endpoint output.
    // Query order is [6M, 9M, 1Y], so original pillars map to row 0 and row 2.
    for (size_t j = 0; j < strikes.size(); ++j) {
        const double expected6M = impliedNodeVols[static_cast<int>(j)][0];
        const double actual6M = r->vols()->Get(static_cast<flatbuffers::uoffset_t>(j));
        EXPECT_NEAR(actual6M, expected6M, 2.0e-4);

        const double expected1Y = impliedNodeVols[static_cast<int>(j)][1];
        const double actual1Y = r->vols()->Get(static_cast<flatbuffers::uoffset_t>(2 * strikes.size() + j));
        EXPECT_NEAR(actual1Y, expected1Y, 2.0e-4);
    }
}

TEST_F(QuantraComparisonTest, BlackVolSurface_NodeOrientationMatchesInput) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceGrid(
        b, "eq_black_orientation",
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(2, QuantLib::Months)},
        {90.0, 100.0},
        {0.10, 0.11, 0.20, 0.21},
        "2026-01-02");
    b.Finish(volSurface);

    auto spec = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer());
    auto entry = parseBlackVol(spec, nullptr);
    ASSERT_FALSE(entry.handle.empty());

    const QuantLib::Date ref(2, QuantLib::January, 2026);
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const QuantLib::Date d1M = cal.advance(ref, QuantLib::Period(1, QuantLib::Months), bdc);
    const QuantLib::Date d2M = cal.advance(ref, QuantLib::Period(2, QuantLib::Months), bdc);

    EXPECT_NEAR(entry.handle->blackVol(d1M, 90.0), 0.10, 1.0e-12);
    EXPECT_NEAR(entry.handle->blackVol(d1M, 100.0), 0.11, 1.0e-12);
    EXPECT_NEAR(entry.handle->blackVol(d2M, 90.0), 0.20, 1.0e-12);
    EXPECT_NEAR(entry.handle->blackVol(d2M, 100.0), 0.21, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackRejectsSmileSlice) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "eq_black_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1.Finish()});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{100.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_const");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_output_mode(quantra::VolOutputMode_SmileSlice);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackRejectsSpreadFromAtmAxis) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "eq_black_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1.Finish()});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{-5.0, 0.0, 5.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_SpreadFromATM); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_const");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_MaxPointsGuard) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSurface(b, "swp_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.01, 0.02, 0.03});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    quantra::QueryOptionsBuilder qob(b);
    qob.add_max_points(2);
    auto opts = qob.Finish();
    auto volId = b.CreateString("swp_const");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_options(opts);
    qsb.add_swap_index_id(swapIdx);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_NoExtrapolationGuard) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swp_abs",
        {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)},
        {QuantLib::Period(5, QuantLib::Years)},
        {0.01, 0.02},
        std::vector<double>(2 * 1 * 2, 0.01),
        quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e5(b); e5.add_n(5); e5.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e5.Finish()});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5.Finish()});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.05});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    quantra::QueryOptionsBuilder qob(b); qob.add_allow_extrapolation(false);
    auto opts = qob.Finish();
    auto volId = b.CreateString("swp_abs");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_options(opts);
    qsb.add_swap_index_id(swapIdx);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

// ======================== EQUITY OPTION ========================
TEST_F(QuantraComparisonTest, EquityOption_EuropeanVanilla_NPVMatches) {
    const double spot = 100.0;
    const double strike = 100.0;
    const double vol = 0.20;
    const auto expiry = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);

    auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, strike);
    auto exercise = std::make_shared<QuantLib::EuropeanExercise>(expiry);
    auto qlOption = std::make_shared<QuantLib::VanillaOption>(payoff, exercise);
    auto qlSpot = QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
    auto qlVol = QuantLib::Handle<QuantLib::BlackVolTermStructure>(
        std::make_shared<QuantLib::BlackConstantVol>(
            evaluationDate_, QuantLib::TARGET(), vol, QuantLib::Actual365Fixed()));
    auto process = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        qlSpot, discountHandle_, discountHandle_, qlVol);
    qlOption->setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
    const double qlNpv = qlOption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto asof = b.CreateString("2025-01-15");

    auto curveDiscount = buildCurve(b, "discount");
    auto curveDividend = buildCurve(b, "div");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        curveDiscount, curveDividend});

    auto quoteId = b.CreateString("EQ_SPOT");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quoteId);
    qb.add_kind(quantra::QuoteKind_Price);
    qb.add_value(spot);
    qb.add_quote_type(quantra::QuoteType_Curve);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto volRef = b.CreateString("2025-01-15");
    quantra::BlackVolBaseSpecBuilder bvbb(b);
    bvbb.add_reference_date(volRef);
    bvbb.add_calendar(quantra::enums::Calendar_TARGET);
    bvbb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    bvbb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    bvbb.add_shape(quantra::enums::VolSurfaceShape_Constant);
    bvbb.add_constant_vol(vol);
    auto blackBase = bvbb.Finish();
    quantra::BlackVolSpecBuilder bvsb(b);
    bvsb.add_base(blackBase);
    auto blackSpec = bvsb.Finish();
    auto volId = b.CreateString("eq_vol");
    quantra::VolSurfaceSpecBuilder vssb(b);
    vssb.add_id(volId);
    vssb.add_payload_type(quantra::VolPayload_BlackVolSpec);
    vssb.add_payload(blackSpec.Union());
    auto volSurface = vssb.Finish();
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    quantra::EquityVanillaModelSpecBuilder emsb(b);
    emsb.add_model_type(quantra::enums::EquityModelType_BlackScholesAnalytic);
    auto eqModelSpec = emsb.Finish();
    auto modelId = b.CreateString("eq_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(modelId);
    msb.add_payload_type(quantra::ModelPayload_EquityVanillaModelSpec);
    msb.add_payload(eqModelSpec.Union());
    auto model = msb.Finish();
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto undId = b.CreateString("EQ1");
    auto undCcy = b.CreateString("USD");
    auto divCurveId = b.CreateString("div");
    quantra::EquityUnderlyingSpecBuilder usb(b);
    usb.add_id(undId);
    usb.add_currency(undCcy);
    usb.add_spot_quote_id(quoteId);
    usb.add_dividend_yield_curve_id(divCurveId);
    auto und = usb.Finish();
    auto underlyings =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>{und});
    auto indices = buildIndicesVector(b);

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, vols, models, underlyings);

    quantra::EquityPlainVanillaPayoffBuilder pvb(b);
    pvb.add_option_type(quantra::enums::EquityOptionType_Call);
    pvb.add_strike(strike);
    auto payoffFb = pvb.Finish();

    auto expiryStr = b.CreateString("2026-01-15");
    quantra::EquityEuropeanExerciseBuilder eeb(b);
    eeb.add_expiry_date(expiryStr);
    auto exFb = eeb.Finish();

    auto tradeId = b.CreateString("EQ_CALL_1Y");
    quantra::EquityOptionBuilder eob(b);
    eob.add_trade_id(tradeId);
    eob.add_underlying_id(undId);
    eob.add_quantity(1.0);
    eob.add_settlement(quantra::enums::EquitySettlementType_Physical);
    eob.add_payoff_type(quantra::EquityPayoff_EquityPlainVanillaPayoff);
    eob.add_payoff(payoffFb.Union());
    eob.add_exercise_type(quantra::EquityExercise_EquityEuropeanExercise);
    eob.add_exercise(exFb.Union());
    auto option = eob.Finish();

    auto discId = b.CreateString("discount");
    quantra::PriceEquityOptionBuilder peob(b);
    peob.add_option(option);
    peob.add_discounting_curve(discId);
    peob.add_volatility(volId);
    peob.add_model(modelId);
    auto po = peob.Finish();
    auto options = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceEquityOption>>{po});

    quantra::PriceEquityOptionRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_options(options);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceEquityOptionRequest>(b.GetBufferPointer());
    EquityOptionPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::PriceEquityOptionResponse>(outBuilder->GetBufferPointer());

    ASSERT_NE(resp->options(), nullptr);
    ASSERT_EQ(resp->options()->size(), 1u);
    const auto* r = resp->options()->Get(0);
    ASSERT_NE(r, nullptr);
    EXPECT_NEAR(r->npv(), qlNpv, 1.0e-8);
}

TEST_F(QuantraComparisonTest, EquityOption_BlackTermShapeMissingGridRejected) {
    flatbuffers::grpc::MessageBuilder b;
    auto asof = b.CreateString("2025-01-15");

    auto curveDiscount = buildCurve(b, "discount");
    auto curveDividend = buildCurve(b, "div");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        curveDiscount, curveDividend});

    auto quoteId = b.CreateString("EQ_SPOT");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quoteId);
    qb.add_kind(quantra::QuoteKind_Price);
    qb.add_value(100.0);
    qb.add_quote_type(quantra::QuoteType_Curve);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto volRef = b.CreateString("2025-01-15");
    quantra::BlackVolBaseSpecBuilder bvbb(b);
    bvbb.add_reference_date(volRef);
    bvbb.add_calendar(quantra::enums::Calendar_TARGET);
    bvbb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    bvbb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    bvbb.add_shape(quantra::enums::VolSurfaceShape_AtmMatrix2D);
    bvbb.add_constant_vol(0.20);
    auto blackBase = bvbb.Finish();
    quantra::BlackVolSpecBuilder bvsb(b);
    bvsb.add_base(blackBase);
    auto blackSpec = bvsb.Finish();
    auto volId = b.CreateString("eq_surface_like_vol");
    quantra::VolSurfaceSpecBuilder vssb(b);
    vssb.add_id(volId);
    vssb.add_payload_type(quantra::VolPayload_BlackVolSpec);
    vssb.add_payload(blackSpec.Union());
    auto volSurface = vssb.Finish();
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    quantra::EquityVanillaModelSpecBuilder emsb(b);
    emsb.add_model_type(quantra::enums::EquityModelType_BlackScholesAnalytic);
    auto eqModelSpec = emsb.Finish();
    auto modelId = b.CreateString("eq_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(modelId);
    msb.add_payload_type(quantra::ModelPayload_EquityVanillaModelSpec);
    msb.add_payload(eqModelSpec.Union());
    auto model = msb.Finish();
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto undId = b.CreateString("EQ1");
    auto undCcy = b.CreateString("USD");
    auto divCurveId = b.CreateString("div");
    quantra::EquityUnderlyingSpecBuilder usb(b);
    usb.add_id(undId);
    usb.add_currency(undCcy);
    usb.add_spot_quote_id(quoteId);
    usb.add_dividend_yield_curve_id(divCurveId);
    auto und = usb.Finish();
    auto underlyings =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>{und});
    auto indices = buildIndicesVector(b);

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, vols, models, underlyings);

    quantra::EquityPlainVanillaPayoffBuilder pvb(b);
    pvb.add_option_type(quantra::enums::EquityOptionType_Call);
    pvb.add_strike(100.0);
    auto payoffFb = pvb.Finish();

    auto expiryStr = b.CreateString("2026-01-15");
    quantra::EquityEuropeanExerciseBuilder eeb(b);
    eeb.add_expiry_date(expiryStr);
    auto exFb = eeb.Finish();

    auto tradeId = b.CreateString("EQ_CALL_SURFACE_SHAPE");
    quantra::EquityOptionBuilder eob(b);
    eob.add_trade_id(tradeId);
    eob.add_underlying_id(undId);
    eob.add_quantity(1.0);
    eob.add_settlement(quantra::enums::EquitySettlementType_Physical);
    eob.add_payoff_type(quantra::EquityPayoff_EquityPlainVanillaPayoff);
    eob.add_payoff(payoffFb.Union());
    eob.add_exercise_type(quantra::EquityExercise_EquityEuropeanExercise);
    eob.add_exercise(exFb.Union());
    auto option = eob.Finish();

    auto discId = b.CreateString("discount");
    quantra::PriceEquityOptionBuilder peob(b);
    peob.add_option(option);
    peob.add_discounting_curve(discId);
    peob.add_volatility(volId);
    peob.add_model(modelId);
    auto po = peob.Finish();
    auto options = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceEquityOption>>{po});

    quantra::PriceEquityOptionRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_options(options);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceEquityOptionRequest>(b.GetBufferPointer());
    EquityOptionPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(handler.request(outBuilder, req), QuantraError);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_ZeroRate_TenorGridMatchesPillars) {
    flatbuffers::grpc::MessageBuilder b;

    // Pricing (needs at least one discount curve)
    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    // Inflation index spec
    auto idxId = b.CreateString("EUHICP");
    auto idxFamily = b.CreateString("EU HICP");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    auto fixingDec = b.CreateString("2024-12-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(100.0);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(100.2);
    auto fixNov = fixNovBuilder.Finish();
    quantra::FixingBuilder fixDecBuilder(b);
    fixDecBuilder.add_date(fixingDec);
    fixDecBuilder.add_value(100.4);
    auto fixDec = fixDecBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{
        fixOct, fixNov, fixDec
    });

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    // Inflation curve spec built from QuantLib ZCIIS helpers.
    auto curveId = b.CreateString("HICP_ZC");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");

    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    auto p5y = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);

    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();

    quantra::ZeroCouponInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_value(0.0210);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h2 = h2Builder.Finish();

    quantra::ZeroCouponInflationSwapHelperBuilder h5Builder(b);
    h5Builder.add_quote_value(0.0220);
    h5Builder.add_swap_observation_lag(observationLag);
    h5Builder.add_tenor(p5y);
    h5Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h5Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h5Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h5Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h5 = h5Builder.Finish();

    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    quantra::InflationPointWrapperBuilder pw5Builder(b);
    pw5Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw5Builder.add_point(h5.Union());
    auto pwh5 = pw5Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        pwh1, pwh2, pwh5
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_allow_extrapolation(true);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    // Query: tenor grid that matches curve conventions so sampling hits pillar nodes.
    auto t1 = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto t2 = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    auto t5 = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t1, t2, t5});

    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    tgb.add_calendar(quantra::enums::Calendar_TARGET);
    tgb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto tg = tgb.Finish();

    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();

    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_ZeroRate)
    });

    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());

    ASSERT_NE(resp->results(), nullptr);
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_NE(r, nullptr);
    ASSERT_TRUE(r->error() == nullptr);
    ASSERT_NE(r->grid_dates(), nullptr);
    ASSERT_NE(r->series(), nullptr);
    ASSERT_EQ(r->grid_dates()->size(), 3u);
    ASSERT_EQ(r->series()->size(), 1u);

    const auto* s = r->series()->Get(0);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->measure(), quantra::enums::InflationCurveMeasure_ZeroRate);
    ASSERT_NE(s->values(), nullptr);
    ASSERT_EQ(s->values()->size(), 3u);

    // QuantLib's inflation term structures can map a date to an inflation period
    // (e.g., month start) when computing zero rates, so the sampled values may
    // deviate slightly from the raw node quotes. We still expect them to be close.
    EXPECT_NEAR(s->values()->Get(0), 0.0200, 1.5e-3);
    EXPECT_NEAR(s->values()->Get(1), 0.0210, 1.5e-3);
    EXPECT_NEAR(s->values()->Get(2), 0.0220, 5e-4);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_ZeroHelpersCanResolveQuoteIds) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP");
    auto idxFamily = b.CreateString("EU HICP");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    auto fixingDec = b.CreateString("2024-12-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(100.0);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(100.2);
    auto fixNov = fixNovBuilder.Finish();
    quantra::FixingBuilder fixDecBuilder(b);
    fixDecBuilder.add_date(fixingDec);
    fixDecBuilder.add_value(100.4);
    auto fixDec = fixDecBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov, fixDec});

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto q1Id = b.CreateString("HICP_ZC_1Y");
    quantra::QuoteSpecBuilder q1Builder(b);
    q1Builder.add_id(q1Id);
    q1Builder.add_kind(quantra::QuoteKind_Rate);
    q1Builder.add_quote_type(quantra::QuoteType_Curve);
    q1Builder.add_value(0.0205);
    auto q1 = q1Builder.Finish();
    auto q2Id = b.CreateString("HICP_ZC_2Y");
    quantra::QuoteSpecBuilder q2Builder(b);
    q2Builder.add_id(q2Id);
    q2Builder.add_kind(quantra::QuoteKind_Rate);
    q2Builder.add_quote_type(quantra::QuoteType_Curve);
    q2Builder.add_value(0.0215);
    auto q2 = q2Builder.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{q1, q2});

    auto curveId = b.CreateString("HICP_ZC_QID");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_id(q1Id);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();
    quantra::ZeroCouponInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_id(q2Id);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h2 = h2Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1, pwh2});

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y, p2y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    tgb.add_calendar(quantra::enums::Calendar_TARGET);
    tgb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();

    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_ZeroRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_TRUE(r->error() == nullptr);
    const auto* s = r->series()->Get(0);
    ASSERT_EQ(s->values()->size(), 2u);
    EXPECT_NEAR(s->values()->Get(0), 0.0205, 5e-4);
    EXPECT_NEAR(s->values()->Get(1), 0.0215, 5e-4);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_YoYRate_TenorGridMatchesHelpers) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP_YY");
    auto idxFamily = b.CreateString("EU HICP YoY");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(0.0180);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(0.0190);
    auto fixNov = fixNovBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov});

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_YY");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    quantra::YearOnYearInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h1Builder.add_nominal_curve_id(discCurveId);
    auto h1 = h1Builder.Finish();
    quantra::YearOnYearInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_value(0.0210);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h2Builder.add_nominal_curve_id(discCurveId);
    auto h2 = h2Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1, pwh2});

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y, p2y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    tgb.add_calendar(quantra::enums::Calendar_TARGET);
    tgb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();

    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_YoYRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_TRUE(r->error() == nullptr);
    const auto* s = r->series()->Get(0);
    ASSERT_EQ(s->values()->size(), 2u);
    EXPECT_NEAR(s->values()->Get(0), 0.0200, 1.5e-3);
    EXPECT_NEAR(s->values()->Get(1), 0.0210, 1.5e-3);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_MissingFixingsReturnsItemError) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);
    auto idxId = b.CreateString("EUHICP");
    auto idxFamily = b.CreateString("EU HICP");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_ZC_ERR");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1});

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();
    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_ZeroRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_NE(r->error(), nullptr);
    EXPECT_NE(std::string(r->error()->error_message()->c_str()).find("base fixing unavailable"), std::string::npos);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_MissingNominalCurveReturnsItemError) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);
    auto idxId = b.CreateString("EUHICP_YY");
    auto idxFamily = b.CreateString("EU HICP YoY");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(0.0180);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(0.0190);
    auto fixNov = fixNovBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov});

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_YY_ERR");
    auto curveRef = b.CreateString("2025-01-15");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto missingNominal = b.CreateString("MISSING_CURVE");
    quantra::YearOnYearInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h1Builder.add_nominal_curve_id(missingNominal);
    auto h1 = h1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1});

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    icb.add_index_id(idxId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();
    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_YoYRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_NE(r->error(), nullptr);
    EXPECT_NE(std::string(r->error()->error_message()->c_str()).find("nominal_curve_id"), std::string::npos);
}

TEST_F(QuantraComparisonTest, PriceZeroCouponInflationSwap_MatchesQuantLib) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP");
    auto idxFamily = b.CreateString("EU HICP");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    auto fixingDec = b.CreateString("2024-12-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(100.0);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(100.2);
    auto fixNov = fixNovBuilder.Finish();
    quantra::FixingBuilder fixDecBuilder(b);
    fixDecBuilder.add_date(fixingDec);
    fixDecBuilder.add_value(100.4);
    auto fixDec = fixDecBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{
        fixOct, fixNov, fixDec
    });

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_ZC");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    auto p5y = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);

    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();

    quantra::ZeroCouponInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_value(0.0210);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h2 = h2Builder.Finish();

    quantra::ZeroCouponInflationSwapHelperBuilder h5Builder(b);
    h5Builder.add_quote_value(0.0220);
    h5Builder.add_swap_observation_lag(observationLag);
    h5Builder.add_tenor(p5y);
    h5Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h5Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h5Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h5Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h5 = h5Builder.Finish();

    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    quantra::InflationPointWrapperBuilder pw5Builder(b);
    pw5Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw5Builder.add_point(h5.Union());
    auto pwh5 = pw5Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        pwh1, pwh2, pwh5
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_allow_extrapolation(true);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto startDate = b.CreateString("2025-01-15");
    auto maturityDate = b.CreateString("2030-01-15");
    quantra::ZeroCouponInflationSwapBuilder zcib(b);
    zcib.add_swap_type(quantra::enums::SwapType_Payer);
    zcib.add_notional(1000000.0);
    zcib.add_start_date(startDate);
    zcib.add_maturity_date(maturityDate);
    zcib.add_fixed_calendar(quantra::enums::Calendar_TARGET);
    zcib.add_fixed_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    zcib.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    zcib.add_fixed_rate(0.0217);
    zcib.add_inflation_index_id(idxId);
    zcib.add_observation_lag(observationLag);
    zcib.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    zcib.add_adjust_observation_dates(false);
    zcib.add_inflation_calendar(quantra::enums::Calendar_NullCalendar);
    zcib.add_inflation_convention(quantra::enums::BusinessDayConvention_Following);
    auto zciis = zcib.Finish();

    quantra::PriceZeroCouponInflationSwapBuilder pzb(b);
    pzb.add_zero_coupon_inflation_swap(zciis);
    pzb.add_discounting_curve(discCurveId);
    pzb.add_inflation_curve(curveId);
    auto trade = pzb.Finish();
    auto trades = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceZeroCouponInflationSwap>>{trade});

    quantra::PriceZeroCouponInflationSwapRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_swaps(trades);
    reqb.add_include_flows(true);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceZeroCouponInflationSwapRequest>(b.GetBufferPointer());
    ZeroCouponInflationSwapPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::PriceZeroCouponInflationSwapResponse>(outBuilder->GetBufferPointer());
    ASSERT_NE(resp->swaps(), nullptr);
    ASSERT_EQ(resp->swaps()->size(), 1u);
    const auto* priced = resp->swaps()->Get(0);
    ASSERT_NE(priced, nullptr);
    ASSERT_NE(priced->fixed_leg_flows(), nullptr);
    ASSERT_NE(priced->inflation_leg_flows(), nullptr);
    EXPECT_EQ(priced->fixed_leg_flows()->size(), 1u);
    EXPECT_EQ(priced->inflation_leg_flows()->size(), 1u);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(req->pricing());
    auto discountIt = reg.rates.curves.find("DISC");
    ASSERT_NE(discountIt, reg.rates.curves.end());
    auto indexIt = reg.inflation.inflationIndices.find("EUHICP");
    ASSERT_NE(indexIt, reg.inflation.inflationIndices.end());
    auto zeroIndex = std::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(indexIt->second);
    ASSERT_TRUE(static_cast<bool>(zeroIndex));

    auto expected = std::make_shared<QuantLib::ZeroCouponInflationSwap>(
        QuantLib::ZeroCouponInflationSwap::Payer,
        1000000.0,
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2030),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        QuantLib::Actual365Fixed(),
        0.0217,
        zeroIndex,
        QuantLib::Period(3, QuantLib::Months),
        QuantLib::CPI::Linear,
        false,
        QuantLib::NullCalendar(),
        QuantLib::Following);
    expected->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    EXPECT_NEAR(priced->npv(), expected->NPV(), 1e-8);
    EXPECT_NEAR(priced->fair_rate(), expected->fairRate(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_bps(), expected->fixedLegBPS(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_npv(), expected->fixedLegNPV(), 1e-8);
    EXPECT_NEAR(priced->inflation_leg_npv(), expected->inflationLegNPV(), 1e-8);
}

TEST_F(QuantraComparisonTest, PriceYearOnYearInflationSwap_MatchesQuantLib) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP_YY");
    auto idxFamily = b.CreateString("EU HICP YoY");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(0.0180);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(0.0190);
    auto fixNov = fixNovBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov});

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_YY");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);

    quantra::YearOnYearInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h1Builder.add_nominal_curve_id(discCurveId);
    auto h1 = h1Builder.Finish();

    quantra::YearOnYearInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_value(0.0210);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h2Builder.add_nominal_curve_id(discCurveId);
    auto h2 = h2Builder.Finish();

    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        pwh1, pwh2
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto effective = b.CreateString("2025-01-15");
    auto termination = b.CreateString("2027-01-15");
    quantra::ScheduleBuilder fixedSb(b);
    fixedSb.add_effective_date(effective);
    fixedSb.add_termination_date(termination);
    fixedSb.add_calendar(quantra::enums::Calendar_TARGET);
    fixedSb.add_frequency(quantra::enums::Frequency_Annual);
    fixedSb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixedSb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixedSb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSchedule = fixedSb.Finish();

    quantra::ScheduleBuilder yoySb(b);
    yoySb.add_effective_date(effective);
    yoySb.add_termination_date(termination);
    yoySb.add_calendar(quantra::enums::Calendar_TARGET);
    yoySb.add_frequency(quantra::enums::Frequency_Annual);
    yoySb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    yoySb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    yoySb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto yoySchedule = yoySb.Finish();

    quantra::YearOnYearInflationSwapBuilder yyb(b);
    yyb.add_swap_type(quantra::enums::SwapType_Receiver);
    yyb.add_notional(1000000.0);
    yyb.add_fixed_schedule(fixedSchedule);
    yyb.add_fixed_rate(0.0204);
    yyb.add_fixed_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    yyb.add_yoy_schedule(yoySchedule);
    yyb.add_inflation_index_id(idxId);
    yyb.add_observation_lag(observationLag);
    yyb.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    yyb.add_spread(0.0002);
    yyb.add_yoy_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    yyb.add_payment_calendar(quantra::enums::Calendar_TARGET);
    yyb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto yyiis = yyb.Finish();

    quantra::PriceYearOnYearInflationSwapBuilder pyb(b);
    pyb.add_year_on_year_inflation_swap(yyiis);
    pyb.add_discounting_curve(discCurveId);
    pyb.add_inflation_curve(curveId);
    auto trade = pyb.Finish();
    auto trades = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceYearOnYearInflationSwap>>{trade});

    quantra::PriceYearOnYearInflationSwapRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_swaps(trades);
    reqb.add_include_flows(true);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceYearOnYearInflationSwapRequest>(b.GetBufferPointer());
    YearOnYearInflationSwapPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::PriceYearOnYearInflationSwapResponse>(outBuilder->GetBufferPointer());
    ASSERT_NE(resp->swaps(), nullptr);
    ASSERT_EQ(resp->swaps()->size(), 1u);
    const auto* priced = resp->swaps()->Get(0);
    ASSERT_NE(priced, nullptr);
    ASSERT_NE(priced->fixed_leg_flows(), nullptr);
    ASSERT_NE(priced->yoy_leg_flows(), nullptr);
    EXPECT_EQ(priced->fixed_leg_flows()->size(), 2u);
    EXPECT_EQ(priced->yoy_leg_flows()->size(), 2u);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(req->pricing());
    auto discountIt = reg.rates.curves.find("DISC");
    ASSERT_NE(discountIt, reg.rates.curves.end());
    auto indexIt = reg.inflation.inflationIndices.find("EUHICP_YY");
    ASSERT_NE(indexIt, reg.inflation.inflationIndices.end());
    auto yoyIndex = std::dynamic_pointer_cast<QuantLib::YoYInflationIndex>(indexIt->second);
    ASSERT_TRUE(static_cast<bool>(yoyIndex));

    auto fixedQlSchedule = QuantLib::Schedule(
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2027),
        QuantLib::Period(QuantLib::Annual),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward,
        false);
    auto yoyQlSchedule = fixedQlSchedule;

    auto expected = std::make_shared<QuantLib::YearOnYearInflationSwap>(
        QuantLib::YearOnYearInflationSwap::Receiver,
        1000000.0,
        fixedQlSchedule,
        0.0204,
        QuantLib::Actual365Fixed(),
        yoyQlSchedule,
        yoyIndex,
        QuantLib::Period(3, QuantLib::Months),
        QuantLib::CPI::Linear,
        0.0002,
        QuantLib::Actual365Fixed(),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing);
    QuantLib::setCouponPricer(
        expected->yoyLeg(),
        QuantLib::ext::make_shared<QuantLib::BlackYoYInflationCouponPricer>(
            QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink())));
    expected->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    EXPECT_NEAR(priced->npv(), expected->NPV(), 1e-8);
    EXPECT_NEAR(priced->fair_rate(), expected->fairRate(), 1e-10);
    EXPECT_NEAR(priced->fair_spread(), expected->fairSpread(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_bps(), expected->legBPS(0), 1e-10);
    EXPECT_NEAR(priced->yoy_leg_bps(), expected->legBPS(1), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_npv(), expected->fixedLegNPV(), 1e-8);
    EXPECT_NEAR(priced->yoy_leg_npv(), expected->yoyLegNPV(), 1e-8);
}

}} // namespace
