# Quantra C++ Client Library

A clean, modular C++ client for the Quantra pricing engine.

## Structure

```
client/cpp/
├── include/
│   ├── quantra_client.h      # Main public header
│   └── product_registry.h    # Product definitions (ADD NEW PRODUCTS HERE)
├── src/
│   ├── json_parser.cpp       # JSON <-> FlatBuffers conversion
│   └── quantra_client.cpp    # Client implementation
├── CMakeLists.txt
└── README.md
```

## Usage

```cpp
#include "quantra_client.h"

// Create client
quantra::QuantraClient client("localhost:50051");

// JSON API (easy, slower)
quantra::JsonResponse result = client.PriceFixedRateBondJSON(json_string);
if (result.ok) {
    std::cout << result.body << std::endl;
}

// Native API (fast, requires FlatBuffers knowledge)
flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse> response;
grpc::Status status = client.PriceFixedRateBond(request, &response);
```

## Adding a New Product

### Step 1: Add to product_registry.h

```cpp
// In GetProductSchemas():
{ProductType::ExoticOption, {
    "price_exotic_option_request.fbs",
    "exotic_option_response.fbs"
}},

// In ProductTypeToString():
case ProductType::ExoticOption: return "ExoticOption";
```

### Step 2: Add to quantra_client.h

```cpp
// In enum ProductType:
ExoticOption,

// Add method declarations:
JsonResponse PriceExoticOptionJSON(const std::string& json);

grpc::Status PriceExoticOption(
    const Message<PriceExoticOptionRequest>& request,
    Message<PriceExoticOptionResponse>* response);
```

### Step 3: Add to quantra_client.cpp

```cpp
// JSON API:
JsonResponse QuantraClient::PriceExoticOptionJSON(const std::string& json) {
    return impl_->CallJSON<PriceExoticOptionRequest, PriceExoticOptionResponse>(
        ProductType::ExoticOption, json, &QuantraServer::Stub::PriceExoticOption
    );
}

// Native API:
grpc::Status QuantraClient::PriceExoticOption(
    const Message<PriceExoticOptionRequest>& request,
    Message<PriceExoticOptionResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceExoticOption(&context, request, response);
}
```

### Step 4: Don't forget!

- Create the `.fbs` schema files
- Add the RPC to `quantraserver.fbs`
- Regenerate FlatBuffers code
- Implement the server-side handler

## Configuration

Edit `Config` in `quantra_client.h` to change paths:

```cpp
struct Config {
    static constexpr const char* FBS_INCLUDE_DIR = "/workspace/flatbuffers/fbs";
    static constexpr const char* FBS_DIR = "/workspace/flatbuffers/fbs";
};
```

## FlatBuffers Version

This library requires FlatBuffers v24+ which uses `GenText()` returning `const char*`.

- `nullptr` = success
- non-null = error message

## Building

The library is built automatically as part of the main project:

```bash
cd /workspace/build
cmake ..
make quantra_client
```

Or standalone:

```bash
cd /workspace/client/cpp
mkdir build && cd build
cmake ..
make
```