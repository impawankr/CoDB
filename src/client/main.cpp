#include <grpcpp/grpcpp.h>
#include "kvstore.grpc.pb.h"
#include "kvstore.pb.h"
#include <iostream>
int main(int argc, char **argv)
{
    std::string addr = "localhost:50051";
    if (argc > 1)
        addr = argv[1];

    // Create gRPC channel and stub
    auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    auto stub = codb::KVStore::NewStub(channel);

    // Parse command-line arguments
    if (argc < 4)
    {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <host:port> put <key> <value>\n"
                  << "  " << argv[0] << " <host:port> get <key>\n"
                  << "  " << argv[0] << " <host:port> delete <key>\n";
        return 1;
    }

    std::string cmd = argv[2];
    std::string key = argv[3];
    if (cmd == "put")
    {
        if (argc < 5)
        {
            std::cerr << "Error: 'put' command requires a value\n";
            return 1;
        }
        std::string value = argv[4];
        codb::PutRequest req;
        req.set_key(key);
        req.set_value(value);
        codb::PutResponse resp;
        grpc::ClientContext ctx;
        auto status = stub->Put(&ctx, req, &resp);
        if (status.ok())
        {
            std::cout << "Put success: " << resp.success() << "\n";
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << "\n";
        }
    }
    else if (cmd == "get")
    {
        codb::GetRequest req;
        req.set_key(key);
        codb::GetResponse resp;
        grpc::ClientContext ctx;
        auto status = stub->Get(&ctx, req, &resp);
        if (status.ok())
        {
            if (resp.found())
            {
                std::cout << resp.value() << "\n";
            }
            else
            {
                std::cout << "(not found)\n";
            }
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << "\n";
        }
    }
    else if (cmd == "delete")
    {
        codb::DeleteRequest req;
        req.set_key(key);
        codb::DeleteResponse resp;
        grpc::ClientContext ctx;
        auto status = stub->Delete(&ctx, req, &resp);
        if (status.ok())
        {
            std::cout << "Delete success: " << resp.success() << "\n";
        }
        else
        {
            std::cerr << "RPC failed: " << status.error_message() << "\n";
        }
    }
    else
    {
        std::cerr << "Unknown command: " << cmd << "\n";
    }
    return 0;
}
