// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchGql.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "util/Helpers.hpp"
#include "util/RapidjsonHelpers.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QSet>
#include <QStringBuilder>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

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

// Undocumented viewer poll query (User.viewablePoll). Must stay byte-for-byte stable for
// SHA256 in extensions.persistedQuery; if Twitch removes fields, adjust and re-verify.
const QString QUERY_VIEWABLE_POLL_BY_CHANNEL_ID = QStringLiteral(R"(
query ChannelPollContext_GetViewablePoll($channelID: ID!) {
  channel(id: $channelID) {
    owner {
      viewablePoll {
        id
        title
        status
        remainingDurationMilliseconds
        durationSeconds
        startedAt
        endedAt
        totalVoters
        isViewable
        choices {
          id
          title
          totalVoters
          votes {
            total
            base
            bits
            communityPoints
          }
        }
        self {
          voter {
            id
            choices {
              id
            }
          }
        }
      }
    }
  }
}
)");

const QString QUERY_VIEWABLE_POLL_BY_LOGIN = QStringLiteral(R"(
query ChannelPollContext_GetViewablePoll($login: String!) {
  user(login: $login) {
    viewablePoll {
      id
      title
      status
      remainingDurationMilliseconds
      durationSeconds
      startedAt
      endedAt
      totalVoters
      isViewable
      choices {
        id
        title
        totalVoters
        votes {
          total
          base
          bits
          communityPoints
        }
      }
      self {
        voter {
          id
          choices {
            id
          }
        }
      }
    }
  }
}
)");

