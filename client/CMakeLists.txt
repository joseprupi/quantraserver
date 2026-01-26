# Client CMakeLists.txt
include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/parser
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs
    ${CMAKE_SOURCE_DIR}/request
    ${CMAKE_SOURCE_DIR}/common
    cpp)

file(GLOB CPP_CLIENT_SOURCES cpp/*.cpp)

add_library(cpp_client STATIC ${CPP_CLIENT_SOURCES})

target_link_libraries(cpp_client ${GRPC_ALL_LIBS} flatbuffers quantra_grpc QuantLib)