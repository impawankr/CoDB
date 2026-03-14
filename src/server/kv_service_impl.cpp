#include "kv_service_impl.h"

KVServiceImpl::KVServiceImpl(std::shared_ptr<IKVStore> store)
    : store_(std::move(store)) {};

grpc::Status KVServiceImpl::Put(grpc::ServerContext *,
                                const codb::PutRequest *req,
                                codb::PutResponse *resp)
{
    bool ok = store_->put(req->key(), req->value());
    resp->set_success(ok);
    return grpc::Status::OK;
}

grpc::Status KVServiceImpl::Get(grpc::ServerContext *,
                                const codb::GetRequest *req,
                                codb::GetResponse *resp)
{
    auto val = store_->get(req->key());
    if (val.has_value())
    {
        resp->set_found(true);
        resp->set_value(*val);
    }
    else
    {
        resp->set_found(false);
    }
    return grpc::Status::OK;
}

grpc::Status KVServiceImpl::Delete(grpc::ServerContext *,
                                   const codb::DeleteRequest *req,
                                   codb::DeleteResponse *resp)
{
    bool ok = store_->remove(req->key());
    resp->set_success(ok);
    return grpc::Status::OK;
}
