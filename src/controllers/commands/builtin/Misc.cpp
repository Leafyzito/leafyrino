// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/Misc.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/translation/Translator.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchGql.hpp"
#include "providers/twitch/ModerationActionLogs.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchNameHistory.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/FormatTime.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/StreamLink.hpp"
#include "util/Twitch.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCommandLineParser>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPoint>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextOption>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace chatterino::commands {

namespace {

QString followAction(bool unfollow)
{
    return unfollow ? "unfollow" : "follow";
}

QString followActionNoun(bool unfollow)
{
    return unfollow ? "unfollowing users" : "following users";
}

bool selectedTwitchUserMatches(const QString &userId, const QString &login)
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (!current || current->isAnon())
    {
        return false;
    }

    const auto currentUserId = current->getUserId().trimmed();
    const auto normalizedUserId = userId.trimmed();
    if (!currentUserId.isEmpty() && !normalizedUserId.isEmpty())
    {
        return normalizedUserId == currentUserId;
    }

    const auto currentLogin = current->getUserName().trimmed().toLower();
    const auto normalizedLogin = login.trimmed().toLower();
    if (currentLogin.isEmpty() || normalizedLogin.isEmpty())
    {
        return false;
    }

    return normalizedLogin == currentLogin;
}

QString normalizeFollowError(bool unfollow, const QString &error)
{
    auto normalized =
        MoltorinoAuth::normalizeAuthError(followActionNoun(unfollow), error);
    if (normalized.contains("failed integrity check", Qt::CaseInsensitive))
    {
        normalized =
            "This saved login cannot use follow commands. Re-login with "
            "Device Login in Settings -> Moltorino -> Authentication, then "
            "try again.";
    }
    return normalized;
}

QString formatNameHistoryRow(const TwitchNameHistoryEntry &entry)
{
    return QStringLiteral("%1: %2 - %3")
        .arg(entry.login, entry.leftText, entry.rightText);
}

QString modLogNumber(int value)
{
    return QLocale().toString(value);
}

QString modLogCountPhrase(int value, const QString &singular,
                          const QString &plural)
{
    return QStringLiteral("%1 %2").arg(modLogNumber(value),
                                       value == 1 ? singular : plural);
}

struct ModLogRange {
    int days = 7;
    QString text = QStringLiteral("last 7 days");
};

enum class ModLogRangeParseState {
    NotRange,
    Valid,
    Invalid,
};

struct ModLogRangeParseResult {
    ModLogRangeParseState state = ModLogRangeParseState::NotRange;
    ModLogRange range;
};

constexpr int MAX_MOD_LOG_CHAT_ROWS = 10;

class ModLogSummaryRowLayoutElement final : public MessageLayoutElement
{
public:
    ModLogSummaryRowLayoutElement(MessageElement &creator, QString left,
                                  QString right, QSizeF size, QColor color,
                                  FontStyle style, float scale)
        : MessageLayoutElement(creator, size)
        , left_(std::move(left))
        , right_(std::move(right))
        , color_(std::move(color))
        , style_(style)
        , scale_(scale)
    {
        this->setText(this->left_ + QStringLiteral(" ") + this->right_);
    }

    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override
    {
        const auto text = this->getText();
        const auto start = std::min<int>(from, text.size());
        const auto end = std::min<int>(to, text.size());
        if (end > start)
        {
            str += text.mid(start, end - start);
        }
    }

    size_t getSelectionIndexCount() const override
    {
        return this->getText().size();
    }

    void paint(QPainter &painter, const MessageColors &) override
    {
        const auto font =
            getApp()->getFonts()->getFont(this->style_, this->scale_);
        const QFontMetricsF metrics(font);
        const auto rect = this->getRect();
        const auto gap = 12 * this->scale_;
        const auto rightWidth = metrics.horizontalAdvance(this->right_);

        auto leftRect = rect;
        leftRect.setRight(
            std::max(leftRect.left(), rect.right() - rightWidth - gap));

        QTextOption leftOption(Qt::AlignLeft | Qt::AlignVCenter);
        leftOption.setWrapMode(QTextOption::NoWrap);
        QTextOption rightOption(Qt::AlignRight | Qt::AlignVCenter);
        rightOption.setWrapMode(QTextOption::NoWrap);

        painter.setPen(this->color_);
        painter.setFont(font);
        painter.drawText(
            leftRect,
            metrics.elidedText(this->left_, Qt::ElideRight, leftRect.width()),
            leftOption);
        painter.drawText(rect, this->right_, rightOption);
    }

    bool paintAnimated(QPainter &, qreal) override
    {
        return false;
    }

    int getMouseOverIndex(QPointF abs) const override
    {
        return abs.x() < this->getRect().center().x()
                   ? 0
                   : static_cast<int>(this->getSelectionIndexCount());
    }

    qreal getXFromIndex(size_t index) override
    {
        return index == 0 ? this->getRect().left() : this->getRect().right();
    }

private:
    QString left_;
    QString right_;
    QColor color_;
    FontStyle style_;
    float scale_;
};

class ModLogSummaryRowElement final : public MessageElement
{
public:
    static constexpr std::string_view TYPE = "moltorino-modlog-row";

    ModLogSummaryRowElement(QString left, QString right,
                            MessageElementFlags flags,
                            const MessageColor &color = MessageColor::System,
                            FontStyle style = FontStyle::ChatMedium)
        : MessageElement(flags)
        , left_(std::move(left))
        , right_(std::move(right))
        , color_(color)
        , style_(style)
    {
        this->setTrailingSpace(false);
    }

    void addToContainer(MessageLayoutContainer &container,
                        const MessageLayoutContext &ctx) override
    {
        if (!ctx.flags.hasAny(this->getFlags()))
        {
            return;
        }

        if (!container.atStartOfLine())
        {
            container.breakLine();
        }

        const auto metrics = getApp()->getFonts()->getFontMetrics(
            this->style_, container.getScale());
        auto color = this->color_.getColor(ctx.messageColors);
        const auto width = std::max<qreal>(container.remainingWidth(), 1);
        auto *element = new ModLogSummaryRowLayoutElement(
            *this, this->left_, this->right_, QSizeF(width, metrics.height()),
            color, this->style_, container.getScale());
        element->setTrailingSpace(false);
        container.addElementNoLineBreak(element);
    }

    std::unique_ptr<MessageElement> clone() const override
    {
        auto element = std::make_unique<ModLogSummaryRowElement>(
            this->left_, this->right_, this->getFlags(), this->color_,
            this->style_);
        element->cloneFrom(*this);
        return element;
    }

    std::string_view type() const override
    {
        return TYPE;
    }

private:
    QString left_;
    QString right_;
    MessageColor color_;
    FontStyle style_;
};

