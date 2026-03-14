#include "kv_service_impl.h"
#include "mem_kv_store.h"
#include <grpcpp/grpcpp.h>

int main(int argc, char **argv)
{
    std::string port = "50051";
    if (argc > 1)
        port = argv[1];

    // Create storage backend (MemKVStore)
    // we can swap this out later with a WAL-backed store
    // without changing the service layer.
    auto store = std::make_shared<MemKVStore>();
    KVServiceImpl service(store);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(
        "0.0.0.0:" + port,
        grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    std::cout << "[codb] Server listening on port " << port << "\n";
    server->Wait();
    return 0;
}

// 1. We create a MemKVStore instance, which is our in-memory key-value store.
// 2. We create a KVServiceImpl instance, passing the MemKVStore to it
// 3. We set up a gRPC server using grpc::ServerBuilder:
//    - We specify the address and port to listen on (0.0.0.0:50051 by default)
//    - We register our KVServiceImpl instance as the service handler
// 4. We start the server and block indefinitely, waiting for client requests.

// Usage:
//   ./codb_server             # listens on :50051 (default)
//   ./codb_server 50052       # listens on :50052
