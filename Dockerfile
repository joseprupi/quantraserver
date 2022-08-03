FROM debian:buster AS builder

RUN     apt-get update && \
        apt-get install -y apt-transport-https ca-certificates curl gnupg2 software-properties-common && \
        curl -sL 'https://getenvoy.io/gpg' | apt-key add - && \
        apt-key fingerprint 6FF974DB | grep "5270 CEAC" && \
        add-apt-repository "deb [arch=amd64] https://dl.bintray.com/tetrate/getenvoy-deb $(lsb_release -cs) stable" && \
        apt-get update && \
        apt-get install -y \
        git python3-pip build-essential autoconf libtool pkg-config cmake libssl-dev libboost-all-dev tar wget getenvoy-envoy && \
        pip3 install PyYAML requests

# gRPC
RUN     cd root && \
        git clone -b v1.37.0 https://github.com/grpc/grpc && \
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
        git clone -b 2.0.0 https://github.com/google/flatbuffers && \
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
        cd /root/flatbuffers/build && \
        make test ARGS=-V

# Quantlib
RUN     cd /root && \
        wget https://github.com/lballabio/QuantLib/releases/download/QuantLib-v1.22/QuantLib-1.22.tar.gz && \
        tar -zxvf QuantLib-1.22.tar.gz && \
        cd QuantLib-1.22 && \
        ./configure --enable-std-pointers && \
        make -j && \
        make install && \
        ldconfig

# Quantra
RUN     cd /root && \
        git clone https://github.com/joseprupi/quantraserver && \
        cd quantragrpc && \
        . ./scripts/config_vars.sh && \
        mkdir build && \
        cd build && \
        cmake ../ && \ 
        make -j