QString modLogRangeText(int amount, QChar unit)
{
    switch (unit.toLower().unicode())
    {
        case 'd':
            if (amount == 1)
            {
                return QStringLiteral("last 24 hours");
            }
            return QStringLiteral("last %1 days").arg(amount);
        case 'w':
            return QStringLiteral("last %1 %2")
                .arg(amount)
                .arg(amount == 1 ? QStringLiteral("week")
                                 : QStringLiteral("weeks"));
        case 'm':
            return QStringLiteral("last %1 %2")
                .arg(amount)
                .arg(amount == 1 ? QStringLiteral("month")
                                 : QStringLiteral("months"));
    }

    return QString();
}

ModLogRangeParseResult parseModLogRange(QString value)
{
    value = value.trimmed().toLower();
    if (value.isEmpty())
    {
        return {};
    }

    QChar unit = 'd';
    auto numberPart = value;
    if (value.back().isLetter())
    {
        unit = value.back();
        numberPart.chop(1);
    }

    bool ok = false;
    const auto amount = numberPart.toInt(&ok);
    if (!ok)
    {
        return {};
    }
    if (amount <= 0)
    {
        return {ModLogRangeParseState::Invalid, {}};
    }

    int days = amount;
    if (unit == 'w')
    {
        days = amount * 7;
    }
    else if (unit == 'm')
    {
        days = amount * 30;
    }
    else if (unit != 'd')
    {
        return {ModLogRangeParseState::Invalid, {}};
    }

    return {
        ModLogRangeParseState::Valid,
        {
            days,
            modLogRangeText(amount, unit),
        },
    };
}

QString modLogCompactCountText(const ModerationActionLogCounts &counts)
{
    return QStringLiteral("%1 | %2 | %3")
        .arg(modLogNumber(counts.bans), modLogNumber(counts.timeouts),
             modLogNumber(counts.countedTotal()));
}

QString modLogRawNumber(int value)
{
    return QString::number(value);
}

QString modLogPaddedCell(QString value, int width, bool rightAligned)
{
    return rightAligned ? value.rightJustified(width, QLatin1Char(' '))
                        : value.leftJustified(width, QLatin1Char(' '));
}

QString buildModLogsFullListText(
    const QString &channelLogin, const QString &rangeText,
    const ModerationActionLogScanSnapshot &snapshot)
{
    QStringList lines;
    lines.reserve(snapshot.moderators.size() + 14);
    const auto generatedAt =
        QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat);

    int rankWidth =
        std::max<int>(QStringLiteral("RANK").size(),
                      modLogRawNumber(snapshot.moderators.size()).size());
    int moderatorWidth = QStringLiteral("MODERATOR").size();
    int bansWidth = QStringLiteral("BANS").size();
    int timeoutsWidth = QStringLiteral("TIMEOUTS").size();
    int totalWidth = QStringLiteral("TOTAL").size();

    auto updateWidths = [&](const QString &moderator,
                            const ModerationActionLogCounts &counts) {
        moderatorWidth = std::max<int>(moderatorWidth, moderator.size());
        bansWidth =
            std::max<int>(bansWidth, modLogRawNumber(counts.bans).size());
        timeoutsWidth = std::max<int>(timeoutsWidth,
                                      modLogRawNumber(counts.timeouts).size());
        totalWidth = std::max<int>(
            totalWidth, modLogRawNumber(counts.countedTotal()).size());
    };

    updateWidths(QStringLiteral("All mods"), snapshot.totals);
    for (const auto &mod : snapshot.moderators)
    {
        updateWidths(mod.displayName, mod.counts);
    }

    auto row = [&](const QString &rank, const QString &moderator,
                   const QString &bans, const QString &timeouts,
                   const QString &total) {
        return QStringLiteral("%1 | %2 | %3 | %4 | %5")
            .arg(modLogPaddedCell(rank, rankWidth, true),
                 modLogPaddedCell(moderator, moderatorWidth, false),
                 modLogPaddedCell(bans, bansWidth, true),
                 modLogPaddedCell(timeouts, timeoutsWidth, true),
                 modLogPaddedCell(total, totalWidth, true));
    };

    const auto header = row(QStringLiteral("RANK"), QStringLiteral("MODERATOR"),
                            QStringLiteral("BANS"), QStringLiteral("TIMEOUTS"),
                            QStringLiteral("TOTAL"));
    const auto separator = QString(header.size(), QLatin1Char('-'));

    lines.append(
        QStringLiteral("Mod actions in %1, %2").arg(channelLogin, rangeText));
    lines.append(QStringLiteral("Generated: %1").arg(generatedAt));
    if (snapshot.truncated)
    {
        lines.append(QStringLiteral("Status: partial, page limit reached"));
    }
    lines.append(QString());
    lines.append(QStringLiteral("Total Mods: %1")
                     .arg(modLogRawNumber(snapshot.moderators.size())));
    lines.append(QStringLiteral("Total Actions: %1")
                     .arg(modLogRawNumber(snapshot.totals.countedTotal())));
    lines.append(QStringLiteral("  - Bans: %1")
                     .arg(modLogRawNumber(snapshot.totals.bans)));
    lines.append(QStringLiteral("  - Timeouts: %1")
                     .arg(modLogRawNumber(snapshot.totals.timeouts)));
    lines.append(QString());
    lines.append(separator);
    lines.append(header);
    lines.append(separator);

    int rank = 1;
    for (const auto &mod : snapshot.moderators)
    {
        lines.append(row(modLogRawNumber(rank++), mod.displayName,
                         modLogRawNumber(mod.counts.bans),
                         modLogRawNumber(mod.counts.timeouts),
                         modLogRawNumber(mod.counts.countedTotal())));
    }
    lines.append(separator);

    return lines.join(QLatin1Char('\n'));
}

void uploadModLogsFullList(
    const QString &content,
    std::function<void(const QString &)> completionCallback)
{
    auto callback = std::make_shared<std::function<void(const QString &)>>(
        std::move(completionCallback));
    QJsonObject payload{
        {QStringLiteral("source"), QStringLiteral("client")},
        {QStringLiteral("content"), content},
    };

    NetworkRequest(QUrl(QStringLiteral("https://h.moltorino.com/api/paste")),
                   NetworkRequestType::Post)
        .timeout(10000)
        .hideRequestBody()
        .json(payload)
        .onSuccess([callback](const NetworkResult &result) {
            const auto root = result.parseJson();
            auto rawUrl =
                root.value(QStringLiteral("rawUrl")).toString().trimmed();
            if (rawUrl.isEmpty())
            {
                const auto url =
                    root.value(QStringLiteral("url")).toString().trimmed();
                if (!url.isEmpty())
                {
                    rawUrl = url + QStringLiteral("/raw");
                }
            }
            if (!rawUrl.startsWith(QStringLiteral("https://")) &&
                !rawUrl.startsWith(QStringLiteral("http://")))
            {
                rawUrl.clear();
            }
            (*callback)(rawUrl);
        })
        .onError([callback](const NetworkResult &) {
            (*callback)(QString());
        })
        .execute();
}

