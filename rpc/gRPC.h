#pragma once

#include <atomic>
#include <functional>
#include <memory>

#ifndef NKR_NO_GRPC

#include "go/grpc_server/gen/libcore.pb.h"
#include <QString>

namespace QtGrpc {
    class Http2GrpcChannelPrivate;
}

namespace NekoGui_rpc {
    class TestClient {
    public:
        ~TestClient();
        libcore::TestResp Test(bool *rpcOK, const libcore::TestReq &request,
                              const std::atomic_bool *cancelled = nullptr);

    private:
        friend class Client;
        TestClient(std::unique_ptr<QtGrpc::Http2GrpcChannelPrivate> channel,
                   std::function<void(const QString &)> onError);
        std::unique_ptr<QtGrpc::Http2GrpcChannelPrivate> channel;
        std::function<void(const QString &)> onError;
    };

    class Client {
    public:
        explicit Client(std::function<void(const QString &)> onError, const QString &target, const QString &token);
        ~Client();

        void Exit();

        bool KeepAlive();

        libcore::CapabilitiesResp Capabilities(bool *rpcOK);

        libcore::CheckResp Check(bool *rpcOK, const libcore::LoadConfigReq &request);

        // QString returns is error string

        QString Start(bool *rpcOK, const libcore::LoadConfigReq &request);

        QString Stop(bool *rpcOK);

        long long QueryStats(const std::string &tag, const std::string &direct);

        std::string ListConnections();

        libcore::TestResp Test(bool *rpcOK, const libcore::TestReq &request);

        // Keep one independent RPC connection per batch worker.
        std::unique_ptr<TestClient> CreateTestClient() const;

        libcore::UpdateResp Update(bool *rpcOK, const libcore::UpdateReq &request);

    private:
        std::function<std::unique_ptr<QtGrpc::Http2GrpcChannelPrivate>()> make_grpc_channel;
        std::unique_ptr<QtGrpc::Http2GrpcChannelPrivate> default_grpc_channel;
        std::function<void(const QString &)> onError;
    };

    inline Client *defaultClient;
} // namespace NekoGui_rpc
#endif
