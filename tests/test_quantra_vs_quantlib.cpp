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
 *   - IndexDef objects registered in Pricing.indices / BootstrapCurvesRequest.indices
 */

#include <gtest/gtest.h>
#include <ql/quantlib.hpp>
#include <iostream>
#include <iomanip>

#include "fixed_rate_bond_pricing_request.h"
#include "vanilla_swap_pricing_request.h"
#include "fra_pricing_request.h"
#include "cap_floor_pricing_request.h"
#include "swaption_pricing_request.h"
#include "cds_pricing_request.h"
#include "bootstrap_curves_request.h"
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
#include "ois_swap_generated.h"
#include "price_cds_request_generated.h"
#include "cds_response_generated.h"
#include "volatility_generated.h"
#include "model_generated.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"
#include "index_generated.h"
#include "swap_index_generated.h"
#include "quotes_generated.h"

namespace quantra { namespace testing {

class QuantraComparisonTest : public ::testing::Test {
protected:
    void SetUp() override {
        evaluationDate_ = QuantLib::Date(15, QuantLib::January, 2025);
        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        flatRate_ = 0.03;
        buildBootstrappedCurve();
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
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Ibor);
        idb.add_tenor_number(3);
        idb.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
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
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Ibor);
        idb.add_tenor_number(6);
        idb.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
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
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Overnight);
        idb.add_tenor_number(0);
        idb.add_tenor_time_unit(quantra::enums::TimeUnit_Days);
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

    // =========================================================================
    // Yield curve builder for Quantra (with IndexRef for SwapHelpers)
    // =========================================================================

