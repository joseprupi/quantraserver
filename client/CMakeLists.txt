include_directories(${CMAKE_CURRENT_SOURCE_DIR}/cpp
	/root/grpc/install/include
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/examples/data
    ${CMAKE_SOURCE_DIR}/request
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/parser)

file(GLOB CPP_CLIENT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.h)

find_package(Threads REQUIRED)
link_directories(/root/grpc/install/lib/)

add_library(cpp_client STATIC ${CPP_CLIENT_SOURCES})
target_link_libraries(cpp_client gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads)
