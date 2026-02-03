# Quantra

Quantra is a high-performance pricing engine built on QuantLib. It exposes QuantLib's functionality through gRPC and REST APIs, enabling distributed computations with FlatBuffers serialization.

## Motivation

QuantLib is a widely-used C++ library for quantitative finance, but it has limitations that make it difficult to scale:

- Single-threaded by design due to global state (e.g., `Settings::instance().evaluationDate()`)
- Requires C++ knowledge to use directly
- No built-in support for distributed computing

Quantra addresses these by wrapping QuantLib in a server architecture that supports parallel processing across multiple worker processes, with clients available in C++, Python, and any language that can speak gRPC or REST.

## Supported Instruments

- Fixed Rate Bonds
- Floating Rate Bonds
- Vanilla Interest Rate Swaps
- Forward Rate Agreements (FRA)
- Caps and Floors
- Swaptions
- Credit Default Swaps (CDS)

## Architecture

Quantra runs multiple QuantLib instances as separate worker processes behind an Envoy proxy that load-balances requests across them.

```
                    Client Request
                          │
                          ▼
                 ┌─────────────────┐
                 │  Envoy Proxy    │  Port 50051
                 │  (Round Robin)  │
                 └────────┬────────┘
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
   ┌─────────┐       ┌─────────┐       ┌─────────┐
   │ Worker 1│       │ Worker 2│       │ Worker N│
   │  :50055 │       │  :50056 │       │ :50055+N│
   └─────────┘       └─────────┘       └─────────┘
```

Two server types are available:

- **gRPC Server** - High-performance binary protocol using FlatBuffers
- **JSON Server** - REST API for easier integration (see [API Documentation](jsonserver/openapi/docs.html))

## Quick Start

### Using Docker (Recommended)

```bash
# Build and run
docker build -t quantra .
docker run -p 50051:50051 -p 8080:8080 quantra

# Test with curl (JSON API)
curl -X POST http://localhost:8080/price/fixed-rate-bond \
  -H "Content-Type: application/json" \
  -d @examples/data/fixed_rate_bond_request.json
```

### Local Build

See [BUILD_GUIDE.md](BUILD_GUIDE.md) for detailed instructions.

```bash
# Quick start
git clone https://github.com/joseprupi/quantraserver
cd quantraserver
./scripts/build.sh

# Start server cluster with 4 workers
./scripts/quantra start --workers 4

# Run example
./build/examples/bond_request 100
```

## Usage Examples

### C++ Client

```cpp
#include "quantra_client.h"

int main() {
    quantra::QuantraClient client("localhost:50051");
    
    // Load request from JSON file
    std::ifstream file("examples/data/fixed_rate_bond_request.json");
    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    
    auto result = client.PriceFixedRateBondJSON(json);
    if (result.ok) {
        std::cout << result.body << std::endl;
    }
    return 0;
}
```

### Python Client

```python
from quantra_client import Client

client = Client(target="localhost:50051")

# Build request using generated types
request = PriceFixedRateBondRequestT()
# ... configure request ...

response = client.price_fixed_rate_bonds(request)
print(f"NPV: {response.Bonds(0).Npv()}")
```

### REST API

```bash
curl -X POST http://localhost:8080/price/vanilla-swap \
  -H "Content-Type: application/json" \
  -d @examples/data/vanilla_swap_request.json
```

## Data Formats

Quantra uses FlatBuffers for efficient serialization. Requests can be built using:

- **C++ structs** - Native types for maximum performance
- **JSON** - Human-readable, easy to debug
- **Binary FlatBuffers** - Compact wire format

Example of a deposit helper for yield curve construction:

```json
{
  "point_wrapper_type": "DepositHelper",
  "point_wrapper": {
    "rate": 0.0096,
    "tenor_time_unit": "Months",
    "tenor_number": 3,
    "fixing_days": 3,
    "calendar": "TARGET",
    "business_day_convention": "ModifiedFollowing",
    "day_counter": "Actual365Fixed"
  }
}
```

See [examples/data/](examples/data/) for complete request examples for all supported instruments.

## Performance

Benchmarks run on AMD Ryzen 9 3900X (12 cores), pricing fixed-rate bonds with full curve bootstrapping:

| Workers | Bonds | Quantra (ms) | QuantLib (ms) | Speedup |
|---------|-------|--------------|---------------|---------|
| 1       | 1,000 | 941          | 556           | 0.6x    |
| 5       | 5,000 | 1,017        | 2,759         | 2.7x    |
| 10      | 10,000| 1,057        | 5,578         | 5.3x    |
| 10      | 100,000| 10,544      | 56,324        | 5.3x    |

With curve reuse (single bootstrap):

| Workers | Bonds   | Quantra (ms) | QuantLib (ms) |
|---------|---------|--------------|---------------|
| 10      | 10,000  | 213          | 176           |
| 10      | 100,000 | 1,618        | 1,675         |

Quantra excels when curve bootstrapping dominates computation time or when processing large batches in parallel.

## Project Structure

```
quantraserver/
├── client/              # C++ client library
├── examples/            # Usage examples and sample data
├── flatbuffers/         # Schema definitions and generated code
│   ├── fbs/             # FlatBuffers schema files (.fbs)
│   ├── cpp/             # Generated C++ headers
│   ├── python/          # Generated Python modules
│   └── json/            # JSON schemas
├── grpc/                # gRPC service definition
├── jsonserver/          # REST API server (Crow-based)
│   └── openapi/         # API documentation
├── parser/              # FlatBuffers to QuantLib converters
├── quantra-python/      # Python client package
├── request/             # Request handlers
├── scripts/             # Build and management scripts
├── server/              # gRPC server
├── tests/               # Test suite
└── tools/               # Process management utilities
```

## Documentation

- [BUILD_GUIDE.md](BUILD_GUIDE.md) - Build instructions and Docker setup
- [scripts/README.md](scripts/README.md) - Script reference
- [client/README.md](client/README.md) - C++ client library
- [tests/README.md](tests/README.md) - Testing framework
- [tools/quantra-manager/README.md](tools/quantra-manager/README.md) - Process manager CLI
- [API Documentation](jsonserver/openapi/docs.html) - REST API reference (OpenAPI)

## Requirements

- gRPC v1.60.0+
- FlatBuffers v24.12.23+
- QuantLib 1.22+
- CMake 3.16+
- GCC 12+ or Clang 14+
- Envoy proxy (for load balancing)

## License

MIT / Apache 2.0 (same as QuantLib)
