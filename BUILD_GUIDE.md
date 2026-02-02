# Quantraserver Build Guide

## Quick Start

### Option 1: Docker Build (Recommended)

```bash
# Build production image
docker build -t quantraserver .

# Run server
docker run -p 50051:50051 quantraserver

# Or enter dev shell
docker build --target dev -t quantraserver-dev .
docker run -it -v $(pwd):/workspace quantraserver-dev
```

### Option 2: Dev Container (VS Code)

1. Install "Remote - Containers" extension
2. Open project folder
3. Click "Reopen in Container" when prompted

### Option 3: Local Build

See [Local Development Setup](#local-development-setup) below.

---

## Docker Build Targets

The Dockerfile has multiple stages:

| Target | Purpose | Usage |
|--------|---------|-------|
| `base` | Base OS with tools | Internal |
| `deps` | All dependencies built | `--target deps` |
| `dev` | Development environment | `--target dev` |
| `builder` | Builds the application | Internal |
| `production` | Minimal runtime image | Default |

### Examples

```bash
# Production build (minimal image)
docker build -t quantraserver .

# Dev container (includes debug tools)
docker build --target dev -t quantraserver-dev .

# Run dev container with source mounted
docker run -it \
    -v $(pwd):/workspace \
    -p 50051:50051 \
    quantraserver-dev

# Inside dev container:
regen-flatbuffers.sh  # Regenerate FlatBuffers code
build.sh              # Build the project
build.sh Release      # Build in release mode
run-tests.sh          # Run test suite
```

---

## Local Development Setup

### Prerequisites

Install these versions (or compatible):
- gRPC: v1.60.0
- Flatbuffers: v24.12.23  
- QuantLib: 1.22
- CMake: 3.16+
- GCC: 12+ (or Clang 14+)

### Installing Dependencies Locally

#### Option A: Use the Docker image to extract deps

```bash
# Build deps stage
docker build --target deps -t quantra-deps .

# Copy deps to local machine
docker create --name temp quantra-deps
docker cp temp:/opt/quantra-deps ./quantra-deps
docker rm temp

# Set environment
export CMAKE_PREFIX_PATH=$(pwd)/quantra-deps
export LD_LIBRARY_PATH=$(pwd)/quantra-deps/lib:$LD_LIBRARY_PATH
```

#### Option B: Build deps manually

```bash
# gRPC v1.60.0
git clone -b v1.60.0 --recurse-submodules https://github.com/grpc/grpc
cd grpc && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DgRPC_BUILD_TESTS=OFF ..
make -j$(nproc) && sudo make install

# Flatbuffers v24.12.23
git clone -b v24.12.23 https://github.com/google/flatbuffers
cd flatbuffers && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
make -j$(nproc) && sudo make install

# QuantLib 1.22
wget https://github.com/lballabio/QuantLib/releases/download/QuantLib-v1.22/QuantLib-1.22.tar.gz
tar -xzf QuantLib-1.22.tar.gz && cd QuantLib-1.22
./configure --enable-std-pointers && make -j$(nproc) && sudo make install
sudo ldconfig
```

### Building Locally

```bash
# First time: regenerate FlatBuffers for new versions
mkdir build && cd build
cmake -DREGEN_FLATBUFFERS=ON ..
make -j$(nproc)

# Subsequent builds (no regeneration needed)
cmake ..
make -j$(nproc)

# With custom deps path
cmake -DCMAKE_PREFIX_PATH=/path/to/quantra-deps ..
```

### Running Tests

```bash
# Using test script
./test_build.sh

# Quick smoke test
./test_build.sh --quick

# Verbose output
./test_build.sh --verbose

# Manual testing
./build/server/sync_server 50051 &
./build/examples/bond_request 10
```

---

## Upgrading Dependencies

When upgrading gRPC or FlatBuffers versions:

1. **Update Dockerfile** - Change version in build args
2. **Regenerate code** - The generated `*_generated.h` and `*.grpc.fb.h` files must match library versions
3. **Test thoroughly** - Run full test suite

```bash
# In Docker
docker build -t quantraserver .
docker run -it quantraserver /app/test_build.sh

# Locally  
cmake -DREGEN_FLATBUFFERS=ON ..
make
./test_build.sh
```

### Version Compatibility Notes

| gRPC | Flatbuffers | Status |
|------|-------------|--------|
| v1.37.0 | v1.11.x | Original (doesn't compile with GCC 12+) |
| v1.60.0 | v24.12.23 | Current (tested, working) |

---

## Troubleshooting

### "SIGSTKSZ" error during gRPC build
**Cause:** Old abseil-cpp (in gRPC < v1.50) incompatible with glibc 2.34+
**Solution:** Use gRPC v1.60.0 or newer

### Flatbuffers verification failures
**Cause:** Generated code doesn't match library version
**Solution:** Regenerate with `cmake -DREGEN_FLATBUFFERS=ON ..`

### Missing libraries at runtime
```bash
# Check what's missing
ldd ./build/server/sync_server | grep "not found"

# Fix by setting LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/opt/quantra-deps/lib:$LD_LIBRARY_PATH
```

### CMake can't find packages
```bash
# Specify where deps are installed
cmake -DCMAKE_PREFIX_PATH=/opt/quantra-deps ..
```

---

## CI/CD Integration

```yaml
# GitHub Actions example
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Build
        run: docker build -t quantraserver .
      
      - name: Test
        run: |
          docker run quantraserver /app/test_build.sh --quick
```

---

## File Structure

```
quantraserver/
├── Dockerfile              # Multi-stage build (dev + production)
├── CMakeLists.txt          # Root CMake (finds deps automatically)
├── test_build.sh           # Test suite
├── flatbuffers/
│   ├── fbs/                # Schema files (.fbs)
│   └── cpp/                # Generated C++ (*_generated.h)
├── grpc/
│   ├── quantraserver.fbs   # Service definition
│   ├── quantraserver.grpc.fb.h   # Generated (regenerate on upgrade!)
│   └── quantraserver.grpc.fb.cc
├── server/
├── client/
├── examples/
└── tests/
```