QString gqlDocumentSha256Hex(const QString &query)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(query.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

const QString QUERY_GET_PINNED_CHAT = QStringLiteral(R"(
query GetPinnedChat($channelID: ID!, $count: Int!) {
  channel(id: $channelID) {
    pinnedChatMessages(first: $count) {
      edges {
        node {
          pinnedMessage {
            id
            sentAt
            sender {
              displayName
              login
              id
              chatColor
            }
            content {
              text
              fragments {
                text
                content {
                  __typename
                  ... on Emote {
                    id
                    token
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}
)");

QJsonObject gqlPinnedChatRequestBody(const QString &query,
                                     const QJsonObject &variables)
{
    QJsonObject persisted;
    persisted.insert(QStringLiteral("version"), 1);
    persisted.insert(QStringLiteral("sha256Hash"), gqlDocumentSha256Hex(query));

    QJsonObject extensions;
    extensions.insert(QStringLiteral("persistedQuery"), persisted);

    QJsonObject body;
    body.insert(QStringLiteral("operationName"),
                QStringLiteral("GetPinnedChat"));
    body.insert(QStringLiteral("query"), query);
    body.insert(QStringLiteral("variables"), variables);
    body.insert(QStringLiteral("extensions"), extensions);
    return body;
}

std::vector<TwitchEmoteOccurrence> parsePinnedChatEmoteFragments(
    const QJsonArray &fragments)
{
    std::vector<TwitchEmoteOccurrence> twitchEmotes;
    if (fragments.isEmpty())
    {
        return twitchEmotes;
    }

    auto *twitchEmoteCache = getApp()->getEmotes()->getTwitchEmotes();
    int cursor = 0;

    for (const auto fragmentValue : fragments)
    {
        const auto fragment = fragmentValue.toObject();
        const QString fragmentText =
            fragment.value(QStringLiteral("text")).toString();
        if (fragmentText.isEmpty())
        {
            continue;
        }

        const auto content =
            fragment.value(QStringLiteral("content")).toObject();
        if (content.value(QStringLiteral("__typename")).toString() ==
            QStringLiteral("Emote"))
        {
            const QString emoteID =
                content.value(QStringLiteral("id")).toString();
            if (!emoteID.isEmpty())
            {
                const QString token =
                    content.value(QStringLiteral("token")).toString();
                const EmoteName emoteName{token.isEmpty() ? fragmentText
                                                          : token};
                const int fragmentLength =
                    static_cast<int>(fragmentText.length());
                twitchEmotes.push_back(TwitchEmoteOccurrence{
                    cursor,
                    cursor + fragmentLength - 1,
                    twitchEmoteCache->getOrCreateEmote(EmoteId{emoteID},
                                                       emoteName),
                    emoteName,
                });
            }
        }

        cursor += static_cast<int>(fragmentText.length());
    }

    return twitchEmotes;
}

int jsonNumberToInt(const QJsonValue &v)
{
    if (v.isDouble())
    {
        return static_cast<int>(v.toDouble());
    }
    if (v.isString())
    {
        bool ok = false;
        const int n = v.toString().toInt(&ok);
        return ok ? n : 0;
    }
    return v.toInt();
}

std::optional<TwitchGql::ViewablePoll> tryParseViewablePollObject(
    const QJsonObject &poll)
{
    if (poll.isEmpty())
    {
        return std::nullopt;
    }
    if (poll.contains(QStringLiteral("isViewable")) &&
        !poll.value(QStringLiteral("isViewable")).toBool())
    {
        return std::nullopt;
    }

    TwitchGql::ViewablePoll out;
    out.id = poll.value(QStringLiteral("id")).toString();
    out.title = poll.value(QStringLiteral("title")).toString();
    out.status = poll.value(QStringLiteral("status")).toString();
    const bool active = TwitchGql::pollStatusIsActive(out.status);
    const bool terminal = TwitchGql::pollStatusIsTerminal(out.status);
    if (!active && !terminal)
    {
        qCDebug(chatterinoTwitch).nospace()
            << "Twitch GQL viewablePoll unknown status: " << out.status;
        return std::nullopt;
    }
    if (out.id.trimmed().isEmpty())
    {
        return std::nullopt;
    }

    const QJsonValue rem =
        poll.value(QStringLiteral("remainingDurationMilliseconds"));
    if (rem.isDouble())
    {
        out.remainingDurationMs = static_cast<qint64>(rem.toDouble());
    }
    else
    {
        out.remainingDurationMs =
            static_cast<qint64>(rem.toVariant().toLongLong());
    }
    out.durationSeconds =
        jsonNumberToInt(poll.value(QStringLiteral("durationSeconds")));
    out.startedAt = poll.value(QStringLiteral("startedAt")).toString();
    out.endedAt = poll.value(QStringLiteral("endedAt")).toString();
    out.totalVoters =
        jsonNumberToInt(poll.value(QStringLiteral("totalVoters")));

    const auto choicesArr = poll.value(QStringLiteral("choices")).toArray();
    out.choices.reserve(static_cast<size_t>(choicesArr.size()));
    for (const auto &cv : choicesArr)
    {
        const auto c = cv.toObject();
        TwitchGql::ViewablePollChoice ch;
        ch.id = c.value(QStringLiteral("id")).toString();
        ch.title = c.value(QStringLiteral("title")).toString();
        ch.totalVoters =
            jsonNumberToInt(c.value(QStringLiteral("totalVoters")));
        const auto votes = c.value(QStringLiteral("votes")).toObject();
        ch.votesTotal = jsonNumberToInt(votes.value(QStringLiteral("total")));
        if (ch.id.trimmed().isEmpty())
        {
            continue;
        }
        out.choices.push_back(std::move(ch));
    }

    const auto self = poll.value(QStringLiteral("self")).toObject();
    const auto voter = self.value(QStringLiteral("voter")).toObject();
    for (const auto &vc : voter.value(QStringLiteral("choices")).toArray())
    {
        const QString cid =
            vc.toObject().value(QStringLiteral("id")).toString();
        if (!cid.isEmpty())
        {
            out.viewerVotedChoiceIds.push_back(cid);
        }
    }

    if (terminal && !out.choices.empty())
    {
        int bestVotes = -1;
        for (const auto &ch : out.choices)
        {
            const int v = std::max(0, ch.votesTotal);
            if (v > bestVotes)
            {
                bestVotes = v;
                out.winningChoiceId = ch.id;
            }
        }
    }

    return out;
}

std::optional<TwitchGql::ViewablePoll> tryParseViewablePollFromChannelResponse(
    const QJsonObject &root)
{
    const auto data = root.value(QStringLiteral("data")).toObject();
    const QJsonValue pollVal = data.value(QStringLiteral("channel"))
                                   .toObject()
                                   .value(QStringLiteral("owner"))
                                   .toObject()
                                   .value(QStringLiteral("viewablePoll"));
    if (pollVal.isNull() || !pollVal.isObject())
    {
        return std::nullopt;
    }
    return tryParseViewablePollObject(pollVal.toObject());
}

std::optional<TwitchGql::ViewablePoll> tryParseViewablePollFromUserResponse(
    const QJsonObject &root)
{
    const auto data = root.value(QStringLiteral("data")).toObject();
    const QJsonValue pollVal = data.value(QStringLiteral("user"))
                                   .toObject()
                                   .value(QStringLiteral("viewablePoll"));
    if (pollVal.isNull() || !pollVal.isObject())
    {
        return std::nullopt;
    }
    return tryParseViewablePollObject(pollVal.toObject());
}

QJsonObject gqlPollRequestBody(const QString &query,
                               const QJsonObject &variables)
{
    QJsonObject persisted;
    persisted.insert(QStringLiteral("version"), 1);
    persisted.insert(QStringLiteral("sha256Hash"), gqlDocumentSha256Hex(query));

    QJsonObject extensions;
    extensions.insert(QStringLiteral("persistedQuery"), persisted);

    QJsonObject body;
    body.insert(QStringLiteral("operationName"),
                QStringLiteral("ChannelPollContext_GetViewablePoll"));
    body.insert(QStringLiteral("query"), query);
    body.insert(QStringLiteral("variables"), variables);
    body.insert(QStringLiteral("extensions"), extensions);
    return body;
}

const QString MUTATION_VOTE_IN_POLL = QStringLiteral(R"(
mutation VoteInPoll($input: VoteInPollInput!) {
  voteInPoll(input: $input) {
    __typename
  }
}
)");

QJsonObject gqlVoteInPollRequestBody(const QString &query,
                                     const QJsonObject &variables)
{
    QJsonObject persisted;
    persisted.insert(QStringLiteral("version"), 1);
    persisted.insert(QStringLiteral("sha256Hash"), gqlDocumentSha256Hex(query));

    QJsonObject extensions;
    extensions.insert(QStringLiteral("persistedQuery"), persisted);

    QJsonObject body;
    body.insert(QStringLiteral("operationName"), QStringLiteral("VoteInPoll"));
    body.insert(QStringLiteral("query"), query);
    body.insert(QStringLiteral("variables"), variables);
    body.insert(QStringLiteral("extensions"), extensions);
    return body;
}

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
    const auto content =
        pinnedMessage.value(QStringLiteral("content")).toObject();
    out.text = content.value(QStringLiteral("text")).toString();
    out.twitchEmotes = parsePinnedChatEmoteFragments(
        content.value(QStringLiteral("fragments")).toArray());
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

        const QJsonObject body =
            gqlPinnedChatRequestBody(QUERY_GET_PINNED_CHAT, variables);

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

void fetchViewablePollForChannel(
    const QString &channelId, const QString &channelLogin,
    const QString &clientId, const QString &oauthToken, const QObject *caller,
    std::function<void(std::optional<ViewablePoll>)> onSuccess,
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
            query = QUERY_VIEWABLE_POLL_BY_CHANNEL_ID;
            variables.insert(QStringLiteral("channelID"), channelId);
        }
        else
        {
            query = QUERY_VIEWABLE_POLL_BY_LOGIN;
            variables.insert(QStringLiteral("login"), channelLogin.toLower());
        }

        const QJsonObject body = gqlPollRequestBody(query, variables);

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
                        << "Twitch GQL viewable poll errors:"
                        << summarizeGraphqlErrors(errs);
                    onError(summarizeGraphqlErrors(errs));
                    return;
                }

                if (!errs.isEmpty())
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL viewable poll partial errors:"
                        << summarizeGraphqlErrors(errs);
                }

                if (byId && channelObjectIsNull(root) &&
                    !channelLogin.isEmpty() && !*triedNameFallback)
                {
                    *triedNameFallback = true;
                    (*runGql)(integrityJwt, false, afterIntegrityRetry);
                    return;
                }

                std::optional<ViewablePoll> picked =
                    tryParseViewablePollFromChannelResponse(root);
                if (!picked.has_value())
                {
                    picked = tryParseViewablePollFromUserResponse(root);
                }
                onSuccess(std::move(picked));
            })
            .onError([onError](const NetworkResult &res) {
                onError(res.formatError());
            })
            .execute();
    };

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

void voteInPoll(const QString &pollId, const QString &choiceId,
                const QString &userId, const QString &oauthToken,
                const QString &gqlClientId, const QObject *caller,
                std::function<void()> onSuccess,
                std::function<void(QString)> onError)
{
    if (oauthToken.isEmpty())
    {
        onError(QStringLiteral("Missing OAuth token"));
        return;
    }
    if (pollId.isEmpty() || choiceId.isEmpty() || userId.isEmpty())
    {
        onError(QStringLiteral("Missing poll, choice, or user id"));
        return;
    }

    const bool useTvGqlClient =
        gqlClientId.compare(QString::fromLatin1(TWITCH_TV_GQL_CLIENT_ID),
                            Qt::CaseInsensitive) == 0;

    QJsonObject input;
    input.insert(QStringLiteral("pollID"), pollId);
    input.insert(QStringLiteral("choiceID"), choiceId);
    input.insert(QStringLiteral("userID"), userId);
    input.insert(QStringLiteral("voteID"), choiceId);

    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const QJsonObject body =
        gqlVoteInPollRequestBody(MUTATION_VOTE_IN_POLL, variables);

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
                    qCDebug(chatterinoTwitch) << "Twitch GQL VoteInPoll errors:"
                                              << summarizeGraphqlErrors(errs);
                    onError(summarizeGraphqlErrors(errs));
                    return;
                }

                if (!errs.isEmpty())
                {
                    qCDebug(chatterinoTwitch)
                        << "Twitch GQL VoteInPoll partial errors:"
                        << summarizeGraphqlErrors(errs);
                }

                const auto data = root.value(QStringLiteral("data")).toObject();
                const auto vip = data.value(QStringLiteral("voteInPoll"));
                if (vip.isNull() || !vip.isObject())
                {
                    onError(QStringLiteral("Unexpected response"));
                    return;
                }
                const auto vipObj = vip.toObject();
                const QJsonValue errVal = vipObj.value(QStringLiteral("error"));
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
                                      ? QStringLiteral("Could not cast vote")
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

namespace {

    QString normalizeCustomTwitchAuthToken(QString token)
    {
        token = token.trimmed();

        while (!token.isEmpty())
        {
            const auto previous = token;

            if (token.size() >= 2 &&
                ((token.startsWith('"') && token.endsWith('"')) ||
                 (token.startsWith('\'') && token.endsWith('\''))))
            {
                token = token.mid(1, token.size() - 2).trimmed();
            }

            if (token.startsWith("Authorization:", Qt::CaseInsensitive))
            {
                token = token.mid(QString("Authorization:").size()).trimmed();
            }

            if (token.startsWith("OAuth ", Qt::CaseInsensitive))
            {
                token = token.mid(QString("OAuth ").size()).trimmed();
            }

            if (token.startsWith("Bearer ", Qt::CaseInsensitive))
            {
                token = token.mid(QString("Bearer ").size()).trimmed();
            }

            if (token.startsWith("oauth:", Qt::CaseInsensitive))
            {
                token = token.mid(QString("oauth:").size()).trimmed();
            }

            if (token == previous)
            {
                break;
            }
        }

        return token;
    }

    const QString &twitchGqlDeviceId()
    {
        static const QString deviceId = [] {
            auto uuid = generateUuid();
            uuid.remove('{').remove('}').remove('-');
            return uuid;
        }();
        return deviceId;
    }

    const QString &twitchGqlSessionId()
    {
        static const QString sessionId = [] {
            auto uuid = generateUuid();
            uuid.remove('{').remove('}').remove('-');
            return uuid;
        }();
        return sessionId;
    }

    QString predictionCreateOutcomeColor(int index, int outcomeCount)
    {
        if (outcomeCount == 2)
        {
            return index == 0 ? "BLUE" : "PINK";
        }

        return "BLUE";
    }

    constexpr auto TWITCH_GQL_BROWSER_CLIENT_VERSION =
        "ef928475-9403-42f2-8a34-55784bd08e16";
    constexpr auto TWITCH_GQL_BROWSER_USER_AGENT =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
    constexpr auto TWITCH_GQL_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";
    constexpr auto TWITCH_GQL_TV_USER_AGENT =
        "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
    constexpr auto TWITCH_GQL_TV_ORIGIN = "https://android.tv.twitch.tv";
    constexpr auto TWITCH_GQL_TV_REFERER = "https://android.tv.twitch.tv/";
    constexpr int TWITCH_GQL_TIMEOUT_MS = 15 * 1000;

    NetworkRequest makeGqlRequest(const char *query, const QJsonObject &variables,
                                  std::shared_ptr<TwitchAccount> account)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", "kimne78kx3ncx6br8ac4cd5ao176ut")
                .payload(QJsonDocument(payload).toJson());

        if (account)
        {
            const auto &token = account->getOAuthToken();
            if (!token.isEmpty())
            {
                request = std::move(request).header("Authorization", "OAuth " + token);
            }
        }

        return request;
    }

    NetworkRequest makeGqlRequest(const char *query,
                                  const QJsonObject &variables,
                                  const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko")
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payload);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makePersistedGqlRequest(const QString &operationName,
                                           const QString &sha256Hash,
                                           const QJsonObject &variables,
                                           std::shared_ptr<TwitchAccount> account)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko") // Web client ID required for these endpoints
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        if (account)
        {
            const auto &token = account->getOAuthToken();
            if (!token.isEmpty())
            {
                request = std::move(request).header("Authorization", "OAuth " + token);
            }
        }

        return request;
    }

    NetworkRequest makePersistedGqlRequest(const QString &operationName,
                                           const QString &sha256Hash,
                                           const QJsonObject &variables,
                                           const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko")
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeTvPersistedGqlRequest(const QString &operationName,
                                             const QString &sha256Hash,
                                             const QJsonObject &variables,
                                             const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", TWITCH_GQL_TV_CLIENT_ID)
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("Origin", TWITCH_GQL_TV_ORIGIN)
                .header("Referer", TWITCH_GQL_TV_REFERER)
                .header("User-Agent", TWITCH_GQL_TV_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeInlineGqlRequest(const char *query,
                                        const QJsonObject &variables,
                                        const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", "kimne78kx3ncx6brgo4mv6wki5h1ko")
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("User-Agent", TWITCH_GQL_BROWSER_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    NetworkRequest makeTvInlineGqlRequest(const char *query,
                                          const QJsonObject &variables,
                                          const QString &oauthToken)
    {
        QJsonObject payload;
        payload.insert("query", query);
        payload.insert("variables", variables);

        QJsonArray payloadArray;
        payloadArray.append(payload);

        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", TWITCH_GQL_TV_CLIENT_ID)
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("Origin", TWITCH_GQL_TV_ORIGIN)
                .header("Referer", TWITCH_GQL_TV_REFERER)
                .header("User-Agent", TWITCH_GQL_TV_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    QString extractFirstGqlErrorMessage(const rapidjson::Document &doc)
    {
        const rapidjson::Value *payload = nullptr;

        if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject())
        {
            payload = &doc[0];
        }
        else if (doc.IsObject())
        {
            payload = &doc;
        }

        if (payload == nullptr || !payload->HasMember("errors") ||
            !(*payload)["errors"].IsArray() || (*payload)["errors"].Empty())
        {
            return {};
        }

        for (const auto &error : (*payload)["errors"].GetArray())
        {
            QString message;
            if (rj::getSafe(error, "message", message) && !message.isEmpty())
            {
                return message;
            }
        }

        return "Twitch rejected the token";
    }

    QJsonObject firstPayloadObject(const QJsonValue &value)
    {
        if (value.isArray())
        {
            const auto array = value.toArray();
            if (!array.isEmpty() && array.first().isObject())
            {
                return array.first().toObject();
            }
        }
        else if (value.isObject())
        {
            return value.toObject();
        }

        return {};
    }

    bool readInteger(const rapidjson::Value &value, qint64 &out)
    {
        if (value.IsInt64())
        {
            out = value.GetInt64();
            return true;
        }
        if (value.IsUint64())
        {
            const auto raw = value.GetUint64();
            if (raw > quint64(std::numeric_limits<qint64>::max()))
            {
                return false;
            }
            out = qint64(raw);
            return true;
        }
        if (value.IsDouble())
        {
            const auto raw = value.GetDouble();
            if (!std::isfinite(raw) || raw < 0 ||
                raw > double(std::numeric_limits<qint64>::max()))
            {
                return false;
            }
            out = qint64(raw);
            return true;
        }
        return false;
    }

    bool readInteger(const rapidjson::Value &obj, const char *key, qint64 &out)
    {
        if (!obj.IsObject() || !obj.HasMember(key))
        {
            return false;
        }

        return readInteger(obj[key], out);
    }

    qint64 jsonIntegerValue(const QJsonValue &value, qint64 fallback = -1)
    {
        if (value.isDouble())
        {
            return value.toInteger(fallback);
        }

        return fallback;
    }

    QString extractFirstGqlErrorMessageFromPayload(const QJsonObject &payload)
    {
        const auto errors = payload.value("errors").toArray();
        for (const auto &errorValue : errors)
        {
            const auto error = errorValue.toObject();
            const auto message = error.value("message").toString();
            if (!message.isEmpty())
            {
                return message;
            }
        }
        return {};
    }

    QString extractFirstGqlErrorMessage(const QJsonValue &value)
    {
        if (value.isArray())
        {
            for (const auto &payloadValue : value.toArray())
            {
                const auto message =
                    extractFirstGqlErrorMessageFromPayload(
                        payloadValue.toObject());
                if (!message.isEmpty())
                {
                    return message;
                }
            }
            return {};
        }

        return extractFirstGqlErrorMessageFromPayload(firstPayloadObject(value));
    }

    QJsonObject payloadDataObject(const QJsonValue &value)
    {
        const auto payload = firstPayloadObject(value);
        return payload.value("data").toObject();
    }

    QJsonObject payloadDataObjectForOperation(const QJsonValue &value,
                                              const QString &operationName)
    {
        if (value.isArray())
        {
            const auto array = value.toArray();
            for (const auto &payloadValue : array)
            {
                const auto payload = payloadValue.toObject();
                const auto payloadOperation = payload.value("extensions")
                                                  .toObject()
                                                  .value("operationName")
                                                  .toString();
                if (payloadOperation.compare(operationName,
                                             Qt::CaseInsensitive) == 0)
                {
                    return payload.value("data").toObject();
                }
            }
        }

        return payloadDataObject(value);
    }

    QJsonObject persistedPayload(const QString &operationName,
                                 const QJsonObject &variables,
                                 const QString &sha256Hash)
    {
        QJsonObject payload;
        payload.insert("operationName", operationName);
        payload.insert("variables", variables);

        QJsonObject persistedQuery;
        persistedQuery.insert("version", 1);
        persistedQuery.insert("sha256Hash", sha256Hash);

        QJsonObject extensions;
        extensions.insert("persistedQuery", persistedQuery);
        payload.insert("extensions", extensions);
        return payload;
    }

    NetworkRequest makeTvPersistedGqlBatchRequest(
        const QJsonArray &payloadArray, const QString &oauthToken)
    {
        auto request =
            NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
                .timeout(TWITCH_GQL_TIMEOUT_MS)
                .header("Client-Id", TWITCH_GQL_TV_CLIENT_ID)
                .header("Client-Session-Id", twitchGqlSessionId())
                .header("Client-Version", TWITCH_GQL_BROWSER_CLIENT_VERSION)
                .header("Origin", TWITCH_GQL_TV_ORIGIN)
                .header("Referer", TWITCH_GQL_TV_REFERER)
                .header("User-Agent", TWITCH_GQL_TV_USER_AGENT)
                .header("X-Device-Id", twitchGqlDeviceId())
                .json(payloadArray);

        const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
        if (!normalizedToken.isEmpty())
        {
            request = std::move(request).header("Authorization",
                                                "OAuth " + normalizedToken);
        }

        return request;
    }

    void sendTerminatePollRequest(
        const QString &pollId, const QString &currentUserId,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("pollID", pollId);

        QJsonObject variables;
        variables.insert("input", input);

        QJsonArray payloadArray;
        payloadArray.append(persistedPayload(
            "TerminatePoll", variables,
            "2701ef0594dae5f532ce68e58cc3036a6d020755eef49927f98c14017fd819b2"));
        if (!currentUserId.trimmed().isEmpty())
        {
            QJsonObject spadeVariables;
            spadeVariables.insert("id", currentUserId.trimmed());
            payloadArray.append(persistedPayload(
                "Core_Services_Spade_ChatEvent_User", spadeVariables,
                "9cb0f182474382a0e72e817318460eeefc7c1cab0d163ac064a603d850b085ea"));
        }

        makeTvPersistedGqlBatchRequest(payloadArray, oauthToken)
            .onSuccess([successCallback, failureCallback, pollId](
                           const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto data =
                    payloadDataObjectForOperation(root, "TerminatePoll");
                const auto returnedPollId = data.value("terminatePoll")
                                                .toObject()
                                                .value("poll")
                                                .toObject()
                                                .value("id")
                                                .toString();
                if (returnedPollId.isEmpty() || returnedPollId != pollId)
                {
                    failureCallback("Twitch API Error: Failed to end poll");
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    GqlChannelSelfData channelSelfDataFromObject(const QJsonObject &self)
    {
        GqlChannelSelfData data;

        const auto badges = self.value("displayBadges").toArray();
        for (const auto &badgeValue : badges)
        {
            const auto badge = badgeValue.toObject();
            const auto setId = badge.value("setID").toString();
            const auto title = badge.value("title").toString();
            if (setId.compare("lead_moderator", Qt::CaseInsensitive) == 0 ||
                title.compare("Lead Moderator", Qt::CaseInsensitive) == 0)
            {
                data.isLeadModerator = true;
                break;
            }
        }

        return data;
    }

    QString gqlPayloadErrorMessage(const QJsonValue &value,
                                   const QString &fallback)
    {
        if (value.isUndefined() || value.isNull())
        {
            return {};
        }
        if (value.isString())
        {
            return value.toString().trimmed();
        }
        if (!value.isObject())
        {
            return fallback;
        }

        const auto obj = value.toObject();
        for (const auto &key : {
                 QStringLiteral("code"),
                 QStringLiteral("reason"),
                 QStringLiteral("message"),
             })
        {
            const auto text = obj.value(key).toString().trimmed();
            if (!text.isEmpty())
            {
                return text;
            }
        }

        return fallback;
    }

    void runRoleMutation(const QString &operationName, const QString &hash,
                         const QString &payloadName,
                         const QString &targetInputName,
                         const QString &channelId,
                         const QString &targetLogin,
                         const QString &oauthToken,
                         const QString &fallbackError,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("channelID", channelId);
        input.insert(targetInputName, targetLogin);

        QJsonObject variables;
        variables.insert("input", input);

        makePersistedGqlRequest(operationName, hash, variables, oauthToken)
            .onSuccess([payloadName, fallbackError, successCallback,
                        failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto payload =
                    payloadDataObject(root).value(payloadName).toObject();
                if (payload.isEmpty())
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                const auto payloadError =
                    gqlPayloadErrorMessage(payload.value("error"), fallbackError);
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    void runTvRoleMutation(const QString &operationName, const QString &hash,
                           const QString &payloadName,
                           const QString &targetInputName,
                           const QString &channelId,
                           const QString &targetValue,
                           const QString &oauthToken,
                           const QString &fallbackError,
                           const QString &roleId,
                           const QString &successFlagName,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("channelID", channelId);
        input.insert(targetInputName, targetValue);
        if (!roleId.isEmpty())
        {
            input.insert("roleID", roleId);
        }

        QJsonObject variables;
        variables.insert("input", input);

        makeTvPersistedGqlRequest(operationName, hash, variables, oauthToken)
            .onSuccess([operationName, payloadName, fallbackError,
                        successFlagName, successCallback,
                        failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto payload =
                    payloadDataObjectForOperation(root, operationName)
                        .value(payloadName)
                        .toObject();
                if (payload.isEmpty())
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                const auto payloadError = gqlPayloadErrorMessage(
                    payload.value("error"), fallbackError);
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }

                if (!successFlagName.isEmpty() &&
                    !payload.value(successFlagName).toBool(false))
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    void runFollowMutation(const QString &operationName, const QString &hash,
                           const QString &payloadName, const QString &targetId,
                           bool disableNotifications,
                           const QString &oauthToken,
                           const QString &fallbackError,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
    {
        QJsonObject input;
        input.insert("targetID", targetId);
        if (disableNotifications)
        {
            input.insert("disableNotifications", true);
        }
        else if (operationName == "FollowButton_FollowUser")
        {
            input.insert("disableNotifications", false);
        }

        QJsonObject variables;
        variables.insert("input", input);

        makeTvPersistedGqlRequest(operationName, hash, variables, oauthToken)
            .onSuccess([payloadName, fallbackError, successCallback,
                        failureCallback](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    failureCallback("Failed to parse GQL response");
                    return;
                }

                const auto gqlError = extractFirstGqlErrorMessage(root);
                if (!gqlError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + gqlError);
                    return;
                }

                const auto payload =
                    payloadDataObject(root).value(payloadName).toObject();
                if (payload.isEmpty())
                {
                    failureCallback("Twitch API Error: " + fallbackError);
                    return;
                }

                const auto payloadError =
                    gqlPayloadErrorMessage(payload.value("error"), fallbackError);
                if (!payloadError.isEmpty())
                {
                    failureCallback("Twitch API Error: " + payloadError);
                    return;
                }

                successCallback();
            })
            .onError([failureCallback](const NetworkResult &result) {
                failureCallback("Network Error: " + result.formatError());
            })
            .execute();
    }

    GqlBlockedTerm blockedTermFromObject(const QJsonObject &obj)
    {
        GqlBlockedTerm term;
        term.id = obj.value("id").toString().trimmed();
        term.phrase = obj.value("phrase").toString().trimmed();
        term.expiresAt = obj.value("expiresAt").toString().trimmed();
        term.isModEditable = obj.value("isModEditable").toBool(false);
        term.hitCount = obj.value("hitCount").toInt(0);
        return term;
    }

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
    QString makeTransactionId()
    {
        auto uuid = generateUuid();
        uuid.remove('{').remove('}').remove('-');
        return uuid;
    }

    QString imageUrlFromRewardObject(const QJsonObject &obj)
    {
        auto image = obj.value("image").toObject();
        if (image.isEmpty())
        {
            image = obj.value("defaultImage").toObject();
        }

        auto url = image.value("url2x").toString();
        if (url.isEmpty())
        {
            url = image.value("url").toString();
        }
        if (url.isEmpty())
        {
            url = image.value("url_2x").toString();
        }
        if (url.isEmpty())
        {
            url = image.value("url_1x").toString();
        }
        return url;
    }

    int rewardCostFromObject(const QJsonObject &obj)
    {
        auto cost = obj.value("cost").toInt(-1);
        if (cost < 0 || obj.value("cost").isNull())
        {
            cost = obj.value("defaultCost").toInt(0);
        }
        if (cost <= 0)
        {
            cost = obj.value("bitsCost").toInt(0);
        }
        if (cost <= 0)
        {
            cost = obj.value("defaultBitsCost").toInt(0);
        }
        return cost;
    }

    QString automaticRewardTitle(const QString &type)
    {
        if (type == "RANDOM_SUB_EMOTE_UNLOCK")
        {
            return "Unlock a Random Emote";
        }
        if (type == "CHOSEN_SUB_EMOTE_UNLOCK")
        {
            return "Choose an Emote to Unlock";
        }
        if (type == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK")
        {
            return "Modify a Single Emote";
        }
        if (type == "SINGLE_MESSAGE_BYPASS_SUB_MODE")
        {
            return "Send a Message in Sub-Only";
        }
        if (type == "SEND_HIGHLIGHTED_MESSAGE")
        {
            return "Highlight My Message";
        }
        if (type == "SEND_ANIMATED_MESSAGE")
        {
            return "Message Effects";
        }
        if (type == "SEND_GIGANTIFIED_EMOTE")
        {
            return "Gigantify an Emote";
        }
        if (type == "CELEBRATION")
        {
            return "On-Screen Celebration";
        }
        return type;
    }

    QString automaticRewardPrompt(const QString &type)
    {
        if (type == "RANDOM_SUB_EMOTE_UNLOCK")
        {
            return "Unlock a random subscriber emote for 24 hours.";
        }
        if (type == "CHOSEN_SUB_EMOTE_UNLOCK")
        {
            return "Pick a subscriber emote to unlock for 24 hours.";
        }
        if (type == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK")
        {
            return "Pick an emote and modifier to unlock for 24 hours.";
        }
        if (type == "SINGLE_MESSAGE_BYPASS_SUB_MODE")
        {
            return "Send one message while sub-only mode is active.";
        }
        if (type == "SEND_HIGHLIGHTED_MESSAGE")
        {
            return "Send one highlighted message.";
        }
        return {};
    }

    GqlChannelPointReward channelPointRewardFromObject(const QJsonObject &obj,
                                                       bool automatic)
    {
        GqlChannelPointReward reward;
        reward.isAutomatic = automatic;
        reward.id = obj.value("id").toString();
        reward.rewardType = automatic ? obj.value("type").toString()
                                      : QStringLiteral("CUSTOM_REWARD");
        reward.title = automatic ? automaticRewardTitle(reward.rewardType)
                                 : obj.value("title").toString();
        reward.prompt = automatic ? automaticRewardPrompt(reward.rewardType)
                                  : obj.value("prompt").toString();
        reward.pricingType = obj.value("pricingType").toString("POINTS");
        reward.backgroundColor =
            obj.value("backgroundColor")
                .toString(obj.value("defaultBackgroundColor").toString());
        reward.imageUrl = imageUrlFromRewardObject(obj);
        reward.cost = rewardCostFromObject(obj);
        reward.isEnabled = obj.value("isEnabled").toBool(false) &&
                           !obj.value("isPaused").toBool(false);
        reward.isInStock = obj.value("isInStock").toBool(true);
        reward.isUserInputRequired =
            obj.value("isUserInputRequired").toBool(false);
        return reward;
    }

    GqlChannelPointRedeemResult redeemResultFromPayload(
        const QJsonObject &payload)
    {
        GqlChannelPointRedeemResult result;
        result.balance = jsonIntegerValue(payload.value("balance"));
        const auto emote = payload.value("emote").toObject();
        result.emoteId = emote.value("id").toString();
        result.emoteToken = emote.value("token").toString();
        return result;
    }

    bool rejectGqlOrPayloadError(
        const QJsonValue &root, const QJsonObject &payload,
        const QString &fallback,
        const std::function<void(const QString &)> &failureCallback)
    {
        const auto gqlError = extractFirstGqlErrorMessage(root);
        if (!gqlError.isEmpty())
        {
            failureCallback("Twitch API Error: " + gqlError);
            return true;
        }

        const auto payloadError =
            gqlPayloadErrorMessage(payload.value("error"), fallback);
        if (!payloadError.isEmpty())
        {
            failureCallback("Twitch API Error: " + payloadError);
            return true;
        }

        return false;
    }

#endif

    QString userDisplayNameFromValue(const QJsonValue &value)
    {
        const auto obj = value.toObject();
        auto name = obj.value("displayName").toString();
        if (name.isEmpty())
        {
            name = obj.value("display_name").toString();
        }
        if (name.isEmpty())
        {
            name = obj.value("login").toString();
        }
        if (name.isEmpty())
        {
            name = obj.value("name").toString();
        }
        return name;
    }

    QDateTime parseGqlDateTime(const QJsonValue &value)
    {
        const auto str = value.toString();
        if (str.isEmpty())
        {
            return {};
        }

        auto dt = QDateTime::fromString(str, Qt::ISODate);
        if (!dt.isValid())
        {
            dt = QDateTime::fromString(str, Qt::ISODateWithMs);
        }
        return dt;
    }

    struct GqlFragmentUser {
        QString id;
        QString login;
        QString displayName;
        int fragmentIndex = -1;
    };

    QString tokenText(const QJsonObject &token)
    {
        auto text = token.value("text").toString();
        if (text.isEmpty())
        {
            text = token.value("localizedText").toString();
        }
        if (text.isEmpty())
        {
            text = token.value("displayName").toString();
        }
        if (text.isEmpty())
        {
            text = token.value("login").toString();
        }
        return text;
    }

    QJsonArray localizedFragments(const QJsonObject &obj)
    {
        auto fragments = obj.value("localizedStringFragments").toArray();
        if (fragments.isEmpty())
        {
            fragments = obj.value("fragments").toArray();
        }
        return fragments;
    }

    QJsonArray actionFragments(const QJsonObject &node,
                               const QString &fieldName)
    {
        return localizedFragments(node.value(fieldName).toObject());
    }

    QVector<GqlFragmentUser> usersFromFragments(const QJsonArray &fragments)
    {
        QVector<GqlFragmentUser> users;
        for (int i = 0; i < fragments.size(); ++i)
        {
            const auto fragment = fragments.at(i).toObject();
            const auto token = fragment.value("token").toObject();
            auto displayName = token.value("displayName").toString();
            const auto login = token.value("login").toString();
            const auto id = token.value("id").toString();
            const auto type = token.value("__typename").toString();
            if (displayName.isEmpty() && type == "User")
            {
                displayName = tokenText(token);
            }
            if (displayName.isEmpty() && login.isEmpty() && id.isEmpty())
            {
                continue;
            }

            users.push_back({
                .id = id,
                .login = login,
                .displayName = displayName.isEmpty() ? login : displayName,
                .fragmentIndex = i,
            });
        }
        return users;
    }

    QString textFromFragments(const QJsonArray &fragments)
    {
        QString text;
        for (const auto &fragmentValue : fragments)
        {
            const auto fragment = fragmentValue.toObject();
            const auto token = fragment.value("token").toObject();
            auto part = tokenText(token);
            if (part.isEmpty())
            {
                part = fragment.value("text").toString();
            }
            text += part;
        }
        return text.simplified();
    }

    QString textBeforeFragment(const QJsonArray &fragments, int index,
                               int lookBehind = 2)
    {
        QString text;
        const int begin = std::max(0, index - lookBehind);
        for (int i = begin; i < index; ++i)
        {
            const auto fragment = fragments.at(i).toObject();
            auto part = tokenText(fragment.value("token").toObject());
            if (part.isEmpty())
            {
                part = fragment.value("text").toString();
            }
            text += part;
        }
        return text;
    }

    std::optional<GqlFragmentUser> userAfterByText(
        const QJsonArray &fragments)
    {
        const auto users = usersFromFragments(fragments);
        for (const auto &user : users)
        {
            const auto before = textBeforeFragment(fragments, user.fragmentIndex)
                                    .toLower();
            if (before.contains(" by ") || before.endsWith("by ") ||
                before.contains("automated by "))
            {
                return user;
            }
        }
        return std::nullopt;
    }

    void assignUser(GqlFragmentUser user, QString &id, QString &login,
                    QString &displayName)
    {
        id = std::move(user.id);
        login = std::move(user.login);
        displayName = std::move(user.displayName);
    }

    GqlModerationActionKind moderationActionKind(const QString &category,
                                                 const QString &icon,
                                                 const QString &text)
    {
        const auto cat = category.toUpper();
        const auto ico = icon.toUpper();
        const auto lowerText = text.toLower();

        if (ico == "BAN")
        {
            return GqlModerationActionKind::Ban;
        }
        if (ico == "UNBAN")
        {
            return GqlModerationActionKind::Unban;
        }
        if (ico == "TIMEOUT")
        {
            return GqlModerationActionKind::Timeout;
        }
        if (ico == "UNTIMEOUT")
        {
            return GqlModerationActionKind::Untimeout;
        }
        if (cat.contains("BANS_AND_UNBANS"))
        {
            return lowerText.contains("unban") ? GqlModerationActionKind::Unban
                                               : GqlModerationActionKind::Ban;
        }
        if (cat.contains("TIMEOUTS_AND_UNTIMEOUTS"))
        {
            return lowerText.contains("untimeout") ||
                           lowerText.contains("timeout removed")
                       ? GqlModerationActionKind::Untimeout
                       : GqlModerationActionKind::Timeout;
        }
        if (cat.contains("DELETE") || ico.contains("DELETE") ||
            lowerText.contains("message deleted") ||
            lowerText.contains("deleted message") ||
            lowerText.contains("was deleted"))
        {
            return GqlModerationActionKind::Delete;
        }
        if (cat.contains("MESSAGE") || ico.contains("MESSAGE"))
        {
            return GqlModerationActionKind::Message;
        }
        return GqlModerationActionKind::Other;
    }

    GqlModerationActionLogEntry moderationActionFromNode(
        const QJsonObject &edge)
    {
        const auto node = edge.value("node").toObject();
        GqlModerationActionLogEntry action;
        action.cursor = edge.value("cursor").toString();
        action.id = node.value("id").toString();
        action.category = node.value("filterCategoryID").toString();
        action.icon = node.value("icon").toString();
        action.createdAt = parseGqlDateTime(node.value("createdAt"));

        const auto contentFragments = actionFragments(node, "content");
        const auto bodyFragments = actionFragments(node, "contentBody");
        const auto titleFragments = actionFragments(node, "title");

        action.text = textFromFragments(contentFragments);
        if (action.text.isEmpty())
        {
            action.text = textFromFragments(bodyFragments);
        }
        if (action.text.isEmpty())
        {
            action.text = textFromFragments(titleFragments);
        }

        action.kind =
            moderationActionKind(action.category, action.icon, action.text);

        if (auto moderator = userAfterByText(contentFragments))
        {
            assignUser(std::move(*moderator), action.moderatorId,
                       action.moderatorLogin, action.moderatorDisplayName);
        }
        else if (auto bodyModerator = userAfterByText(bodyFragments))
        {
            assignUser(std::move(*bodyModerator), action.moderatorId,
                       action.moderatorLogin, action.moderatorDisplayName);
        }

        const auto titleUsers = usersFromFragments(titleFragments);
        if (!titleUsers.empty())
        {
            const auto &target = titleUsers.front();
            action.targetId = target.id;
            action.targetLogin = target.login;
            action.targetDisplayName = target.displayName;
        }

        if (action.moderatorId.isEmpty() && action.moderatorLogin.isEmpty())
        {
            const auto contentUsers = usersFromFragments(contentFragments);
            if (!contentUsers.empty())
            {
                const auto &candidate = contentUsers.front();
                const bool sameAsTarget =
                    (!candidate.id.isEmpty() &&
                     candidate.id == action.targetId) ||
                    (!candidate.login.isEmpty() &&
                     candidate.login.compare(action.targetLogin,
                                             Qt::CaseInsensitive) == 0);
                if (!sameAsTarget)
                {
                    action.moderatorId = candidate.id;
                    action.moderatorLogin = candidate.login;
                    action.moderatorDisplayName = candidate.displayName;
                }
            }
        }

        const auto user = node.value("user").toObject();
        if (action.moderatorId.isEmpty() && action.moderatorLogin.isEmpty() &&
            !user.isEmpty())
        {
            const auto id = user.value("id").toString();
            const auto login = user.value("login").toString();
            const bool sameAsTarget =
                (!id.isEmpty() && id == action.targetId) ||
                (!login.isEmpty() &&
                 login.compare(action.targetLogin, Qt::CaseInsensitive) == 0);
            if (!sameAsTarget)
            {
                action.moderatorId = id;
                action.moderatorLogin = login;
                action.moderatorDisplayName =
                    user.value("displayName").toString(login);
            }
        }

        if (action.targetId.isEmpty() && action.targetLogin.isEmpty())
        {
            const auto contentUsers = usersFromFragments(contentFragments);
            for (const auto &userCandidate : contentUsers)
            {
                const bool isModerator =
                    (!userCandidate.id.isEmpty() &&
                     userCandidate.id == action.moderatorId) ||
                    (!userCandidate.login.isEmpty() &&
                     userCandidate.login.compare(action.moderatorLogin,
                                                 Qt::CaseInsensitive) == 0);
                if (!isModerator)
                {
                    action.targetId = userCandidate.id;
                    action.targetLogin = userCandidate.login;
                    action.targetDisplayName = userCandidate.displayName;
                    break;
                }
            }
        }

        return action;
    }

    bool isConnectionObject(const QJsonObject &obj)
    {
        return obj.value("edges").isArray() || obj.value("nodes").isArray();
    }

    QJsonObject findModeratedChannelsConnection(const QJsonValue &value)
    {
        if (value.isArray())
        {
            const auto array = value.toArray();
            for (const auto &item : array)
            {
                auto found = findModeratedChannelsConnection(item);
                if (!found.isEmpty())
                {
                    return found;
                }
            }
            return {};
        }

        if (!value.isObject())
        {
            return {};
        }

        const auto obj = value.toObject();
        const auto direct = obj.value("moderatedChannels");
        if (direct.isObject())
        {
            const auto connection = direct.toObject();
            if (isConnectionObject(connection))
            {
                return connection;
            }
        }

        for (auto it = obj.begin(); it != obj.end(); ++it)
        {
            auto found = findModeratedChannelsConnection(it.value());
            if (!found.isEmpty())
            {
                return found;
            }
        }

        return {};
    }

    QString raidObjectString(const QJsonObject &obj,
                             std::initializer_list<QString> keys)
    {
        for (const auto &key : keys)
        {
            const auto value = obj.value(key);
            if (value.isString())
            {
                const auto text = value.toString().trimmed();
                if (!text.isEmpty())
                {
                    return text;
                }
            }
            if (value.isDouble())
            {
                return QString::number(qint64(value.toDouble()));
            }
        }

        return {};
    }

    QString raidUserIdFromObject(const QJsonObject &obj)
    {
        return raidObjectString(obj,
                                {
                                    QStringLiteral("id"),
                                    QStringLiteral("userID"),
                                    QStringLiteral("userId"),
                                    QStringLiteral("user_id"),
                                });
    }

    QString normalizedRaidLogin(QString value)
    {
        value = value.trimmed().toLower();
        while (value.startsWith(QLatin1Char('@')) ||
               value.startsWith(QLatin1Char('#')))
        {
            value = value.mid(1).trimmed();
        }
        return value;
    }

    QString raidErrorMessage(const QJsonValue &value)
    {
        if (value.isUndefined() || value.isNull())
        {
            return {};
        }
        if (value.isString())
        {
            return value.toString().trimmed();
        }
        if (!value.isObject())
        {
            return QStringLiteral("Twitch rejected the raid action");
        }

        const auto obj = value.toObject();
        const auto message =
            raidObjectString(obj,
                             {
                                 QStringLiteral("code"),
                                 QStringLiteral("reason"),
                                 QStringLiteral("message"),
                             });
        return message.isEmpty() ? QStringLiteral("Twitch rejected the raid action")
                                 : message;
    }

    QString raidFailureMessage(QString error)
    {
        error = error.trimmed();
        if (error.startsWith(QStringLiteral("Twitch API Error:"),
                             Qt::CaseInsensitive))
        {
            error = error.mid(QStringLiteral("Twitch API Error:").size())
                        .trimmed();
        }

        if (error.isEmpty())
        {
            return QStringLiteral("Twitch rejected the raid action");
        }

        const auto upper = error.toUpper();
        if (upper.contains(QStringLiteral("TARGET_SETTINGS_DO_NOT_ALLOW")) ||
            (upper.contains(QStringLiteral("TARGET")) &&
             upper.contains(QStringLiteral("SETTING"))))
        {
            return QStringLiteral(
                "That channel's raid settings do not allow this raid. "
                "They may require more viewers than you currently have.");
        }

        if (upper.contains(QStringLiteral("EDITOR")) ||
            upper.contains(QStringLiteral("BROADCASTER")) ||
            upper.contains(QStringLiteral("NOT_AUTHORIZED")) ||
            upper.contains(QStringLiteral("NOT AUTHORIZED")) ||
            upper.contains(QStringLiteral("FORBIDDEN")) ||
            upper.contains(QStringLiteral("PERMISSION")) ||
            upper == QStringLiteral("SERVICE ERROR"))
        {
            return QStringLiteral(
                "You need broadcaster or editor raid permission in this "
                "channel.");
        }

        if (upper.contains(QStringLiteral("NO_ACTIVE_RAID")) ||
            upper.contains(QStringLiteral("NO_RAID")) ||
            upper.contains(QStringLiteral("NO RAID")))
        {
            return QStringLiteral("There is no active raid in this channel.");
        }

        if (upper.contains(QStringLiteral("CANT_RAID_YOURSELF")) ||
            upper.contains(QStringLiteral("CAN'T RAID YOURSELF")) ||
            upper.contains(QStringLiteral("CANNOT RAID YOURSELF")))
        {
            return QStringLiteral("A channel cannot raid itself.");
        }

        return QStringLiteral("Twitch API Error: ") + error;
    }

    std::optional<TwitchChannel::PollEvent> parsePollEventFromGql(
        const QJsonObject &viewablePoll, const QString &currentUserId = {})
    {
        if (viewablePoll.isEmpty())
        {
            return std::nullopt;
        }

        TwitchChannel::PollEvent poll;
        poll.id = viewablePoll.value("id").toString();
        poll.title = viewablePoll.value("title").toString();
        poll.status = viewablePoll.value("status").toString();
        poll.remainingDurationMilliseconds =
            viewablePoll.value("remainingDurationMilliseconds").toInt();
        poll.createdAt = parseGqlDateTime(viewablePoll.value("createdAt"));
        auto endsAt = parseGqlDateTime(viewablePoll.value("endsAt"));
        if (endsAt.isValid())
        {
            poll.endsAt = endsAt;
            if (poll.createdAt.isValid())
            {
                poll.durationSeconds =
                    std::max(0, int(poll.createdAt.secsTo(*poll.endsAt)));
            }
        }

        poll.createdByName =
            userDisplayNameFromValue(viewablePoll.value("createdBy"));
        poll.currentUserId = currentUserId;

        const auto settings = viewablePoll.value("settings").toObject();
        const auto cpVotes =
            settings.value("communityPointsVotes").toObject();
        poll.channelPointsVotingEnabled =
            cpVotes.value("isEnabled").toBool();
        poll.pointsPerVote = cpVotes.value("cost").toInt();

        const auto topContributor =
            viewablePoll.value("topChannelPointsContributor").toObject();
        const auto topContributorName =
            userDisplayNameFromValue(topContributor);
        const int topContributorAmount =
            topContributor.value("contribution").toInt(
                topContributor.value("amount").toInt());

        const auto choices = viewablePoll.value("choices").toArray();
        poll.choices.reserve(size_t(choices.size()));
        for (const auto &choiceValue : choices)
        {
            const auto choiceObj = choiceValue.toObject();
            TwitchChannel::PollChoice choice;
            choice.id = choiceObj.value("id").toString();
            choice.title = choiceObj.value("title").toString();
            const auto votes = choiceObj.value("votes").toObject();
            choice.totalVotes = votes.value("total").toInt();
            choice.freeVotes = votes.value("base").toInt();
            choice.channelPointsVotes =
                votes.value("communityPoints").toInt();
            choice.totalVoters = choiceObj.value("totalVoters").toInt();
            if (!topContributorName.isEmpty() && choice.channelPointsVotes > 0)
            {
                choice.topChannelPointsContribution = topContributorAmount;
                choice.topChannelPointsContributorName = topContributorName;
            }
            poll.totalVotes += choice.totalVotes;
            poll.choices.push_back(std::move(choice));
        }

        const auto voter = viewablePoll.value("self").toObject().value("voter").toObject();
        const auto voterChoices = voter.value("choices").toArray();
        poll.selfVotes.reserve(size_t(voterChoices.size()));
        for (const auto &voterChoiceValue : voterChoices)
        {
            const auto voterChoice = voterChoiceValue.toObject();
            TwitchChannel::PollSelfVote selfVote;
            selfVote.choiceId =
                voterChoice.value("pollChoice").toObject().value("id").toString();
            const auto votes = voterChoice.value("votes").toObject();
            selfVote.freeVotes = votes.value("base").toInt();
            selfVote.channelPointsVotes =
                votes.value("communityPoints").toInt();
            if (!selfVote.choiceId.isEmpty())
            {
                poll.selfVotes.push_back(std::move(selfVote));
            }
        }

        if (poll.id.isEmpty() || poll.title.isEmpty() || poll.choices.empty())
        {
            return std::nullopt;
        }

        return poll;
    }

}  // namespace

void TwitchGql::pinMessage(const QString &channelId, const QString &messageId,
                           int durationSeconds, const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("messageID", messageId);
    if (durationSeconds > 0)
    {
        input.insert("durationSeconds", durationSeconds);
    }
    input.insert("type", "MOD");
    variables.insert("input", input);

    makePersistedGqlRequest("PinChatMessage", "214191369c21f1ad67ac074795d53832329c70e4088c979040c9f86334a7d736", variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                qCDebug(chatterinoTwitch)
                    << "Twitch API Error in PinChatMessage:" << gqlError;
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("pinChatMessage").toObject();
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to pin message"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getUserByLogin(
    const QString &login, const QString &oauthToken,
    std::function<void(std::optional<GqlUser>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    static constexpr char QUERY[] = R"(
        query MoltorinoUserByLogin($login: String!) {
            user(login: $login) {
                id
                login
                displayName
            }
        }
    )";

    QJsonObject variables;
    variables.insert("login", login);

    makeGqlRequest(QUERY, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto userObject =
                payloadDataObject(root).value("user").toObject();
            GqlUser user;
            user.id = userObject.value("id").toString();
            user.login = userObject.value("login").toString();
            user.displayName = userObject.value("displayName").toString();

            if (user.id.isEmpty())
            {
                successCallback(std::nullopt);
                return;
            }

            successCallback(std::move(user));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::followUser(
    const QString &targetId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runFollowMutation(
        "FollowButton_FollowUser",
        "800e7346bdf7e5278a3c1d3f21b2b56e2639928f86815677a7126b093b2fdd08",
        "followUser", targetId, false, oauthToken,
        "Twitch did not follow the user", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::unfollowUser(
    const QString &targetId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runFollowMutation(
        "FollowButton_UnfollowUser",
        "f7dae976ebf41c755ae2d758546bfd176b4eeb856656098bb40e0a672ca0d880",
        "unfollowUser", targetId, false, oauthToken,
        "Twitch did not unfollow the user", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::getLatestModLogMessageBySender(
    const QString &channelId, const QString &senderId,
    const QString &oauthToken,
    std::function<void(std::optional<GqlModLogMessage>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("senderID", senderId);

    makePersistedGqlRequest(
        "ViewerCardModLogsMessagesBySender",
        "eb4e9869e1bb0b3ed553e1ed657fa09f8553781093569c3a5813ad09ee9c0776",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto messages = data.value("viewerCardModLogs")
                                      .toObject()
                                      .value("messages")
                                      .toObject();
            const auto edges = messages.value("edges").toArray();
            for (const auto &edgeValue : edges)
            {
                const auto node =
                    edgeValue.toObject().value("node").toObject();
                if (node.value("isDeleted").toBool(false))
                {
                    continue;
                }

                GqlModLogMessage message;
                message.id = node.value("id").toString();
                message.sentAt = node.value("sentAt").toString();
                message.text =
                    node.value("content").toObject().value("text").toString();
                if (!message.id.isEmpty())
                {
                    successCallback(std::move(message));
                    return;
                }
            }

            successCallback(std::nullopt);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getUsercardMessagesBySender(
    const QString &channelId, const QString &senderId, const QString &cursor,
    const QString &oauthToken,
    std::function<void(GqlUsercardMessagePage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("senderID", senderId);
    if (!cursor.isEmpty())
    {
        variables.insert("cursor", cursor);
    }

    makeTvPersistedGqlRequest(
        "ViewerCardModLogsMessagesBySender",
        "eb4e9869e1bb0b3ed553e1ed657fa09f8553781093569c3a5813ad09ee9c0776",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto messages = data.value("viewerCardModLogs")
                                      .toObject()
                                      .value("messages")
                                      .toObject();
            const auto edges = messages.value("edges").toArray();

            GqlUsercardMessagePage page;
            page.hasNextPage = messages.value("pageInfo")
                                   .toObject()
                                   .value("hasNextPage")
                                   .toBool(false);
            page.messages.reserve(edges.size());

            for (const auto &edgeValue : edges)
            {
                const auto edge = edgeValue.toObject();
                const auto node = edge.value("node").toObject();
                const auto content = node.value("content").toObject();
                const auto sender = node.value("sender").toObject();

                GqlUsercardMessage message;
                message.id = node.value("id").toString();
                message.sentAt = node.value("sentAt").toString();
                message.text = content.value("text").toString();
                message.cursor = edge.value("cursor").toString();
                message.isDeleted = node.value("isDeleted").toBool(false);
                message.deletedBy = node.value("lastUpdatedBy")
                                        .toObject()
                                        .value("displayName")
                                        .toString();
                message.senderId = sender.value("id").toString();
                message.senderLogin = sender.value("login").toString();
                message.senderDisplayName =
                    sender.value("displayName").toString();
                message.senderColor = sender.value("chatColor").toString();
                QStringList badges;
                const auto displayBadges =
                    sender.value("displayBadges").toArray();
                badges.reserve(displayBadges.size());
                for (const auto &badgeValue : displayBadges)
                {
                    const auto badge = badgeValue.toObject();
                    const auto setId = badge.value("setID").toString();
                    const auto version = badge.value("version").toString();
                    if (!setId.isEmpty() && !version.isEmpty())
                    {
                        badges.push_back(setId + u"/" + version);
                    }
                }
                message.senderBadges = badges.join(u',');

                if (!message.cursor.isEmpty())
                {
                    page.nextCursor = message.cursor;
                }
                if (!message.id.isEmpty() && !message.text.isEmpty() &&
                    !message.senderLogin.isEmpty())
                {
                    page.messages.push_back(std::move(message));
                }
            }

            successCallback(std::move(page));
        })
        .onError([failureCallback](const NetworkResult &result) {
            auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                failureCallback(QString("Network Error: %1 | %2")
                                    .arg(result.formatError(), body.left(200)));
                return;
            }

            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModerationActionLogs(
    const QString &channelId, const QString &cursor, const QString &oauthToken,
    std::function<void(GqlModerationActionLogPage)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("after", cursor.isEmpty() ? QJsonValue() : cursor);

    makeTvPersistedGqlRequest(
        "ModActionsList",
        "f09041ba19fdd3d0ceb6e9b9163d5d903eddd625e1b156e8af6deb207ed3e77e",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto logs = payloadDataObject(root)
                                  .value("channel")
                                  .toObject()
                                  .value("moderationActionLogs")
                                  .toObject();
            if (logs.isEmpty())
            {
                failureCallback("Twitch did not return moderation action logs");
                return;
            }

            GqlModerationActionLogPage page;
            const auto edges = logs.value("edges").toArray();
            page.actions.reserve(edges.size());
            for (const auto &edgeValue : edges)
            {
                const auto edge = edgeValue.toObject();
                auto action = moderationActionFromNode(edge);
                if (action.cursor.isEmpty())
                {
                    action.cursor = edge.value("cursor").toString();
                }
                page.nextCursor = action.cursor;
                page.actions.push_back(std::move(action));
            }
            page.hasNextPage = logs.value("pageInfo")
                                   .toObject()
                                   .value("hasNextPage")
                                   .toBool(!page.nextCursor.isEmpty());

            successCallback(std::move(page));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unpinMessage(const QString &pinId,
                             const QString &oauthToken,
                             std::function<void()> successCallback,
                             std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", pinId);
    input.insert("reason", "UNPIN");
    variables.insert("input", input);

    makePersistedGqlRequest(
        "unpinChatMessage",
        "86409b9c86510bdc9f2c6d8e58fdc4041963c001de53577160ab649e03334511",
        variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("unpinChatMessage").toObject();
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to unpin message"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::updatePinnedMessage(const QString &pinId,
                                    std::optional<int> durationSeconds,
                                    const QString &oauthToken,
                                    std::function<void()> successCallback,
                                    std::function<void(const QString &)>
                                        failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", pinId);
    if (durationSeconds.has_value())
    {
        input.insert("durationSeconds", *durationSeconds);
    }
    variables.insert("input", input);

    makePersistedGqlRequest(
        "UpdatePinnedChatMessage",
        "e69a15a7aaa412857a066fc98f52e74ccefd5b82429c7d2bf747559ab78f6af9",
        variables, oauthToken)
        .onSuccess([successCallback,
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload = payloadDataObject(root)
                                     .value("updatePinnedChatMessage")
                                     .toObject();
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Failed to update pinned message"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getCurrentPin(const QString &channelId,
                              std::shared_ptr<TwitchAccount> account,
                              std::function<void(std::optional<TwitchChannel::PinnedMessage>)>
                                  successCallback,
                              std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("count", 1);

    makePersistedGqlRequest("GetPinnedChat", "2d099d4c9b6af80a07d8440140c4f3dbb04d516b35c401aab7ce8f60765308d5", variables, std::shared_ptr<TwitchAccount>(nullptr))
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const rapidjson::Value *dataVal = nullptr;

            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() && doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") && doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (dataVal)
            {
                const auto &data = *dataVal;
                if (data.HasMember("channel") && data["channel"].IsObject())
                {
                    const auto &channel = data["channel"];
                    if (channel.HasMember("pinnedChatMessages") && channel["pinnedChatMessages"].IsObject())
                    {
                        const auto &pinnedChatMessages = channel["pinnedChatMessages"];
                        if (pinnedChatMessages.HasMember("edges") && pinnedChatMessages["edges"].IsArray())
                        {
                            const auto &edges = pinnedChatMessages["edges"];
                            if (!edges.Empty() && edges[0].IsObject())
                            {
                                const auto &edge = edges[0];
                                if (edge.HasMember("node") && edge["node"].IsObject())
                                {
                                    const auto &node = edge["node"];
                                    TwitchChannel::PinnedMessage pin;
                                    rj::getSafe(node, "id", pin.pinId);

                                    if (node.HasMember("pinnedMessage") && node["pinnedMessage"].IsObject()) {
                                        const auto &pinnedMessage = node["pinnedMessage"];
                                        rj::getSafe(pinnedMessage, "id", pin.messageId);

                                        if (pinnedMessage.HasMember("content") && pinnedMessage["content"].IsObject()) {
                                            const auto &content = pinnedMessage["content"];
                                            rj::getSafe(content, "text", pin.text);
                                        }
                                        if (pinnedMessage.HasMember("sender") && pinnedMessage["sender"].IsObject()) {
                                            const auto &sender = pinnedMessage["sender"];
                                            rj::getSafe(sender, "displayName", pin.authorName);
                                            if (!rj::getSafe(sender, "login", pin.authorLogin) || pin.authorLogin.isEmpty()) {
                                                pin.authorLogin = pin.authorName;
                                            }
                                            rj::getSafe(sender, "id", pin.authorId);
                                            rj::getSafe(sender, "chatColor", pin.authorColor);

                                            if (sender.HasMember("displayBadges") && sender["displayBadges"].IsArray()) {
                                                QStringList badgeList;
                                                for (const auto &badge : sender["displayBadges"].GetArray()) {
                                                    if (badge.IsObject()) {
                                                        QString setID, version;
                                                        rj::getSafe(badge, "setID", setID);
                                                        rj::getSafe(badge, "version", version);
                                                        if (!setID.isEmpty()) {
                                                            badgeList << QString("%1/%2").arg(setID, version);
                                                        }
                                                    }
                                                }
                                                pin.authorBadges = badgeList.join(",");
                                            }
                                        }
                                    }

                                    QString endsAtStr;
                                    if (rj::getSafe(node, "endsAt", endsAtStr) && !endsAtStr.isEmpty()) {
                                        pin.endsAt = QDateTime::fromString(endsAtStr, Qt::ISODate);
                                    }

                                    QString updatedAtStr;
                                    if (rj::getSafe(node, "updatedAt", updatedAtStr) && !updatedAtStr.isEmpty()) {
                                        pin.pinnedAt = QDateTime::fromString(updatedAtStr, Qt::ISODate);
                                    } else {
                                        pin.pinnedAt = QDateTime::currentDateTimeUtc();
                                    }

                                    if (node.HasMember("pinnedBy") && node["pinnedBy"].IsObject()) {
                                        const auto &pinnedBy = node["pinnedBy"];
                                        rj::getSafe(pinnedBy, "displayName", pin.pinnerName);
                                        if (!rj::getSafe(pinnedBy, "login", pin.pinnerLogin) || pin.pinnerLogin.isEmpty()) {
                                            pin.pinnerLogin = pin.pinnerName;
                                        }
                                    }

                                    successCallback(pin);
                                    return;
                                }
                            }
                        }
                    }
                }
            }

            successCallback(std::nullopt);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::getActivePrediction(const QString &channelLogin,
                                    const QString &oauthToken,
                                    std::function<void(std::optional<TwitchChannel::PredictionEvent>)>
                                        successCallback,
                                    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin);

    static const char *predictionQuery = R"(
        query ChannelPointsPredictionContext($channelLogin: String!) {
            channel(name: $channelLogin) {
                id
                activePredictionEvents {
                    id
                    title
                    status
                    predictionWindowSeconds
                    createdAt
                    lockedAt
                    outcomes {
                        id
                        title
                        totalUsers
                        totalPoints
                        color
                        topPredictors {
                            points
                            user {
                                displayName
                                login
                            }
                        }
                    }
                    createdBy { ... on User { displayName login } }
                    lockedBy { ... on User { displayName login } }
                    endedBy { ... on User { displayName login } }
                }
                lockedPredictionEvents {
                    id
                    title
                    status
                    predictionWindowSeconds
                    createdAt
                    lockedAt
                    outcomes {
                        id
                        title
                        totalUsers
                        totalPoints
                        color
                        topPredictors {
                            points
                            user {
                                displayName
                                login
                            }
                        }
                    }
                    createdBy { ... on User { displayName login } }
                    lockedBy { ... on User { displayName login } }
                    endedBy { ... on User { displayName login } }
                }
            }
        }
    )";

    makeInlineGqlRequest(predictionQuery, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() && doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") && doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (!dataVal || !dataVal->HasMember("channel") || !(*dataVal)["channel"].IsObject())
            {
                successCallback(std::nullopt);
                return;
            }

            auto channel = (*dataVal)["channel"].GetObject();
            const rapidjson::Value *nodePtr = nullptr;

            if (channel.HasMember("activePredictionEvents") &&
                channel["activePredictionEvents"].IsArray())
            {
                const auto &activeEvents = channel["activePredictionEvents"];
                if (!activeEvents.Empty() && activeEvents[0].IsObject())
                {
                    nodePtr = &activeEvents[0];
                }
            }

            if (nodePtr == nullptr && channel.HasMember("lockedPredictionEvents") &&
                channel["lockedPredictionEvents"].IsArray())
            {
                const auto &lockedEvents = channel["lockedPredictionEvents"];
                if (!lockedEvents.Empty() && lockedEvents[0].IsObject())
                {
                    nodePtr = &lockedEvents[0];
                }
            }

            if (nodePtr == nullptr && channel.HasMember("predictionEvents") &&
                channel["predictionEvents"].IsObject())
            {
                const auto &predictionEvents = channel["predictionEvents"];
                if (predictionEvents.HasMember("edges") &&
                    predictionEvents["edges"].IsArray())
                {
                    const auto &edges = predictionEvents["edges"];
                    if (!edges.Empty() && edges[0].IsObject() &&
                        edges[0].HasMember("node") &&
                        edges[0]["node"].IsObject())
                    {
                        nodePtr = &edges[0]["node"];
                    }
                }
            }

            if (nodePtr == nullptr)
            {
                successCallback(std::nullopt);
                return;
            }

            const auto &node = *nodePtr;
            TwitchChannel::PredictionEvent prediction;
            rj::getSafe(node, "id", prediction.id);
            rj::getSafe(node, "title", prediction.title);
            rj::getSafe(node, "status", prediction.status);
            rj::getSafe(node, "predictionWindowSeconds", prediction.predictionWindowSeconds);

            if (prediction.status.compare("ACTIVE", Qt::CaseInsensitive) != 0 &&
                prediction.status.compare("LOCKED", Qt::CaseInsensitive) != 0)
            {
                successCallback(std::nullopt);
                return;
            }

            QString createdAtStr;
            if (rj::getSafe(node, "createdAt", createdAtStr) && !createdAtStr.isEmpty())
            {
                prediction.createdAt = QDateTime::fromString(createdAtStr, Qt::ISODate);
            }

            QString lockedAtStr;
            if (rj::getSafe(node, "lockedAt", lockedAtStr) && !lockedAtStr.isEmpty())
            {
                prediction.lockedAt = QDateTime::fromString(lockedAtStr, Qt::ISODate);
            }

            if (node.HasMember("createdBy") && node["createdBy"].IsObject())
            {
                rj::getSafe(node["createdBy"], "displayName", prediction.createdByName);
                if (prediction.createdByName.isEmpty())
                {
                    rj::getSafe(node["createdBy"], "login", prediction.createdByName);
                }
            }
            if (node.HasMember("lockedBy") && node["lockedBy"].IsObject())
            {
                rj::getSafe(node["lockedBy"], "displayName", prediction.lockedByName);
                if (prediction.lockedByName.isEmpty())
                {
                    rj::getSafe(node["lockedBy"], "login", prediction.lockedByName);
                }
            }
            if (node.HasMember("endedBy") && node["endedBy"].IsObject())
            {
                rj::getSafe(node["endedBy"], "displayName", prediction.endedByName);
                if (prediction.endedByName.isEmpty())
                {
                    rj::getSafe(node["endedBy"], "login", prediction.endedByName);
                }
            }

            if (node.HasMember("self") && node["self"].IsObject())
            {
                const auto &self = node["self"];
                rj::getSafe(self, "pointsParticipated", prediction.selfPoints);
                if (self.HasMember("outcome") && self["outcome"].IsObject())
                {
                    rj::getSafe(self["outcome"], "id", prediction.selfOutcomeId);
                }
            }

            if (node.HasMember("outcomes") && node["outcomes"].IsArray())
            {
                const auto &outcomesArr = node["outcomes"];
                int outcomeCount = outcomesArr.Size();
                for (int i = 0; i < outcomeCount; ++i)
                {
                    if (!outcomesArr[i].IsObject()) continue;
                    const auto &oObj = outcomesArr[i];
                    TwitchChannel::PredictionOutcome outcome;
                    rj::getSafe(oObj, "id", outcome.id);
                    rj::getSafe(oObj, "title", outcome.title);
                    rj::getSafe(oObj, "totalUsers", outcome.totalUsers);

                    if (oObj.HasMember("totalPoints") &&
                        oObj["totalPoints"].IsNumber())
                    {
                        outcome.totalPoints =
                            static_cast<qlonglong>(oObj["totalPoints"].GetInt64());
                    }

                    if (outcomeCount == 2)
                        outcome.color = (i == 0) ? "BLUE" : "PINK";
                    else if (outcomeCount == 3)
                        outcome.color = (i == 0) ? "BLUE" : (i == 1 ? "PINK" : "GREEN");
                    else
                        outcome.color = "BLUE";

                    if (oObj.HasMember("topPredictors") && oObj["topPredictors"].IsArray())
                    {
                        const auto &predictors = oObj["topPredictors"].GetArray();
                        if (predictors.Size() > 0 && predictors[0].IsObject())
                        {
                            const auto &top = predictors[0];
                            if (top.HasMember("points") && top["points"].IsNumber())
                            {
                                outcome.topPoints =
                                    static_cast<qlonglong>(top["points"].GetInt64());
                            }
                            if (top.HasMember("user") && top["user"].IsObject())
                            {
                                rj::getSafe(top["user"], "displayName",
                                            outcome.topPredictorName);
                                if (outcome.topPredictorName.isEmpty())
                                {
                                    rj::getSafe(top["user"], "login",
                                                outcome.topPredictorName);
                                }
                            }
                        }
                    }

                    prediction.outcomes.push_back(std::move(outcome));
                }
            }

            successCallback(prediction);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::makePrediction(const QString &eventID, const QString &outcomeID,
                               int points, const QString &oauthToken,
                               std::function<void()> successCallback,
                               std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("eventID", eventID);
    input.insert("outcomeID", outcomeID);
    input.insert("points", points);

    auto uuid = generateUuid();
    uuid.remove('{').remove('}').remove('-');
    input.insert("transactionID", uuid);

    variables.insert("input", input);

    makePersistedGqlRequest("MakePrediction", "b44682ecc88358817009f20e69d75081b1e58825bb40aa53d5dbadcc17c881d8", variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            QString errorMessage;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() && doc[0].HasMember("errors")) {
                hasErrors = true;
                const auto &errors = doc[0]["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            } else if (doc.IsObject() && doc.HasMember("errors")) {
                hasErrors = true;
                const auto &errors = doc["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            }

            if (!hasErrors && doc.IsArray() && doc.Size() > 0 &&
                doc[0].IsObject() && doc[0].HasMember("data"))
            {
                const auto &data = doc[0]["data"];
                if (!data.IsObject() || !data.HasMember("makePrediction"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["makePrediction"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                    }
                }
            }
            else if (!hasErrors && doc.IsObject() && doc.HasMember("data"))
            {
                const auto &data = doc["data"];
                if (!data.IsObject() || !data.HasMember("makePrediction"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["makePrediction"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                    }
                }
            }
            else if (!hasErrors)
            {
                hasErrors = true;
            }

            if (hasErrors)
            {
                failureCallback(errorMessage.isEmpty()
                                    ? "Twitch API Error: Failed to place prediction"
                                    : "Twitch API Error: " + errorMessage);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::createPredictionEvent(
    const QString &channelId, const QString &title,
    const QStringList &outcomes, int predictionWindowSeconds,
    const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("title", title);
    input.insert("predictionWindowSeconds", predictionWindowSeconds);

    QJsonArray outcomesArray;
    const int outcomeCount = outcomes.size();
    for (int i = 0; i < outcomeCount; ++i)
    {
        QJsonObject outcome;
        outcome.insert("title", outcomes.at(i));
        outcome.insert("color", predictionCreateOutcomeColor(i, outcomeCount));
        outcomesArray.append(outcome);
    }
    input.insert("outcomes", outcomesArray);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "createPredictionEvent",
        "92268878ac4abe722bcdcba85a4e43acdd7a99d86b05851759e1d8f385cc32ea",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            QString errorMessage;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("errors"))
            {
                hasErrors = true;
                const auto &errors = doc[0]["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            }
            else if (doc.IsObject() && doc.HasMember("errors"))
            {
                hasErrors = true;
                const auto &errors = doc["errors"];
                if (errors.IsArray() && errors.Size() > 0 && errors[0].IsObject() &&
                    errors[0].HasMember("message") && errors[0]["message"].IsString())
                {
                    errorMessage = QString::fromUtf8(errors[0]["message"].GetString());
                }
            }

            if (!hasErrors && doc.IsArray() && doc.Size() > 0 &&
                doc[0].IsObject() && doc[0].HasMember("data"))
            {
                const auto &data = doc[0]["data"];
                if (!data.IsObject() || !data.HasMember("createPredictionEvent"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["createPredictionEvent"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                        else if (payloadError.HasMember("message") &&
                                 payloadError["message"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["message"].GetString());
                        }
                    }
                    else if (!payload.HasMember("predictionEvent") ||
                             !payload["predictionEvent"].IsObject() ||
                             !payload["predictionEvent"].HasMember("id") ||
                             !payload["predictionEvent"]["id"].IsString() ||
                             QString::fromUtf8(payload["predictionEvent"]["id"]
                                                    .GetString())
                                 .isEmpty())
                    {
                        hasErrors = true;
                    }
                }
            }
            else if (!hasErrors && doc.IsObject() && doc.HasMember("data"))
            {
                const auto &data = doc["data"];
                if (!data.IsObject() || !data.HasMember("createPredictionEvent"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["createPredictionEvent"];
                    if (!payload.IsObject())
                    {
                        hasErrors = true;
                    }
                    else if (payload.HasMember("error") &&
                             payload["error"].IsObject() &&
                             !payload["error"].IsNull())
                    {
                        hasErrors = true;
                        const auto &payloadError = payload["error"];
                        if (payloadError.HasMember("code") &&
                            payloadError["code"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["code"].GetString());
                        }
                        else if (payloadError.HasMember("message") &&
                                 payloadError["message"].IsString())
                        {
                            errorMessage = QString::fromUtf8(
                                payloadError["message"].GetString());
                        }
                    }
                    else if (!payload.HasMember("predictionEvent") ||
                             !payload["predictionEvent"].IsObject() ||
                             !payload["predictionEvent"].HasMember("id") ||
                             !payload["predictionEvent"]["id"].IsString() ||
                             QString::fromUtf8(payload["predictionEvent"]["id"]
                                                    .GetString())
                                 .isEmpty())
                    {
                        hasErrors = true;
                    }
                }
            }
            else if (!hasErrors)
            {
                hasErrors = true;
            }

            if (hasErrors)
            {
                failureCallback(errorMessage.isEmpty()
                                    ? "Twitch API Error: Failed to create prediction"
                                    : "Twitch API Error: " + errorMessage);
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::getPredictionTemplates(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(QVector<PredictionTemplate>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("count", 5);
    variables.insert("channelLogin", channelLogin);

    makePersistedGqlRequest(
        "ChannelPointsPredictionContext",
        "beb846598256b75bd7c1fe54a80431335996153e358ca9c7837ce7bb83d7d383",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            if (const auto error = extractFirstGqlErrorMessage(doc);
                !error.isEmpty())
            {
                failureCallback(error);
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") &&
                     doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (dataVal == nullptr || !dataVal->HasMember("community") ||
                !(*dataVal)["community"].IsObject())
            {
                failureCallback("Missing prediction history");
                return;
            }

            const auto &community = (*dataVal)["community"];
            if (!community.HasMember("channel") ||
                !community["channel"].IsObject())
            {
                failureCallback("Missing prediction channel");
                return;
            }

            const auto &channel = community["channel"];
            if (!channel.HasMember("resolvedPredictionEvents") ||
                !channel["resolvedPredictionEvents"].IsObject())
            {
                successCallback({});
                return;
            }

            const auto &connection = channel["resolvedPredictionEvents"];
            if (!connection.HasMember("edges") ||
                !connection["edges"].IsArray())
            {
                successCallback({});
                return;
            }

            QVector<PredictionTemplate> templates;
            templates.reserve(5);

            for (const auto &edge : connection["edges"].GetArray())
            {
                if (!edge.IsObject() || !edge.HasMember("node") ||
                    !edge["node"].IsObject())
                {
                    continue;
                }

                const auto &node = edge["node"];
                PredictionTemplate predictionTemplate;
                rj::getSafe(node, "title", predictionTemplate.title);
                rj::getSafe(node, "predictionWindowSeconds",
                            predictionTemplate.durationSeconds);

                if (predictionTemplate.title.trimmed().isEmpty() ||
                    !node.HasMember("outcomes") ||
                    !node["outcomes"].IsArray())
                {
                    continue;
                }

                for (const auto &outcome : node["outcomes"].GetArray())
                {
                    QString title;
                    if (outcome.IsObject() &&
                        rj::getSafe(outcome, "title", title) &&
                        !title.trimmed().isEmpty())
                    {
                        predictionTemplate.outcomes.push_back(title.trimmed());
                    }
                }

                if (predictionTemplate.outcomes.size() < 2)
                {
                    continue;
                }

                templates.push_back(std::move(predictionTemplate));
                if (templates.size() >= 5)
                {
                    break;
                }
            }

            successCallback(std::move(templates));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " +
                            QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::lockPrediction(
    const QString &eventId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", eventId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "LockPrediction",
        "1f2b1eb44af35f055308e78ffbe81c2f958408f9b32d076a759a84ab213285d4",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("errors"))
            {
                hasErrors = true;
            }
            else if (doc.IsObject() && doc.HasMember("errors"))
            {
                hasErrors = true;
            }

            if (hasErrors)
            {
                failureCallback(
                    "Twitch API Error: Failed to lock prediction");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::cancelPrediction(
    const QString &eventId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("id", eventId);
    variables.insert("input", input);

    makePersistedGqlRequest(
            "DeletePrediction",
            "35d375614e426624456ee7be4a2e0fbc0a410c0a91c21f6044cb3cd5c38c4e4d",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("errors"))
            {
                hasErrors = true;
            }
            else if (doc.IsObject() && doc.HasMember("errors"))
            {
                hasErrors = true;
            }

            if (!hasErrors && doc.IsArray() && doc.Size() > 0 &&
                doc[0].IsObject() && doc[0].HasMember("data"))
            {
                const auto &data = doc[0]["data"];
                if (!data.IsObject() || !data.HasMember("cancelPredictionEvent"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["cancelPredictionEvent"];
                    if (!payload.IsObject() || !payload.HasMember("error") ||
                        !payload["error"].IsNull())
                    {
                        hasErrors = true;
                    }
                }
            }
            else if (!hasErrors && doc.IsObject() && doc.HasMember("data"))
            {
                const auto &data = doc["data"];
                if (!data.IsObject() || !data.HasMember("cancelPredictionEvent"))
                {
                    hasErrors = true;
                }
                else
                {
                    const auto &payload = data["cancelPredictionEvent"];
                    if (!payload.IsObject() || !payload.HasMember("error") ||
                        !payload["error"].IsNull())
                    {
                        hasErrors = true;
                    }
                }
            }

            if (hasErrors)
            {
                failureCallback(
                    "Twitch API Error: Failed to delete prediction");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::resolvePrediction(
    const QString &eventId, const QString &outcomeId,
    const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("eventID", eventId);
    input.insert("outcomeID", outcomeId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "ResolvePrediction",
        "10c803ec11bb8c2957d66bc6a47349dc3c5f51d694585b5ebc37ba656da413c1",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            bool hasErrors = false;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("errors"))
            {
                hasErrors = true;
            }
            else if (doc.IsObject() && doc.HasMember("errors"))
            {
                hasErrors = true;
            }

            if (hasErrors)
            {
                failureCallback(
                    "Twitch API Error: Failed to resolve prediction");
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(
                "Network Error: " +
                QString::number(result.status().value_or(0)));
        })
        .execute();
}

void TwitchGql::createPollEvent(
    const QString &channelId, const QString &title, const QStringList &choices,
    int durationSeconds, std::optional<int> pointsPerVote,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("title", title);
    input.insert("durationSeconds", durationSeconds);
    input.insert("ownedBy", channelId);
    input.insert("multichoiceEnabled", true);
    input.insert("isCommunityPointsVotingEnabled", pointsPerVote.has_value());
    input.insert("communityPointsCost", pointsPerVote.value_or(0));

    QJsonArray choicesArray;
    for (const auto &choiceTitle : choices)
    {
        QJsonObject choice;
        choice.insert("title", choiceTitle);
        choicesArray.append(choice);
    }
    input.insert("choices", choicesArray);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "CreatePoll",
        "4b1461a13fe166a59044961db192747d606f71a89abc3bfdecf79fe862d205cf",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("createPoll").toObject();
            const auto payloadError = payload.value("error").toObject();
            if (!payloadError.isEmpty())
            {
                auto message = payloadError.value("message").toString();
                if (message.isEmpty())
                {
                    message = payloadError.value("code").toString();
                }
                failureCallback("Twitch API Error: " +
                                (message.isEmpty()
                                     ? QString("Failed to create poll")
                                     : message));
                return;
            }

            if (payload.value("poll").toObject().value("id").toString().isEmpty())
            {
                failureCallback("Twitch API Error: Failed to create poll");
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::terminatePoll(
    const QString &pollId, const QString &currentUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    sendTerminatePollRequest(pollId, currentUserId, oauthToken,
                             std::move(successCallback),
                             std::move(failureCallback));
}

void TwitchGql::archivePoll(
    const QString &pollId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("pollID", pollId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "ArchivePoll",
        "444ead3d68d94601cb66519e36c9f6c6fd9ba8b827a4299b8ed3604e57918d92",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("archivePoll").toObject();
            if (payload.value("poll").toObject().value("id").toString().isEmpty())
            {
                failureCallback("Twitch API Error: Failed to delete poll");
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::addChannelBlockedTerm(
    const QString &channelId, const QString &phrase, const QString &oauthToken,
    std::function<void(GqlAddBlockedTermResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("phrase", phrase);
    input.insert("phrases", QJsonArray{phrase});
    input.insert("isModEditable", true);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "AddChannelBlockedTerm",
        "10f4c5c8dd6817c21058040b50181040e91e894ca324b14beda6b5f5e429aa02",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("addChannelBlockedTerm").toObject();
            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Twitch rejected the blocked term"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            GqlAddBlockedTermResult addResult;
            addResult.term =
                blockedTermFromObject(payload.value("term").toObject());
            addResult.wasRemovedFromPermittedList =
                payload.value("wasRemovedFromPermittedList").toBool(false);

            if (addResult.term.id.isEmpty() || addResult.term.phrase.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to add blocked term");
                return;
            }

            successCallback(std::move(addResult));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelBlockedTerms(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlBlockedTerm>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);

    makePersistedGqlRequest(
        "BlockedTerms",
        "022dc6d166de51129700aa03482dca9e5fffc3a7045ba7f1deeaa3046a39577f",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            const auto blockedTerms = channel.value("blockedTerms").toObject();
            if (channel.isEmpty() || blockedTerms.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to fetch blocked terms");
                return;
            }

            QVector<GqlBlockedTerm> terms;
            const auto edges = blockedTerms.value("edges").toArray();
            terms.reserve(edges.size());
            for (const auto &edgeValue : edges)
            {
                const auto node =
                    edgeValue.toObject().value("node").toObject();
                auto term = blockedTermFromObject(node);
                if (!term.id.isEmpty() && !term.phrase.isEmpty())
                {
                    terms.push_back(std::move(term));
                }
            }

            const auto nodes = blockedTerms.value("nodes").toArray();
            terms.reserve(terms.size() + nodes.size());
            for (const auto &nodeValue : nodes)
            {
                auto term = blockedTermFromObject(nodeValue.toObject());
                if (!term.id.isEmpty() && !term.phrase.isEmpty())
                {
                    terms.push_back(std::move(term));
                }
            }

            successCallback(std::move(terms));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelSelfData(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(GqlChannelSelfData)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin.trimmed().toLower());

    makePersistedGqlRequest(
        "Chat_ChannelData",
        "863fda39ddc5ebac7453856eb00af2a587e27f48a2e521e9c01820c3c8c2c18a",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto channel =
                payloadDataObject(root).value("channel").toObject();
            const auto self = channel.value("self").toObject();
            if (channel.isEmpty() || self.isEmpty())
            {
                failureCallback("Twitch API Error: Missing channel self data");
                return;
            }

            successCallback(channelSelfDataFromObject(self));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::deleteChannelBlockedTerm(
    const QString &channelId, const QString &termId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("id", termId);
    input.insert("channelID", channelId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "DeleteChannelBlockedTerm",
        "bdfacf843eb536eef2720110cf73a4540506833b17a3f15313e461e57165c813",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("deleteChannelBlockedTermByID")
                    .toObject();
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to remove blocked term");
                return;
            }

            const auto payloadError = gqlPayloadErrorMessage(
                payload.value("error"),
                QStringLiteral("Twitch rejected the blocked term removal"));
            if (!payloadError.isEmpty())
            {
                failureCallback("Twitch API Error: " + payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::grantVIP(const QString &channelId, const QString &targetLogin,
                         const QString &oauthToken,
                         std::function<void()> successCallback,
                         std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "VIPUser",
        "e8c397f1ed8b1fdbaa201eedac92dd189ecfb2d828985ec159d4ae77f9920170",
        "grantVIP", "granteeLogin", channelId, targetLogin, oauthToken,
        "Failed to add VIP", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::revokeVIP(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "UnVIPUser",
        "2ce4fcdf6667d013aa1f820010e699d1d4abdda55e26539ecf4efba8aff2d661",
        "revokeVIP", "revokeeLogin", channelId, targetLogin, oauthToken,
        "Failed to remove VIP", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::modUser(const QString &channelId, const QString &targetLogin,
                        const QString &oauthToken,
                        std::function<void()> successCallback,
                        std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "ModUser",
        "46da4ec4229593fe4b1bce911c75625c299638e228262ff621f80d5067695a8a",
        "modUser", "targetLogin", channelId, targetLogin, oauthToken,
        "Failed to add moderator", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::unmodUser(const QString &channelId, const QString &targetLogin,
                          const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback)
{
    runRoleMutation(
        "UnmodUser",
        "1ed42ccb3bc3a6e79f51e954a2df233827f94491fbbb9bd05b22b1aaaf219b8b",
        "unmodUser", "targetLogin", channelId, targetLogin, oauthToken,
        "Failed to remove moderator", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::assignLeadModerator(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "AssignChannelRole",
        "2d373c90d0d0e6d4fe771bc6136febe6a148eb3d5700d2a0575883a043fbd581",
        "assignChannelRole", "targetUserID", channelId, targetUserId,
        oauthToken, "Failed to add lead moderator", "lead_mod", "isAssigned",
        std::move(successCallback), std::move(failureCallback));
}

void TwitchGql::unassignLeadModerator(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "UnassignChannelRole",
        "5edbf17877acdb91e65243b5148cfd15b98adc6d8f980492dcde9a7f2e8255e2",
        "unassignChannelRole", "targetUserID", channelId, targetUserId,
        oauthToken, "Failed to remove lead moderator", "lead_mod",
        "isUnassigned", std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::addEditorUser(
    const QString &channelId, const QString &targetLogin,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "AddEditorUser",
        "3b52bf904ff9ce1b000ac2358080f538fbd1972c1869804f0d0f345d1a56676c",
        "addEditor", "targetUserLogin", channelId, targetLogin, oauthToken,
        "Failed to add editor", {}, {}, std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::removeEditorUser(
    const QString &channelId, const QString &targetLogin,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    runTvRoleMutation(
        "RemoveEditorUser",
        "4699d38183050854dba547d07e340e72bf1f04578f1037a38a1189fa1827790f",
        "removeEditor", "targetUserLogin", channelId, targetLogin, oauthToken,
        "Failed to remove editor", {}, {}, std::move(successCallback),
        std::move(failureCallback));
}

void TwitchGql::getRaidChannelIDs(
    const QString &sourceLogin, const QString &targetLogin,
    const QString &oauthToken,
    std::function<void(RaidChannelIDs)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("sourceLogin", normalizedRaidLogin(sourceLogin));
    variables.insert("targetLogin", normalizedRaidLogin(targetLogin));

    static const char *raidLookupQuery = R"(
        query MoltorinoRaidLookup($sourceLogin: String!, $targetLogin: String!) {
            source: user(login: $sourceLogin) {
                id
                login
                displayName
            }
            target: user(login: $targetLogin) {
                id
                login
                displayName
            }
        }
    )";

    makeInlineGqlRequest(raidLookupQuery, variables, oauthToken)
        .onSuccess([successCallback, failureCallback, sourceLogin,
                    targetLogin](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto sourceObject = data.value("source").toObject();
            const auto targetObject = data.value("target").toObject();

            RaidChannelIDs ids;
            ids.sourceId = raidUserIdFromObject(sourceObject);
            ids.targetId = raidUserIdFromObject(targetObject);

            ids.targetLogin =
                raidObjectString(targetObject,
                                 {
                                     QStringLiteral("login"),
                                     QStringLiteral("name"),
                                 });
            ids.targetDisplayName =
                raidObjectString(targetObject,
                                 {
                                     QStringLiteral("displayName"),
                                     QStringLiteral("display_name"),
                                     QStringLiteral("login"),
                                     QStringLiteral("name"),
                                 });
            if (ids.targetLogin.isEmpty())
            {
                ids.targetLogin = normalizedRaidLogin(targetLogin);
            }
            if (ids.targetDisplayName.isEmpty())
            {
                ids.targetDisplayName = ids.targetLogin;
            }

            if (ids.sourceId.isEmpty())
            {
                failureCallback(
                    QString("Could not resolve the broadcaster account for #%1.")
                        .arg(normalizedRaidLogin(sourceLogin)));
                return;
            }
            if (ids.targetId.isEmpty())
            {
                failureCallback(QString("Could not look up user: %1. Check the username or log in again.")
                                    .arg(normalizedRaidLogin(targetLogin)));
                return;
            }

            successCallback(std::move(ids));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::createRaid(
    const QString &sourceId, const QString &targetId, const QString &oauthToken,
    std::function<void(const QString &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("sourceID", sourceId);
    input.insert("targetID", targetId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "chatCreateRaid",
        "f4fc7ac482599d81dfb6aa37100923c8c9edeea9ca2be854102a6339197f840a",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(raidFailureMessage(gqlError));
                return;
            }

            const auto payload =
                payloadDataObject(root).value("createRaid").toObject();
            const auto payloadError = raidErrorMessage(payload.value("error"));
            if (!payloadError.isEmpty())
            {
                failureCallback(raidFailureMessage(payloadError));
                return;
            }

            const auto raidId =
                payload.value("raid").toObject().value("id").toString();
            if (raidId.isEmpty())
            {
                failureCallback("Failed to start raid");
                return;
            }

            successCallback(raidId);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendRaidNow(
    const QString &sourceId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("sourceID", sourceId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "GoRaid",
        "878ca88bed0c5a5f0687ad07562cffc0bf6a3136f15e5015c0f5f5f7f367f70a",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(raidFailureMessage(gqlError));
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::cancelRaidGql(
    const QString &sourceId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("sourceID", sourceId);

    QJsonObject variables;
    variables.insert("input", input);

    makePersistedGqlRequest(
        "CancelRaid",
        "42a2a699ac85256d72fff2471c75803f7ffbc767ba790725de5ad5d6e0163648",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(raidFailureMessage(gqlError));
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::voteInPoll(const QString &pollId, const QString &choiceId,
                           const QString &userId, int extraVotes,
                           std::optional<int> pointsPerVote,
                           const QString &oauthToken,
                           std::function<void()> successCallback,
                           std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("pollID", pollId);
    input.insert("choiceID", choiceId);
    input.insert("userID", userId);
    input.insert("voteID",
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (extraVotes > 0)
    {
        if (!pointsPerVote.has_value() || *pointsPerVote <= 0)
        {
            failureCallback(
                "Twitch API Error: Paid voting is enabled, but the point cost is unknown. Refresh the poll and try again.");
            return;
        }

        QJsonObject tokens;
        tokens.insert("channelPoints", extraVotes * *pointsPerVote);
        input.insert("tokens", tokens);
    }
    variables.insert("input", input);

    static const char *voteInPollMutation = R"(
        mutation VoteInPoll($input: VoteInPollInput!) {
            voteInPoll(input: $input) {
                error {
                    code
                }
            }
        }
    )";

    makeInlineGqlRequest(voteInPollMutation, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto payload =
                payloadDataObject(root).value("voteInPoll").toObject();
            const auto payloadError = payload.value("error").toObject();
            if (!payloadError.isEmpty())
            {
                const auto message = payloadError.value("code").toString();
                failureCallback("Twitch API Error: " +
                                (message.isEmpty() ? QString("Failed to vote")
                                                   : message));
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelPoints(const QString &channelLogin,
                                 const QString &oauthToken,
                                 std::function<void(qint64)> successCallback,
                                 std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin);

    static const char *channelPointsQuery = R"(
        query ChannelPointsContext($channelLogin: String!) {
            channel(name: $channelLogin) {
                self {
                    communityPoints {
                        balance
                    }
                }
            }
        }
    )";

    makeTvInlineGqlRequest(channelPointsQuery, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() && doc[0].HasMember("data") && doc[0]["data"].IsObject())
                dataVal = &doc[0]["data"];
            else if (doc.IsObject() && doc.HasMember("data") && doc["data"].IsObject())
                dataVal = &doc["data"];

            if (dataVal && dataVal->HasMember("community") && (*dataVal)["community"].IsObject())
            {
                const auto &community = (*dataVal)["community"];
                if (community.HasMember("channel") && community["channel"].IsObject())
                {
                    const auto &channel = community["channel"];
                    if (channel.HasMember("self") && channel["self"].IsObject())
                    {
                        const auto &self = channel["self"];
                        if (self.HasMember("communityPoints") && self["communityPoints"].IsObject())
                        {
                            const auto &cp = self["communityPoints"];
                            qint64 points = 0;
                            if (readInteger(cp, "balance", points))
                            {
                                successCallback(points);
                                return;
                            }
                        }
                    }
                }
            }
            if (dataVal && dataVal->HasMember("currentUser") && (*dataVal)["currentUser"].IsObject())
            {
                const auto &currentUser = (*dataVal)["currentUser"];
                if (currentUser.HasMember("communityPoints") && currentUser["communityPoints"].IsObject())
                {
                    const auto &cp = currentUser["communityPoints"];
                    qint64 points = 0;
                    if (readInteger(cp, "balance", points))
                    {
                        successCallback(points);
                        return;
                    }
                }
            }
            if (dataVal && dataVal->HasMember("channel") && (*dataVal)["channel"].IsObject())
            {
                const auto &channel = (*dataVal)["channel"];
                if (channel.HasMember("self") && channel["self"].IsObject())
                {
                    const auto &self = channel["self"];
                    if (self.HasMember("communityPoints") && self["communityPoints"].IsObject())
                    {
                        const auto &cp = self["communityPoints"];
                        qint64 points = 0;
                        if (readInteger(cp, "balance", points))
                        {
                            successCallback(points);
                            return;
                        }
                    }
                }
            }
            failureCallback("Could not parse channel points balance");
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS
void TwitchGql::getChannelPointRewards(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(GqlChannelPointRewards)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelLogin", channelLogin);
    variables.insert("includeGoalTypes",
                     QJsonArray{QStringLiteral("CREATOR"),
                                QStringLiteral("BOOST")});

    makeTvPersistedGqlRequest(
        "ChannelPointsContext",
        "7fe050e3761eb2cf258d70ee1a21cbd76fa8cf3d7e7b12fc437e7029d446b5e3",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            const auto community = data.value("community").toObject();
            const auto channel = community.value("channel").toObject();
            const auto settings =
                channel.value("communityPointsSettings").toObject();
            const auto self = channel.value("self").toObject();
            const auto points =
                self.value("communityPoints").toObject();

            if (community.isEmpty() || channel.isEmpty() || settings.isEmpty())
            {
                failureCallback("Channel point rewards are unavailable");
                return;
            }

            GqlChannelPointRewards rewards;
            rewards.channelId = community.value("id").toString();
            rewards.channelDisplayName =
                community.value("displayName").toString();
            rewards.balance = jsonIntegerValue(points.value("balance"));

            for (const auto &value : settings.value("customRewards").toArray())
            {
                const auto reward =
                    channelPointRewardFromObject(value.toObject(), false);
                if (reward.pricingType != "POINTS" || reward.cost <= 0)
                {
                    continue;
                }
                rewards.rewards.push_back(reward);
            }

            for (const auto &value :
                 settings.value("automaticRewards").toArray())
            {
                const auto reward =
                    channelPointRewardFromObject(value.toObject(), true);
                if (reward.pricingType != "POINTS" || reward.cost <= 0)
                {
                    continue;
                }
                rewards.rewards.push_back(reward);
            }

            successCallback(std::move(rewards));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::redeemCustomReward(
    const QString &channelId, const GqlChannelPointReward &reward,
    const QString &textInput, const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", reward.cost);
    input.insert("pricingType", "POINTS");
    input.insert("rewardID", reward.id);
    input.insert("title", reward.title);
    input.insert("transactionID", makeTransactionId());
    input.insert("prompt", reward.prompt.trimmed().isEmpty()
                               ? QJsonValue(QJsonValue::Null)
                               : QJsonValue(reward.prompt));
    if (!textInput.trimmed().isEmpty())
    {
        input.insert("textInput", textInput);
    }

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "RedeemCustomReward",
        "d56249a7adb4978898ea3412e196688d4ac3cea1c0c2dfd65561d229ea5dcc42",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("redeemCommunityPointsCustomReward")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to redeem reward",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to redeem reward");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendHighlightedChatMessage(
    const QString &channelId, int cost, const QString &message,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", cost);
    input.insert("message", message);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "SendHighlightedChatMessage",
        "bb187d763156dc5c25c6457e1b32da6c5033cb7504854e6d33a8b876d10444b6",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("sendHighlightedChatMessage")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to send highlighted message",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to send highlighted message");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::sendSubOnlyBypassMessage(
    const QString &channelId, int cost, const QString &message,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", cost);
    input.insert("message", message);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    static const char *query = R"(
        mutation SendSubsOnlyMessage($input: SendChatMessageThroughSubscriberModeInput!) {
            sendChatMessageThroughSubscriberMode(input: $input) {
                balance
                error {
                    code
                }
            }
        }
    )";

    makeTvInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("sendChatMessageThroughSubscriberMode")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload,
                                        "Failed to send sub-only message",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback(
                    "Twitch API Error: Failed to send sub-only message");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unlockRandomSubscriberEmote(
    const QString &channelId, int cost, const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("cost", cost);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "UnlockRandomSubscriberEmote",
        "f548e89966b21d0094f3dc35233232eb6ec76d63e02594c8a494407712a85350",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("unlockRandomSubscriberEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to unlock emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to unlock emote");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unlockChosenSubscriberEmote(
    const QString &channelId, const QString &emoteId, int cost,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("emoteID", emoteId);
    input.insert("cost", cost);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    static const char *query = R"(
        mutation UnlockChosenSubscriberEmote($input: UnlockChosenSubscriberEmoteInput!) {
            unlockChosenSubscriberEmote(input: $input) {
                balance
                error {
                    code
                }
            }
        }
    )";

    makeTvInlineGqlRequest(query, variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("unlockChosenSubscriberEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to unlock emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to unlock emote");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::unlockModifiedSubscriberEmote(
    const QString &channelId, const QString &modifiedEmoteId, int cost,
    const QString &oauthToken,
    std::function<void(GqlChannelPointRedeemResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject input;
    input.insert("channelID", channelId);
    input.insert("emoteID", modifiedEmoteId);
    input.insert("cost", cost);
    input.insert("transactionID", makeTransactionId());

    QJsonObject variables;
    variables.insert("input", input);

    makeTvPersistedGqlRequest(
        "UnlockModifiedEmote",
        "30e8cc29b1d6d96809f5e35f5e7a550ae8bf5d26966a9637d919477ffd0bfc52",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto payload =
                payloadDataObject(root)
                    .value("unlockChosenModifiedSubscriberEmote")
                    .toObject();
            if (rejectGqlOrPayloadError(root, payload, "Failed to unlock emote",
                                        failureCallback))
            {
                return;
            }
            if (payload.isEmpty())
            {
                failureCallback("Twitch API Error: Failed to unlock emote");
                return;
            }

            successCallback(redeemResultFromPayload(payload));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getAvailableChannelPointEmotes(
    const QString &channelId, const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelOwnerID", channelId);

    makeTvPersistedGqlRequest(
        "EmotePicker_EmotePicker_UserSubscriptionProducts",
        "511bebfb513d0127d24a7fe49aa2b7717306a611e1f4269a93e0cc76e8a65a81",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            QVector<GqlChannelPointEmote> emotes;
            QSet<QString> seenIds;

            const auto user = payloadDataObject(root).value("user").toObject();
            const auto ownerLogin = user.value("login").toString();
            const auto ownerDisplayName = user.value("displayName").toString();
            for (const auto &productValue :
                 user.value("subscriptionProducts").toArray())
            {
                const auto product = productValue.toObject();
                for (const auto &emoteValue :
                     product.value("emotes").toArray())
                {
                    const auto emoteObj = emoteValue.toObject();
                    GqlChannelPointEmote emote;
                    emote.id = emoteObj.value("id").toString();
                    emote.token = emoteObj.value("token").toString();
                    emote.type = emoteObj.value("assetType")
                                     .toString(emoteObj.value("type")
                                                   .toString());
                    emote.ownerLogin = ownerLogin;
                    emote.ownerDisplayName = ownerDisplayName;
                    if (emote.id.isEmpty() || emote.token.isEmpty() ||
                        seenIds.contains(emote.id))
                    {
                        continue;
                    }
                    seenIds.insert(emote.id);
                    emotes.push_back(std::move(emote));
                }
            }

            successCallback(std::move(emotes));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModifiableChannelPointEmotes(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmote>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject contextVariables;
    contextVariables.insert("channelLogin", channelLogin);
    contextVariables.insert("includeGoalTypes",
                            QJsonArray{QStringLiteral("CREATOR"),
                                       QStringLiteral("BOOST")});

    QJsonArray payloadArray;
    payloadArray.append(persistedPayload(
        "ModifyEmoteOwnedEmotes", QJsonObject{},
        "e882551bf6a6abf14a1ec2deac4fe9a0af22f89f863818f7228da98d6b849cb4"));
    payloadArray.append(persistedPayload(
        "ChannelPointsContext", contextVariables,
        "7fe050e3761eb2cf258d70ee1a21cbd76fa8cf3d7e7b12fc437e7029d446b5e3"));

    makeTvPersistedGqlBatchRequest(payloadArray, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            QSet<QString> seenOwnedIds;
            const auto currentUser =
                payloadDataObjectForOperation(root, "ModifyEmoteOwnedEmotes")
                    .value("currentUser")
                    .toObject();
            for (const auto &setValue : currentUser.value("emoteSets").toArray())
            {
                const auto set = setValue.toObject();
                for (const auto &emoteValue : set.value("emotes").toArray())
                {
                    const auto emoteObj = emoteValue.toObject();
                    const auto id = emoteObj.value("id").toString();
                    if (!id.isEmpty())
                    {
                        seenOwnedIds.insert(id);
                    }
                    for (const auto &modifierValue :
                         emoteObj.value("modifiers").toArray())
                    {
                        const auto code = modifierValue.toObject()
                                              .value("code")
                                              .toString();
                        if (!id.isEmpty() && !code.isEmpty())
                        {
                            seenOwnedIds.insert(id + "_" + code);
                        }
                    }
                }
            }

            QVector<GqlChannelPointEmote> emotes;
            QSet<QString> seenVariantIds;
            const auto data =
                payloadDataObjectForOperation(root, "ChannelPointsContext");
            const auto community = data.value("community").toObject();
            const auto channel = community.value("channel").toObject();
            const auto settings =
                channel.value("communityPointsSettings").toObject();
            const auto ownerLogin = community.value("login").toString();
            const auto ownerDisplayName =
                community.value("displayName").toString();

            for (const auto &variantValue :
                 settings.value("emoteVariants").toArray())
            {
                const auto variant = variantValue.toObject();
                const auto baseObj = variant.value("emote").toObject();
                GqlChannelPointEmote emote;
                emote.id = baseObj.value("id").toString(
                    variant.value("id").toString());
                emote.token = baseObj.value("token").toString();
                emote.type = QStringLiteral("CHANNEL_POINTS_VARIANT");
                emote.ownerLogin = ownerLogin;
                emote.ownerDisplayName = ownerDisplayName;

                if (emote.id.isEmpty() || emote.token.isEmpty() ||
                    seenVariantIds.contains(emote.id))
                {
                    continue;
                }

                const auto isUnlockable =
                    variant.value("isUnlockable").toBool(false);
                if (!isUnlockable && !seenOwnedIds.contains(emote.id))
                {
                    continue;
                }

                for (const auto &modificationValue :
                     variant.value("modifications").toArray())
                {
                    const auto modification = modificationValue.toObject();
                    const auto modifierId = modification.value("modifier")
                                                .toObject()
                                                .value("id")
                                                .toString();
                    const auto modifiedEmote =
                        modification.value("emote").toObject();

                    GqlChannelPointEmoteModification parsedModification;
                    parsedModification.modifierId = modifierId;
                    parsedModification.emoteId =
                        modifiedEmote.value("id").toString(
                            modification.value("id").toString());
                    parsedModification.emoteToken =
                        modifiedEmote.value("token").toString();

                    if (parsedModification.modifierId.isEmpty() ||
                        parsedModification.emoteId.isEmpty() ||
                        seenOwnedIds.contains(parsedModification.emoteId))
                    {
                        continue;
                    }
                    if (parsedModification.emoteToken.isEmpty())
                    {
                        parsedModification.emoteToken = emote.token;
                    }

                    emote.modifications.push_back(std::move(parsedModification));
                }

                if (emote.modifications.isEmpty())
                {
                    continue;
                }

                seenVariantIds.insert(emote.id);
                emotes.push_back(std::move(emote));
            }

            successCallback(std::move(emotes));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getChannelPointEmoteModifiers(
    const QString &oauthToken,
    std::function<void(QVector<GqlChannelPointEmoteModifier>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    makeTvPersistedGqlRequest(
        "ChannelPointsGlobalContext",
        "d3fa3a96e78a3e62bdd3ef3c4effafeda52442906cec41a9440e609a388679e2",
        {}, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }
            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            QVector<GqlChannelPointEmoteModifier> modifiers;
            for (const auto &value :
                 payloadDataObject(root).value("emoteModifiers").toArray())
            {
                const auto obj = value.toObject();
                GqlChannelPointEmoteModifier modifier;
                modifier.id = obj.value("id").toString();
                modifier.title = obj.value("title").toString();
                if (!modifier.id.isEmpty())
                {
                    modifiers.push_back(std::move(modifier));
                }
            }

            successCallback(std::move(modifiers));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}
#endif

void TwitchGql::getChatWarningStatus(
    const QString &channelId, const QString &targetUserId,
    const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::ChatWarning>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("channelID", channelId);
    variables.insert("targetUserID", targetUserId);

    makePersistedGqlRequest(
        "ChatModeratorStrikeStatus",
        "7f50f7190a840cd9fe9a91398f34ebb690eeba7cb28bce70e4cbf7ed1d06f268",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback, channelId](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            const auto status =
                payloadDataObject(root)
                    .value("chatModeratorStrikeStatus")
                    .toObject();
            const auto warningDetails =
                status.value("warningDetails").toObject();
            if (warningDetails.isEmpty())
            {
                successCallback(std::nullopt);
                return;
            }

            TwitchChannel::ChatWarning warning;
            warning.channelId = channelId;
            warning.id = warningDetails.value("id").toString();
            warning.reason = warningDetails.value("reason").toString();
            warning.createdAt =
                parseGqlDateTime(warningDetails.value("createdAt"));

            successCallback(std::move(warning));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::acknowledgeChatWarning(
    const QString &channelId, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert("channelID", channelId);
    variables.insert("input", input);

    makePersistedGqlRequest(
        "AcknowledgeChatWarning",
        "f97404a69caf9d152118bae17e962eca27c87c8a85224538173b3dfcd6c9df60",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback("Twitch API Error: " + gqlError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getActivePoll(
    const QString &channelLogin, const QString &oauthToken,
    std::function<void(std::optional<TwitchChannel::PollEvent>)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    variables.insert("login", channelLogin);

    makePersistedGqlRequest(
        "ChannelPollContext_GetViewablePoll",
        "e83188a3836c636393df3191665e543a03733d7c51d3ade3d85e42aa46c2bf55",
        variables, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback("Failed to parse GQL response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(gqlError);
                return;
            }

            const auto data = payloadDataObject(root);
            auto context = data.value("channel").toObject();
            if (context.isEmpty())
            {
                context = data.value("user").toObject();
            }

            const auto currentUserId = data.value("currentUser")
                                           .toObject()
                                           .value("id")
                                           .toString();
            successCallback(parsePollEventFromGql(
                context.value("viewablePoll").toObject(), currentUserId));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::validateCustomAuthToken(
    const QString &oauthToken,
    std::function<void(CustomAuthValidationResult)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto normalizedToken = normalizeCustomTwitchAuthToken(oauthToken);
    if (normalizedToken.isEmpty())
    {
        failureCallback("No token provided");
        return;
    }

    static const char *validationQuery = R"(
        query MoltorinoValidateCustomAuthToken {
            currentUser {
                id
                login
                displayName
            }
        }
    )";

    makeInlineGqlRequest(validationQuery, QJsonObject{}, normalizedToken)
        .onSuccess([successCallback, failureCallback, normalizedToken](
                       const NetworkResult &result) {
            auto doc = result.parseRapidJson();
            if (doc.HasParseError())
            {
                failureCallback("Failed to parse Twitch response");
                return;
            }

            const auto gqlError = extractFirstGqlErrorMessage(doc);
            if (!gqlError.isEmpty())
            {
                failureCallback(gqlError);
                return;
            }

            const rapidjson::Value *dataVal = nullptr;
            if (doc.IsArray() && doc.Size() > 0 && doc[0].IsObject() &&
                doc[0].HasMember("data") && doc[0]["data"].IsObject())
            {
                dataVal = &doc[0]["data"];
            }
            else if (doc.IsObject() && doc.HasMember("data") &&
                     doc["data"].IsObject())
            {
                dataVal = &doc["data"];
            }

            if (dataVal == nullptr || !dataVal->HasMember("currentUser") ||
                !(*dataVal)["currentUser"].IsObject())
            {
                failureCallback("Twitch did not return a user for this token");
                return;
            }

            const auto &currentUser = (*dataVal)["currentUser"];

            CustomAuthValidationResult validation;
            validation.normalizedToken = normalizedToken;

            if (!rj::getSafe(currentUser, "id", validation.userId) ||
                validation.userId.isEmpty())
            {
                failureCallback("Validated token did not include a user ID");
                return;
            }

            rj::getSafe(currentUser, "login", validation.login);
            rj::getSafe(currentUser, "displayName", validation.displayName);

            if (validation.displayName.isEmpty())
            {
                validation.displayName = validation.login;
            }
            if (validation.login.isEmpty())
            {
                validation.login = validation.displayName;
            }

            successCallback(std::move(validation));
        })
        .onError([failureCallback](const NetworkResult &result) {
            auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                failureCallback(QString("Network Error: %1 | %2")
                                    .arg(result.formatError(), body.left(200)));
                return;
            }

            failureCallback("Network Error: " + result.formatError());
        })
        .execute();
}

void TwitchGql::getModeratedChannels(
    const QString &oauthToken,
    std::function<void(QVector<GqlModeratedChannel>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    struct FetchState {
        QString oauthToken;
        QVector<GqlModeratedChannel> channels;
        QSet<QString> seenChannels;
        QSet<QString> seenCursors;
        bool completed = false;
        int pageCount = 0;
        std::shared_ptr<std::function<void(QString)>> requestForwardPage;
        std::function<void(QVector<GqlModeratedChannel>)> successCallback;
        std::function<void(const QString &)> failureCallback;
    };

    constexpr int MAX_FORWARD_PAGES = 100;
    static constexpr auto FORWARD_QUERY = R"(
query ModeratedChannels($cursor: Cursor) {
  moderatedChannels(first: 50, after: $cursor) {
    edges {
      cursor
      node {
        id
        login
        displayName
      }
    }
    pageInfo {
      hasNextPage
    }
  }
}
)";

    auto state = std::make_shared<FetchState>();
    state->oauthToken = oauthToken;
    state->successCallback = std::move(successCallback);
    state->failureCallback = std::move(failureCallback);

    auto appendChannel = [](const std::shared_ptr<FetchState> &state,
                            const QJsonObject &node) {
        GqlModeratedChannel channel;
        channel.id = node.value("id").toString().trimmed();
        channel.login = node.value("login").toString().trimmed();
        channel.displayName = node.value("displayName").toString().trimmed();
        if (channel.id.isEmpty() && channel.login.isEmpty())
        {
            return false;
        }

        const auto key = channel.id.isEmpty()
                             ? QStringLiteral("login:") +
                                   channel.login.toLower()
                             : QStringLiteral("id:") + channel.id;
        if (state->seenChannels.contains(key))
        {
            return false;
        }

        state->seenChannels.insert(key);
        state->channels.push_back(std::move(channel));
        return true;
    };

    auto appendConnection = [appendChannel](
                                const std::shared_ptr<FetchState> &state,
                                const QJsonObject &connection) {
        const auto edges = connection.value("edges").toArray();
        const auto nodes = connection.value("nodes").toArray();

        int added = 0;
        for (const auto &edgeValue : edges)
        {
            const auto edge = edgeValue.toObject();
            const auto node = edge.value("node").toObject();
            if (appendChannel(state, node.isEmpty() ? edge : node))
            {
                ++added;
            }
        }
        for (const auto &nodeValue : nodes)
        {
            if (appendChannel(state, nodeValue.toObject()))
            {
                ++added;
            }
        }

        return added;
    };

    auto finishSuccess = [](const std::shared_ptr<FetchState> &state) {
        if (state->completed || !state->successCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->successCallback);
        callback(std::move(state->channels));
    };

    auto finishFailure = [](const std::shared_ptr<FetchState> &state,
                            const QString &error) {
        if (state->completed || !state->failureCallback)
        {
            return;
        }

        state->completed = true;
        auto callback = std::move(state->failureCallback);
        callback(error);
    };

    auto requestForwardPage = std::make_shared<std::function<void(QString)>>();
    state->requestForwardPage = requestForwardPage;
    std::weak_ptr<FetchState> weakState = state;
    std::weak_ptr<std::function<void(QString)>> weakRequestForwardPage =
        requestForwardPage;
    *requestForwardPage = [weakState, weakRequestForwardPage, appendConnection,
                           finishSuccess,
                           finishFailure](QString cursor) mutable {
        auto state = weakState.lock();
        if (!state)
        {
            return;
        }
        if (state->completed)
        {
            return;
        }

        if (++state->pageCount > MAX_FORWARD_PAGES)
        {
            finishFailure(state, "Moderated channel list has too many pages");
            return;
        }

        QJsonObject variables;
        if (!cursor.isEmpty())
        {
            variables.insert("cursor", cursor);
        }

        makeTvInlineGqlRequest(FORWARD_QUERY, variables, state->oauthToken)
            .onSuccess([state, weakRequestForwardPage, appendConnection,
                        finishSuccess, finishFailure](
                           const NetworkResult &result) mutable {
                const auto value = result.parseJsonValue();
                const auto gqlError = extractFirstGqlErrorMessage(value);
                if (!gqlError.isEmpty())
                {
                    finishFailure(state, gqlError);
                    return;
                }

                const auto data = payloadDataObject(value);
                const auto moderatedChannels =
                    findModeratedChannelsConnection(data);
                if (moderatedChannels.isEmpty())
                {
                    finishFailure(
                        state,
                        "Could not find moderated channels in response");
                    return;
                }

                const auto edges = moderatedChannels.value("edges").toArray();
                QString lastEdgeCursor;
                for (const auto &edgeValue : edges)
                {
                    const auto cursor =
                        edgeValue.toObject().value("cursor").toString();
                    if (!cursor.isEmpty())
                    {
                        lastEdgeCursor = cursor;
                    }
                }

                const auto added = appendConnection(state, moderatedChannels);

                const auto pageInfo =
                    moderatedChannels.value("pageInfo").toObject();
                const auto hasNextPage =
                    pageInfo.value("hasNextPage").toBool(false);
                if (!hasNextPage)
                {
                    finishSuccess(state);
                    return;
                }

                const auto nextCursor = lastEdgeCursor;
                if (nextCursor.isEmpty())
                {
                    finishFailure(
                        state,
                        "Twitch did not return a moderated channel pagination cursor");
                    return;
                }

                if (added == 0)
                {
                    finishFailure(state,
                                  "Twitch returned no new moderated channels");
                    return;
                }

                if (state->seenCursors.contains(nextCursor))
                {
                    finishFailure(
                        state,
                        "Twitch repeated a moderated channel pagination cursor");
                    return;
                }

                state->seenCursors.insert(nextCursor);
                if (auto requestForwardPage = weakRequestForwardPage.lock())
                {
                    (*requestForwardPage)(nextCursor);
                }
            })
            .onError([state, finishFailure](const NetworkResult &result) mutable {
                auto body = QString::fromUtf8(result.getData()).trimmed();
                if (!body.isEmpty())
                {
                    finishFailure(
                        state,
                        QString("Network Error: %1 | %2")
                            .arg(result.formatError(), body.left(200)));
                    return;
                }

                finishFailure(state, "Network Error: " + result.formatError());
            })
            .execute();
    };

    (*requestForwardPage)({});
}

}  // namespace chatterino
