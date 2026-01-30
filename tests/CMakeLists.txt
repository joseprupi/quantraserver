# Tests CMakeLists.txt
include_directories(
   ${CMAKE_SOURCE_DIR}/grpc
   ${CMAKE_SOURCE_DIR}/parser 
   ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
   ${CMAKE_SOURCE_DIR}/request 
   ${CMAKE_SOURCE_DIR}/common
   ${CMAKE_SOURCE_DIR}/client/cpp
   ${CMAKE_SOURCE_DIR}/examples)

# Only build benchmark for now
add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark ${GRPC_ALL_LIBS} flatbuffers quantra_grpc QuantLib cpp_client)

# Build test.cpp if it exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test.cpp")
    add_executable(test test.cpp)
    target_link_libraries(test ${GRPC_ALL_LIBS} flatbuffers quantra_grpc QuantLib cpp_client)
endif()