project(quantra)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "-Wreturn-type -fPIC")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O3")

set(FLATBUFFERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers/)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/grpc/
                    ${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers/cpp)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/client)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/grpc)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/examples)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/server)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/jsonserver)