// kv_service_impl.cpp — Phase 1
// Full implementation available after proto generation (cmake build step).
//
// This file will implement:
//
//   class KVServiceImpl final : public codb::KVStore::Service {
//   public:
//       explicit KVServiceImpl(std::shared_ptr<IKVStore> store)
//           : store_(std::move(store)) {}
//
//       grpc::Status Put(grpc::ServerContext*,
//                        const codb::PutRequest* req,
//                        codb::PutResponse* resp) override {
//           bool ok = store_->put(req->key(), req->value());
//           resp->set_success(ok);
//           return grpc::Status::OK;
//       }
//
//       grpc::Status Get(grpc::ServerContext*,
//                        const codb::GetRequest* req,
//                        codb::GetResponse* resp) override {
//           auto val = store_->get(req->key());
//           if (val.has_value()) {
//               resp->set_found(true);
//               resp->set_value(*val);
//           } else {
//               resp->set_found(false);
//           }
//           return grpc::Status::OK;
//       }
//
//       grpc::Status Delete(grpc::ServerContext*,
//                           const codb::DeleteRequest* req,
//                           codb::DeleteResponse* resp) override {
//           bool ok = store_->remove(req->key());
//           resp->set_success(ok);
//           return grpc::Status::OK;
//       }
//
//   private:
//       std::shared_ptr<IKVStore> store_;
//   };
//
// See Plan/02_phase1_kv_store.md for full design rationale.
