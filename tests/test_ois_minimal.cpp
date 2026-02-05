/**
 * Minimal test to isolate OISHelper / ESTR crash
 * 
 * Compile:
 *   g++ -std=c++17 -I/opt/quantra-deps/include -L/opt/quantra-deps/lib \
 *       test_ois_minimal.cpp -lQuantLib -o test_ois_minimal
 * 
 * Run:
 *   ./test_ois_minimal
 */

#include <iostream>
#include <memory>

#include <ql/quantlib.hpp>

using namespace QuantLib;

int main() {
    try {
        std::cout << "QuantLib version: " << QL_VERSION << std::endl;
        std::cout << "QuantLib hex version: 0x" << std::hex << QL_HEX_VERSION << std::dec << std::endl;
        
        // Set evaluation date
        Date today(15, January, 2026);
        Settings::instance().evaluationDate() = today;
        std::cout << "Evaluation date: " << today << std::endl;
        
        // Test 1: Create generic OvernightIndex for ESTR
        std::cout << "\n=== Test 1: Generic OvernightIndex for ESTR ===" << std::endl;
        try {
            auto estrGeneric = std::make_shared<OvernightIndex>(
                "ESTR", 0,
                EURCurrency(),
                TARGET(),
                Actual360());
            std::cout << "  Created generic ESTR index: " << estrGeneric->name() << std::endl;
        } catch (std::exception& e) {
            std::cout << "  FAILED: " << e.what() << std::endl;
        }
        
        // Test 2: Try to create Estr class (may not exist)
        std::cout << "\n=== Test 2: QuantLib::Estr class ===" << std::endl;
#if QL_HEX_VERSION >= 0x011d00
        std::cout << "  QL_HEX_VERSION check passed (>= 0x011d00)" << std::endl;
        try {
            // This line may cause compile error if Estr doesn't exist
            // auto estr = std::make_shared<Estr>();
            // std::cout << "  Created Estr: " << estr->name() << std::endl;
            std::cout << "  Skipping Estr class test (may not exist in QL 1.41)" << std::endl;
        } catch (std::exception& e) {
            std::cout << "  FAILED: " << e.what() << std::endl;
        }
#else
        std::cout << "  QL_HEX_VERSION check failed (< 0x011d00)" << std::endl;
#endif

        // Test 3: Create OISRateHelper with generic OvernightIndex
        std::cout << "\n=== Test 3: OISRateHelper with generic ESTR ===" << std::endl;
        try {
            auto estrIndex = std::make_shared<OvernightIndex>(
                "ESTR", 0,
                EURCurrency(),
                TARGET(),
                Actual360());
            
            Handle<Quote> rate(std::make_shared<SimpleQuote>(0.03));
            
            auto helper = std::make_shared<OISRateHelper>(
                2,           // settlement days
                1 * Years,   // tenor
                rate,
                estrIndex);
            
            std::cout << "  Created OISRateHelper successfully!" << std::endl;
            std::cout << "  Maturity: " << helper->maturityDate() << std::endl;
        } catch (std::exception& e) {
            std::cout << "  FAILED: " << e.what() << std::endl;
        }
        
        // Test 4: Bootstrap a simple OIS curve
        std::cout << "\n=== Test 4: Bootstrap OIS curve ===" << std::endl;
        try {
            auto estrIndex = std::make_shared<OvernightIndex>(
                "ESTR", 0,
                EURCurrency(),
                TARGET(),
                Actual360());
            
            std::vector<std::shared_ptr<RateHelper>> helpers;
            
            Handle<Quote> rate1y(std::make_shared<SimpleQuote>(0.03));
            helpers.push_back(std::make_shared<OISRateHelper>(2, 1*Years, rate1y, estrIndex));
            
            Handle<Quote> rate5y(std::make_shared<SimpleQuote>(0.029));
            helpers.push_back(std::make_shared<OISRateHelper>(2, 5*Years, rate5y, estrIndex));
            
            Handle<Quote> rate10y(std::make_shared<SimpleQuote>(0.028));
            helpers.push_back(std::make_shared<OISRateHelper>(2, 10*Years, rate10y, estrIndex));
            
            auto curve = std::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(
                today, helpers, Actual360());
            
            curve->enableExtrapolation();
            
            std::cout << "  Bootstrapped OIS curve successfully!" << std::endl;
            std::cout << "  Reference date: " << curve->referenceDate() << std::endl;
            std::cout << "  DF(1Y): " << curve->discount(today + 1*Years) << std::endl;
            std::cout << "  DF(5Y): " << curve->discount(today + 5*Years) << std::endl;
            std::cout << "  DF(10Y): " << curve->discount(today + 10*Years) << std::endl;
        } catch (std::exception& e) {
            std::cout << "  FAILED: " << e.what() << std::endl;
        }
        
        std::cout << "\n=== All tests completed ===" << std::endl;
        return 0;
        
    } catch (std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return 1;
    }
}