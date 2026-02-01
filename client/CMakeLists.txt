# Client CMakeLists.txt
# 
# The Quantra client is now header-only (quantra_client.h)
# This CMakeLists sets up an INTERFACE library for consumers

include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/client/cpp
)

# Create an INTERFACE library (header-only)
add_library(quantra_client INTERFACE)

target_include_directories(quantra_client INTERFACE
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/client/cpp
)

# Link dependencies that consumers will need
target_link_libraries(quantra_client INTERFACE
    ${GRPC_ALL_LIBS}
    flatbuffers
    quantra_grpc
)