void addModLogTextLine(MessageBuilder &builder, QString &searchText,
                       const QString &line)
{
    if (!searchText.isEmpty())
    {
        searchText += '\n';
        builder.emplace<LinebreakElement>(MessageElementFlag::Text);
    }

    searchText += line;
    builder.emplace<TextElement>(line, MessageElementFlag::Text,
                                 MessageColor::System);
}

void addModLogSummaryRow(MessageBuilder &builder, QString &searchText,
                         const QString &left, const QString &right)
{
    if (!searchText.isEmpty())
    {
        searchText += '\n';
        builder.emplace<LinebreakElement>(MessageElementFlag::Text);
    }

    const auto rowText = QStringLiteral("%1 %2").arg(left, right);
    searchText += rowText;
    builder.emplace<ModLogSummaryRowElement>(left, right,
                                             MessageElementFlag::Text);
}

void addModLogLinkLine(MessageBuilder &builder, QString &searchText,
                       const QString &prefix, const QString &url)
{
    if (!searchText.isEmpty())
    {
        searchText += '\n';
        builder.emplace<LinebreakElement>(MessageElementFlag::Text);
    }

    auto displayUrl = url;
    displayUrl.remove(QRegularExpression(QStringLiteral("^https?://")));

    searchText += prefix + QLatin1Char(' ') + displayUrl;
    builder.emplace<TextElement>(prefix, MessageElementFlag::Text,
                                 MessageColor::System);
    builder.emplace<LinkElement>(
        LinkElement::Parsed{.lowercase = displayUrl.toLower(),
                            .original = displayUrl},
        url, MessageElementFlag::Text, MessageColor(MessageColor::Link));
}

void addModLogsResultMessage(const ChannelPtr &channel,
                             const QString &channelLogin,
                             const QString &rangeText,
                             const QString &moderatorLogin,
                             const ModerationActionLogScanSnapshot &snapshot,
                             const QString &fullListRawUrl = QString())
{
    if (channel == nullptr)
    {
        return;
    }

    MessageBuilder builder;
    QString searchText;

    if (moderatorLogin.isEmpty())
    {
        addModLogTextLine(builder, searchText,
                          QStringLiteral("Mod actions in %1, %2")
                              .arg(channelLogin, rangeText));
        if (snapshot.moderators.isEmpty())
        {
            addModLogTextLine(builder, searchText, QString());
            addModLogTextLine(builder, searchText,
                              "No matching moderation actions found.");
        }
        else
        {
            addModLogTextLine(
                builder, searchText,
                QStringLiteral("%1 across %2")
                    .arg(modLogCountPhrase(snapshot.totals.countedTotal(),
                                           QStringLiteral("counted action"),
                                           QStringLiteral("counted actions")),
                         modLogCountPhrase(snapshot.moderators.size(),
                                           QStringLiteral("moderator"),
                                           QStringLiteral("moderators"))));
            addModLogTextLine(builder, searchText, QString());
            addModLogSummaryRow(builder, searchText, QStringLiteral(""),
                                QStringLiteral("bans | timeouts | total"));
            addModLogSummaryRow(builder, searchText,
                                QStringLiteral("All mods:"),
                                modLogCompactCountText(snapshot.totals));
            for (int i = 0;
                 i < snapshot.moderators.size() && i < MAX_MOD_LOG_CHAT_ROWS;
                 ++i)
            {
                const auto &mod = snapshot.moderators.at(i);
                addModLogSummaryRow(builder, searchText,
                                    mod.displayName + QStringLiteral(":"),
                                    modLogCompactCountText(mod.counts));
            }
            if (snapshot.moderators.size() > MAX_MOD_LOG_CHAT_ROWS)
            {
                addModLogTextLine(
                    builder, searchText,
                    QStringLiteral("Showing top %1 of %2 moderators.")
                        .arg(MAX_MOD_LOG_CHAT_ROWS)
                        .arg(snapshot.moderators.size()));
                if (!fullListRawUrl.isEmpty())
                {
                    addModLogTextLine(builder, searchText, QString());
                    addModLogLinkLine(builder, searchText,
                                      QStringLiteral("Full list:"),
                                      fullListRawUrl);
                }
            }
        }
        if (snapshot.truncated)
        {
            addModLogTextLine(builder, searchText,
                              "Result may be incomplete; page limit was "
                              "reached before the full range.");
        }
    }
    else
    {
        addModLogTextLine(
            builder, searchText,
            QStringLiteral("Mod actions by %1").arg(moderatorLogin));
        addModLogTextLine(
            builder, searchText,
            QStringLiteral("in %1, %2").arg(channelLogin, rangeText));
        if (snapshot.totals.rawTotal() == 0)
        {
            addModLogTextLine(builder, searchText, QString());
            addModLogTextLine(builder, searchText,
                              "No matching moderation actions found.");
        }
        else
        {
            addModLogTextLine(builder, searchText, QString());
            addModLogSummaryRow(builder, searchText, QStringLiteral("Bans:"),
                                modLogNumber(snapshot.totals.bans));
            addModLogSummaryRow(builder, searchText,
                                QStringLiteral("Timeouts:"),
                                modLogNumber(snapshot.totals.timeouts));
            addModLogSummaryRow(builder, searchText, QStringLiteral("Total:"),
                                modLogNumber(snapshot.totals.countedTotal()));
        }
        if (snapshot.truncated)
        {
            addModLogTextLine(builder, searchText,
                              "Result may be incomplete; page limit was "
                              "reached before the full range.");
        }
    }

    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->messageText = searchText;
    builder->searchText = searchText;
    channel->addMessage(builder.release(), MessageContext::Original);
}

void addNameHistorySystemMessage(const ChannelPtr &channel,
                                 const TwitchNameHistory &history)
{
    if (history.entries.empty())
    {
        channel->addSystemMessage("No name history found.");
        return;
    }

    MessageBuilder builder;
    QString searchText;

    for (auto it = history.entries.cbegin(); it != history.entries.cend(); ++it)
    {
        const auto row = formatNameHistoryRow(*it);
        if (!searchText.isEmpty())
        {
            searchText += '\n';
            builder.emplace<LinebreakElement>(MessageElementFlag::Text);
        }
        searchText += row;

        builder.emplace<TextElement>(it->login + ':', MessageElementFlag::Text,
                                     MessageColor::System,
                                     FontStyle::ChatMediumBold);
        builder.emplace<TextElement>(" " + it->leftText + " - " + it->rightText,
                                     MessageElementFlag::Text,
                                     MessageColor::System);
    }

    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->messageText = searchText;
    builder->searchText = searchText;

    channel->addMessage(builder.release(), MessageContext::Original);
}

