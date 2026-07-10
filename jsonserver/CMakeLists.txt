# JSON Server CMake configuration
# Builds the HTTP/JSON API server that fronts the gRPC pricing engine

include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

find_package(Boost COMPONENTS system REQUIRED)

# Build/version metadata (QUANTRA_GIT_SHA, QUANTRA_API_VERSION, dependency
# versions, ...) is discovered once in cmake/quantra_build_info.cmake, included
# from the top-level CMakeLists, and inherited here. QUANTRA_OPENAPI_VERSION is
# gateway-only and mirrors the API version.

# -----------------------------------------------------------------------------
# JSON Server - Uses quantra_client library
# -----------------------------------------------------------------------------
add_executable(json_server server.cpp)
target_compile_definitions(json_server PRIVATE
    ${QUANTRA_BUILD_INFO_DEFINITIONS}
    QUANTRA_OPENAPI_VERSION="${QUANTRA_OPENAPI_VERSION}"
)
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