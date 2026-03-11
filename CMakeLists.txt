cmake_minimum_required(VERSION 3.15)

project(grpc_system_metrics
    VERSION     1.0.0
    DESCRIPTION "Telemetry Monitoring Microservice using gRPC and Protocol Buffers"
    LANGUAGES   CXX
)

# ─────────────────────────────────────────────────────────────────
# Global compiler settings
# ─────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # Use -std=c++17, not -std=gnu++17

# Default to Release when the user hasn't specified a build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─────────────────────────────────────────────────────────────────
# 1.  Locate dependencies
#
# We support two environments:
#  - Modern installs that provide gRPC CMake config targets
#  - Older distro packages (e.g., Ubuntu 22.04) that only provide
#    pkg-config metadata for grpc++.
# ─────────────────────────────────────────────────────────────────
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG QUIET)

set(GRPC_USE_PKG_CONFIG OFF)

if(gRPC_FOUND)
    message(STATUS "Using gRPC via CMake package config")
    set(GRPC_LINK_LIB gRPC::grpc++)

    if(TARGET gRPC::grpc_cpp_plugin)
        get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
    endif()
else()
    message(STATUS "gRPC CMake config not found, falling back to pkg-config")
    set(GRPC_USE_PKG_CONFIG ON)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)

    set(GRPC_LINK_LIB PkgConfig::GRPCPP)
endif()

# Resolve protoc executable robustly across CMake/package variants
if(TARGET protobuf::protoc)
    get_target_property(PROTOC_EXE protobuf::protoc LOCATION)
endif()
if(NOT PROTOC_EXE)
    set(PROTOC_EXE "${Protobuf_PROTOC_EXECUTABLE}")
endif()
if(NOT PROTOC_EXE)
    find_program(PROTOC_EXE protoc REQUIRED)
endif()

# Resolve grpc_cpp_plugin even when gRPC CMake package is unavailable
if(NOT GRPC_CPP_PLUGIN)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

# Resolve protobuf link target for old/new FindProtobuf variants
if(TARGET protobuf::libprotobuf)
    set(PROTOBUF_LINK_LIB protobuf::libprotobuf)
else()
    set(PROTOBUF_LINK_LIB ${Protobuf_LIBRARIES})
endif()

message(STATUS "Protobuf version : ${Protobuf_VERSION}")
message(STATUS "protoc            : ${PROTOC_EXE}")
message(STATUS "grpc_cpp_plugin   : ${GRPC_CPP_PLUGIN}")

# ─────────────────────────────────────────────────────────────────
# 3.  Proto → C++ code generation
#
# add_custom_command() re-runs protoc whenever metrics.proto changes.
# Generated files land in <build-dir>/generated/ to keep the source
# tree clean.
# ─────────────────────────────────────────────────────────────────
set(PROTO_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILE    "${PROTO_SRC_DIR}/metrics.proto")

set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

# Paths of the files that protoc will produce
set(PB_SRCS   "${PROTO_GEN_DIR}/metrics.pb.cc")
set(PB_HDRS   "${PROTO_GEN_DIR}/metrics.pb.h")
set(GRPC_SRCS "${PROTO_GEN_DIR}/metrics.grpc.pb.cc")
set(GRPC_HDRS "${PROTO_GEN_DIR}/metrics.grpc.pb.h")

add_custom_command(
    OUTPUT  ${PB_SRCS} ${PB_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC_EXE}
            # Tell protoc where to find imported .proto files
            --proto_path=${PROTO_SRC_DIR}
            # Standard C++ output (message serialisation / deserialisation)
            --cpp_out=${PROTO_GEN_DIR}
            # gRPC service stub output (uses the grpc_cpp_plugin)
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${PROTO_FILE}
    DEPENDS  ${PROTO_FILE}
    COMMENT  "Generating C++ bindings from ${PROTO_FILE}"
    VERBATIM
)

# ─────────────────────────────────────────────────────────────────
# 4.  Shared static library for the generated proto/gRPC code
#
# Both the server and client link against this instead of
# compiling the generated sources twice.
# ─────────────────────────────────────────────────────────────────
add_library(metrics_proto STATIC
    ${PB_SRCS}
    ${GRPC_SRCS}
)

# Consumers only need to add PROTO_GEN_DIR to their include search
# path; PUBLIC propagates that requirement transitively.
target_include_directories(metrics_proto
    PUBLIC "${PROTO_GEN_DIR}"
)

if(GRPC_USE_PKG_CONFIG)
    target_include_directories(metrics_proto PUBLIC ${GRPCPP_INCLUDE_DIRS})
endif()

if(NOT TARGET protobuf::libprotobuf)
    target_include_directories(metrics_proto PUBLIC ${Protobuf_INCLUDE_DIRS})
endif()

target_link_libraries(metrics_proto
    PUBLIC
        ${PROTOBUF_LINK_LIB}    # Protobuf runtime
        ${GRPC_LINK_LIB}        # gRPC C++ runtime
)

# ─────────────────────────────────────────────────────────────────
# 5.  Server executable
# ─────────────────────────────────────────────────────────────────
add_executable(telemetry_server
    server/telemetry_server.cpp
)

target_link_libraries(telemetry_server
    PRIVATE
        metrics_proto           # Generated stubs + transitive gRPC/protobuf libs
)

# ─────────────────────────────────────────────────────────────────
# 6.  Client executable
# ─────────────────────────────────────────────────────────────────
add_executable(telemetry_client
    client/telemetry_client.cpp
)

target_link_libraries(telemetry_client
    PRIVATE
        metrics_proto
)

# ─────────────────────────────────────────────────────────────────
# 7.  Install targets  (optional: run `cmake --install .`)
# ─────────────────────────────────────────────────────────────────
install(TARGETS telemetry_server telemetry_client
    RUNTIME DESTINATION bin
)
