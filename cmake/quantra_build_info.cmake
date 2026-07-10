# =============================================================================
# quantra_build_info.cmake
# -----------------------------------------------------------------------------
# Single-source build/version metadata shared by BOTH server-facing targets:
#   - the gRPC pricing engine (server/sync_server, via the Meta RPC), and
#   - the JSON gateway (jsonserver/json_server, via GET /meta).
#
# Deriving these values once here keeps the two surfaces in agreement, and keeps
# the API version single-sourced from the top-level VERSION file (the same file
# scripts/generate_openapi.py reads for OpenAPI info.version).
#
# Sets, in the including (directory) scope so add_subdirectory children inherit:
#   QUANTRA_GIT_SHA, QUANTRA_BUILD_TIME_UTC, QUANTRA_API_VERSION,
#   QUANTRA_BACKEND_VERSION, QUANTRA_OPENAPI_VERSION, QUANTRA_GRPC_VERSION,
#   QUANTRA_FLATBUFFERS_VERSION, QUANTRA_QUANTLIB_VERSION.
#
# Expects DEPS_PREFIX to be defined for dependency-version discovery.
# =============================================================================

# Short git sha (best effort; "unknown" outside a checkout).
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

# API version: single source of truth is the top-level VERSION file. Rebuild
# when it changes so a version bump is always reflected.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" QUANTRA_API_VERSION)
string(STRIP "${QUANTRA_API_VERSION}" QUANTRA_API_VERSION)
if(QUANTRA_API_VERSION STREQUAL "")
    message(FATAL_ERROR "VERSION file at ${CMAKE_SOURCE_DIR}/VERSION is empty")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")

set(QUANTRA_BACKEND_VERSION "1.0.0")
set(QUANTRA_OPENAPI_VERSION "${QUANTRA_API_VERSION}")
set(QUANTRA_GRPC_VERSION "unknown")
set(QUANTRA_FLATBUFFERS_VERSION "unknown")
set(QUANTRA_QUANTLIB_VERSION "unknown")

# Dependency versions, discovered from the pinned headers under DEPS_PREFIX.
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

# The compile-definition list shared verbatim by the engine and gateway targets.
# (QUANTRA_OPENAPI_VERSION is gateway-only and appended there.)
set(QUANTRA_BUILD_INFO_DEFINITIONS
    QUANTRA_GIT_SHA="${QUANTRA_GIT_SHA}"
    QUANTRA_BUILD_TIME_UTC="${QUANTRA_BUILD_TIME_UTC}"
    QUANTRA_API_VERSION="${QUANTRA_API_VERSION}"
    QUANTRA_BACKEND_VERSION="${QUANTRA_BACKEND_VERSION}"
    QUANTRA_GRPC_VERSION="${QUANTRA_GRPC_VERSION}"
    QUANTRA_FLATBUFFERS_VERSION="${QUANTRA_FLATBUFFERS_VERSION}"
    QUANTRA_QUANTLIB_VERSION="${QUANTRA_QUANTLIB_VERSION}"
)
