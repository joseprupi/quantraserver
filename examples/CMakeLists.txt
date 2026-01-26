# Examples CMakeLists.txt
include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/parser 
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
    ${CMAKE_SOURCE_DIR}/request 
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/client/cpp
    data)

file(GLOB ASYNC_SOURCES  
    ${CMAKE_SOURCE_DIR}/parser/*.cpp 
    ${CMAKE_SOURCE_DIR}/common/*.cpp 
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    data/fixed_rate_bond_request_fbs.cpp
    ./async_client.cpp)

file(GLOB JSON_SOURCES  
    ${CMAKE_SOURCE_DIR}/parser/*.cpp 
    ${CMAKE_SOURCE_DIR}/common/*.cpp 
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    data/fixed_rate_bond_request_fbs.cpp
    ./bond_request_to_json.cpp)

file(GLOB BOND_REQUEST_SORUCES
    ${CMAKE_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/common/*.cpp
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    ./bond_request_example.cpp)

find_package(Threads REQUIRED)

add_executable(bond_request ${BOND_REQUEST_SORUCES})
add_executable(async_client ${ASYNC_SOURCES})
add_executable(bond_request_to_json ${JSON_SOURCES})

target_link_libraries(bond_request ${GRPC_ALL_LIBS} flatbuffers quantra_grpc QuantLib Threads::Threads cpp_client)
target_link_libraries(async_client ${GRPC_ALL_LIBS} flatbuffers quantra_grpc QuantLib Threads::Threads)
target_link_libraries(bond_request_to_json ${GRPC_ALL_LIBS} flatbuffers quantra_grpc QuantLib Threads::Threads)