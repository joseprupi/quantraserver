cmake_minimum_required(VERSION 3.16)
project(quantra)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_FLAGS "-Wreturn-type -fPIC")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O3")

set(FLATBUFFERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers/)

# Set up dependencies path
if(DEFINED CMAKE_PREFIX_PATH)
    set(DEPS_PREFIX ${CMAKE_PREFIX_PATH})
else()
    set(DEPS_PREFIX "/opt/quantra-deps")
endif()

# Global include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/grpc/
    ${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers/cpp
    ${DEPS_PREFIX}/include
)

# Global link directories
link_directories(${DEPS_PREFIX}/lib)

# =============================================================================
# Define common gRPC libraries for all targets
# Uses --start-group/--end-group to handle circular dependencies
# =============================================================================
set(GRPC_LINK_FLAGS "-Wl,--start-group")
set(GRPC_LINK_FLAGS_END "-Wl,--end-group")

set(GRPC_CORE_LIBS
    grpc++
    grpc
    gpr
    address_sorting
    upb
    re2
    cares
)

set(ABSL_LIBS
    absl_bad_optional_access
    absl_bad_variant_access
    absl_base
    absl_city
    absl_civil_time
    absl_cord
    absl_cord_internal
    absl_cordz_functions
    absl_cordz_handle
    absl_cordz_info
    absl_crc32c
    absl_crc_cord_state
    absl_crc_cpu_detect
    absl_crc_internal
    absl_debugging_internal
    absl_demangle_internal
    absl_examine_stack
    absl_exponential_biased
    absl_graphcycles_internal
    absl_hash
    absl_hashtablez_sampler
    absl_int128
    absl_kernel_timeout_internal
    absl_log_internal_check_op
    absl_log_internal_conditions
    absl_log_internal_format
    absl_log_internal_globals
    absl_log_internal_log_sink_set
    absl_log_internal_message
    absl_log_internal_nullguard
    absl_log_internal_proto
    absl_log_severity
    absl_log_sink
    absl_low_level_hash
    absl_malloc_internal
    absl_raw_hash_set
    absl_raw_logging_internal
    absl_spinlock_wait
    absl_stacktrace
    absl_status
    absl_statusor
    absl_str_format_internal
    absl_strerror
    absl_string_view
    absl_strings
    absl_strings_internal
    absl_symbolize
    absl_synchronization
    absl_throw_delegate
    absl_time
    absl_time_zone
)

set(SYSTEM_LIBS
    ssl
    crypto
    z
    pthread
)

# Combined for use in subdirectories
set(GRPC_ALL_LIBS
    ${GRPC_LINK_FLAGS}
    ${GRPC_CORE_LIBS}
    ${ABSL_LIBS}
    ${GRPC_LINK_FLAGS_END}
    ${SYSTEM_LIBS}
)

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/grpc)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/client)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/examples)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/server)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)

# JSON Server - HTTP/JSON API gateway
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/jsonserver)