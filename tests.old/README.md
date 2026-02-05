# Quantra Testing Framework

## Overview

This testing framework provides unit tests that verify Quantra pricing matches raw QuantLib calculations, integration tests for full server/client gRPC communication, and the necessary test infrastructure including scripts, fixtures, and helpers.

## Directory Structure

```
tests/
├── test_quantra_vs_quantlib.cpp  # Unit tests comparing Quantra to QuantLib
├── test_server_client.cpp        # gRPC integration tests
├── test_json_api_vs_quantlib.py  # JSON API tests
├── test_python_client.py         # Python client tests
├── run_all_tests.sh              # Full test runner
├── smoke_test.sh                 # Quick validation
└── CMakeLists.txt                # Test build configuration
```

## Building Tests

Tests are built automatically as part of the main project:

```bash
cd build
cmake ..
make -j$(nproc)
```

## Running Tests

Run all tests:
```bash
./tests/run_all_tests.sh
```

Run specific product tests:
```bash
./tests/run_all_tests.sh FixedRateBond
./tests/run_all_tests.sh VanillaSwap
./tests/run_all_tests.sh FRA
./tests/run_all_tests.sh CapFloor
./tests/run_all_tests.sh Swaption
./tests/run_all_tests.sh CDS
```

Run with verbose output:
```bash
./tests/run_all_tests.sh --verbose
```

Run directly with GoogleTest:
```bash
cd build/tests

# All tests
./quantra_tests

# Specific test
./quantra_tests --gtest_filter="*SwapNPV*"

# List all tests
./quantra_tests --gtest_list_tests
```

## Test Categories

### Unit Tests

Each product has tests that verify basic pricing (NPV calculation), payer/receiver parity (Long + Short = 0), at-market pricing (instruments at fair rate have zero NPV), and sensitivity tests (e.g., higher vol increases option value).

### Integration Tests

Test the full request/response cycle: build FlatBuffers request, send to gRPC server, parse response, and verify results match direct QuantLib calculation.

## Adding New Tests

### Add a Test for New Product

1. Create `test_new_product.cpp`:
   ```cpp
   #include "test_framework.h"
   
   namespace quantra {
   namespace testing {
   
   class NewProductPricingTest : public QuantraPricingTest
   {
   protected:
       void SetUp() override
       {
           QuantraPricingTest::SetUp();
           // Setup product-specific parameters
       }
   };
   
   TEST_F(NewProductPricingTest, BasicPricing)
   {
       // Test implementation
   }
   
   } // namespace testing
   } // namespace quantra
   ```

2. Add to CMakeLists.txt:
   ```cmake
   add_executable(test_new_product
       test_new_product.cpp
       ${COMMON_SOURCES}
   )
   target_link_libraries(test_new_product
       GTest::gtest_main
       QuantLib::QuantLib
   )
   add_test(NAME NewProductTests COMMAND test_new_product)
   ```

## Test Helpers

### Price Comparison Macros

```cpp
EXPECT_PRICE_EQ(expected, actual)  // Tolerance: 1e-6
EXPECT_RATE_EQ(expected, actual)   // Tolerance: 1e-8
```

### Yield Curve Builder

```cpp
auto curve = YieldCurveBuilder(evaluationDate)
    .addDeposit(0.03, 1*Months)
    .addDeposit(0.032, 3*Months)
    .addSwap(0.035, 2*Years)
    .addSwap(0.04, 5*Years)
    .build();
```

## CI/CD Integration

Example GitHub Actions workflow:

```yaml
name: Tests
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build
        run: |
          mkdir build && cd build
          cmake ..
          make -j$(nproc)
      - name: Test
        run: |
          cd build
          ctest --output-on-failure
```

## Coverage

To generate coverage reports:

```bash
# Build with coverage
cmake -DCMAKE_BUILD_TYPE=Debug -DCOVERAGE=ON ..
make -j$(nproc)

# Run tests
ctest

# Generate report
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
```
