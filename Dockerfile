# =============================================================================
# Quantraserver - Unified Build Environment
# 
# Versions:
#   - gRPC: v1.60.0 
#   - Flatbuffers: v24.12.23
#   - QuantLib: 1.22
# =============================================================================

FROM debian:bookworm AS base

RUN apt-get update && \
    apt-get install -y \
    git \
    build-essential \
    autoconf \
    libtool \
    pkg-config \
    cmake \
    libssl-dev \
    libboost-all-dev \
    tar \
    wget \
    curl \
    ninja-build \
    ca-certificates \
    gdb \
    valgrind \
    vim \
    bc \
    && rm -rf /var/lib/apt/lists/*

# =============================================================================
# Stage: deps - Build all dependencies
# =============================================================================
FROM base AS deps

ARG GRPC_VERSION=v1.60.0
ARG FLATBUFFERS_VERSION=v24.12.23
ARG QUANTLIB_VERSION=1.22

ENV DEPS_INSTALL_PREFIX=/opt/quantra-deps

# -----------------------------------------------------------------------------
# gRPC
# -----------------------------------------------------------------------------
RUN echo "=== Building gRPC ${GRPC_VERSION} ===" && \
    cd /tmp && \
    git clone -b ${GRPC_VERSION} --depth 1 --recurse-submodules --shallow-submodules https://github.com/grpc/grpc && \
    cd grpc && \
    mkdir -p cmake/build && \
    cd cmake/build && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_PREFIX} \
        -GNinja \
        ../.. && \
    ninja -j$(nproc) && \
    ninja install && \
    cd /tmp && rm -rf grpc && \
    ldconfig

# -----------------------------------------------------------------------------
# Flatbuffers (just flatc with gRPC codegen, no tests)
# -----------------------------------------------------------------------------
RUN echo "=== Building Flatbuffers ${FLATBUFFERS_VERSION} ===" && \
    cd /tmp && \
    git clone -b ${FLATBUFFERS_VERSION} --depth 1 https://github.com/google/flatbuffers && \
    cd flatbuffers && \
    mkdir build && \
    cd build && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DFLATBUFFERS_BUILD_TESTS=OFF \
        -DFLATBUFFERS_BUILD_GRPCTEST=OFF \
        -DCMAKE_INSTALL_PREFIX=${DEPS_INSTALL_PREFIX} \
        -GNinja \
        .. && \
    ninja -j$(nproc) && \
    ninja install && \
    cd /tmp && rm -rf flatbuffers

# Update ldconfig
RUN echo "${DEPS_INSTALL_PREFIX}/lib" > /etc/ld.so.conf.d/quantra-deps.conf && ldconfig

# -----------------------------------------------------------------------------
# QuantLib
# -----------------------------------------------------------------------------
RUN echo "=== Building QuantLib ${QUANTLIB_VERSION} ===" && \
    cd /tmp && \
    wget -q https://github.com/lballabio/QuantLib/releases/download/QuantLib-v${QUANTLIB_VERSION}/QuantLib-${QUANTLIB_VERSION}.tar.gz && \
    tar -zxf QuantLib-${QUANTLIB_VERSION}.tar.gz && \
    cd QuantLib-${QUANTLIB_VERSION} && \
    ./configure --enable-std-pointers --prefix=${DEPS_INSTALL_PREFIX} --quiet && \
    make -j$(nproc) && \
    make install && \
    cd /tmp && rm -rf QuantLib-${QUANTLIB_VERSION} QuantLib-${QUANTLIB_VERSION}.tar.gz && \
    ldconfig

# Verify installations
RUN echo "=== Verifying installations ===" && \
    echo "Flatbuffers:" && ${DEPS_INSTALL_PREFIX}/bin/flatc --version && \
    echo "gRPC libs:" && ls ${DEPS_INSTALL_PREFIX}/lib/libgrpc++.so* | head -2 && \
    echo "QuantLib:" && ls ${DEPS_INSTALL_PREFIX}/lib/libQuantLib.so* | head -2

# =============================================================================
# Stage: dev - Development environment
# =============================================================================
FROM deps AS dev

ENV DEPS_INSTALL_PREFIX=/opt/quantra-deps
ENV PATH="${DEPS_INSTALL_PREFIX}/bin:${PATH}"
ENV LD_LIBRARY_PATH="${DEPS_INSTALL_PREFIX}/lib:${LD_LIBRARY_PATH}"
ENV CMAKE_PREFIX_PATH="${DEPS_INSTALL_PREFIX}"

WORKDIR /workspace

# Create helper scripts
RUN echo '#!/bin/bash' > /usr/local/bin/regen-flatbuffers.sh && \
    echo 'set -e' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'cd /workspace' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'echo "Regenerating Flatbuffers code..."' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'echo "Using flatc version: $(flatc --version)"' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'for fbs in flatbuffers/fbs/*.fbs; do' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo '    echo "Processing $fbs"' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo '    flatc --cpp -o flatbuffers/cpp/ "$fbs"' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'done' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'flatc --grpc --cpp -o grpc/ grpc/quantraserver.fbs' >> /usr/local/bin/regen-flatbuffers.sh && \
    echo 'echo "Done!"' >> /usr/local/bin/regen-flatbuffers.sh && \
    chmod +x /usr/local/bin/regen-flatbuffers.sh

RUN echo '#!/bin/bash' > /usr/local/bin/build.sh && \
    echo 'set -e' >> /usr/local/bin/build.sh && \
    echo 'cd /workspace' >> /usr/local/bin/build.sh && \
    echo 'mkdir -p build' >> /usr/local/bin/build.sh && \
    echo 'cd build' >> /usr/local/bin/build.sh && \
    echo 'cmake -DCMAKE_PREFIX_PATH=${DEPS_INSTALL_PREFIX} -DCMAKE_BUILD_TYPE=${1:-Debug} ..' >> /usr/local/bin/build.sh && \
    echo 'make -j$(nproc)' >> /usr/local/bin/build.sh && \
    echo 'echo "Build complete!"' >> /usr/local/bin/build.sh && \
    chmod +x /usr/local/bin/build.sh

RUN echo '#!/bin/bash' > /usr/local/bin/run-tests.sh && \
    echo 'set -e' >> /usr/local/bin/run-tests.sh && \
    echo 'cd /workspace/build' >> /usr/local/bin/run-tests.sh && \
    echo 'echo "=== Starting server ==="' >> /usr/local/bin/run-tests.sh && \
    echo './server/sync_server 50051 &' >> /usr/local/bin/run-tests.sh && \
    echo 'SERVER_PID=$!' >> /usr/local/bin/run-tests.sh && \
    echo 'sleep 2' >> /usr/local/bin/run-tests.sh && \
    echo 'echo "=== Running bond request test ==="' >> /usr/local/bin/run-tests.sh && \
    echo './examples/bond_request 10' >> /usr/local/bin/run-tests.sh && \
    echo 'kill $SERVER_PID 2>/dev/null || true' >> /usr/local/bin/run-tests.sh && \
    echo 'echo "=== Tests passed! ==="' >> /usr/local/bin/run-tests.sh && \
    chmod +x /usr/local/bin/run-tests.sh

CMD ["/bin/bash"]

# =============================================================================
# Stage: builder - Build the application
# =============================================================================
FROM deps AS builder

ENV DEPS_INSTALL_PREFIX=/opt/quantra-deps
ENV PATH="${DEPS_INSTALL_PREFIX}/bin:${PATH}"

# Create symlinks so original CMakeLists.txt paths work
# Original uses: /root/grpc/install/include and /root/grpc/install/lib
RUN mkdir -p /root/grpc && \
    ln -s ${DEPS_INSTALL_PREFIX} /root/grpc/install && \
    # Also add QuantLib headers to the same include path (for <ql/quantlib.hpp>)
    ln -s ${DEPS_INSTALL_PREFIX}/include/ql /root/grpc/install/include/ql

COPY . /src
WORKDIR /src

# Regenerate Flatbuffers code for new versions
# Note: quantraserver.fbs uses includes like "/fbs/..." so we use -I to set include path
RUN echo "=== Regenerating Flatbuffers code ===" && \
    echo "Using flatc version: $(${DEPS_INSTALL_PREFIX}/bin/flatc --version)" && \
    for fbs in flatbuffers/fbs/*.fbs; do \
        echo "Processing $fbs"; \
        ${DEPS_INSTALL_PREFIX}/bin/flatc --cpp -o flatbuffers/cpp/ "$fbs"; \
    done && \
    ${DEPS_INSTALL_PREFIX}/bin/flatc --grpc --cpp \
        -I flatbuffers \
        -o grpc/ \
        grpc/quantraserver.fbs

# Set environment for pkg-config and library paths
ENV PKG_CONFIG_PATH="${DEPS_INSTALL_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH}"
ENV LD_LIBRARY_PATH="${DEPS_INSTALL_PREFIX}/lib:${LD_LIBRARY_PATH}"

# Debug: List available abseil libraries
RUN echo "=== Available abseil libraries ===" && \
    ls -la ${DEPS_INSTALL_PREFIX}/lib/libabsl*.so* | head -20 && \
    echo "=== pkg-config grpc++ ===" && \
    pkg-config --libs grpc++ || echo "pkg-config for grpc++ failed"

# Build
RUN mkdir -p build && \
    cd build && \
    cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=${DEPS_INSTALL_PREFIX} \
        .. && \
    make -j$(nproc)

# =============================================================================
# Stage: production - Minimal runtime image
# =============================================================================
FROM debian:bookworm-slim AS production

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libssl3 \
    libboost-all-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/quantra-deps/lib /opt/quantra-deps/lib
COPY --from=builder /etc/ld.so.conf.d/quantra-deps.conf /etc/ld.so.conf.d/
RUN ldconfig

COPY --from=builder /src/build/server/sync_server /app/sync_server
COPY --from=builder /src/build/examples /app/examples

WORKDIR /app
ENV LD_LIBRARY_PATH=/opt/quantra-deps/lib

EXPOSE 50051

CMD ["./sync_server", "50051"]