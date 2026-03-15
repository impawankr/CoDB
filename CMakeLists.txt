cmake_minimum_required(VERSION 3.20)
project(codb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json → VS Code IntelliSense picks up all include paths automatically
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ──────────────────────────────────────────────
#find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ── Proto generation ──────────────────────────────────────────
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_GENERATED_DIR})

get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

add_custom_command(
  OUTPUT
    "${PROTO_GENERATED_DIR}/kvstore.pb.cc"
    "${PROTO_GENERATED_DIR}/kvstore.pb.h"
    "${PROTO_GENERATED_DIR}/kvstore.grpc.pb.cc"
    "${PROTO_GENERATED_DIR}/kvstore.grpc.pb.h"
  COMMAND protobuf::protoc
    --cpp_out=${PROTO_GENERATED_DIR}
    --grpc_out=${PROTO_GENERATED_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    -I${PROTO_DIR}
    ${PROTO_DIR}/kvstore.proto
  DEPENDS ${PROTO_DIR}/kvstore.proto
  COMMENT "Generating protobuf/gRPC sources from kvstore.proto"
)

add_library(codb_proto
  "${PROTO_GENERATED_DIR}/kvstore.pb.cc"
  "${PROTO_GENERATED_DIR}/kvstore.grpc.pb.cc"
)
target_include_directories(codb_proto PUBLIC ${PROTO_GENERATED_DIR})
target_link_libraries(codb_proto PUBLIC
  gRPC::grpc++
  protobuf::libprotobuf
)

# ── Storage library ───────────────────────────────────────────
# Phase 1: mem_kv_store  (in-memory, volatile)
# Phase 2: WAL sources   (log_entry, log_writer, log_reader, log_manager)
#          wal_kv_store  (IKVStore backed by WAL — wired in once implemented)
add_library(codb_storage
  src/storage/mem_kv_store.cpp
  # ── Phase 2: WAL ──────────────────────────────────────────
  src/storage/wal/log_entry.cpp
  src/storage/wal/log_writer.cpp
  src/storage/wal/log_reader.cpp
  src/storage/wal/log_manager.cpp
  src/storage/wal_kv_store.cpp
)
target_include_directories(codb_storage PUBLIC
  src/storage          # kv_store.h, mem_kv_store.h, wal_kv_store.h
  src/storage/wal      # log_entry.h, log_writer.h, log_reader.h, log_manager.h
)
target_link_libraries(codb_storage PUBLIC Threads::Threads)

# ── Server ────────────────────────────────────────────────────
add_executable(codb_server
  src/server/kv_service_impl.cpp
  src/server/main.cpp
)
target_include_directories(codb_server PRIVATE src/server src/storage)
target_link_libraries(codb_server PRIVATE
  codb_proto
  codb_storage
  gRPC::grpc++
)

# ── Client ────────────────────────────────────────────────────
add_executable(codb_client
  src/client/main.cpp
)
target_include_directories(codb_client PRIVATE src/client)
target_link_libraries(codb_client PRIVATE
  codb_proto
  gRPC::grpc++
)

# ── Tests (optional, requires GTest) ──────────────────────────
# find_package(GTest CONFIG REQUIRED)
# enable_testing()
# add_subdirectory(tests)
