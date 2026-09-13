#pragma once

#include <QString>
#include <QList>
#include <QMutex>
#include <atomic>
#include <memory>

#include "TrafficData.hpp"

namespace NekoGui_rpc { class Client; }

namespace NekoGui_traffic {
    class TrafficLooper {
    public:
        std::atomic_bool loop_enabled{false};
        std::atomic_bool looping{false};
        std::atomic<quint64> generation{0};
        QMutex loop_mutex;
#ifndef NKR_NO_GRPC
        std::shared_ptr<NekoGui_rpc::Client> client;
        std::shared_ptr<NekoGui_rpc::Client> direct_client;
#endif

        QList<std::shared_ptr<TrafficData>> items;
        TrafficData *proxy = nullptr;

        void UpdateAll();

        void Loop();

    private:
        TrafficData *bypass = new TrafficData("bypass");

        [[nodiscard]] TrafficData *update_stats(TrafficData *item);

        [[nodiscard]] QJsonArray get_connection_list();
    };

    extern TrafficLooper *trafficLooper;
} // namespace NekoGui_traffic
