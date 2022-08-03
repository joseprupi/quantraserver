include_directories(
	/root/grpc/install/include	
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/parser 
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
    ${CMAKE_SOURCE_DIR}/request 
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/client/cpp
    data
    ${Boost_INCLUDE_DIR})

file(GLOB JSON_SERVER_SORUCES  
    ./server.cpp
    )

find_package(Boost COMPONENTS system filesystem REQUIRED)
link_directories(${Boost_LIBRARIES})
link_directories(/root/grpc/install/lib/)

add_executable(json_server ${JSON_SERVER_SORUCES})


target_link_libraries(json_server gpr grpc grpc++ flatbuffers quantra_grpc cpp_client)
target_link_libraries(json_server ${Boost_LIBRARIES})