QString commandWordsAfter(const CommandContext &ctx, int wordCount)
{
    return ctx.words.mid(wordCount).join(QLatin1Char(' ')).trimmed();
}

QString supportedTranslationLanguageText()
{
    return QStringLiteral(
        "examples: en, es, pt, fr, de, ja, ko, zh-cn, zh-tw, ar");
}

void addTranslationSystemMessage(const ChannelPtr &channel,
                                 const TranslationResult &result,
                                 const QString &targetLanguage)
{
    if (channel == nullptr)
    {
        return;
    }

    const auto targetName = translationLanguageName(targetLanguage);
    const auto detectedLanguage =
        normalizedLanguageCode(result.detectedLanguage);
    const auto detectedName = translationLanguageName(detectedLanguage);

    QString prefix = QStringLiteral("Translation");
    if (!detectedName.isEmpty() && detectedLanguage != targetLanguage)
    {
        prefix += QStringLiteral(" (%1 -> %2)").arg(detectedName, targetName);
    }
    else if (!targetName.isEmpty())
    {
        prefix += QStringLiteral(" (%1)").arg(targetName);
    }

    channel->addSystemMessage(
        QStringLiteral("%1: %2").arg(prefix, result.translatedText.trimmed()));
}

QString runTranslatePreviewCommand(const CommandContext &ctx,
                                   const QString &targetLanguage,
                                   const QString &message, const QString &usage)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage(usage);
        return "";
    }

    requestTextTranslation(
        message, targetLanguage, nullptr,
        [channel = ctx.channel,
         targetLanguage](const TranslationResult &result) {
            addTranslationSystemMessage(channel, result, targetLanguage);
        },
        [channel = ctx.channel](const QString &) {
            channel->addSystemMessage(
                QStringLiteral("Translation failed. Try again later."));
        });

    return "";
}

QString runTranslateSendCommand(const CommandContext &ctx,
                                const QString &targetLanguage,
                                const QString &message)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (message.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /tl <language> <message>");
        return "";
    }

    requestTextTranslation(
        message, targetLanguage, nullptr,
        [channel = ctx.channel](const TranslationResult &result) {
            auto translatedText = result.translatedText.trimmed();
            translatedText.replace('\n', ' ');
            if (translatedText.isEmpty())
            {
                channel->addSystemMessage(
                    QStringLiteral("Translation failed, so nothing was sent."));
                return;
            }
            if (translatedText.size() > TWITCH_MESSAGE_LIMIT)
            {
                channel->addSystemMessage(QStringLiteral(
                    "The translated message is too long for Twitch."));
                return;
            }

            channel->sendMessage(translatedText);
        },
        [channel = ctx.channel](const QString &) {
            channel->addSystemMessage(
                QStringLiteral("Translation failed, so nothing was sent."));
        });

    return "";
}

void runNameHistoryLookup(const ChannelPtr &channel, const QString &userId,
                          const QString &targetName,
                          const QString &expectedLogin, bool announceFetch)
{
    if (channel == nullptr)
    {
        return;
    }

    if (const auto cached = getCachedTwitchNameHistory(userId, expectedLogin))
    {
        addNameHistorySystemMessage(channel, *cached);
        return;
    }

    if (announceFetch)
    {
        channel->addSystemMessage("Fetching name history...");
    }

    fetchTwitchNameHistoryByUserId(
        userId, expectedLogin,
        [channel](TwitchNameHistory history) {
            addNameHistorySystemMessage(channel, history);
        },
        [channel, targetName](const QString &error) {
            channel->addSystemMessage(
                QString("Failed to fetch name history for %1: %2")
                    .arg(targetName, error));
        });
}

void runFollowMutation(const ChannelPtr &channel,
                       const MoltorinoAuthToken &auth, const QString &targetId,
                       const QString &targetLogin, const QString &targetName,
                       bool unfollow)
{
    const auto requestUserId = auth.userId;
    const auto requestLogin = auth.login;
    auto successCallback = [channel, requestUserId, requestLogin, targetId,
                            targetLogin, targetName, unfollow] {
        if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get());
            twitchChannel != nullptr &&
            selectedTwitchUserMatches(requestUserId, requestLogin) &&
            (twitchChannel->roomId() == targetId ||
             twitchChannel->getName().compare(targetLogin,
                                              Qt::CaseInsensitive) == 0))
        {
            std::optional<QDateTime> followedAt;
            if (!unfollow)
            {
                followedAt = QDateTime::currentDateTimeUtc();
            }
            twitchChannel->setFollowingStatus(!unfollow, followedAt);
        }

        channel->addSystemMessage(
            unfollow ? QString("You unfollowed %1.").arg(targetName)
                     : QString("You followed %1.").arg(targetName));
    };
    auto failureCallback = [channel, targetName,
                            unfollow](const QString &error) {
        channel->addSystemMessage(
            QString("Failed to %1 %2: %3")
                .arg(followAction(unfollow), targetName,
                     normalizeFollowError(unfollow, error)));
    };

    if (unfollow)
    {
        TwitchGql::unfollowUser(targetId, auth.token,
                                std::move(successCallback),
                                std::move(failureCallback));
    }
    else
    {
        TwitchGql::followUser(targetId, auth.token, std::move(successCallback),
                              std::move(failureCallback));
    }
}

QString runFollowCommand(const CommandContext &ctx, bool unfollow)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    auto target = ctx.words.value(1).trimmed();
    if (target.isEmpty())
    {
        if (ctx.twitchChannel == nullptr)
        {
            ctx.channel->addSystemMessage(
                QString("Usage: /%1 <username>").arg(followAction(unfollow)));
            return "";
        }
        target = ctx.twitchChannel->getName();
    }

    auto [targetLogin, targetId] = parseUserNameOrID(target);
    targetLogin = targetLogin.trimmed().toLower();
    targetId = targetId.trimmed();

    if (targetId.isEmpty() &&
        (targetLogin.isEmpty() ||
         !twitchUserLoginRegexp().match(targetLogin).hasMatch()))
    {
        ctx.channel->addSystemMessage(
            QString("Usage: /%1 [username]").arg(followAction(unfollow)));
        return "";
    }

    QString authError;
    const auto auth = MoltorinoAuth::resolveSelectedUserToken(&authError);
    if (!auth.hasToken())
    {
        ctx.channel->addSystemMessage(
            authError.isEmpty()
                ? MoltorinoAuth::authRequiredMessage(followActionNoun(unfollow))
                : authError);
        return "";
    }

    if (!targetId.isEmpty())
    {
        runFollowMutation(ctx.channel, auth, targetId, QString(),
                          QString("id:%1").arg(targetId), unfollow);
        return "";
    }

    TwitchGql::getUserByLogin(
        targetLogin, auth.token,
        [channel = ctx.channel, auth, targetLogin,
         unfollow](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QString("Could not find Twitch user %1.").arg(targetLogin));
                return;
            }

            const auto targetName =
                user->displayName.isEmpty() ? user->login : user->displayName;
            runFollowMutation(channel, auth, user->id, user->login, targetName,
                              unfollow);
        },
        [channel = ctx.channel, targetLogin, unfollow](const QString &error) {
            channel->addSystemMessage(
                QString("Failed to look up %1: %2")
                    .arg(targetLogin, normalizeFollowError(unfollow, error)));
        });

    return "";
}

}  // namespace

