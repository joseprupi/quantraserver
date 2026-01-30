/**
 * Quantra vs QuantLib Performance Benchmark
 * 
 * Compares pricing performance between:
 * - Quantra (distributed via Envoy load balancer to N workers)
 * - QuantLib (serial, single-threaded)
 * 
 * Usage:
 *   ./benchmark <bonds_per_request> <num_requests> [share_curve]
 * 
 * Arguments:
 *   bonds_per_request - Number of bonds in each request
 *   num_requests      - Number of parallel requests (should match worker count)
 *   share_curve       - 0: bootstrap curve for each bond, 1: share curve (default: 0)
 * 
 * Examples:
 *   ./benchmark 100 10 0    # 1000 bonds total, 10 requests, bootstrap each
 *   ./benchmark 1000 10 1   # 10000 bonds total, 10 requests, shared curve
 */

#include <vector>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>

// Include QuantLib first with explicit namespace usage
#include <ql/instruments/bonds/fixedratebond.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/bondhelpers.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

// Then include Quantra client
#include "quantra_client.h"
#include "request_data.h"

// Build a yield curve using the same parameters as the Quantra request
std::shared_ptr<QuantLib::PricingEngine> build_bond_engine() {
    QuantLib::Calendar calendar = QuantLib::TARGET();
    QuantLib::Date settlementDate(18, QuantLib::September, 2008);
    QuantLib::Integer fixingDays = 3;
    QuantLib::Natural settlementDays = 3;
    
    QuantLib::Settings::instance().evaluationDate() = settlementDate;
    
    // Deposit rates
    QuantLib::Rate zc3mQuote = 0.0096;
    QuantLib::Rate zc6mQuote = 0.0145;
    QuantLib::Rate zc1yQuote = 0.0194;
    
    auto zc3mRate = std::make_shared<QuantLib::SimpleQuote>(zc3mQuote);
    auto zc6mRate = std::make_shared<QuantLib::SimpleQuote>(zc6mQuote);
    auto zc1yRate = std::make_shared<QuantLib::SimpleQuote>(zc1yQuote);
    
    QuantLib::DayCounter zcBondsDayCounter = QuantLib::Actual365Fixed();
    
    auto zc3m = std::make_shared<QuantLib::DepositRateHelper>(
        QuantLib::Handle<QuantLib::Quote>(zc3mRate), 3 * QuantLib::Months, fixingDays,
        calendar, QuantLib::ModifiedFollowing, true, zcBondsDayCounter);
    auto zc6m = std::make_shared<QuantLib::DepositRateHelper>(
        QuantLib::Handle<QuantLib::Quote>(zc6mRate), 6 * QuantLib::Months, fixingDays,
        calendar, QuantLib::ModifiedFollowing, true, zcBondsDayCounter);
    auto zc1y = std::make_shared<QuantLib::DepositRateHelper>(
        QuantLib::Handle<QuantLib::Quote>(zc1yRate), 1 * QuantLib::Years, fixingDays,
        calendar, QuantLib::ModifiedFollowing, true, zcBondsDayCounter);
    
    // Bond helpers
    QuantLib::Date issueDates[] = {
        QuantLib::Date(15, QuantLib::March, 2005), 
        QuantLib::Date(15, QuantLib::June, 2005), 
        QuantLib::Date(30, QuantLib::June, 2006),
        QuantLib::Date(15, QuantLib::November, 2002), 
        QuantLib::Date(15, QuantLib::May, 1987)
    };
    QuantLib::Date maturities[] = {
        QuantLib::Date(31, QuantLib::August, 2010), 
        QuantLib::Date(31, QuantLib::August, 2011), 
        QuantLib::Date(31, QuantLib::August, 2013),
        QuantLib::Date(15, QuantLib::August, 2018), 
        QuantLib::Date(15, QuantLib::May, 2038)
    };
    QuantLib::Real couponRates[] = {0.02375, 0.04625, 0.03125, 0.04000, 0.04500};
    QuantLib::Real marketQuotes[] = {100.390625, 106.21875, 100.59375, 101.6875, 102.140625};
    
    std::vector<std::shared_ptr<QuantLib::RateHelper>> bondInstruments;
    bondInstruments.push_back(zc3m);
    bondInstruments.push_back(zc6m);
    bondInstruments.push_back(zc1y);
    
    for (int i = 0; i < 5; i++) {
        auto quote = std::make_shared<QuantLib::SimpleQuote>(marketQuotes[i]);
        QuantLib::RelinkableHandle<QuantLib::Quote> quoteHandle;
        quoteHandle.linkTo(quote);
        
        QuantLib::Schedule schedule(issueDates[i], maturities[i], QuantLib::Period(QuantLib::Semiannual),
                         QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
                         QuantLib::Unadjusted, QuantLib::Unadjusted, QuantLib::DateGeneration::Backward, false);
        
        auto bondHelper = std::make_shared<QuantLib::FixedRateBondHelper>(
            quoteHandle, settlementDays, 100.0, schedule,
            std::vector<QuantLib::Rate>(1, couponRates[i]),
            QuantLib::ActualActual(QuantLib::ActualActual::Bond),
            QuantLib::Unadjusted, 100.0, issueDates[i]);
        
        bondInstruments.push_back(bondHelper);
    }
    
    QuantLib::DayCounter termStructureDayCounter = QuantLib::ActualActual(QuantLib::ActualActual::ISDA);
    
    auto bondDiscountingTermStructure = std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
        settlementDate, bondInstruments, termStructureDayCounter);
    
    QuantLib::RelinkableHandle<QuantLib::YieldTermStructure> discountingTermStructure;
    discountingTermStructure.linkTo(bondDiscountingTermStructure);
    
    return std::make_shared<QuantLib::DiscountingBondEngine>(discountingTermStructure);
}

