// client/main.cpp — Phase 1
// Full implementation available after proto generation (cmake build step).
//
// CLI client for CoDB:
//
//   Usage:
//     ./codb_client <host:port> put <key> <value>
//     ./codb_client <host:port> get <key>
//     ./codb_client <host:port> delete <key>
//
//   Examples:
//     ./codb_client localhost:50051 put name elon
//     ./codb_client localhost:50051 get name
//     > elon
//     ./codb_client localhost:50051 delete name
//     ./codb_client localhost:50051 get name
//     > (not found)
//
// Implementation sketch:
//
//   auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
//   auto stub = codb::KVStore::NewStub(channel);
//
//   if (cmd == "put") {
//       codb::PutRequest req;
//       req.set_key(key); req.set_value(value);
//       codb::PutResponse resp;
//       grpc::ClientContext ctx;
//       auto status = stub->Put(&ctx, req, &resp);
//       ...
//   }
