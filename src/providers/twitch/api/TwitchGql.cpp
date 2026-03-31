// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchGql.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrl>
#include <QUuid>

#include <limits>

namespace {

using namespace chatterino;

// Twilight / twitch.tv web Client-ID. Helix-registered OAuth Client-IDs are often
// rejected with HTTP 400 on gql.twitch.tv; Bearer token still identifies the user.
constexpr char TWITCH_WEB_GQL_CLIENT_ID[] = "kimne78kx3ncx6brgo4mv6wki5h1ko";

// Android TV app Client-ID (device-code OAuth); channel-points-miner uses this for GQL.
constexpr char TWITCH_TV_GQL_CLIENT_ID[] = "ue6666qo983tsx6so1t0vnawi233wa";

// Default `CLIENT_VERSION` in miner constants.py (Twilight build id).
constexpr char TWITCH_GQL_CLIENT_VERSION[] =
    "ef928475-9403-42f2-8a34-55784bd08e16";

constexpr char TWITCH_TV_USER_AGENT[] =
    "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/108.0.0.0 Safari/537.36";

QString twitchGqlStaticDeviceId()
{
    static QString id;
    if (!id.isEmpty())
    {
        return id;
    }
    id.resize(32);
    static constexpr char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (qsizetype i = 0; i < 32; ++i)
    {
        id[i] = QLatin1Char(charset[QRandomGenerator::global()->bounded(
            int(sizeof(charset) - 1))]);
    }
    return id;
}

QString twitchGqlStaticSessionId()
{
    static QString sid;
    if (!sid.isEmpty())
    {
        return sid;
    }
    QByteArray bytes(16, '\0');
    for (int i = 0; i < 16; ++i)
    {
        bytes[i] = char(QRandomGenerator::global()->bounded(256));
    }
    sid = QString::fromLatin1(bytes.toHex());
    return sid;
}

/// Only surface resolved predictions from GQL if `endedAt` is within this window.
constexpr int RESOLVED_UI_MAX_AGE_SECS = 30;

QDateTime parseCreatedAtUtcLocal(const QString &s)
{
    QString t = s.trimmed();
    if (t.isEmpty())
    {
        return {};
    }
    QDateTime dt = QDateTime::fromString(t, Qt::ISODate);
    if (dt.isValid())
    {
        return dt.toUTC();
    }
    const int dot = t.indexOf('.');
    const int z = t.lastIndexOf(QLatin1Char('Z'));
    if (dot >= 0 && z > dot)
    {
        const QString frac = t.mid(dot + 1, z - dot - 1);
        if (frac.length() > 3)
        {
            t = t.left(dot + 1) + frac.left(3) + QLatin1Char('Z');
            dt = QDateTime::fromString(t, Qt::ISODateWithMs);
            if (dt.isValid())
            {
                return dt.toUTC();
            }
        }
    }
    return {};
}

QString gqlTimeFieldToIsoString(const QJsonValue &v)
{
    if (v.isString())
    {
        return v.toString();
    }
    if (v.isDouble())
    {
        const auto ms = static_cast<qint64>(v.toDouble());
        return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC)
            .toString(Qt::ISODateWithMs);
    }
    return {};
}

