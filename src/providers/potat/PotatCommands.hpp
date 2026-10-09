#pragma once

#include "controllers/completion/CommandUsage.hpp"

#include <pajlada/signals/signal.hpp>
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace chatterino {

struct PotatCommand {
    QString name;
    QString usage;
    bool dynamicUsage = true;
    bool alias = false;
    std::shared_ptr<const completion::CommandUsage> argumentHint;

    bool operator==(const PotatCommand &other) const
    {
        return this->name == other.name && this->usage == other.usage &&
               this->dynamicUsage == other.dynamicUsage &&
               this->alias == other.alias &&
               (this->argumentHint == other.argumentHint ||
                (this->argumentHint && other.argumentHint &&
                 *this->argumentHint == *other.argumentHint));
    }
};

namespace potat::detail {

std::vector<PotatCommand> parseCommands(const QByteArray &payload);

}

class PotatCommands final : public QObject
{
public:
    PotatCommands() = default;

    void ensureLoaded();
    const std::vector<PotatCommand> &commands() const;
    bool isLoading() const;

    pajlada::Signals::NoArgSignal commandsUpdated;

private:
    void loadCache();
    void requestCatalog(bool useEtag = true);
    bool saveCache(const QByteArray &payload) const;
    void saveMetadata() const;

    std::vector<PotatCommand> commands_;
    QString etag_;
    bool cacheChecked_ = false;
    bool requestInFlight_ = false;
    bool fetchedThisSession_ = false;
    bool retryWithoutEtag_ = false;
    int requestAttempts_ = 0;
    QDateTime nextRetryAt_;
};

}