// Price a single bond using QuantLib
double price_bond_quantlib(const std::shared_ptr<QuantLib::PricingEngine>& engine) {
    QuantLib::Natural settlementDays = 3;
    QuantLib::Real faceAmount = 100;
    
    QuantLib::Schedule fixedBondSchedule(
        QuantLib::Date(15, QuantLib::May, 2007), 
        QuantLib::Date(15, QuantLib::May, 2017), 
        QuantLib::Period(QuantLib::Semiannual),
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
        QuantLib::Unadjusted, QuantLib::Unadjusted, QuantLib::DateGeneration::Backward, false);
    
    QuantLib::FixedRateBond fixedRateBond(
        settlementDays, faceAmount, fixedBondSchedule,
        std::vector<QuantLib::Rate>(1, 0.045),
        QuantLib::ActualActual(QuantLib::ActualActual::Bond),
        QuantLib::ModifiedFollowing, 100.0, QuantLib::Date(15, QuantLib::May, 2007));
    
    fixedRateBond.setPricingEngine(engine);
    return fixedRateBond.NPV();
}

void print_separator() {
    std::cout << std::string(70, '=') << std::endl;
}

void print_results(const std::string& label, int total_bonds, double npv, long ms) {
    std::cout << std::left << std::setw(15) << label 
              << std::right << std::setw(10) << total_bonds << " bonds"
              << std::setw(15) << std::fixed << std::setprecision(2) << npv << " NPV"
              << std::setw(10) << ms << " ms" << std::endl;
}

