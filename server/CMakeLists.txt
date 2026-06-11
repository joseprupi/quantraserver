# Server CMakeLists.txt
include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/src/transport
    ${CMAKE_SOURCE_DIR}/src/mappers
    ${CMAKE_SOURCE_DIR}/src/evaluators
    ${CMAKE_SOURCE_DIR}/src/parsers
    ${CMAKE_SOURCE_DIR}/src/market
    ${CMAKE_SOURCE_DIR}/src/domain
    ${CMAKE_SOURCE_DIR}/src/common
)

file(GLOB SYNC_SOURCES  ${CMAKE_SOURCE_DIR}/src/*/*.cpp
                        ./*.cpp)

add_executable(sync_server ${SYNC_SOURCES})

target_link_libraries(sync_server
    quantra_grpc
    QuantLib
    redis++
    hiredis
    ${GRPC_ALL_LIBS}
)