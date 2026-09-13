#pragma once

#include "db/Database.hpp"

namespace NekoGui_sub {
    class RawUpdater {
    public:
        void updateClash(const QString &str);

        void update(const QString &str);

        int gid_add_to = -1; // 导入到指定组 -1 为当前选中组

        // Parsing a refresh must never change the live group. The caller
        // commits only after the entire document and identity map are ready.
        bool stage_only = false;
        int rejected_count = 0;
        QStringList rejection_reasons;
        QString parse_error;

        QList<std::shared_ptr<NekoGui::ProxyEntity>> updated_order; // 新增的配置，按照导入时处理的先后排序

    private:
        bool ParseLink(const QString &str);
        bool AddParsedProfile(const std::shared_ptr<NekoGui::ProxyEntity> &profile);
        void Reject(const QString &reason);
    };

    struct SubscriptionUpdateResult {
        bool applied = false;
        QString error;
        int added = 0;
        int removed = 0;
        int preserved = 0;
        int ambiguous = 0;
        int retained_running = 0;
    };

    // Model-only commit also used by regression tests with synthetic profiles.
    // All nodes in parsed must still be detached (id == -1).
    SubscriptionUpdateResult CommitSubscriptionUpdate(const std::shared_ptr<NekoGui::Group> &group,
                                                       const RawUpdater &parsed, const QString &subUserInfo);

    class GroupUpdater : public QObject {
        Q_OBJECT

    public:
        void AsyncUpdate(const QString &str, int _sub_gid = -1, const std::function<void()> &finish = nullptr);

    signals:

        void asyncUpdateCallback(int gid);

    private:
        // Apply parsed imports on the GUI/model thread. A subscription refresh
        // reconciles its group; a plain import only appends to its fixed target.
        void UpdateContent(const QString &content, int subGid, int targetGid, const QString &subUserInfo);
    };

    extern GroupUpdater *groupUpdater;
} // namespace NekoGui_sub

// 更新所有订阅 关闭分组窗口时 更新动作继续执行
void UI_update_all_groups(bool onlyAllowed = false);