bool resolvedEndedRecentlyForUi(const HelixPrediction &p)
{
    const QDateTime ended = parseCreatedAtUtcLocal(p.endedAt);
    if (!ended.isValid())
    {
        return false;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (ended > now.addSecs(5))
    {
        return false;
    }
    return ended >= now.addSecs(-RESOLVED_UI_MAX_AGE_SECS);
}

// Single read: channel + prediction lists. Field set from community schema docs
// (e.g. kawcco); Twitch may rename or restrict ad-hoc queries — if so, capture a
// persistedQuery hash from twitch.tv DevTools and add a codepath (hashes rot).
const QString QUERY_PREDICTIONS_BY_CHANNEL_ID = QStringLiteral(R"(
query ChannelPredictions($channelID: ID!) {
  channel(id: $channelID) {
    activePredictionEvents {
      id
      title
      status
      createdAt
      endedAt
      predictionWindowSeconds
      winningOutcome {
        id
        title
      }
      outcomes {
        id
        title
        totalPoints
        totalUsers
        color
      }
      self {
        prediction {
          points
          outcome {
            id
          }
        }
      }
    }
    lockedPredictionEvents {
      id
      title
      status
      createdAt
      endedAt
      predictionWindowSeconds
      winningOutcome {
        id
        title
      }
      outcomes {
        id
        title
        totalPoints
        totalUsers
        color
      }
      self {
        prediction {
          points
          outcome {
            id
          }
        }
      }
    }
    resolvedPredictionEvents(first: 5) {
      edges {
        node {
          id
          title
          status
          createdAt
          endedAt
          predictionWindowSeconds
          winningOutcome {
            id
            title
          }
          outcomes {
            id
            title
            totalPoints
            totalUsers
            color
          }
          self {
            prediction {
              points
              outcome {
                id
              }
            }
          }
        }
      }
    }
  }
}
)");

const QString QUERY_PREDICTIONS_BY_LOGIN = QStringLiteral(R"(
query ChannelPredictionsByLogin($login: String!) {
  channel(name: $login) {
    activePredictionEvents {
      id
      title
      status
      createdAt
      endedAt
      predictionWindowSeconds
      winningOutcome {
        id
        title
      }
      outcomes {
        id
        title
        totalPoints
        totalUsers
        color
      }
      self {
        prediction {
          points
          outcome {
            id
          }
        }
      }
    }
    lockedPredictionEvents {
      id
      title
      status
      createdAt
      endedAt
      predictionWindowSeconds
      winningOutcome {
        id
        title
      }
      outcomes {
        id
        title
        totalPoints
        totalUsers
        color
      }
      self {
        prediction {
          points
          outcome {
            id
          }
        }
      }
    }
    resolvedPredictionEvents(first: 5) {
      edges {
        node {
          id
          title
          status
          createdAt
          endedAt
          predictionWindowSeconds
          winningOutcome {
            id
            title
          }
          outcomes {
            id
            title
            totalPoints
            totalUsers
            color
          }
          self {
            prediction {
              points
              outcome {
                id
              }
            }
          }
        }
      }
    }
  }
}
)");

QJsonObject gqlEventToHelixPredictionJson(const QJsonObject &ev)
{
    QJsonArray outcomes;
    for (const auto &v : ev.value(QStringLiteral("outcomes")).toArray())
    {
        const auto o = v.toObject();
        QJsonObject ho;
        ho.insert(QStringLiteral("id"), helixPredictionOutcomeIdFromJson(
                                            o.value(QStringLiteral("id"))));
        ho.insert(QStringLiteral("title"), o.value(QStringLiteral("title")));
        ho.insert(QStringLiteral("users"),
                  o.value(QStringLiteral("totalUsers")));
        ho.insert(QStringLiteral("channel_points"),
                  o.value(QStringLiteral("totalPoints")));
        ho.insert(QStringLiteral("color"),
                  o.value(QStringLiteral("color")).toString());
        outcomes.append(ho);
    }

    QJsonObject p;
    p.insert(QStringLiteral("id"),
             helixPredictionOutcomeIdFromJson(ev.value(QStringLiteral("id"))));
    p.insert(QStringLiteral("title"), ev.value(QStringLiteral("title")));
    p.insert(QStringLiteral("status"), ev.value(QStringLiteral("status")));
    const auto winning = ev.value(QStringLiteral("winningOutcome")).toObject();
    p.insert(
        QStringLiteral("winning_outcome_id"),
        helixPredictionOutcomeIdFromJson(winning.value(QStringLiteral("id"))));
    p.insert(QStringLiteral("winning_outcome_title"),
             winning.value(QStringLiteral("title")).toString());
    p.insert(QStringLiteral("outcomes"), outcomes);

    p.insert(QStringLiteral("created_at"),
             gqlTimeFieldToIsoString(ev.value(QStringLiteral("createdAt"))));
    p.insert(QStringLiteral("ended_at"),
             gqlTimeFieldToIsoString(ev.value(QStringLiteral("endedAt"))));
    p.insert(QStringLiteral("prediction_window"),
             ev.value(QStringLiteral("predictionWindowSeconds")).toInt());

    const auto self = ev.value(QStringLiteral("self")).toObject();
    const auto viewerPred = self.value(QStringLiteral("prediction")).toObject();
    QString viewerOutcomeId;
    int viewerPoints = 0;
    if (!viewerPred.isEmpty())
    {
        viewerPoints = viewerPred.value(QStringLiteral("points")).toInt();
        const auto vo = viewerPred.value(QStringLiteral("outcome")).toObject();
        viewerOutcomeId =
            helixPredictionOutcomeIdFromJson(vo.value(QStringLiteral("id")));
    }
    p.insert(QStringLiteral("viewer_prediction_outcome_id"), viewerOutcomeId);
    p.insert(QStringLiteral("viewer_prediction_points"), viewerPoints);

    return p;
}

bool predictionStatusIsResolvedLike(const QString &status)
{
    return status.compare(QStringLiteral("RESOLVED"), Qt::CaseInsensitive) ==
               0 ||
           status.compare(QStringLiteral("RESOLVE_PENDING"),
                          Qt::CaseInsensitive) == 0;
}

std::optional<HelixPrediction> pickVisibleFromChannelObject(
    const QJsonObject &channel)
{
    std::vector<HelixPrediction> preds;

    for (const auto &v :
         channel.value(QStringLiteral("activePredictionEvents")).toArray())
    {
        preds.emplace_back(gqlEventToHelixPredictionJson(v.toObject()));
    }
    for (const auto &v :
         channel.value(QStringLiteral("lockedPredictionEvents")).toArray())
    {
        preds.emplace_back(gqlEventToHelixPredictionJson(v.toObject()));
    }

    const auto rpe =
        channel.value(QStringLiteral("resolvedPredictionEvents")).toObject();
    for (const auto &edge : rpe.value(QStringLiteral("edges")).toArray())
    {
        const auto node =
            edge.toObject().value(QStringLiteral("node")).toObject();
        if (!node.isEmpty())
        {
            preds.emplace_back(gqlEventToHelixPredictionJson(node));
        }
    }

    for (const auto &p : preds)
    {
        if (p.status.compare(QStringLiteral("ACTIVE"), Qt::CaseInsensitive) ==
            0)
        {
            return p;
        }
    }
    for (const auto &p : preds)
    {
        if (p.status.compare(QStringLiteral("LOCKED"), Qt::CaseInsensitive) ==
            0)
        {
            return p;
        }
    }

    std::optional<HelixPrediction> bestResolved;
    QDateTime bestTime;
    for (const auto &p : preds)
    {
        if (!predictionStatusIsResolvedLike(p.status))
        {
            continue;
        }
        if (!resolvedEndedRecentlyForUi(p))
        {
            continue;
        }
        const QDateTime t = parseCreatedAtUtcLocal(p.endedAt);
        if (!bestResolved.has_value())
        {
            bestResolved = p;
            bestTime = t;
            continue;
        }
        const bool pWin = !p.winningOutcomeID.isEmpty();
        const bool bWin = !bestResolved->winningOutcomeID.isEmpty();
        if (pWin && !bWin)
        {
            bestResolved = p;
            bestTime = t;
        }
        else if (pWin == bWin)
        {
            if (t.isValid() && (!bestTime.isValid() || t > bestTime))
            {
                bestResolved = p;
                bestTime = t;
            }
        }
    }

    return bestResolved;
}

bool channelObjectIsNull(const QJsonObject &root)
{
    const auto data = root.value(QStringLiteral("data")).toObject();
    if (!data.contains(QStringLiteral("channel")))
    {
        return true;
    }
    const auto ch = data.value(QStringLiteral("channel"));
    return ch.isNull() || ch.isUndefined() || !ch.isObject();
}

std::optional<HelixPrediction> tryParsePredictions(const QJsonObject &root)
{
    const auto data = root.value(QStringLiteral("data")).toObject();
    const auto ch = data.value(QStringLiteral("channel")).toObject();
    if (ch.isEmpty())
    {
        return std::nullopt;
    }
    return pickVisibleFromChannelObject(ch);
}

bool errorsSuggestIntegrity(const QJsonArray &errors)
{
    for (const auto &e : errors)
    {
        const auto msg =
            e.toObject().value(QStringLiteral("message")).toString().toLower();
        if (msg.contains(QStringLiteral("integrity")) ||
            msg.contains(QStringLiteral("failed integrity")))
        {
            return true;
        }
    }
    return false;
}

QString summarizeGraphqlErrors(const QJsonArray &errors)
{
    QString out;
    for (const auto &e : errors)
    {
        if (!out.isEmpty())
        {
            out += QStringLiteral("; ");
        }
        out += e.toObject().value(QStringLiteral("message")).toString();
    }
    return out.isEmpty() ? QStringLiteral("GraphQL error") : out;
}

std::optional<TwitchGql::PinnedChatMessage> tryParsePinnedChat(
    const QJsonObject &root)
{
    const auto data = root.value(QStringLiteral("data")).toObject();
    const auto channel = data.value(QStringLiteral("channel")).toObject();
    if (channel.isEmpty())
    {
        return std::nullopt;
    }

    const auto pinnedConn =
        channel.value(QStringLiteral("pinnedChatMessages")).toObject();
    const auto edges = pinnedConn.value(QStringLiteral("edges")).toArray();
    if (edges.isEmpty())
    {
        return std::nullopt;
    }
    const auto node =
        edges.at(0).toObject().value(QStringLiteral("node")).toObject();
    if (node.isEmpty())
    {
        return std::nullopt;
    }
    const auto pinnedMessage =
        node.value(QStringLiteral("pinnedMessage")).toObject();
    if (pinnedMessage.isEmpty())
    {
        return std::nullopt;
    }

    TwitchGql::PinnedChatMessage out;
    out.id = pinnedMessage.value(QStringLiteral("id")).toString();
    out.sentAt = pinnedMessage.value(QStringLiteral("sentAt")).toString();
    out.text = pinnedMessage.value(QStringLiteral("content"))
                   .toObject()
                   .value(QStringLiteral("text"))
                   .toString();
    const auto sender =
        pinnedMessage.value(QStringLiteral("sender")).toObject();
    out.senderDisplayName =
        sender.value(QStringLiteral("displayName")).toString();
    out.senderLogin = sender.value(QStringLiteral("login")).toString();
    out.senderId = sender.value(QStringLiteral("id")).toString();
    out.senderChatColor = sender.value(QStringLiteral("chatColor")).toString();
    if (out.id.isEmpty() || out.senderDisplayName.isEmpty())
    {
        return std::nullopt;
    }
    return out;
}

/// POST https://gql.twitch.tv/integrity — response shape is undocumented; common
/// pattern is JSON `token` (JWT). If parsing fails, returns empty (caller omits header).
void fetchIntegrityToken(const QString &oauthToken, const QObject *caller,
                         std::function<void(QString)> onDone)
{
    auto done =
        std::make_shared<std::function<void(QString)>>(std::move(onDone));
    QJsonObject emptyBody;
    NetworkRequest(QUrl(QStringLiteral("https://gql.twitch.tv/integrity")),
                   NetworkRequestType::Post)
        .timeout(10'000)
        .header("Client-ID", TWITCH_WEB_GQL_CLIENT_ID)
        .header("Authorization", QStringLiteral("Bearer %1").arg(oauthToken))
        .header("Content-Type", "application/json")
        .caller(caller)
        .json(emptyBody)
        .onSuccess([done](const NetworkResult &res) {
            const auto root = res.parseJson();
            QString token = root.value(QStringLiteral("token")).toString();
            if (token.isEmpty())
            {
                token = root.value(QStringLiteral("data")).toString();
            }
            (*done)(token);
        })
        .onError([done](const NetworkResult &) {
            (*done)({});
        })
        .execute();
}

/// Integrity token with TV-style headers (matches channel-points-miner).
void fetchIntegrityTokenTv(const QString &oauthToken, const QObject *caller,
                           std::function<void(QString)> onDone)
{
    auto done =
        std::make_shared<std::function<void(QString)>>(std::move(onDone));
    QJsonObject emptyBody;
    NetworkRequest(QUrl(QStringLiteral("https://gql.twitch.tv/integrity")),
                   NetworkRequestType::Post)
        .timeout(10'000)
        .header("Client-ID", TWITCH_TV_GQL_CLIENT_ID)
        .header("Authorization", QStringLiteral("OAuth %1").arg(oauthToken))
        .header("Client-Session-Id", twitchGqlStaticSessionId())
        .header("Client-Version", TWITCH_GQL_CLIENT_VERSION)
        .header("User-Agent", TWITCH_TV_USER_AGENT)
        .header("X-Device-Id", twitchGqlStaticDeviceId())
        .header("Content-Type", "application/json")
        .caller(caller)
        .json(emptyBody)
        .onSuccess([done](const NetworkResult &res) {
            const auto root = res.parseJson();
            QString token = root.value(QStringLiteral("token")).toString();
            if (token.isEmpty())
            {
                token = root.value(QStringLiteral("data")).toString();
            }
            (*done)(token);
        })
        .onError([done](const NetworkResult &) {
            (*done)({});
        })
        .execute();
}

}  // namespace

namespace chatterino {

namespace TwitchGql {

void fetchPinnedChatMessage(
    const QString &channelId, int count, const QString &oauthToken,
    const QString &gqlClientId, const QObject *caller,
    std::function<void(std::optional<PinnedChatMessage>)> onSuccess,
    std::function<void(QString)> onError)
{
    if (oauthToken.isEmpty())
    {
        onError(QStringLiteral("Missing OAuth token"));
        return;
    }
    const QString cid = channelId.trimmed();
    if (cid.isEmpty())
    {
        onError(QStringLiteral("Missing channel id"));
        return;
    }

    if (count <= 0)
    {
        count = 1;
    }

    const bool useTvGqlClient =
        gqlClientId.compare(QString::fromLatin1(TWITCH_TV_GQL_CLIENT_ID),
                            Qt::CaseInsensitive) == 0;

    auto runGql =
        std::make_shared<std::function<void(const QString &, bool)>>();

    *runGql = [runGql, cid, count, oauthToken, caller, onSuccess, onError,
               useTvGqlClient](const QString &integrityJwt,
                               bool afterIntegrityRetry) {
        QJsonObject variables;
        variables.insert(QStringLiteral("channelID"), cid);
        variables.insert(QStringLiteral("count"), count);

        QJsonObject persisted;
        persisted.insert(QStringLiteral("version"), 1);
        persisted.insert(QStringLiteral("sha256Hash"),
                         QStringLiteral("2d099d4c9b6af80a07d8440140c4f3dbb04d51"
                                        "6b35c401aab7ce8f60765308d5"));

        QJsonObject extensions;
        extensions.insert(QStringLiteral("persistedQuery"), persisted);

        QJsonObject body;
        body.insert(QStringLiteral("operationName"),
                    QStringLiteral("GetPinnedChat"));
        body.insert(QStringLiteral("variables"), variables);
        body.insert(QStringLiteral("extensions"), extensions);

        NetworkRequest req = [&] {
            if (useTvGqlClient)
            {
                return NetworkRequest(
                           QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                           NetworkRequestType::Post)
                    .timeout(10'000)
                    .header("Content-Type", "application/json")
                    .caller(caller)
                    .header("Client-ID", TWITCH_TV_GQL_CLIENT_ID)
                    .header("Authorization",
                            QStringLiteral("OAuth %1").arg(oauthToken))
                    .header("Client-Session-Id", twitchGqlStaticSessionId())
                    .header("Client-Version", TWITCH_GQL_CLIENT_VERSION)
                    .header("User-Agent", TWITCH_TV_USER_AGENT)
                    .header("X-Device-Id", twitchGqlStaticDeviceId());
            }
            return NetworkRequest(
                       QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                       NetworkRequestType::Post)
                .timeout(10'000)
                .header("Content-Type", "application/json")
                .caller(caller)
                .header("Client-ID", TWITCH_WEB_GQL_CLIENT_ID)
                .header("Authorization",
                        QStringLiteral("Bearer %1").arg(oauthToken));
        }();

        if (!integrityJwt.isEmpty())
        {
            req = std::move(req).header("Client-Integrity", integrityJwt);
        }

        std::move(req)
            .json(body)
            .onSuccess([runGql, afterIntegrityRetry, oauthToken, caller,
                        onError, onSuccess,
                        useTvGqlClient](const NetworkResult &res) {
                const auto root = res.parseJson();
                const auto errs =
                    root.value(QStringLiteral("errors")).toArray();

                if (!errs.isEmpty() && errorsSuggestIntegrity(errs) &&
                    !afterIntegrityRetry)
                {
                    if (useTvGqlClient)
                    {
                        fetchIntegrityTokenTv(oauthToken, caller,
                                              [runGql](QString jwt) {
                                                  (*runGql)(jwt, true);
                                              });
                    }
                    else
                    {
                        fetchIntegrityToken(oauthToken, caller,
                                            [runGql](QString jwt) {
                                                (*runGql)(jwt, true);
                                            });
                    }
                    return;
                }

                if (!errs.isEmpty() &&
                    (root.value(QStringLiteral("data")).isNull() ||
                     root.value(QStringLiteral("data")).toObject().isEmpty()))
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL GetPinnedChat errors:"
                        << summarizeGraphqlErrors(errs);
                    onError(summarizeGraphqlErrors(errs));
                    return;
                }

                if (!errs.isEmpty())
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL GetPinnedChat partial errors:"
                        << summarizeGraphqlErrors(errs);
                }

                onSuccess(tryParsePinnedChat(root));
            })
            .onError([onError](const NetworkResult &res) {
                onError(res.formatError());
            })
            .execute();
    };

    if (useTvGqlClient)
    {
        (*runGql)(QString(), false);
    }
    else
    {
        fetchIntegrityToken(oauthToken, caller, [runGql](QString jwt) {
            (*runGql)(jwt, false);
        });
    }
}

void fetchPredictionsForChannel(
    const QString &channelId, const QString &channelLogin,
    const QString &clientId, const QString &oauthToken, const QObject *caller,
    std::function<void(std::optional<HelixPrediction>)> onSuccess,
    std::function<void(QString)> onError)
{
    (void)clientId;
    if (oauthToken.isEmpty())
    {
        onError(QStringLiteral("Missing OAuth token"));
        return;
    }

    auto triedNameFallback = std::make_shared<bool>(false);

    auto runGql =
        std::make_shared<std::function<void(const QString &, bool, bool)>>();

    *runGql = [runGql, triedNameFallback, channelId, channelLogin, oauthToken,
               caller, onSuccess,
               onError](const QString &integrityJwt, bool useChannelId,
                        bool afterIntegrityRetry) {
        const bool byId = useChannelId && !channelId.isEmpty();
        QJsonObject variables;
        QString query;
        if (byId)
        {
            query = QUERY_PREDICTIONS_BY_CHANNEL_ID;
            variables.insert(QStringLiteral("channelID"), channelId);
        }
        else
        {
            query = QUERY_PREDICTIONS_BY_LOGIN;
            variables.insert(QStringLiteral("login"), channelLogin.toLower());
        }

        QJsonObject body;
        body.insert(QStringLiteral("query"), query);
        body.insert(QStringLiteral("variables"), variables);

        auto req =
            NetworkRequest(QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                           NetworkRequestType::Post)
                .timeout(10'000)
                .header("Client-ID", TWITCH_WEB_GQL_CLIENT_ID)
                .header("Authorization",
                        QStringLiteral("Bearer %1").arg(oauthToken))
                .header("Content-Type", "application/json")
                .caller(caller);

        if (!integrityJwt.isEmpty())
        {
            req = std::move(req).header("Client-Integrity", integrityJwt);
        }

        std::move(req)
            .json(body)
            .onSuccess([runGql, triedNameFallback, afterIntegrityRetry,
                        useChannelId, oauthToken, caller, onError, onSuccess,
                        byId, channelLogin,
                        integrityJwt](const NetworkResult &res) {
                const auto root = res.parseJson();
                const auto errs =
                    root.value(QStringLiteral("errors")).toArray();

                if (!errs.isEmpty() && errorsSuggestIntegrity(errs) &&
                    !afterIntegrityRetry)
                {
                    fetchIntegrityToken(oauthToken, caller,
                                        [runGql, useChannelId](QString jwt) {
                                            (*runGql)(jwt, useChannelId, true);
                                        });
                    return;
                }

                if (!errs.isEmpty() &&
                    (root.value(QStringLiteral("data")).isNull() ||
                     root.value(QStringLiteral("data")).toObject().isEmpty()))
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL errors:" << summarizeGraphqlErrors(errs);
                    onError(summarizeGraphqlErrors(errs));
                    return;
                }

                if (!errs.isEmpty())
                {
                    qCDebug(chatterinoTwitch) << "Twitch GQL partial errors:"
                                              << summarizeGraphqlErrors(errs);
                }

                if (byId && channelObjectIsNull(root) &&
                    !channelLogin.isEmpty() && !*triedNameFallback)
                {
                    *triedNameFallback = true;
                    (*runGql)(integrityJwt, false, afterIntegrityRetry);
                    return;
                }

                auto picked = tryParsePredictions(root);
                onSuccess(std::move(picked));
            })
            .onError([onError](const NetworkResult &res) {
                onError(res.formatError());
            })
            .execute();
    };

    // Twitch often requires Client-Integrity; fetch first (empty token if call fails).
    fetchIntegrityToken(oauthToken, caller, [runGql](QString jwt) {
        (*runGql)(jwt, true, false);
    });
}

void fetchChannelPointsBalance(const QString &channelLogin,
                               const QString &oauthToken,
                               const QString &gqlClientId,
                               const QObject *caller,
                               std::function<void(int)> onSuccess,
                               std::function<void(QString)> onError)
{
    if (oauthToken.isEmpty())
    {
        onError(QStringLiteral("Missing OAuth token"));
        return;
    }
    const QString login = channelLogin.trimmed().toLower();
    if (login.isEmpty())
    {
        onError(QStringLiteral("Missing channel login"));
        return;
    }

    const bool useTvGqlClient =
        gqlClientId.compare(QString::fromLatin1(TWITCH_TV_GQL_CLIENT_ID),
                            Qt::CaseInsensitive) == 0;

    auto runGql =
        std::make_shared<std::function<void(const QString &, bool)>>();

    *runGql = [runGql, login, oauthToken, caller, onSuccess, onError,
               useTvGqlClient](const QString &integrityJwt,
                               bool afterIntegrityRetry) {
        QJsonObject variables;
        variables.insert(QStringLiteral("channelLogin"), login);

        QJsonObject persisted;
        persisted.insert(QStringLiteral("version"), 1);
        persisted.insert(QStringLiteral("sha256Hash"),
                         QStringLiteral("1530a003a7d374b0380b79db0be0534f30ff46"
                                        "e61cffa2bc0e2468a909fbc024"));

        QJsonObject extensions;
        extensions.insert(QStringLiteral("persistedQuery"), persisted);

        QJsonObject body;
        body.insert(QStringLiteral("operationName"),
                    QStringLiteral("ChannelPointsContext"));
        body.insert(QStringLiteral("variables"), variables);
        body.insert(QStringLiteral("extensions"), extensions);

        NetworkRequest req = [&] {
            if (useTvGqlClient)
            {
                // Match channel-points-miner `post_gql_request` (TV token + TV Client-Id).
                return NetworkRequest(
                           QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                           NetworkRequestType::Post)
                    .timeout(10'000)
                    .header("Content-Type", "application/json")
                    .caller(caller)
                    .header("Client-ID", TWITCH_TV_GQL_CLIENT_ID)
                    .header("Authorization",
                            QStringLiteral("OAuth %1").arg(oauthToken))
                    .header("Client-Session-Id", twitchGqlStaticSessionId())
                    .header("Client-Version", TWITCH_GQL_CLIENT_VERSION)
                    .header("User-Agent", TWITCH_TV_USER_AGENT)
                    .header("X-Device-Id", twitchGqlStaticDeviceId());
            }
            return NetworkRequest(
                       QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                       NetworkRequestType::Post)
                .timeout(10'000)
                .header("Content-Type", "application/json")
                .caller(caller)
                .header("Client-ID", TWITCH_WEB_GQL_CLIENT_ID)
                .header("Authorization",
                        QStringLiteral("Bearer %1").arg(oauthToken));
        }();

        if (!integrityJwt.isEmpty())
        {
            req = std::move(req).header("Client-Integrity", integrityJwt);
        }

        std::move(req)
            .json(body)
            .onSuccess([runGql, afterIntegrityRetry, oauthToken, caller,
                        onError, onSuccess,
                        useTvGqlClient](const NetworkResult &res) {
                const auto root = res.parseJson();
                const auto errs =
                    root.value(QStringLiteral("errors")).toArray();

                if (!errs.isEmpty() && errorsSuggestIntegrity(errs) &&
                    !afterIntegrityRetry)
                {
                    if (useTvGqlClient)
                    {
                        fetchIntegrityTokenTv(oauthToken, caller,
                                              [runGql](QString jwt) {
                                                  (*runGql)(jwt, true);
                                              });
                    }
                    else
                    {
                        fetchIntegrityToken(oauthToken, caller,
                                            [runGql](QString jwt) {
                                                (*runGql)(jwt, true);
                                            });
                    }
                    return;
                }

                if (!errs.isEmpty() &&
                    (root.value(QStringLiteral("data")).isNull() ||
                     root.value(QStringLiteral("data")).toObject().isEmpty()))
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL ChannelPointsContext errors:"
                        << summarizeGraphqlErrors(errs);
                    onError(summarizeGraphqlErrors(errs));
                    return;
                }

                if (!errs.isEmpty())
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL ChannelPointsContext partial errors:"
                        << summarizeGraphqlErrors(errs);
                }

                const auto data = root.value(QStringLiteral("data")).toObject();
                const QJsonValue communityVal =
                    data.value(QStringLiteral("community"));
                if (communityVal.isNull() || !communityVal.isObject())
                {
                    onError(QStringLiteral("Channel not found"));
                    return;
                }

                const auto community = communityVal.toObject();
                const auto channel =
                    community.value(QStringLiteral("channel")).toObject();
                if (channel.isEmpty())
                {
                    onError(QStringLiteral("Channel not found"));
                    return;
                }

                const auto self =
                    channel.value(QStringLiteral("self")).toObject();
                const auto cp =
                    self.value(QStringLiteral("communityPoints")).toObject();
                const QJsonValue balVal = cp.value(QStringLiteral("balance"));
                qint64 balanceLong = 0;
                if (balVal.isDouble())
                {
                    balanceLong = static_cast<qint64>(balVal.toDouble());
                }
                else if (balVal.isString())
                {
                    bool convOk = false;
                    balanceLong = balVal.toString().toLongLong(&convOk);
                    if (!convOk)
                    {
                        onError(QStringLiteral("Invalid balance"));
                        return;
                    }
                }
                else if (balVal.isUndefined() || balVal.isNull())
                {
                    onError(QStringLiteral("Invalid balance"));
                    return;
                }
                else
                {
                    balanceLong = balVal.toInteger();
                }
                int balance = 0;
                if (balanceLong >
                    static_cast<qint64>(std::numeric_limits<int>::max()))
                {
                    balance = std::numeric_limits<int>::max();
                }
                else if (balanceLong <
                         static_cast<qint64>(std::numeric_limits<int>::min()))
                {
                    balance = std::numeric_limits<int>::min();
                }
                else
                {
                    balance = static_cast<int>(balanceLong);
                }

                onSuccess(balance);
            })
            .onError([onError](const NetworkResult &res) {
                onError(res.formatError());
            })
            .execute();
    };

    if (useTvGqlClient)
    {
        // Miner does not pre-fetch integrity for GQL; try without first.
        (*runGql)(QString(), false);
    }
    else
    {
        fetchIntegrityToken(oauthToken, caller, [runGql](QString jwt) {
            (*runGql)(jwt, false);
        });
    }
}

void makePrediction(const QString &eventId, const QString &outcomeId,
                    int points, const QString &oauthToken,
                    const QString &gqlClientId, const QObject *caller,
                    std::function<void()> onSuccess,
                    std::function<void(QString)> onError)
{
    if (oauthToken.isEmpty())
    {
        onError(QStringLiteral("Missing OAuth token"));
        return;
    }
    if (eventId.isEmpty() || outcomeId.isEmpty())
    {
        onError(QStringLiteral("Missing prediction or outcome id"));
        return;
    }
    if (points < 10)
    {
        onError(
            QStringLiteral("Predictions require at least 10 channel points"));
        return;
    }

    const bool useTvGqlClient =
        gqlClientId.compare(QString::fromLatin1(TWITCH_TV_GQL_CLIENT_ID),
                            Qt::CaseInsensitive) == 0;

    QJsonObject input;
    input.insert(QStringLiteral("eventID"), eventId);
    input.insert(QStringLiteral("outcomeID"), outcomeId);
    input.insert(QStringLiteral("points"), points);
    const QString txId = QUuid::createUuid()
                             .toString(QUuid::WithoutBraces)
                             .remove(QLatin1Char('-'));
    input.insert(QStringLiteral("transactionID"), txId);

    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    QJsonObject persisted;
    persisted.insert(QStringLiteral("version"), 1);
    persisted.insert(
        QStringLiteral("sha256Hash"),
        QStringLiteral("b44682ecc88358817009f20e69d75081b1e58825bb40aa53"
                       "d5dbadcc17c881d8"));

    QJsonObject extensions;
    extensions.insert(QStringLiteral("persistedQuery"), persisted);

    QJsonObject body;
    body.insert(QStringLiteral("operationName"),
                QStringLiteral("MakePrediction"));
    body.insert(QStringLiteral("variables"), variables);
    body.insert(QStringLiteral("extensions"), extensions);

    auto runGql =
        std::make_shared<std::function<void(const QString &, bool)>>();

    *runGql = [runGql, body, oauthToken, caller, onSuccess, onError,
               useTvGqlClient](const QString &integrityJwt,
                               bool afterIntegrityRetry) {
        NetworkRequest req = [&] {
            if (useTvGqlClient)
            {
                return NetworkRequest(
                           QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                           NetworkRequestType::Post)
                    .timeout(10'000)
                    .header("Content-Type", "application/json")
                    .caller(caller)
                    .header("Client-ID", TWITCH_TV_GQL_CLIENT_ID)
                    .header("Authorization",
                            QStringLiteral("OAuth %1").arg(oauthToken))
                    .header("Client-Session-Id", twitchGqlStaticSessionId())
                    .header("Client-Version", TWITCH_GQL_CLIENT_VERSION)
                    .header("User-Agent", TWITCH_TV_USER_AGENT)
                    .header("X-Device-Id", twitchGqlStaticDeviceId());
            }
            return NetworkRequest(
                       QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                       NetworkRequestType::Post)
                .timeout(10'000)
                .header("Content-Type", "application/json")
                .caller(caller)
                .header("Client-ID", TWITCH_WEB_GQL_CLIENT_ID)
                .header("Authorization",
                        QStringLiteral("Bearer %1").arg(oauthToken));
        }();

        if (!integrityJwt.isEmpty())
        {
            req = std::move(req).header("Client-Integrity", integrityJwt);
        }

        std::move(req)
            .json(body)
            .onSuccess([runGql, afterIntegrityRetry, oauthToken, caller,
                        onError, onSuccess,
                        useTvGqlClient](const NetworkResult &res) {
                const auto root = res.parseJson();
                const auto errs =
                    root.value(QStringLiteral("errors")).toArray();

                if (!errs.isEmpty() && errorsSuggestIntegrity(errs) &&
                    !afterIntegrityRetry)
                {
                    if (useTvGqlClient)
                    {
                        fetchIntegrityTokenTv(oauthToken, caller,
                                              [runGql](QString jwt) {
                                                  (*runGql)(jwt, true);
                                              });
                    }
                    else
                    {
                        fetchIntegrityToken(oauthToken, caller,
                                            [runGql](QString jwt) {
                                                (*runGql)(jwt, true);
                                            });
                    }
                    return;
                }

                if (!errs.isEmpty() &&
                    (root.value(QStringLiteral("data")).isNull() ||
                     root.value(QStringLiteral("data")).toObject().isEmpty()))
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL MakePrediction errors:"
                        << summarizeGraphqlErrors(errs);
                    onError(summarizeGraphqlErrors(errs));
                    return;
                }

                if (!errs.isEmpty())
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL MakePrediction partial errors:"
                        << summarizeGraphqlErrors(errs);
                }

                const auto data = root.value(QStringLiteral("data")).toObject();
                const auto mpVal = data.value(QStringLiteral("makePrediction"));
                if (mpVal.isNull() || mpVal.isUndefined())
                {
                    onError(QStringLiteral("Unexpected response"));
                    return;
                }
                const auto mpObj = mpVal.toObject();
                const QJsonValue errVal = mpObj.value(QStringLiteral("error"));
                if (errVal.isObject())
                {
                    const auto errObj = errVal.toObject();
                    if (!errObj.isEmpty())
                    {
                        QString msg =
                            errObj.value(QStringLiteral("message")).toString();
                        const QString code =
                            errObj.value(QStringLiteral("code")).toString();
                        if (msg.isEmpty())
                        {
                            msg = code.isEmpty()
                                      ? QStringLiteral(
                                            "Could not place prediction")
                                      : code;
                        }
                        onError(msg);
                        return;
                    }
                }

                onSuccess();
            })
            .onError([onError](const NetworkResult &result) {
                onError(result.formatError());
            })
            .execute();
    };

    if (useTvGqlClient)
    {
        (*runGql)(QString(), false);
    }
    else
    {
        fetchIntegrityToken(oauthToken, caller, [runGql](QString jwt) {
            (*runGql)(jwt, false);
        });
    }
}

}  // namespace TwitchGql

}  // namespace chatterino