    flatbuffers::Offset<quantra::TermStructure> buildCurve(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id) {
        
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;
        
        // 3M deposit
        quantra::DepositHelperBuilder dep3m(b);
        dep3m.add_rate(flatRate_);
        dep3m.add_tenor_number(3);
        dep3m.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
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
        quantra::DepositHelperBuilder dep6m(b);
        dep6m.add_rate(flatRate_);
        dep6m.add_tenor_number(6);
        dep6m.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
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
        quantra::DepositHelperBuilder dep1y(b);
        dep1y.add_rate(flatRate_);
        dep1y.add_tenor_number(1);
        dep1y.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
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
        quantra::SwapHelperBuilder sw5y(b);
        sw5y.add_rate(flatRate_);
        sw5y.add_tenor_number(5);
        sw5y.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
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
        quantra::SwapHelperBuilder sw10y(b);
        sw10y.add_rate(flatRate_);
        sw10y.add_tenor_number(10);
        sw10y.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
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
            quantra::OISHelperBuilder ois(b);
            ois.add_rate(tr.rate);
            ois.add_tenor_number(tr.tenor_number);
            ois.add_tenor_time_unit(tr.tenor_unit);
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
    
    /// Build indices vector for Pricing or BootstrapCurvesRequest
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

        quantra::SwapIndexFloatLegSpecBuilder fl(b);
        fl.add_float_tenor_number(6);
        fl.add_float_tenor_time_unit(quantra::enums::TimeUnit_Months);
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

            quantra::SwapIndexFloatLegSpecBuilder flo(b);
            flo.add_float_tenor_number(1);
            flo.add_float_tenor_time_unit(quantra::enums::TimeUnit_Days);
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
    
    // Build SwaptionVolSurface for Swaptions (using new union-based schema)
    flatbuffers::Offset<quantra::VolSurfaceSpec> buildSwaptionVolSurface(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id, double vol,
        quantra::enums::VolatilityType volType = quantra::enums::VolatilityType_Lognormal,
        double displacement = 0.0,
        const std::string& quoteId = "",
        const std::string& refDate = "2025-01-15") {
        
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

        quantra::SwaptionVolSpecBuilder swpBuilder(b);
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
        const std::string& refDate = "2025-01-15") {

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

        std::vector<flatbuffers::Offset<quantra::PeriodSpec>> expOffsets;
        for (const auto& p : expiries) {
            quantra::PeriodSpecBuilder pb(b);
            pb.add_tenor_number(p.length());
            pb.add_tenor_time_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::PeriodSpec>> tenOffsets;
        for (const auto& p : tenors) {
            quantra::PeriodSpecBuilder pb(b);
            pb.add_tenor_number(p.length());
            pb.add_tenor_time_unit(toFbTimeUnit(p.units()));
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

        quantra::SwaptionVolSpecBuilder swpBuilder(b);
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

        std::vector<flatbuffers::Offset<quantra::PeriodSpec>> expOffsets;
        for (const auto& p : expiries) {
            quantra::PeriodSpecBuilder pb(b);
            pb.add_tenor_number(p.length());
            pb.add_tenor_time_unit(toFbTimeUnit(p.units()));
            expOffsets.push_back(pb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::PeriodSpec>> tenOffsets;
        for (const auto& p : tenors) {
            quantra::PeriodSpecBuilder pb(b);
            pb.add_tenor_number(p.length());
            pb.add_tenor_time_unit(toFbTimeUnit(p.units()));
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
        flatbuffers::Offset<flatbuffers::String> swapIndexIdOff;
        if (!swapIndexId.empty()) {
            swapIndexIdOff = b.CreateString(swapIndexId);
        }
        quantra::SwaptionVolSmileCubeSpecBuilder cubeBuilder(b);
        cubeBuilder.add_base(base);
        cubeBuilder.add_expiries(expVec);
        cubeBuilder.add_tenors(tenVec);
        cubeBuilder.add_strikes(strikeVec);
        cubeBuilder.add_strike_kind(strikeKind);
        cubeBuilder.add_allow_external_atm(allowExternalAtm);
        if (!swapIndexId.empty()) {
            cubeBuilder.add_swap_index_id(swapIndexIdOff);
        }
        if (!atmForwardsFlat.empty()) {
            cubeBuilder.add_atm_forwards(atmMatrix);
        }
        cubeBuilder.add_vols(tensor);
        auto cubePayload = cubeBuilder.Finish();

        quantra::SwaptionVolSpecBuilder swpBuilder(b);
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
        quantra::enums::IrModelType modelType = quantra::enums::IrModelType_Black) {
        
        quantra::SwaptionModelSpecBuilder smBuilder(b);
        smBuilder.add_model_type(modelType);
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
    std::shared_ptr<QuantLib::YieldTermStructure> bootstrappedCurve_;
    QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle_, forwardHandle_;
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
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    pb.add_bond_pricing_details(true);
    auto pricing = pb.Finish();
    
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
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
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
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
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
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    pb.add_swaption_pricing_rebump(true);
    auto pricing = pb.Finish();
    
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
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    auto pricing = pb.Finish();
    
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
    swb.add_underlying_swap(uswap);
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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_swap_indices(swapIndices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    auto pricing = pb.Finish();

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
    swb.add_underlying_swap(uswap);
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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_swap_indices(swapIndices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    auto pricing = pb.Finish();

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
    swb.add_underlying_swap(uswap);
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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_swap_indices(swapIndices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    auto pricing = pb.Finish();

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
    swb.add_underlying_swap(uswap);
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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    pb.add_quotes(quotes);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    auto pricing = pb.Finish();

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
    swb.add_underlying_swap(uswap);
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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    auto pricing = pb.Finish();

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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_indices(indices);
    pb.add_swap_indices(swapIndices);
    pb.add_curves(curves);
    pb.add_vol_surfaces(vols);
    pb.add_models(models);
    pb.add_swaption_pricing_rebump(true);
    auto pricing = pb.Finish();

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

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    pb.add_credit_curves(credit_curves);
    pb.add_models(models);
    auto pricing = pb.Finish();
    
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
    std::vector<flatbuffers::Offset<quantra::Tenor>> tenors;
    
    quantra::TenorBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    
    quantra::TenorBuilder t5y(b);
    t5y.add_n(5); t5y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t5y.Finish());
    
    quantra::TenorBuilder t10y(b);
    t10y.add_n(10); t10y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t10y.Finish());
    
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::CurveGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::CurveGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
    auto measures = b.CreateVector(measures_vec);
    
    quantra::CurveQueryBuilder cqb(b);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    auto curve = buildCurve(b, "test_curve");
    quantra::BootstrapCurveSpecBuilder bcsb(b);
    bcsb.add_curve(curve);
    bcsb.add_query(query);
    auto curve_spec = bcsb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::BootstrapCurveSpec>> curves_vec;
    curves_vec.push_back(curve_spec);
    auto curves = b.CreateVector(curves_vec);
    
    // IndexDefs needed by SwapHelpers
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_as_of_date(as_of);
    reqb.add_indices(indices);
    reqb.add_curves(curves);
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
    
    std::vector<flatbuffers::Offset<quantra::Tenor>> tenors;
    
    quantra::TenorBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    
    quantra::TenorBuilder t5y(b);
    t5y.add_n(5); t5y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t5y.Finish());
    
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::CurveGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::CurveGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_ZERO)};
    auto measures = b.CreateVector(measures_vec);
    
    quantra::CurveQueryBuilder cqb(b);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    auto curve = buildCurve(b, "test_curve");
    quantra::BootstrapCurveSpecBuilder bcsb(b);
    bcsb.add_curve(curve);
    bcsb.add_query(query);
    auto curve_spec = bcsb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::BootstrapCurveSpec>> curves_vec;
    curves_vec.push_back(curve_spec);
    auto curves = b.CreateVector(curves_vec);
    
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_as_of_date(as_of);
    reqb.add_indices(indices);
    reqb.add_curves(curves);
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
    
    std::vector<flatbuffers::Offset<quantra::Tenor>> tenors;
    quantra::TenorBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::CurveGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::CurveGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
    auto measures = b.CreateVector(measures_vec);
    
    quantra::CurveQueryBuilder cqb(b);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    auto curve = buildCurve(b, "test_curve");
    quantra::BootstrapCurveSpecBuilder bcsb(b);
    bcsb.add_curve(curve);
    bcsb.add_query(query);
    auto curve_spec = bcsb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::BootstrapCurveSpec>> curves_vec;
    curves_vec.push_back(curve_spec);
    auto curves = b.CreateVector(curves_vec);
    
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_as_of_date(as_of);
    reqb.add_indices(indices);
    reqb.add_curves(curves);
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

}} // namespace
