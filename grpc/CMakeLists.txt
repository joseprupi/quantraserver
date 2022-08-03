set(grpc_src
    quantraserver_generated.h
    quantraserver.grpc.fb.cc
    quantraserver.grpc.fb.h
)

include_directories(/root/flatbuffers/include/)
include_directories(/root/grpc/install/include)
add_library(quantra_grpc ${grpc_src})
