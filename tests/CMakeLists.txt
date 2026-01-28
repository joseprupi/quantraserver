# Tests CMakeLists.txt
# Quantra Test Suite

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/parser
    ${CMAKE_SOURCE_DIR}/request
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/tests
)

# Quantra sources (for unit tests)
file(GLOB PARSER_SOURCES ${CMAKE_SOURCE_DIR}/parser/*.cpp)
file(GLOB COMMON_SOURCES ${CMAKE_SOURCE_DIR}/common/*.cpp)
file(GLOB REQUEST_SOURCES ${CMAKE_SOURCE_DIR}/request/*.cpp)

# ================================
# Unit Tests: Quantra vs QuantLib
# ================================
add_executable(test_quantra_vs_quantlib
    test_quantra_vs_quantlib.cpp
    ${PARSER_SOURCES}
    ${COMMON_SOURCES}
    ${REQUEST_SOURCES}
)
target_link_libraries(test_quantra_vs_quantlib
    GTest::gtest_main
    quantra_grpc
    QuantLib
    ${GRPC_ALL_LIBS}
)
add_test(NAME QuantraVsQuantLib COMMAND test_quantra_vs_quantlib)

# ================================
# Integration Tests: Server-Client
# ================================
add_executable(test_server_client
    test_server_client.cpp
)
target_link_libraries(test_server_client
    GTest::gtest_main
    quantra_grpc
    QuantLib
    ${GRPC_ALL_LIBS}
)
add_test(NAME ServerClient COMMAND test_server_client)