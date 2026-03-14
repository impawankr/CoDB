#pragma once
#include <memory>
#include <string>
#include "../storage/kv_store.h"
#include "kvstore.grpc.pb.h"

namespace codb
{
    class KVStore;
}

namespace grpc
{
    class ServerContext;
    class Status;
}

// declaration of KVServiceImpl, which implements the gRPC service defined in kvstore.proto
class KVServiceImpl final : public codb::KVStore::Service
{
public:
    explicit KVServiceImpl(std::shared_ptr<IKVStore> store);
    grpc::Status Put(grpc::ServerContext *,
                     const codb::PutRequest *req,
                     codb::PutResponse *resp) override;
    grpc::Status Get(grpc::ServerContext *,
                     const codb::GetRequest *req,
                     codb::GetResponse *resp) override;
    grpc::Status Delete(grpc::ServerContext *,
                        const codb::DeleteRequest *req,
                        codb::DeleteResponse *resp) override;

private:
    std::shared_ptr<IKVStore> store_;
};

/**
 * KVServiceImpl — bridges gRPC RPC calls to the IKVStore storage layer.
 *
 * Intentionally thin: validate input → delegate to storage → map to proto response.
 * No business logic. No storage details. Backend is swapped via IKVStore injection.
 */
