# =============================================================================
# Quantra C++ Client Library
# =============================================================================

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include
)

# -----------------------------------------------------------------------------
# Library: quantra_client (STATIC)
# -----------------------------------------------------------------------------
add_library(quantra_client STATIC
    cpp/src/json_parser.cpp
    cpp/src/quantra_client.cpp
)

target_include_directories(quantra_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include
        ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
        ${CMAKE_SOURCE_DIR}/grpc
)

target_link_libraries(quantra_client
    PUBLIC
        quantra_grpc
        flatbuffers
        ${GRPC_ALL_LIBS}
)

# C++17 required
target_compile_features(quantra_client PUBLIC cxx_std_17)