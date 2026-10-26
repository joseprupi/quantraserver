# Server CMakeLists.txt
include_directories(
    ${CMAKE_SOURCE_DIR}/flatbuffers/grpc
    ${CMAKE_SOURCE_DIR}/src/transport
    ${CMAKE_SOURCE_DIR}/src/mappers/market
    ${CMAKE_SOURCE_DIR}/src/mappers/products
    ${CMAKE_SOURCE_DIR}/src/evaluators
    ${CMAKE_SOURCE_DIR}/src/market
    ${CMAKE_SOURCE_DIR}/src/mc
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/common
)

file(GLOB_RECURSE SYNC_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB SERVER_SOURCES ./*.cpp)
list(APPEND SYNC_SOURCES ${SERVER_SOURCES})

add_executable(sync_server ${SYNC_SOURCES})

# Same single-sourced build/version metadata the JSON gateway gets, so the
# engine's Meta RPC reports identical values (see cmake/quantra_build_info.cmake).
target_compile_definitions(sync_server PRIVATE ${QUANTRA_BUILD_INFO_DEFINITIONS})

target_link_libraries(sync_server
    quantra_grpc
    QuantLib
    ${GRPC_ALL_LIBS}
)