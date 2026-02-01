include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/client/cpp
)

find_package(Boost COMPONENTS system REQUIRED)

add_executable(json_server server.cpp)

target_compile_definitions(json_server PRIVATE CROW_USE_BOOST=1)

target_link_libraries(json_server 
    quantra_grpc
    flatbuffers 
    ${GRPC_ALL_LIBS}
    ${Boost_LIBRARIES}
)