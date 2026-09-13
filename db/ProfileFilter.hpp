#pragma once

#include "ProxyEntity.hpp"

namespace NekoGui {
    struct SubscriptionProfileMatches {
        // One entry per incoming profile. Null means a fresh local identity.
        QList<std::shared_ptr<ProxyEntity>> existing_for_incoming;
        int ambiguous_count = 0;
    };

    class ProfileFilter {
    public:
        // Exact protocol identity, including all connection parameters and
        // custom configuration, but excluding the display name/local metadata.
        static QString SubscriptionIdentity(const std::shared_ptr<ProxyEntity> &ent);

        static SubscriptionProfileMatches MatchSubscription(
            const QList<std::shared_ptr<ProxyEntity>> &existing,
            const QList<std::shared_ptr<ProxyEntity>> &incoming,
            int groupId);

        static void Uniq(
            const QList<std::shared_ptr<ProxyEntity>> &in,
            QList<std::shared_ptr<ProxyEntity>> &out,
            bool by_address = false, // def by bean
            bool keep_last = false   // def keep first
        );

        static void Common(
            const QList<std::shared_ptr<ProxyEntity>> &src,
            const QList<std::shared_ptr<ProxyEntity>> &dst,
            QList<std::shared_ptr<ProxyEntity>> &outSrc,
            QList<std::shared_ptr<ProxyEntity>> &outDst,
            bool by_address = false // def by bean
        );

        static void OnlyInSrc(
            const QList<std::shared_ptr<ProxyEntity>> &src,
            const QList<std::shared_ptr<ProxyEntity>> &dst,
            QList<std::shared_ptr<ProxyEntity>> &out,
            bool by_address = false // def by bean
        );

        static void OnlyInSrc_ByPointer(
            const QList<std::shared_ptr<ProxyEntity>> &src,
            const QList<std::shared_ptr<ProxyEntity>> &dst,
            QList<std::shared_ptr<ProxyEntity>> &out);
    };
} // namespace NekoGui
