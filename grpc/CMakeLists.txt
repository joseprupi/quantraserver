# gRPC CMakeLists.txt
file(GLOB GRPC_SOURCES *.cc)

add_library(quantra_grpc STATIC ${GRPC_SOURCES})

target_include_directories(quantra_grpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})