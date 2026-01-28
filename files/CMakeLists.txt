include_directories(
	/root/grpc/install/include	
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

file(GLOB SWAP_CLIENT_SOURCES
    ${CMAKE_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/common/*.cpp
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    data/vanilla_swap_request_fbs.cpp
    ./swap_client.cpp)

file(GLOB FRA_CLIENT_SOURCES
    ${CMAKE_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/common/*.cpp
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    data/fra_request_fbs.cpp
    ./fra_client.cpp)

file(GLOB CAP_FLOOR_CLIENT_SOURCES
    ${CMAKE_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/common/*.cpp
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    data/cap_floor_request_fbs.cpp
    ./cap_floor_client.cpp)

file(GLOB SWAPTION_CLIENT_SOURCES
    ${CMAKE_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_SOURCE_DIR}/common/*.cpp
    ${CMAKE_SOURCE_DIR}/request/*.cpp  
    ./messages.cpp
    data/swaption_request_fbs.cpp
    ./swaption_client.cpp)

find_package(Threads REQUIRED)
link_directories(/root/grpc/install/lib/ /opt/quantra-deps/lib/)

set(ABSL_LIBS
    absl_synchronization
    absl_base
    absl_raw_logging_internal
    absl_log_severity
    absl_spinlock_wait
    absl_malloc_internal
    absl_time
    absl_time_zone
    absl_int128
    absl_strings
    absl_strings_internal
    absl_throw_delegate
)

add_executable(bond_request ${BOND_REQUEST_SORUCES})
add_executable(async_client ${ASYNC_SOURCES})
add_executable(bond_request_to_json ${JSON_SOURCES})
add_executable(swap_client ${SWAP_CLIENT_SOURCES})
add_executable(fra_client ${FRA_CLIENT_SOURCES})
add_executable(cap_floor_client ${CAP_FLOOR_CLIENT_SOURCES})
add_executable(swaption_client ${SWAPTION_CLIENT_SOURCES})

target_link_libraries(bond_request gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads cpp_client ${ABSL_LIBS})
target_link_libraries(async_client gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads ${ABSL_LIBS})
target_link_libraries(bond_request_to_json gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads ${ABSL_LIBS})
target_link_libraries(swap_client gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads ${ABSL_LIBS})
target_link_libraries(fra_client gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads ${ABSL_LIBS})
target_link_libraries(cap_floor_client gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads ${ABSL_LIBS})
target_link_libraries(swaption_client gpr grpc grpc++ flatbuffers quantra_grpc QuantLib Threads::Threads ${ABSL_LIBS})
