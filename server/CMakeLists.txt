# Server CMakeLists.txt
include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/parser
    ${CMAKE_SOURCE_DIR}/request 
    ${CMAKE_SOURCE_DIR}/common
)
 
file(GLOB SYNC_SOURCES  ${CMAKE_SOURCE_DIR}/parser/*.cpp
                        ${CMAKE_SOURCE_DIR}/common/*.cpp
                        ${CMAKE_SOURCE_DIR}/request/*.cpp  
                        ./*.cpp)

add_executable(sync_server ${SYNC_SOURCES})

target_link_libraries(sync_server 
    quantra_grpc 
    QuantLib
    ${GRPC_ALL_LIBS}
)