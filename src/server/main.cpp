// server/main.cpp — Phase 1
// Full implementation available after proto generation (cmake build step).
//
// This will start a gRPC server backed by MemKVStore:
//
//   #include <grpcpp/grpcpp.h>
//   #include "kv_service_impl.h"
//   #include "mem_kv_store.h"
//
//   int main(int argc, char** argv) {
//       std::string port = "50051";
//       if (argc > 1) port = argv[1];
//
//       auto store = std::make_shared<MemKVStore>();
//       KVServiceImpl service(store);
//
//       grpc::ServerBuilder builder;
//       builder.AddListeningPort(
//           "0.0.0.0:" + port,
//           grpc::InsecureServerCredentials()
//       );
//       builder.RegisterService(&service);
//
//       auto server = builder.BuildAndStart();
//       std::cout << "[codb] Server listening on port " << port << "\n";
//       server->Wait();
//       return 0;
//   }
//
// Usage:
//   ./codb_server             # listens on :50051 (default)
//   ./codb_server 50052       # listens on :50052
