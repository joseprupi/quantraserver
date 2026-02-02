# JSON Server CMake configuration
# Builds the HTTP/JSON API server that fronts the gRPC pricing engine

include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

find_package(Boost COMPONENTS system REQUIRED)

# -----------------------------------------------------------------------------
# JSON Server - Uses quantra_client library
# -----------------------------------------------------------------------------
add_executable(json_server server.cpp)
target_link_libraries(json_server 
    quantra_client
    quantra_grpc
    flatbuffers 
    grpc++
    grpc
    gpr
    ${Boost_LIBRARIES}
    pthread
)