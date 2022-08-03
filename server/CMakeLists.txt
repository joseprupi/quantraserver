#set(CMAKE_BUILD_TYPE Debug)
include_directories(${CMAKE_SOURCE_DIR}/grpc
                    ${CMAKE_SOURCE_DIR}/parser
                    ${CMAKE_SOURCE_DIR}/request 
                    ${CMAKE_SOURCE_DIR}/common
		            /root/grpc/install/include)
 
file(GLOB SYNC_SOURCES  ${CMAKE_SOURCE_DIR}/parser/*.cpp
                        ${CMAKE_SOURCE_DIR}/common/*.cpp
                        ${CMAKE_SOURCE_DIR}/request/*.cpp  
                        ./*.cpp)


link_directories(/root/grpc/install/lib/)
add_executable(sync_server ${SYNC_SOURCES})
target_link_libraries(sync_server gpr grpc grpc++ quantra_grpc QuantLib)