QString follow(const CommandContext &ctx)
{
    return runFollowCommand(ctx, false);
}

QString unfollow(const CommandContext &ctx)
{
    return runFollowCommand(ctx, true);
}

QString nameHistory(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    auto target = ctx.words.value(1).trimmed();
    if (target.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /namehistory <username>");
        return "";
    }

    auto [targetLogin, targetId] = parseUserNameOrID(target);
    targetLogin = targetLogin.trimmed().toLower();
    targetId = targetId.trimmed();

    if (!targetId.isEmpty())
    {
        runNameHistoryLookup(ctx.channel, targetId,
                             QString("id:%1").arg(targetId), QString(), true);
        return "";
    }

    if (targetLogin.isEmpty() ||
        !twitchUserLoginRegexp().match(targetLogin).hasMatch())
    {
        ctx.channel->addSystemMessage("Usage: /namehistory <username>");
        return "";
    }

    if (const auto cached = getCachedTwitchNameHistory(QString(), targetLogin))
    {
        addNameHistorySystemMessage(ctx.channel, *cached);
        return "";
    }

    ctx.channel->addSystemMessage("Fetching name history...");

    TwitchGql::getUserByLogin(
        targetLogin, QString(),
        [channel = ctx.channel, targetLogin](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QString("Could not find Twitch user %1.").arg(targetLogin));
                return;
            }

            const auto targetName =
                user->displayName.isEmpty() ? user->login : user->displayName;
            runNameHistoryLookup(channel, user->id, targetName, user->login,
                                 false);
        },
        [channel = ctx.channel, targetLogin](const QString &error) {
            channel->addSystemMessage(
                QString("Failed to look up %1: %2").arg(targetLogin, error));
        });

    return "";
}

QString logs(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /logs <user> [channel]");
        return "";
    }

    QString userName = ctx.words[1];
    stripUserName(userName);
    userName = userName.trimmed();

    QString channelName;
    if (ctx.words.size() > 2)
    {
        channelName = ctx.words[2];
        stripChannelName(channelName);
        channelName = channelName.trimmed();
    }
    else if (ctx.twitchChannel != nullptr)
    {
        channelName = ctx.twitchChannel->getName();
    }

    if (userName.isEmpty() || channelName.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /logs <user> [channel]");
        return "";
    }

    QUrl url(QStringLiteral("https://tv.supa.sh/logs"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("c"), channelName);
    query.addQueryItem(QStringLiteral("u"), userName);
    url.setQuery(query);

    const auto link = url.toString();
    ctx.channel->addSystemMessage(QStringLiteral("Logs from %1 in %2: %3")
                                      .arg(userName, channelName, link));

    return "";
}

QString modLogs(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto usage =
        QStringLiteral("Usage: /modlogs [all|moderator] [range] [channel]");
    auto normalizedCommandArgument = [](QString value) {
        value = value.trimmed();
        stripUserName(value);
        stripChannelName(value);
        return value;
    };

    const auto args = ctx.words.mid(1);
    QString moderatorLogin;
    ModLogRange range;
    int argIndex = 0;

    const auto first = normalizedCommandArgument(args.value(argIndex));
    const auto firstLower = first.toLower();
    if (!first.isEmpty())
    {
        if (firstLower == "all" || firstLower == "channel")
        {
            argIndex++;
        }
        else
        {
            const auto parsedRange = parseModLogRange(first);
            if (parsedRange.state == ModLogRangeParseState::Valid)
            {
                range = parsedRange.range;
                argIndex++;
            }
            else if (parsedRange.state == ModLogRangeParseState::Invalid)
            {
                ctx.channel->addSystemMessage(usage);
                return "";
            }
            else
            {
                moderatorLogin = first;
                argIndex++;
            }
        }
    }

    if (argIndex < args.size())
    {
        const auto maybeRange = normalizedCommandArgument(args.value(argIndex));
        const auto parsedRange = parseModLogRange(maybeRange);
        if (parsedRange.state == ModLogRangeParseState::Valid)
        {
            range = parsedRange.range;
            argIndex++;
        }
        else if (parsedRange.state == ModLogRangeParseState::Invalid)
        {
            ctx.channel->addSystemMessage(usage);
            return "";
        }
    }

    QString channelLogin;
    QString channelId;
    if (argIndex < args.size())
    {
        channelLogin = normalizedCommandArgument(args.value(argIndex));
        argIndex++;
    }

    if (argIndex < args.size())
    {
        ctx.channel->addSystemMessage(usage);
        return "";
    }

    if (channelLogin.isEmpty() && ctx.twitchChannel != nullptr)
    {
        channelLogin = ctx.twitchChannel->getName();
        channelId = ctx.twitchChannel->roomId();
    }

    if (channelLogin.isEmpty())
    {
        ctx.channel->addSystemMessage(usage);
        return "";
    }

    const auto startScan = [channel = ctx.channel, channelLogin, range,
                            moderatorLogin](const QString &resolvedChannelId) {
        QString authError;
        const auto auth = MoltorinoAuth::resolveModerationToken(
            resolvedChannelId, channelLogin, &authError);
        if (!auth.hasToken())
        {
            channel->addSystemMessage(authError);
            return;
        }

        const auto beginScanner = [channel, channelLogin, range](
                                      ModerationActionLogScanRequest request,
                                      QString moderatorLabel) {
            channel->addSystemMessage("Fetching moderation action logs...");
            auto *scanner = new ModerationActionLogScanner(std::move(request));
            scanner->onDone =
                [channel, channelLogin, range, moderatorLabel,
                 scanner](const ModerationActionLogScanSnapshot &snapshot) {
                    const auto shouldUploadFullList =
                        moderatorLabel.isEmpty() &&
                        snapshot.moderators.size() > MAX_MOD_LOG_CHAT_ROWS;
                    if (!shouldUploadFullList)
                    {
                        addModLogsResultMessage(channel, channelLogin,
                                                range.text, moderatorLabel,
                                                snapshot);
                        scanner->deleteLater();
                        return;
                    }

                    uploadModLogsFullList(
                        buildModLogsFullListText(channelLogin, range.text,
                                                 snapshot),
                        [channel, channelLogin, range, moderatorLabel, snapshot,
                         scanner](const QString &fullListRawUrl) {
                            addModLogsResultMessage(channel, channelLogin,
                                                    range.text, moderatorLabel,
                                                    snapshot, fullListRawUrl);
                            scanner->deleteLater();
                        });
                };
            scanner->onError = [channel, scanner](const QString &error) {
                channel->addSystemMessage(
                    QStringLiteral("Failed to fetch moderation "
                                   "action logs: %1")
                        .arg(error));
                scanner->deleteLater();
            };
            scanner->start();
        };

        ModerationActionLogScanRequest request;
        request.channelId = resolvedChannelId;
        request.channelLogin = channelLogin;
        request.oauthToken = auth.token;
        request.cutoffUtc =
            QDateTime::currentDateTimeUtc().addDays(-range.days);

        if (moderatorLogin.isEmpty())
        {
            beginScanner(std::move(request), QString());
            return;
        }

        TwitchGql::getUserByLogin(
            moderatorLogin, auth.token,
            [channel, request = std::move(request), beginScanner,
             moderatorLogin](std::optional<GqlUser> user) mutable {
                if (!user)
                {
                    channel->addSystemMessage(
                        QStringLiteral("Could not find Twitch user %1.")
                            .arg(moderatorLogin));
                    return;
                }
                request.moderatorId = user->id;
                request.moderatorLogin = user->login;
                const auto label = user->displayName.isEmpty()
                                       ? user->login
                                       : user->displayName;
                beginScanner(std::move(request), label);
            },
            [channel, moderatorLogin](const QString &error) {
                channel->addSystemMessage(
                    QStringLiteral("Failed to look up %1: %2")
                        .arg(moderatorLogin, error));
            });
    };

    if (!channelId.isEmpty())
    {
        startScan(channelId);
        return "";
    }

    QString readAuthError;
    const auto readAuth = MoltorinoAuth::resolveReadToken(&readAuthError);
    if (!readAuth.hasToken())
    {
        ctx.channel->addSystemMessage(readAuthError);
        return "";
    }

    TwitchGql::getUserByLogin(
        channelLogin, readAuth.token,
        [channel = ctx.channel, channelLogin,
         startScan](std::optional<GqlUser> user) {
            if (!user)
            {
                channel->addSystemMessage(
                    QStringLiteral("Could not find Twitch channel %1.")
                        .arg(channelLogin));
                return;
            }
            startScan(user->id);
        },
        [channel = ctx.channel, channelLogin](const QString &error) {
            channel->addSystemMessage(QStringLiteral("Failed to look up %1: %2")
                                          .arg(channelLogin, error));
        });

    return "";
}

