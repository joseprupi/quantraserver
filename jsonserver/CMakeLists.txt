# JSON Server CMake configuration
# Builds the HTTP/JSON API server that fronts the gRPC pricing engine

include_directories(
    ${CMAKE_SOURCE_DIR}/grpc
    ${CMAKE_SOURCE_DIR}/flatbuffers/fbs 
    ${CMAKE_SOURCE_DIR}/flatbuffers/cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

find_package(Boost COMPONENTS system REQUIRED)

# Build metadata for /meta endpoint
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE QUANTRA_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QUANTRA_GIT_SHA)
    set(QUANTRA_GIT_SHA "unknown")
endif()

string(TIMESTAMP QUANTRA_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(QUANTRA_API_VERSION "1.0.0")
set(QUANTRA_BACKEND_VERSION "1.0.0")
set(QUANTRA_OPENAPI_VERSION "1.0.0")
set(QUANTRA_GRPC_VERSION "unknown")
set(QUANTRA_FLATBUFFERS_VERSION "unknown")
set(QUANTRA_QUANTLIB_VERSION "unknown")

# Dependency versions for /meta
if(DEFINED DEPS_PREFIX)
    file(READ "${DEPS_PREFIX}/include/grpcpp/version_info.h" _grpc_ver_h)
    string(REGEX MATCH "#define GRPC_CPP_VERSION_STRING \"([^\"]+)\"" _grpc_match "${_grpc_ver_h}")
    if(CMAKE_MATCH_1)
        set(QUANTRA_GRPC_VERSION "${CMAKE_MATCH_1}")
    endif()

    file(READ "${DEPS_PREFIX}/include/flatbuffers/base.h" _fb_ver_h)
    string(REGEX MATCH "#define FLATBUFFERS_VERSION_MAJOR ([0-9]+)" _fb_major_match "${_fb_ver_h}")
    set(_fb_major "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define FLATBUFFERS_VERSION_MINOR ([0-9]+)" _fb_minor_match "${_fb_ver_h}")
    set(_fb_minor "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define FLATBUFFERS_VERSION_REVISION ([0-9]+)" _fb_patch_match "${_fb_ver_h}")
    set(_fb_patch "${CMAKE_MATCH_1}")
    if(_fb_major AND _fb_minor AND _fb_patch)
        set(QUANTRA_FLATBUFFERS_VERSION "${_fb_major}.${_fb_minor}.${_fb_patch}")
    endif()

    file(READ "${DEPS_PREFIX}/include/ql/version.hpp" _ql_ver_h)
    string(REGEX MATCH "#define QL_VERSION \"([^\"]+)\"" _ql_match "${_ql_ver_h}")
    if(CMAKE_MATCH_1)
        set(QUANTRA_QUANTLIB_VERSION "${CMAKE_MATCH_1}")
    endif()
endif()

# -----------------------------------------------------------------------------
# JSON Server - Uses quantra_client library
# -----------------------------------------------------------------------------
add_executable(json_server server.cpp)
target_compile_definitions(json_server PRIVATE
    QUANTRA_GIT_SHA="${QUANTRA_GIT_SHA}"
    QUANTRA_BUILD_TIME_UTC="${QUANTRA_BUILD_TIME_UTC}"
    QUANTRA_API_VERSION="${QUANTRA_API_VERSION}"
    QUANTRA_BACKEND_VERSION="${QUANTRA_BACKEND_VERSION}"
    QUANTRA_OPENAPI_VERSION="${QUANTRA_OPENAPI_VERSION}"
    QUANTRA_GRPC_VERSION="${QUANTRA_GRPC_VERSION}"
    QUANTRA_FLATBUFFERS_VERSION="${QUANTRA_FLATBUFFERS_VERSION}"
    QUANTRA_QUANTLIB_VERSION="${QUANTRA_QUANTLIB_VERSION}"
)
target_link_libraries(json_server 
    quantra_client
    quantra_grpc
    flatbuffers 
    grpc++
    grpc
    gpr
    ${Boost_LIBRARIES}
    pthread
)