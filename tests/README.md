# Quantra Testing Framework

## Overview

This testing framework provides:

1. **Unit Tests** - Test that Quantra pricing matches raw QuantLib calculations
2. **Integration Tests** - Test full server/client gRPC communication
3. **Test Infrastructure** - Scripts, fixtures, and helpers

## Directory Structure

```
testing/
├── tests/
│   ├── test_framework.h          # Base test classes and helpers
│   ├── test_fixed_rate_bond.cpp  # Fixed rate bond tests
│   ├── test_vanilla_swap.cpp     # Vanilla swap tests
│   ├── test_fra.cpp              # FRA tests
│   ├── test_cap_floor.cpp        # Cap/Floor tests
│   ├── test_swaption.cpp         # Swaption tests
│   ├── test_cds.cpp              # CDS tests
│   ├── test_integration.cpp      # Server/client integration tests
│   └── CMakeLists.txt            # Test build configuration
├── fixtures/
│   └── (test data files)
├── scripts/
│   └── run_tests.sh              # Test runner script
└── README.md
```

## Installation

1. Copy the `tests/` directory to your workspace:
   ```bash
   cp -r testing/tests /workspace/tests_new
   ```

2. Update main CMakeLists.txt to include tests:
   ```cmake
   add_subdirectory(tests_new)
   ```

3. Build:
   ```bash
   cd /workspace/build
   cmake ..
   make -j$(nproc)
   ```

## Running Tests

### Run All Tests
```bash
./scripts/run_tests.sh
```

### Run Specific Product Tests
```bash
./scripts/run_tests.sh FixedRateBond
./scripts/run_tests.sh VanillaSwap
./scripts/run_tests.sh FRA
./scripts/run_tests.sh CapFloor
./scripts/run_tests.sh Swaption
./scripts/run_tests.sh CDS
```

### Run with Verbose Output
```bash
./scripts/run_tests.sh --verbose
```

### Run Directly with GoogleTest
```bash
cd /workspace/build/tests_new

# All tests
./quantra_tests

# Specific test
./quantra_tests --gtest_filter="*SwapNPV*"

# List all tests
./quantra_tests --gtest_list_tests
```

## Test Categories

### Unit Tests

Each product has tests that verify:

1. **Basic Pricing** - NPV calculation works and is reasonable
2. **Payer/Receiver Parity** - Long + Short = 0
3. **At-Market Pricing** - Instruments at fair rate have zero NPV
4. **Sensitivity Tests** - Higher vol increases option value, etc.

### Integration Tests

Test the full request/response cycle:

1. Build FlatBuffers request
2. Send to gRPC server
3. Parse response
4. Verify results match direct QuantLib calculation

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