QString translate(const CommandContext &ctx)
{
    const auto targetLanguage = normalizedTranslationTargetLanguage(
        getSettings()->messageTranslationTargetLanguage.getValue());
    return runTranslatePreviewCommand(
        ctx, targetLanguage, commandWordsAfter(ctx, 1),
        QStringLiteral("Usage: /translate <message>"));
}

QString translateTo(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto targetLanguage =
        translationLanguageCodeFromInput(ctx.words.value(1));
    if (targetLanguage.isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: /translateto <language> <message> (%1)")
                .arg(supportedTranslationLanguageText()));
        return "";
    }

    return runTranslatePreviewCommand(
        ctx, targetLanguage, commandWordsAfter(ctx, 2),
        QStringLiteral("Usage: /translateto <language> <message>"));
}

QString sayTranslate(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto targetLanguage =
        translationLanguageCodeFromInput(ctx.words.value(1));
    if (targetLanguage.isEmpty())
    {
        ctx.channel->addSystemMessage(
            QStringLiteral("Usage: /tl <language> <message> (%1)")
                .arg(supportedTranslationLanguageText()));
        return "";
    }

    return runTranslateSendCommand(ctx, targetLanguage,
                                   commandWordsAfter(ctx, 2));
}

QString uptime(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /uptime command only works in Twitch Channels.");
        return "";
    }

    const auto &streamStatus = ctx.twitchChannel->accessStreamStatus();

    QString messageText =
        streamStatus->live ? streamStatus->uptime : "Channel is not live.";

    ctx.channel->addSystemMessage(messageText);

    return "";
}

QString user(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /user <user> [channel]");
        return "";
    }

    QString userName = ctx.words[1];
    stripUserName(userName);

    QString channelName = ctx.channel->getName();
    if (ctx.words.size() > 2)
    {
        channelName = ctx.words[2];
        stripChannelName(channelName);
    }

    if (userName.isEmpty())
    {
        ctx.channel->addSystemMessage("Usage: /user <user> [channel]");
        return "";
    }

    QDesktopServices::openUrl(
        QUrl(QString("https://www.twitch.tv/popout/%1/viewercard/%2")
                 .arg(channelName, userName)));
    return "";
}

QString requests(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "Usage: /requests [channel]. You can also use the command "
                "without arguments in any Twitch channel to open its "
                "channel points requests queue. Only the broadcaster and "
                "moderators have permission to view the queue.");
            return "";
        }
    }

    stripChannelName(target);
    QDesktopServices::openUrl(QUrl(
        QString("https://www.twitch.tv/popout/%1/reward-queue").arg(target)));

    return "";
}

QString lowtrust(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "Usage: /lowtrust [channel]. You can also use the command "
                "without arguments in any Twitch channel to open its "
                "suspicious user activity feed. Only the broadcaster and "
                "moderators have permission to view this feed.");
            return "";
        }
    }

    stripChannelName(target);
    QDesktopServices::openUrl(QUrl(
        QString("https://www.twitch.tv/popout/moderator/%1/low-trust-users")
            .arg(target)));

    return "";
}

QString clip(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (const auto type = ctx.channel->getType();
        type != Channel::Type::Twitch && type != Channel::Type::TwitchWatching)
    {
        ctx.channel->addSystemMessage(
            "The /clip command only works in Twitch Channels.");
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /clip command only works in Twitch Channels.");
        return "";
    }

    QString title = "";
    std::optional<int> duration = std::nullopt;
    if (!ctx.words.empty())
    {
        QCommandLineParser parser;
        parser.setSingleDashWordOptionMode(
            QCommandLineParser::ParseAsLongOptions);
        parser.setOptionsAfterPositionalArgumentsMode(
            QCommandLineParser::ParseAsPositionalArguments);
        parser.addPositionalArgument("title", "The title of the clip");

        QCommandLineOption durationOption(
            {"d", "duration"}, "The duration of the clip", "duration");
        parser.addOptions({
            durationOption,
        });
        parser.parse(ctx.words);

        title = parser.positionalArguments().join(' ');

        if (parser.isSet(durationOption))
        {
            bool ok = false;
            duration = parser.value(durationOption).toInt(&ok);

            if (!ok)
            {
                ctx.channel->addSystemMessage(
                    "Could not parse clip duration to an integer.");
                return "";
            }
        }
    }

    ctx.twitchChannel->createClip(title, duration);

    return "";
}

