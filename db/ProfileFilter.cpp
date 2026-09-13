#include "ProfileFilter.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace NekoGui {

    QString ProfileFilter::SubscriptionIdentity(const std::shared_ptr<ProxyEntity> &ent) {
        if (!ent || !ent->bean || ent->type.isEmpty() || ent->bean->version == -114514) return {};
        auto identity = ent->type.toUtf8() + '\0';
        identity += QJsonDocument(ent->bean->ToJson({"name"})).toJson(QJsonDocument::Compact);
        return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    }

    SubscriptionProfileMatches ProfileFilter::MatchSubscription(
        const QList<std::shared_ptr<ProxyEntity>> &existing,
        const QList<std::shared_ptr<ProxyEntity>> &incoming, int groupId) {
        QMap<QString, QList<std::shared_ptr<ProxyEntity>>> oldByIdentity;
        QMap<QString, int> newCounts;
        for (const auto &profile: existing) {
            if (!profile || profile->gid != groupId) continue;
            const auto key = SubscriptionIdentity(profile);
            if (!key.isEmpty()) oldByIdentity[key].append(profile);
        }
        for (const auto &profile: incoming) {
            const auto key = SubscriptionIdentity(profile);
            if (!key.isEmpty()) ++newCounts[key];
        }

        SubscriptionProfileMatches matches;
        for (const auto &profile: incoming) {
            const auto key = SubscriptionIdentity(profile);
            const auto candidates = oldByIdentity.value(key);
            if (!key.isEmpty() && (newCounts.value(key) > 1 || candidates.size() > 1)) {
                ++matches.ambiguous_count;
                matches.existing_for_incoming.append(nullptr);
            } else if (!key.isEmpty() && newCounts.value(key) == 1 && candidates.size() == 1) {
                matches.existing_for_incoming.append(candidates.first());
            } else {
                matches.existing_for_incoming.append(nullptr);
            }
        }
        return matches;
    }

    QString ProfileFilter_ent_key(const std::shared_ptr<NekoGui::ProxyEntity> &ent, bool by_address) {
        by_address &= ent->type != "custom";
        return by_address ? (ent->bean->DisplayAddress() + ent->bean->DisplayType())
                          : QJsonObject2QString(ent->bean->ToJson({"c_cfg", "c_out"}), true) + ent->bean->DisplayType();
    }

    void ProfileFilter::Uniq(const QList<std::shared_ptr<ProxyEntity>> &in,
                             QList<std::shared_ptr<ProxyEntity>> &out,
                             bool by_address, bool keep_last) {
        QMap<QString, std::shared_ptr<ProxyEntity>> hashMap;

        for (const auto &ent: in) {
            QString key = ProfileFilter_ent_key(ent, by_address);
            if (hashMap.contains(key)) {
                if (keep_last) {
                    out.removeAll(hashMap[key]);
                    hashMap[key] = ent;
                    out += ent;
                }
            } else {
                hashMap[key] = ent;
                out += ent;
            }
        }
    }

    void ProfileFilter::Common(const QList<std::shared_ptr<ProxyEntity>> &src,
                               const QList<std::shared_ptr<ProxyEntity>> &dst,
                               QList<std::shared_ptr<ProxyEntity>> &outSrc,
                               QList<std::shared_ptr<ProxyEntity>> &outDst,
                               bool by_address) {
        QMap<QString, std::shared_ptr<ProxyEntity>> hashMap;

        for (const auto &ent: src) {
            QString key = ProfileFilter_ent_key(ent, by_address);
            hashMap[key] = ent;
        }
        for (const auto &ent: dst) {
            QString key = ProfileFilter_ent_key(ent, by_address);
            if (hashMap.contains(key)) {
                outDst += ent;
                outSrc += hashMap[key];
            }
        }
    }

    void ProfileFilter::OnlyInSrc(const QList<std::shared_ptr<ProxyEntity>> &src,
                                  const QList<std::shared_ptr<ProxyEntity>> &dst,
                                  QList<std::shared_ptr<ProxyEntity>> &out,
                                  bool by_address) {
        QMap<QString, bool> hashMap;

        for (const auto &ent: dst) {
            QString key = ProfileFilter_ent_key(ent, by_address);
            hashMap[key] = true;
        }
        for (const auto &ent: src) {
            QString key = ProfileFilter_ent_key(ent, by_address);
            if (!hashMap.contains(key)) out += ent;
        }
    }

    void ProfileFilter::OnlyInSrc_ByPointer(const QList<std::shared_ptr<ProxyEntity>> &src,
                                            const QList<std::shared_ptr<ProxyEntity>> &dst,
                                            QList<std::shared_ptr<ProxyEntity>> &out) {
        for (const auto &ent: src) {
            if (!dst.contains(ent)) out += ent;
        }
    }

} // namespace NekoGui
