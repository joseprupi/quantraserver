#pragma once

/**
 * Shared fixture + helpers for the Quantra vs QuantLib parity suite.
 *
 * Every parity test translation unit includes this header and shares the single
 * QuantraComparisonTest fixture, so the whole suite builds into ONE gtest binary
 * (test_quantra_vs_quantlib).
 */

#include <gtest/gtest.h>
#include <ql/quantlib.hpp>
#include <iostream>
#include <iomanip>

#include "fixed_rate_bond_handler.h"
#include "vanilla_swap_handler.h"
#include "fra_handler.h"
#include "cap_floor_handler.h"
#include "swaption_handler.h"
#include "cds_handler.h"
#include "bootstrap_curves_handler.h"
#include "sample_vol_surfaces_handler.h"
#include "bootstrap_inflation_curves_handler.h"
#include "zero_coupon_inflation_swap_handler.h"
#include "year_on_year_inflation_swap_handler.h"
#include "equity_option_handler.h"
#include "floating_rate_bond_handler.h"
#include "ois_swap_handler.h"
#include "basis_swap_handler.h"
#include "calendar_business_days_handler.h"
#include "calendar_holidays_handler.h"
#include "calendar_advance_handler.h"
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
#include "calibrate_swaption_model_handler.h"
#include "swaption_model_calibration.h"
#include "pricing_registry.h"
#include "cms_leg_parser.h"
#include "calibrate_swaption_vol_handler.h"
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
        idb.add_end_of_month(true);
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

    // Build a CouponPricer vector with a single BlackIborCouponPricer wrapping a
    // ConstantOptionletVolatility. Mirrors the conventions the FloatingRateBond
    // pricer feeds into QuantLib (settlement_days/calendar/bdc/vol/day_counter),
    // so a zero-vol pricer leaves plain-vanilla floating coupons unchanged.
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CouponPricer>>>
    buildCouponPricerVector(
        flatbuffers::grpc::MessageBuilder& b,
        const std::string& id,
        double vol = 0.0,
        int settlementDays = 2,
        quantra::enums::Calendar calendar = quantra::enums::Calendar_TARGET,
        quantra::enums::BusinessDayConvention bdc =
            quantra::enums::BusinessDayConvention_ModifiedFollowing,
        quantra::enums::DayCounter dayCounter =
            quantra::enums::DayCounter_Actual365Fixed) {
        quantra::ConstantOptionletVolatilityBuilder ovb(b);
        ovb.add_settlement_days(settlementDays);
        ovb.add_calendar(calendar);
        ovb.add_business_day_convention(bdc);
        ovb.add_volatility(vol);
        ovb.add_day_counter(dayCounter);
        auto ov = ovb.Finish();

        quantra::BlackIborCouponPricerBuilder bcb(b);
        bcb.add_optionlet_volatility(ov);
        auto bc = bcb.Finish();

        auto cpId = b.CreateString(id);
        quantra::CouponPricerBuilder cpb(b);
        cpb.add_id(cpId);
        cpb.add_black_ibor_coupon_pricer(bc);
        auto cp = cpb.Finish();

        return b.CreateVector(
            std::vector<flatbuffers::Offset<quantra::CouponPricer>>{cp});
    }

    // Build an indices vector containing the EUR 6M Ibor index (needed by the
    // curve's SwapHelpers) plus the USD SOFR overnight index used by OIS swaps.
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>>
    buildIndicesVectorWithSofr(flatbuffers::grpc::MessageBuilder& b) {
        std::vector<flatbuffers::Offset<quantra::IndexDef>> defs;
        defs.push_back(buildIndexDef_EUR6M(b));
        defs.push_back(buildIndexDef_USD_SOFR(b));
        return b.CreateVector(defs);
    }

    QuantLib::Date evaluationDate_;
    double flatRate_;
    double dividendFlatRate_;
    std::shared_ptr<QuantLib::YieldTermStructure> bootstrappedCurve_;
    std::shared_ptr<QuantLib::YieldTermStructure> dividendCurve_;
    QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle_, forwardHandle_, dividendHandle_;
};

// ---------------------------------------------------------------------------
// SABR synthetic helpers shared across the swaption / sample_vol_surfaces /
// calibrate_swaption_vol parity tests. Relocated verbatim from the monolith's
// file-local anonymous namespaces; the free function is marked `inline` so it
// can live in this shared header without violating the ODR.
// ---------------------------------------------------------------------------
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

// Compute synthetic market vols for the calibrate-path round-trip test.
// `forwards` is row-major nExp*nTen, `spreads` is per-strike. Returns a
// row-major nExp*nTen*nStrikes vector ready to feed into a
// SwaptionSabrCalibrateSpec.
inline std::vector<double> sabrSyntheticMarketVols(
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

}} // namespace quantra::testing