QString marker(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /marker command only works in Twitch channels.");
        return "";
    }

    // Avoid Helix calls without Client ID and/or OAuth Token
    if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You need to be logged in to create stream markers!");
        return "";
    }

    // Exact same message as in webchat
    if (!ctx.twitchChannel->isLive())
    {
        ctx.channel->addSystemMessage(
            "You can only add stream markers during live streams. Try "
            "again when the channel is live streaming.");
        return "";
    }

    auto arguments = ctx.words;
    arguments.removeFirst();

    getHelix()->createStreamMarker(
        // Limit for description is 140 characters, webchat just crops description
        // if it's >140 characters, so we're doing the same thing
        ctx.twitchChannel->roomId(), arguments.join(" ").left(140),
        [channel{ctx.channel},
         arguments](const HelixStreamMarker &streamMarker) {
            channel->addSystemMessage(
                QString("Successfully added a stream marker at %1%2")
                    .arg(formatTime(streamMarker.positionSeconds))
                    .arg(streamMarker.description.isEmpty()
                             ? ""
                             : QString(": \"%1\"")
                                   .arg(streamMarker.description)));
        },
        [channel{ctx.channel}](auto error) {
            QString errorMessage("Failed to create stream marker - ");

            switch (error)
            {
                case HelixStreamMarkerError::UserNotAuthorized: {
                    errorMessage +=
                        "you don't have permission to perform that action.";
                }
                break;

                case HelixStreamMarkerError::UserNotAuthenticated: {
                    errorMessage += "you need to re-authenticate.";
                }
                break;

                // This would most likely happen if the service is down, or if the JSON payload returned has changed format
                case HelixStreamMarkerError::Unknown:
                default: {
                    errorMessage += "an unknown error occurred.";
                }
                break;
            }

            channel->addSystemMessage(errorMessage);
        });

    return "";
}

QString streamlink(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else if (ctx.kickChannel)
        {
            target = ctx.kickChannel->slug();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "/streamlink [channel]. Open specified Twitch channel in "
                "streamlink. If no channel argument is specified, open the "
                "current Twitch channel instead.");
            return "";
        }
    }

    stripChannelName(target);
    if (ctx.kickChannel)
    {
        openStreamlinkForChannel(target, u"kick.com/");
    }
    else
    {
        openStreamlinkForChannel(target);
    }

    return "";
}

QString popout(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QString target(ctx.words.value(1));

    if (target.isEmpty())
    {
        if (ctx.channel->getType() == Channel::Type::Twitch &&
            !ctx.channel->isEmpty())
        {
            target = ctx.channel->getName();
        }
        else
        {
            ctx.channel->addSystemMessage(
                "Usage: /popout <channel>. You can also use the command "
                "without arguments in any Twitch channel to open its "
                "popout chat.");
            return "";
        }
    }

    stripChannelName(target);
    QDesktopServices::openUrl(QUrl(
        QString("https://www.twitch.tv/popout/%1/chat?popout=").arg(target)));

    return "";
}

QString popup(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    static const auto *usageMessage =
        "Usage: /popup [channel]. Open specified Twitch channel in "
        "a new window. If no channel argument is specified, open "
        "the currently selected split instead.";

    QString target(ctx.words.value(1));
    stripChannelName(target);

    // Popup the current split
    if (target.isEmpty())
    {
        auto *currentPage =
            dynamic_cast<SplitContainer *>(getApp()
                                               ->getWindows()
                                               ->getMainWindow()
                                               .getNotebook()
                                               .getSelectedPage());
        if (currentPage != nullptr)
        {
            auto *currentSplit = currentPage->getSelectedSplit();
            if (currentSplit != nullptr)
            {
                currentSplit->popup();

                return "";
            }
        }

        ctx.channel->addSystemMessage(usageMessage);
        return "";
    }

    // Open channel passed as argument in a popup
    auto targetChannel = getApp()->getTwitch()->getOrAddChannel(target);
    getApp()->getWindows()->openInPopup(targetChannel);

    return "";
}

QString clearmessages(const CommandContext &ctx)
{
    (void)ctx;

    auto *currentPage = getApp()
                            ->getWindows()
                            ->getLastSelectedWindow()
                            ->getNotebook()
                            .getSelectedPage();

    if (auto *split = currentPage->getSelectedSplit())
    {
        split->getChannelView().clearMessages();
    }

    return "";
}

QString openURL(const CommandContext &ctx)
{
    /**
     * The /openurl command
     * Takes a positional argument as the URL to open
     *
     * Accepts the option --private or --no-private (or --incognito or --no-incognito).
     * These options will force the URL to be opened in private or non-private mode, regardless of the
     * default incognito mode setting.
     *
     * Examples:
     *  - /openurl https://twitch.tv/forsen
     *    with the setting "Open links in incognito/private mode" enabled
     *    Opens https://twitch.tv/forsen in private mode
     *  - /openurl https://twitch.tv/forsen
     *    with the setting "Open links in incognito/private mode" disabled
     *    Opens https://twitch.tv/forsen in normal mode
     *  - /openurl https://twitch.tv/forsen --private
     *    with the setting "Open links in incognito/private mode" disabled
     *    Opens https://twitch.tv/forsen in private mode
     *  - /openurl https://twitch.tv/forsen --no-private
     *    with the setting "Open links in incognito/private mode" enabled
     *    Opens https://twitch.tv/forsen in normal mode
     */
    if (ctx.channel == nullptr)
    {
        return "";
    }

    QCommandLineParser parser;
    parser.setOptionsAfterPositionalArgumentsMode(
        QCommandLineParser::ParseAsPositionalArguments);
    parser.addPositionalArgument("URL", "The URL to open");
    QCommandLineOption privateModeOption(
        {
            "private",
            "incognito",
        },
        "Force private mode. Cannot be used together with --no-private");
    QCommandLineOption noPrivateModeOption(
        {
            "no-private",
            "no-incognito",
        },
        "Force non-private mode. Cannot be used together with --private");
    parser.addOptions({
        privateModeOption,
        noPrivateModeOption,
    });
    parser.parse(ctx.words);

    const auto &positionalArguments = parser.positionalArguments();
    if (positionalArguments.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "Usage: /openurl <URL> [--incognito/--no-incognito]");
        return "";
    }
    auto urlString = parser.positionalArguments().join(' ');

    QUrl url = QUrl::fromUserInput(urlString);
    if (!url.isValid())
    {
        ctx.channel->addSystemMessage("Invalid URL specified.");
        return "";
    }

    auto preferPrivateMode = getSettings()->openLinksIncognito.getValue();
    auto forcePrivateMode = parser.isSet(privateModeOption);
    auto forceNonPrivateMode = parser.isSet(noPrivateModeOption);

    if (forcePrivateMode && forceNonPrivateMode)
    {
        ctx.channel->addSystemMessage(
            "Error: /openurl may only be called with --incognito or "
            "--no-incognito, not both at the same time.");
        return "";
    }

    bool usePrivateMode = false;

    if (forceNonPrivateMode)
    {
        usePrivateMode = false;
    }
    else if (supportsIncognitoLinks() &&
             (forcePrivateMode || preferPrivateMode))
    {
        usePrivateMode = true;
    }

    bool res = false;
    if (usePrivateMode)
    {
        res = openLinkIncognito(url.toString(QUrl::FullyEncoded));
    }
    else
    {
        res = QDesktopServices::openUrl(url);
    }

    if (!res)
    {
        ctx.channel->addSystemMessage("Could not open URL.");
    }

    return "";
}

