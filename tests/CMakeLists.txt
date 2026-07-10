# Tests CMakeLists.txt

# =============================================================================
# Google Test Setup
# =============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    message(STATUS "GTest fetched from GitHub")
else()
    message(STATUS "Using system GTest")
endif()

# =============================================================================
# Include directories
# =============================================================================
include_directories(
   ${CMAKE_SOURCE_DIR}/grpc
   ${CMAKE_SOURCE_DIR}/src/transport
   ${CMAKE_SOURCE_DIR}/src/mappers
   ${CMAKE_SOURCE_DIR}/src/evaluators
   ${CMAKE_SOURCE_DIR}/src/parsers
   ${CMAKE_SOURCE_DIR}/src/market
   ${CMAKE_SOURCE_DIR}/src/domain
   ${CMAKE_SOURCE_DIR}/src/common
   ${CMAKE_SOURCE_DIR}/flatbuffers/fbs
   ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
   ${CMAKE_SOURCE_DIR}/client/cpp
   ${CMAKE_SOURCE_DIR}/client/cpp/include
   ${CMAKE_SOURCE_DIR}/examples
)

# =============================================================================
# Collect sources (needed for unit tests)
# =============================================================================
file(GLOB SRC_SOURCES ${CMAKE_SOURCE_DIR}/src/*/*.cpp)

# =============================================================================
# Test: Quantra vs QuantLib Comparison (Unit Tests)
# =============================================================================
# The parity suite was split per-product (refactor 6a). All cases share
# tests/parity/parity_fixture.h and still build into ONE gtest binary so
# run_all_tests Suite 1 continues to report the full case count.
file(GLOB PARITY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/parity/*.cpp)
add_executable(test_quantra_vs_quantlib
    ${PARITY_SOURCES}
    ${SRC_SOURCES}
)
target_include_directories(test_quantra_vs_quantlib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parity)
target_link_libraries(test_quantra_vs_quantlib 
    ${GRPC_ALL_LIBS} 
    flatbuffers 
    quantra_grpc 
    QuantLib
    GTest::gtest
    GTest::gtest_main
)

# =============================================================================
# Test: Server-Client Integration Tests
# =============================================================================
# Generate the standard grpc.health.v1 client stubs from the vendored proto so
# the integration suite can probe the engine's default health service with a
# real gRPC client call (no raw framing, no extra pip/runtime dependency). The
# pinned gRPC ships protoc + grpc_cpp_plugin under DEPS_PREFIX/bin but not the
# generated health stubs.
find_program(PROTOC_EXECUTABLE protoc PATHS ${DEPS_PREFIX}/bin NO_DEFAULT_PATH REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin PATHS ${DEPS_PREFIX}/bin NO_DEFAULT_PATH REQUIRED)

set(HEALTH_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/integration/health.proto)
set(HEALTH_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/health_gen)
file(MAKE_DIRECTORY ${HEALTH_GEN_DIR})
add_custom_command(
    OUTPUT ${HEALTH_GEN_DIR}/health.pb.cc ${HEALTH_GEN_DIR}/health.pb.h
           ${HEALTH_GEN_DIR}/health.grpc.pb.cc ${HEALTH_GEN_DIR}/health.grpc.pb.h
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/integration
            --cpp_out=${HEALTH_GEN_DIR}
            --grpc_out=${HEALTH_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${HEALTH_PROTO}
    DEPENDS ${HEALTH_PROTO}
    COMMENT "Generating grpc.health.v1 client stubs for the health probe test"
    VERBATIM
)

add_executable(test_server_client
    integration/test_server_client.cpp
    # error.cpp defines the QuantraError base ctor used by the RequestBudget unit
    # tests (RequestBudget::check throws QuantraDeadlineExceeded). It is not part
    # of quantra_grpc/quantra_client, so link it in directly here.
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${HEALTH_GEN_DIR}/health.pb.cc
    ${HEALTH_GEN_DIR}/health.grpc.pb.cc
)
target_include_directories(test_server_client PRIVATE ${HEALTH_GEN_DIR})
# The test reads the top-level VERSION file to prove the Meta RPC's api_version
# is single-sourced from it (not just echoing the same compile define).
target_compile_definitions(test_server_client PRIVATE
    QUANTRA_WORKSPACE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(test_server_client
    ${GRPC_ALL_LIBS}
    flatbuffers
    quantra_grpc
    quantra_client
    QuantLib
    protobuf
    GTest::gtest
    GTest::gtest_main
)

# =============================================================================
# Benchmark (disabled - needs migration)
# =============================================================================
# add_executable(benchmark benchmark.cpp)
# target_link_libraries(benchmark ...)