int main(int argc, char** argv) {
    int bonds_per_request = 100;
    int num_requests = 10;
    int share_curve = 0;
    
    if (argc > 1) {
        std::istringstream(argv[1]) >> bonds_per_request;
    }
    if (argc > 2) {
        std::istringstream(argv[2]) >> num_requests;
    }
    if (argc > 3) {
        std::istringstream(argv[3]) >> share_curve;
    }
    
    int total_bonds = bonds_per_request * num_requests;
    
    print_separator();
    std::cout << "Quantra vs QuantLib Performance Benchmark" << std::endl;
    print_separator();
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Bonds per request: " << bonds_per_request << std::endl;
    std::cout << "  Number of requests: " << num_requests << std::endl;
    std::cout << "  Total bonds: " << total_bonds << std::endl;
    std::cout << "  Share curve: " << (share_curve ? "Yes (1 curve)" : "No (bootstrap each)") << std::endl;
    print_separator();
    
    // =========================================================================
    // QUANTRA TEST (Distributed)
    // =========================================================================
    std::cout << "\n[1/2] Running Quantra test (distributed)..." << std::endl;
    
    double total_quantra_npv = 0;
    auto quantra_start = std::chrono::steady_clock::now();
    
    try {
        QuantraClient client("localhost:50051", false);
        
        auto term_structure = term_structure_example();
        auto bond = fixed_rate_bond_example();
        auto request = pricing_request_example();
        
        std::vector<std::shared_ptr<structs::TermStructure>> term_structures;
        std::vector<std::shared_ptr<structs::PriceFixedRateBond>> bonds;
        
        if (share_curve == 1) {
            term_structures.push_back(term_structure);
        }
        
        for (int i = 0; i < bonds_per_request; i++) {
            auto price_fixed_rate_bond = std::make_shared<structs::PriceFixedRateBond>();
            price_fixed_rate_bond->fixed_rate_bond = bond;
            strcpy(price_fixed_rate_bond->discounting_curve, "depos");
            bonds.push_back(price_fixed_rate_bond);
            
            if (share_curve == 0) {
                term_structures.push_back(term_structure);
            }
        }
        
        request->pricing->curves = term_structures;
        request->bonds = bonds;
        
        std::vector<std::shared_ptr<structs::PriceFixedRateBondRequest>> requests;
        for (int i = 0; i < num_requests; i++) {
            requests.push_back(request);
        }
        
        auto responses = client.PriceFixedRateBond(requests);
        
        for (const auto& response : responses) {
            for (const auto& val : *response) {
                total_quantra_npv += val->npv;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Quantra error: " << e.what() << std::endl;
        return 1;
    }
    
    auto quantra_end = std::chrono::steady_clock::now();
    auto quantra_ms = std::chrono::duration_cast<std::chrono::milliseconds>(quantra_end - quantra_start).count();
    
    // =========================================================================
    // QUANTLIB TEST (Serial)
    // =========================================================================
    std::cout << "[2/2] Running QuantLib test (serial)..." << std::endl;
    
    double total_quantlib_npv = 0;
    auto quantlib_start = std::chrono::steady_clock::now();
    
    try {
        std::shared_ptr<QuantLib::PricingEngine> engine;
        
        for (int i = 0; i < total_bonds; i++) {
            // Bootstrap curve for each bond, or reuse
            if ((share_curve == 1 && i == 0) || share_curve == 0) {
                engine = build_bond_engine();
            }
            total_quantlib_npv += price_bond_quantlib(engine);
        }
    } catch (const std::exception& e) {
        std::cerr << "QuantLib error: " << e.what() << std::endl;
        return 1;
    }
    
    auto quantlib_end = std::chrono::steady_clock::now();
    auto quantlib_ms = std::chrono::duration_cast<std::chrono::milliseconds>(quantlib_end - quantlib_start).count();
    
    // =========================================================================
    // RESULTS
    // =========================================================================
    print_separator();
    std::cout << "Results:" << std::endl;
    print_separator();
    print_results("Quantra", total_bonds, total_quantra_npv, quantra_ms);
    print_results("QuantLib", total_bonds, total_quantlib_npv, quantlib_ms);
    print_separator();
    
    double speedup = (quantlib_ms > 0) ? static_cast<double>(quantlib_ms) / quantra_ms : 0;
    std::cout << "Speedup: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
    
    if (quantra_ms > 0 && quantlib_ms > 0) {
        double bonds_per_sec_quantra = (total_bonds * 1000.0) / quantra_ms;
        double bonds_per_sec_quantlib = (total_bonds * 1000.0) / quantlib_ms;
        std::cout << "Throughput:" << std::endl;
        std::cout << "  Quantra:  " << std::fixed << std::setprecision(0) << bonds_per_sec_quantra << " bonds/sec" << std::endl;
        std::cout << "  QuantLib: " << std::fixed << std::setprecision(0) << bonds_per_sec_quantlib << " bonds/sec" << std::endl;
    }
    
    // Verify NPV match (should be very close)
    double npv_diff = std::abs(total_quantra_npv - total_quantlib_npv);
    double npv_diff_pct = (total_quantlib_npv > 0) ? (npv_diff / total_quantlib_npv) * 100 : 0;
    std::cout << "\nNPV difference: " << std::fixed << std::setprecision(6) << npv_diff 
              << " (" << std::setprecision(4) << npv_diff_pct << "%)" << std::endl;
    
    print_separator();
    
    return 0;
}