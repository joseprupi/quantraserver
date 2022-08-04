FROM debian:buster AS builder

RUN     apt-get update && \
        apt-get install -y curl

RUN     curl -1sLf 'https://deb.dl.getenvoy.io/public/setup.deb.sh' | bash && \
        apt-get update && \
        apt-get install getenvoy-envoy

RUN     apt-get install -y git wget apt-transport-https ca-certificates curl gnupg2 software-properties-common python3-pip build-essential autoconf libtool pkg-config libssl-dev libboost-all-dev tar  && \
        pip3 install PyYAML requests && \
        cd /root && \
        mkdir tmp && \
        cd tmp && \
        wget https://github.com/Kitware/CMake/releases/download/v3.24.0-rc5/cmake-3.24.0-rc5-linux-x86_64.sh && \
        chmod +x cmake-3.24.0-rc5-linux-x86_64.sh && \
        bash cmake-3.24.0-rc5-linux-x86_64.sh --skip-license && \
        ln -s  /root/tmp/bin/* /usr/local/bin

# Quantlib
RUN     cd /root && \
        wget https://github.com/lballabio/QuantLib/releases/download/QuantLib-v1.22/QuantLib-1.22.tar.gz && \
        tar -zxvf QuantLib-1.22.tar.gz && \
        cd QuantLib-1.22 && \
        ./configure --enable-std-pointers && \
        make -j && \
        make install && \
        ldconfig

# gRPC
RUN     cd /root && \
        git clone -b v1.39.0 https://github.com/grpc/grpc && \
        cd grpc && \
        git submodule update --init && \
        mkdir -p "third_party/abseil-cpp/cmake/build" && \
        cd ./third_party/abseil-cpp/cmake/build && \
        cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE ../.. && \
        make -j install && \
        cd /root/grpc && \
        mkdir install && \
        mkdir -p cmake/build && \
        cd cmake/build && \
        cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_ABSL_PROVIDER=package \
        -DgRPC_SSL_PROVIDER=package \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX=/root/grpc/install \
        ../.. && \
        make -j && \
        make install

# Flatbuffers
RUN     cd /root && \
        git clone -b v2.0.0 https://github.com/google/flatbuffers && \
        export GRPC_INSTALL_PATH=/root/grpc/install && \
        export PROTOBUF_DOWNLOAD_PATH=/root/grpc/third_party/protobuf && \
        cd flatbuffers && \
        mkdir build && \
        cd build && \
        cmake -DFLATBUFFERS_BUILD_GRPCTEST=ON -DGRPC_INSTALL_PATH=${GRPC_INSTALL_PATH} -DPROTOBUF_DOWNLOAD_PATH=${PROTOBUF_DOWNLOAD_PATH} .. && \
        make -j && \
        make install

# Test flatbuffers
RUN     ln -s ${GRPC_INSTALL_PATH}/lib/libgrpc++_unsecure.so.6 ${GRPC_INSTALL_PATH}/lib/libgrpc++_unsecure.so.1 && \
        export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${GRPC_INSTALL_PATH}/lib && \
        cd /root/flatbuffers/ && \
        /root/flatbuffers/build/flattests && \
        /root/flatbuffers/build/grpctest

# Quantra
RUN     cd /root && \
        git clone https://github.com/joseprupi/quantraserver && \
        cd quantraserver && \
        . ./scripts/config_vars.sh && \
        mkdir build && \
        cd build && \
        cmake ../ && \
        make -j