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
   ${CMAKE_SOURCE_DIR}/parser 
   ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
   ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
   ${CMAKE_SOURCE_DIR}/request 
   ${CMAKE_SOURCE_DIR}/common
   ${CMAKE_SOURCE_DIR}/client/cpp
   ${CMAKE_SOURCE_DIR}/client/cpp/include
   ${CMAKE_SOURCE_DIR}/examples
)

# =============================================================================
# Collect sources (needed for unit tests)
# =============================================================================
file(GLOB REQUEST_SOURCES ${CMAKE_SOURCE_DIR}/request/*.cpp)
file(GLOB PARSER_SOURCES ${CMAKE_SOURCE_DIR}/parser/*.cpp)
file(GLOB COMMON_SOURCES ${CMAKE_SOURCE_DIR}/common/*.cpp)

# =============================================================================
# Test: Quantra vs QuantLib Comparison (Unit Tests)
# =============================================================================
# The parity suite was split per-product (refactor 6a). All cases share
# tests/parity/parity_fixture.h and still build into ONE gtest binary so
# run_all_tests Suite 1 continues to report the full case count.
file(GLOB PARITY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/parity/*.cpp)
add_executable(test_quantra_vs_quantlib
    ${PARITY_SOURCES}
    ${REQUEST_SOURCES}
    ${PARSER_SOURCES}
    ${COMMON_SOURCES}
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
add_executable(test_server_client test_server_client.cpp)
target_link_libraries(test_server_client 
    ${GRPC_ALL_LIBS} 
    flatbuffers 
    quantra_grpc 
    quantra_client
    QuantLib
    GTest::gtest
    GTest::gtest_main
)

# =============================================================================
# Benchmark (disabled - needs migration)
# =============================================================================
# add_executable(benchmark benchmark.cpp)
# target_link_libraries(benchmark ...)