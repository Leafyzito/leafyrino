#pragma once

#include <boost/signals2/connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>

#include <vector>

namespace chatterino {

struct SupibotCommand {
    QString name;
    QString usage;
    bool alias = false;
};

class SupibotCommands final : public QObject
{
public:
    SupibotCommands();

    void ensureLoaded();
    const std::vector<SupibotCommand> &commands() const;
    bool isLoading() const;
    bool channelListReady() const;
    bool isActive(const QString &channelLogin) const;

    pajlada::Signals::NoArgSignal commandsUpdated;

private:
    enum class Catalog { Commands, Channels, Aliases };

    struct FetchState {
        QString etag;
        bool cacheChecked = false;
        bool ready = false;
        bool requestInFlight = false;
        bool fetchedThisSession = false;
        bool retryWithoutEtag = false;
        int requestAttempts = 0;
        QDateTime nextRetryAt;
    };

    void watchUser();
    void onUserChanged();
    void loadCommandCache();
    void loadChannelCache();
    void loadAliasCache();
    void maybeRequest(Catalog catalog);
    void request(Catalog catalog, bool useEtag);
    bool applyPayload(Catalog catalog, const QByteArray &payload);
    void rebuild();
    void noteFailure(FetchState &state) const;
    bool saveCache(Catalog catalog, const QByteArray &payload) const;
    void saveMetadata(Catalog catalog) const;
    FetchState &state(Catalog catalog);
    const FetchState &state(Catalog catalog) const;
    QString urlFor(Catalog catalog) const;
    QString cacheFile(Catalog catalog) const;
    QString metadataFile(Catalog catalog) const;

    std::vector<SupibotCommand> baseCommands_;
    std::vector<SupibotCommand> userAliases_;
    std::vector<SupibotCommand> commands_;
    QSet<QString> activeChannels_;
    QString aliasLogin_;
    FetchState commandsState_;
    FetchState channelsState_;
    FetchState aliasesState_;
    bool watchingUser_ = false;
    boost::signals2::scoped_connection userChanged_;
};

}  // namespace chatterino