QString sendRawMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.channel->isTwitchChannel())
    {
        getApp()->getTwitch()->sendRawMessage(ctx.words.mid(1).join(" "));
    }
    else
    {
        // other code down the road handles this for IRC
        return ctx.words.join(" ");
    }
    return "";
}

QString injectFakeMessage(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (!ctx.channel->isTwitchChannel())
    {
        ctx.channel->addSystemMessage(
            "The /fakemsg command only works in Twitch channels.");
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            "Usage: /fakemsg (raw irc text) - injects raw irc text as "
            "if it was a message received from TMI");
        return "";
    }

    auto ircText = ctx.words.mid(1).join(" ");
    getApp()->getTwitch()->addFakeMessage(ircText);

    return "";
}

QString injectStreamUpdateNoStream(const CommandContext &ctx)
{
    /**
     * /debug-update-to-no-stream makes the current channel mimic going offline
     */
    if (ctx.channel == nullptr)
    {
        return "";
    }
    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /debug-update-to-no-stream command only "
            "works in Twitch channels");
        return "";
    }

    ctx.twitchChannel->updateStreamStatus(std::nullopt, false);
    return "";
}

QString copyToClipboard(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /copy <text> - copies provided "
                                      "text to clipboard.");
        return "";
    }

    crossPlatformCopy(ctx.words.mid(1).join(" "));
    return "";
}

QString unstableSetUserClientSideColor(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /unstable-set-user-color command only "
            "works in Twitch channels.");
        return "";
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            QString("Usage: %1 <TwitchUserID> [color]").arg(ctx.words.at(0)));
        return "";
    }

    auto userID = ctx.words.at(1);

    auto color = ctx.words.value(2);

    getApp()->getUserData()->setUserColor(userID, color);

    return "";
}

QString openUsercard(const CommandContext &ctx)
{
    auto channel = ctx.channel;

    if (channel == nullptr)
    {
        return "";
    }

    if (ctx.words.size() < 2)
    {
        channel->addSystemMessage("Usage: /usercard <username> [channel] or "
                                  "/usercard id:<id> [channel]");
        return "";
    }

    QString userName = ctx.words[1];
    stripUserName(userName);

    if (ctx.words.size() > 2)
    {
        QString channelName = ctx.words[2];
        stripChannelName(channelName);

        ChannelPtr channelTemp =
            getApp()->getTwitch()->getChannelOrEmpty(channelName);

        if (channelTemp->isEmpty())
        {
            channel->addSystemMessage(
                "A usercard can only be displayed for a channel that is "
                "currently opened in Chatterino.");
            return "";
        }

        channel = channelTemp;
    }

    // try to link to current split if possible
    Split *currentSplit = nullptr;
    auto *currentPage = dynamic_cast<SplitContainer *>(getApp()
                                                           ->getWindows()
                                                           ->getMainWindow()
                                                           .getNotebook()
                                                           .getSelectedPage());
    if (currentPage != nullptr)
    {
        currentSplit = currentPage->getSelectedSplit();
    }

    auto differentChannel =
        currentSplit != nullptr && currentSplit->getChannel() != channel;
    if (differentChannel || currentSplit == nullptr)
    {
        // not possible to use current split, try searching for one
        const auto &notebook =
            getApp()->getWindows()->getMainWindow().getNotebook();
        auto count = notebook.getPageCount();
        for (int i = 0; i < count; i++)
        {
            auto *page = notebook.getPageAt(i);
            auto *container = dynamic_cast<SplitContainer *>(page);
            assert(container != nullptr);
            for (auto *split : container->getSplits())
            {
                if (split->getChannel() == channel)
                {
                    currentSplit = split;
                    break;
                }
            }
        }

        // This would have crashed either way.
        assert(currentSplit != nullptr &&
               "something went HORRIBLY wrong with the /usercard "
               "command. It couldn't find a split for a channel which "
               "should be open.");
    }

    auto *userPopup =
        new UserInfoPopup(getSettings()->autoCloseUserPopup, currentSplit);
    userPopup->setData(userName, channel);

    QPoint center = QCursor::pos();
    if (currentSplit != nullptr && currentSplit->window() != nullptr)
    {
        center = currentSplit->window()->geometry().center();
    }

    userPopup->show();
    const auto size = userPopup->size();
    userPopup->showAndMoveTo(
        center - QPoint(size.width() / 2, size.height() / 2),
        widgets::BoundsChecking::DesiredPosition);
    userPopup->raise();
    userPopup->activateWindow();
    return "";
}

QString openLogs(const CommandContext &ctx)
{
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage("Usage: /logs <user> [channel]");
        return "";
    }

    QString userName = ctx.words[1];
    stripUserName(userName);
    QString channelName = ctx.channel->getName();

    if (ctx.words.size() > 2)
    {
        channelName = ctx.words[2];
        stripUserName(channelName);
    }

    QString urlStr =
        "https://tv.supa.sh/logs?c=" + channelName + "&u=" + userName;
    QUrl url = QUrl::fromUserInput(urlStr);

    bool res = false;
    if (supportsIncognitoLinks() && getSettings()->openLinksIncognito)
    {
        res = openLinkIncognito(url.toString(QUrl::FullyEncoded));
    }
    else
    {
        res = QDesktopServices::openUrl(url);
    }

    if (!res)
    {
        ctx.channel->addSystemMessage("Could not open URL.");
    }

    return "";
}

}  // namespace chatterino::commands
