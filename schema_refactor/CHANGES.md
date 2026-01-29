# Schema Refactoring Changes

## Overview

This refactoring makes two key changes:
1. **Consolidate all enums into `enums.fbs`** - cleaner, consistent `quantra::enums::` namespace
2. **Add volatility surfaces to `Pricing`** - create once, reference by ID (like curves)

## Files Changed

### FlatBuffers Schema Files (.fbs)

| File | Change |
|------|--------|
| `enums.fbs` | Added: `SwapType`, `FRAType`, `CapFloorType`, `ExerciseType`, `SettlementType`, `VolatilityType` |
| `volatility.fbs` | Removed `VolatilityType` enum, use `enums.VolatilityType` |
| `vanilla_swap.fbs` | Removed `SwapType` enum, use `enums.SwapType` |
| `fra.fbs` | Removed `FRAType` enum, use `enums.FRAType` |
| `cap_floor.fbs` | Removed `CapFloorType` enum, use `enums.CapFloorType` |
| `swaption.fbs` | Removed `ExerciseType`/`SettlementType` enums, use `enums.*` |
| `common.fbs` | Added `volatilities:[VolatilityTermStructure]` to `Pricing` |
| `price_cap_floor_request.fbs` | Changed `volatility:VolatilityTermStructure` → `volatility:string` |
| `price_swaption_request.fbs` | Changed `volatility:VolatilityTermStructure` → `volatility:string` |

### C++ Parser Files

| File | Change |
|------|--------|
| `parser/common_parser.h` | Added `volatilities` to `PricingStruct` |
| `parser/common_parser.cpp` | Parse `volatilities` field |

### C++ Request Handler Files

| File | Change |
|------|--------|
| `request/cap_floor_pricing_request.cpp` | Build vol map once, lookup by ID |
| `request/swaption_pricing_request.cpp` | Build vol map once, lookup by ID |

### C++ Parser Files (enum namespace changes)

| File | Change |
|------|--------|
| `parser/vanilla_swap_parser.cpp` | `quantra::SwapType_*` → `quantra::enums::SwapType_*` |
| `parser/fra_parser.cpp` | `quantra::FRAType_*` → `quantra::enums::FRAType_*` |
| `parser/cap_floor_parser.cpp` | `quantra::CapFloorType_*` → `quantra::enums::CapFloorType_*` |
| `parser/swaption_parser.cpp` | `quantra::ExerciseType_*` → `quantra::enums::ExerciseType_*` |
| `parser/volatility_parser.cpp` | `quantra::VolatilityType_*` → `quantra::enums::VolatilityType_*` |

## How Volatility Now Works

### Before (inefficient - volatility built per instrument):
```
PriceCapFloorRequest
├── Pricing
│   └── curves: [TermStructure]           ← bootstrapped once
└── cap_floors: [
      ├── cap1 { volatility: VolatilityTermStructure }  ← built
      ├── cap2 { volatility: VolatilityTermStructure }  ← built again (duplicate!)
      └── cap3 { volatility: VolatilityTermStructure }  ← built again (duplicate!)
    ]
```

### After (efficient - volatility built once, referenced by ID):
```
PriceCapFloorRequest
├── Pricing
│   ├── curves: [TermStructure]           ← bootstrapped once
│   └── volatilities: [VolatilityTermStructure]  ← built once
└── cap_floors: [
      ├── cap1 { volatility: "vol_20pct" }  ← reference
      ├── cap2 { volatility: "vol_20pct" }  ← same reference
      └── cap3 { volatility: "vol_25pct" }  ← different vol
    ]
```

## Step-by-Step Migration

### Step 1: Update Schema Files
```bash
cp schema_refactor/fbs/*.fbs /workspace/flatbuffers/fbs/
```

### Step 2: Regenerate C++ Headers
```bash
cd /workspace
./flatbuffers/generate.sh
```

### Step 3: Update Parser Files
```bash
cp schema_refactor/parser/*.cpp /workspace/parser/
cp schema_refactor/parser/*.h /workspace/parser/
```

### Step 4: Update Request Handlers
```bash
cp schema_refactor/request/*.cpp /workspace/request/
```

### Step 5: Fix Enum Namespaces in Parsers
Run these sed commands or manually update:
```bash
# vanilla_swap_parser.cpp
sed -i 's/quantra::SwapType_/quantra::enums::SwapType_/g' parser/vanilla_swap_parser.cpp

# fra_parser.cpp
sed -i 's/quantra::FRAType_/quantra::enums::FRAType_/g' parser/fra_parser.cpp

# cap_floor_parser.cpp
sed -i 's/quantra::CapFloorType_/quantra::enums::CapFloorType_/g' parser/cap_floor_parser.cpp

# swaption_parser.cpp
sed -i 's/quantra::ExerciseType_/quantra::enums::ExerciseType_/g' parser/swaption_parser.cpp
sed -i 's/quantra::SettlementType_/quantra::enums::SettlementType_/g' parser/swaption_parser.cpp

# volatility_parser.cpp
sed -i 's/quantra::VolatilityType_/quantra::enums::VolatilityType_/g' parser/volatility_parser.cpp
```

### Step 6: Rebuild
```bash
cd /workspace/build
cmake ..
make -j$(nproc)
```

### Step 7: Run Tests
```bash
./tests/run_all_tests.sh
```

## Test Changes Required

The tests in `test_quantra_vs_quantlib.cpp` and `test_server_client.cpp` need updates:

1. Add volatility to `Pricing` builder:
```cpp
// Build volatility surface
auto vol = buildVolatility(b, "vol_20pct", 0.20);
auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolatilityTermStructure>>{vol});

// Add to Pricing
pb.add_volatilities(vols);
```

2. Reference by ID in instrument pricing:
```cpp
auto vol_id = b.CreateString("vol_20pct");
pcb.add_volatility(vol_id);  // String reference, not object
```

3. Update enum namespaces:
```cpp
// Before
quantra::SwapType_Payer

// After  
quantra::enums::SwapType_Payer
```
