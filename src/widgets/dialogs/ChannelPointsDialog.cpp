#include "widgets/dialogs/ChannelPointsDialog.hpp"

#if MOLTORINO_ENABLE_CHANNEL_POINT_REWARDS

#include "Application.hpp"
#include "messages/Image.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/helper/Line.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace chatterino {

namespace {

constexpr QSize DEFAULT_DIALOG_SIZE(326, 402);
constexpr int HEADER_SEPARATOR_HEIGHT = 8;
constexpr float CONTENT_SCALE_MULTIPLIER = 1.08F;
constexpr int REWARD_GRID_COLUMNS = 3;
constexpr int EMOTE_GRID_COLUMNS = 4;
constexpr int EMOTE_GRID_INITIAL_LIMIT = 48;
constexpr int EMOTE_GRID_LIMIT_STEP = 48;

float contentScale(float scale)
{
    const float taper = std::clamp((scale - 1.0F) / 0.6F, 0.0F, 1.0F);
    return scale * (CONTENT_SCALE_MULTIPLIER - taper * 0.12F);
}

float readableFontScale(float scale)
{
    return std::max(0.68F, scale);
}

int scaledMetric(float scale, int base, int minimum)
{
    return std::max(minimum, int(std::round(base * scale)));
}

int rewardCardMinimumWidth(float scale)
{
    return scaledMetric(scale, 72, 38);
}

int rewardCardPreferredWidth(float scale)
{
    return std::max(rewardCardMinimumWidth(scale),
                    scaledMetric(scale, 92, 48));
}

int rewardTitleMinimumHeight(float scale)
{
    return scaledMetric(scale, 32, 22);
}

int emoteTileMinimumWidth(float scale)
{
    return scaledMetric(scale, 58, 38);
}

QSize emoteTileSize(float scale)
{
    return {scaledMetric(scale, 62, 42), scaledMetric(scale, 48, 34)};
}

int gridColumnsForWidth(int availableWidth, int minimumItemWidth, int spacing,
                        int maximumColumns, int minimumColumns = 1)
{
    maximumColumns = std::max(1, maximumColumns);
    minimumColumns = std::clamp(minimumColumns, 1, maximumColumns);
    if (availableWidth <= 0)
    {
        return minimumColumns;
    }

    return std::clamp((availableWidth + spacing) /
                          std::max(1, minimumItemWidth + spacing),
                      minimumColumns, maximumColumns);
}

int availableContentWidth(QScrollArea *scrollArea, QLayout *layout)
{
    int width = 0;
    if (scrollArea != nullptr && scrollArea->viewport() != nullptr)
    {
        width = scrollArea->viewport()->width();
    }
    if (layout != nullptr)
    {
        const auto margins = layout->contentsMargins();
        width -= margins.left() + margins.right();
    }
    return std::max(1, width);
}

template <typename T>
QVector<T *> directChildWidgets(QWidget *parent)
{
    QVector<T *> widgets;
    if (parent == nullptr)
    {
        return widgets;
    }

    for (auto *child : parent->children())
    {
        if (auto *widget = dynamic_cast<T *>(child))
        {
            widgets.push_back(widget);
        }
    }
    return widgets;
}

int scaledSeparatorHeight(float scale)
{
    return std::max(1, int(HEADER_SEPARATOR_HEIGHT * scale));
}

QString compactPoints(qint64 value)
{
    return value >= 0 ? formatCompactNumber(value) : QStringLiteral("...");
}

QString fullPoints(qint64 value)
{
    return formatChannelPoints(value);
}

QString emoteImageUrl(const GqlChannelPointEmote &emote)
{
    return QStringLiteral(
               "https://static-cdn.jtvnw.net/emoticons/v2/%1/static/dark/2.0")
        .arg(emote.id);
}

ImagePtr emoteImage(const GqlChannelPointEmote &emote)
{
    if (emote.id.isEmpty())
    {
        return Image::getEmpty();
    }

    return Image::fromUrl(Url{emoteImageUrl(emote)}, 1, {56, 56});
}

ImagePtr rewardImage(const GqlChannelPointReward &reward)
{
    if (reward.imageUrl.isEmpty())
    {
        return Image::getEmpty();
    }

    return Image::fromUrl(Url{reward.imageUrl}, 1, {96, 96});
}

bool drawLoadedImage(QPainter &painter, const QRectF &target,
                     const ImagePtr &image)
{
    if (!image || image->isEmpty())
    {
        return false;
    }

    const auto pixmap = image->pixmapOrLoad();
    if (!pixmap)
    {
        return false;
    }

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QSizeF size = pixmap->size();
    size.scale(target.size(), Qt::KeepAspectRatio);
    const QRectF drawRect(target.center().x() - size.width() / 2.0,
                          target.center().y() - size.height() / 2.0,
                          size.width(), size.height());
    painter.drawPixmap(drawRect, *pixmap, pixmap->rect());
    painter.restore();
    return true;
}

QString emoteLabel(const GqlChannelPointEmote &emote)
{
    if (!emote.token.isEmpty())
    {
        return emote.token;
    }
    if (!emote.id.isEmpty())
    {
        return QStringLiteral("Emote %1").arg(emote.id.right(6));
    }
    return QStringLiteral("Emote");
}

QString emoteSearchText(const GqlChannelPointEmote &emote)
{
    return emote.token + QStringLiteral(" ") + emote.id;
}

QString emoteTooltip(const GqlChannelPointEmote &emote)
{
    const auto label = emote.token.isEmpty() ? emote.id : emote.token;
    if (emote.ownerDisplayName.isEmpty())
    {
        return label;
    }
    return QStringLiteral("%1\n%2").arg(label, emote.ownerDisplayName);
}

const GqlChannelPointEmoteModification *findEmoteModification(
    const GqlChannelPointEmote &emote, const QString &modifierId)
{
    for (const auto &modification : emote.modifications)
    {
        if (modification.modifierId == modifierId)
        {
            return &modification;
        }
    }

    return nullptr;
}

bool hasEmoteForModifier(const QVector<GqlChannelPointEmote> &emotes,
                         const QString &modifierId)
{
    for (const auto &emote : emotes)
    {
        if (findEmoteModification(emote, modifierId) != nullptr)
        {
            return true;
        }
    }

    return false;
}

QString firstAvailableModifierId(
    const QVector<GqlChannelPointEmoteModifier> &modifiers,
    const QVector<GqlChannelPointEmote> &emotes)
{
    for (const auto &modifier : modifiers)
    {
        if (hasEmoteForModifier(emotes, modifier.id))
        {
            return modifier.id;
        }
    }

    return modifiers.isEmpty() ? QString() : modifiers.front().id;
}

QColor rewardColor(const GqlChannelPointReward &reward)
{
    static const std::array<QColor, 12> palette = {{
        QColor("#ff1493"), QColor("#ff8500"), QColor("#9146ff"),
        QColor("#00e015"), QColor("#ff8080"), QColor("#ff1a1a"),
        QColor("#ffbd14"), QColor("#20a8ff"), QColor("#2d7f33"),
        QColor("#12d6c9"), QColor("#ff40d5"), QColor("#7c3cff"),
    }};

    auto color = QColor(reward.backgroundColor);
    if (color.isValid())
    {
        auto hsv = color.toHsv();
        hsv.setHsv(hsv.hue(), std::max(190, hsv.saturation()),
                   std::max(235, hsv.value()), 255);
        color = hsv.toRgb();
        return color;
    }

    const auto seed = reward.id + reward.title + reward.rewardType;
    return palette.at(qHash(seed) % palette.size());
}

bool isSupportedAutomaticReward(const GqlChannelPointReward &reward)
{
    return reward.rewardType == "SINGLE_MESSAGE_BYPASS_SUB_MODE" ||
           reward.rewardType == "SEND_HIGHLIGHTED_MESSAGE" ||
           reward.rewardType == "RANDOM_SUB_EMOTE_UNLOCK" ||
           reward.rewardType == "CHOSEN_SUB_EMOTE_UNLOCK" ||
           reward.rewardType == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK";
}

bool shouldShowReward(const GqlChannelPointReward &reward)
{
    if (!reward.isEnabled || !reward.isInStock)
    {
        return false;
    }
    return !reward.isAutomatic || isSupportedAutomaticReward(reward);
}

float rewardImageSizeRatio(const GqlChannelPointReward &reward)
{
    return isSupportedAutomaticReward(reward) ? 0.40F : 0.50F;
}

QStringList twoLineTitle(const QString &title, const QFontMetrics &metrics,
                         int width)
{
    width = std::max(1, width);
    const auto words = title.simplified().split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty())
    {
        return {};
    }

    QStringList lines;
    QString current;
    for (const auto &word : words)
    {
        const auto candidate = current.isEmpty() ? word : current + ' ' + word;
        if (metrics.horizontalAdvance(candidate) <= width ||
            current.isEmpty())
        {
            current = candidate;
            continue;
        }

        lines.push_back(current);
        current = word;
        if (lines.size() == 1)
        {
            continue;
        }

        break;
    }

    if (lines.size() < 2 && !current.isEmpty())
    {
        lines.push_back(current);
    }
    if (lines.size() > 2)
    {
        lines = lines.mid(0, 2);
    }
    if (lines.size() == 2)
    {
        lines[1] = metrics.elidedText(lines[1], Qt::ElideRight, width);
    }
    else if (lines.size() == 1)
    {
        lines[0] = metrics.elidedText(lines[0], Qt::ElideRight, width);
    }

    return lines;
}

void drawRewardGlyph(QPainter &painter, const QRectF &square,
                     const GqlChannelPointReward &reward, bool unavailable)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor glyph(255, 255, 255, unavailable ? 155 : 245);
    QPen pen(glyph, std::max<qreal>(2.4, square.width() * 0.045),
             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const auto cx = square.center().x();
    const auto cy = square.center().y();
    const auto size = square.width();

    if (reward.rewardType == "SEND_HIGHLIGHTED_MESSAGE" ||
        reward.isUserInputRequired)
    {
        QRectF bubble(cx - size * 0.15, cy - size * 0.12, size * 0.30,
                      size * 0.22);
        painter.setBrush(glyph);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(bubble, 2, 2);
        QPolygonF tail;
        tail << QPointF(bubble.left() + bubble.width() * 0.35,
                        bubble.bottom() - 1)
             << QPointF(bubble.left() + bubble.width() * 0.52,
                        bubble.bottom() - 1)
             << QPointF(bubble.left() + bubble.width() * 0.30,
                        bubble.bottom() + size * 0.08);
        painter.drawPolygon(tail);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        painter.drawLine(QPointF(cx + size * 0.10, cy - size * 0.19),
                         QPointF(cx + size * 0.10, cy - size * 0.27));
        painter.drawLine(QPointF(cx + size * 0.06, cy - size * 0.23),
                         QPointF(cx + size * 0.14, cy - size * 0.23));
    }
    else if (reward.rewardType == "SINGLE_MESSAGE_BYPASS_SUB_MODE")
    {
        QRectF bubble(cx - size * 0.16, cy - size * 0.05, size * 0.28,
                      size * 0.20);
        painter.drawRoundedRect(bubble, 2, 2);
        painter.drawArc(QRectF(cx - size * 0.08, cy - size * 0.19,
                               size * 0.16, size * 0.16),
                        0, 180 * 16);
    }
    else if (reward.rewardType == "RANDOM_SUB_EMOTE_UNLOCK")
    {
        QRectF dice(cx - size * 0.15, cy - size * 0.15, size * 0.30,
                    size * 0.30);
        painter.drawRoundedRect(dice, 3, 3);
        painter.setBrush(glyph);
        painter.setPen(Qt::NoPen);
        const auto dot = size * 0.025;
        for (const auto point :
             {QPointF(cx - size * 0.07, cy - size * 0.07),
              QPointF(cx, cy), QPointF(cx + size * 0.07, cy + size * 0.07)})
        {
            painter.drawEllipse(point, dot, dot);
        }
    }
    else if (reward.rewardType == "CHOSEN_SUB_EMOTE_UNLOCK")
    {
        QRectF body(cx - size * 0.13, cy - size * 0.04, size * 0.26,
                    size * 0.22);
        painter.drawRoundedRect(body, 2, 2);
        painter.drawArc(QRectF(cx - size * 0.10, cy - size * 0.19,
                               size * 0.20, size * 0.22),
                        0, 180 * 16);
    }
    else if (reward.rewardType == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK")
    {
        painter.drawLine(QPointF(cx - size * 0.14, cy + size * 0.14),
                         QPointF(cx + size * 0.12, cy - size * 0.12));
        painter.drawLine(QPointF(cx + size * 0.10, cy - size * 0.20),
                         QPointF(cx + size * 0.10, cy - size * 0.30));
        painter.drawLine(QPointF(cx + size * 0.05, cy - size * 0.25),
                         QPointF(cx + size * 0.15, cy - size * 0.25));
        painter.drawLine(QPointF(cx - size * 0.05, cy - size * 0.12),
                         QPointF(cx - size * 0.05, cy - size * 0.22));
        painter.drawLine(QPointF(cx - size * 0.10, cy - size * 0.17),
                         QPointF(cx, cy - size * 0.17));
    }
    else
    {
        QRectF ring(cx - size * 0.16, cy - size * 0.16, size * 0.32,
                    size * 0.32);
        painter.drawArc(ring, 25 * 16, 300 * 16);
        painter.drawPoint(QPointF(cx - size * 0.075, cy - size * 0.04));
    }

    painter.restore();
}

TwitchChannel *twitchChannelFromWeak(const std::weak_ptr<Channel> &weak)
{
    auto shared = weak.lock();
    if (!shared)
    {
        return nullptr;
    }
    return dynamic_cast<TwitchChannel *>(shared.get());
}

void applyChannelPointRedeemResult(const std::weak_ptr<Channel> &weak,
                                   const GqlChannelPointRedeemResult &result,
                                   const QString &message,
                                   int fallbackCost = 0)
{
    auto *channel = twitchChannelFromWeak(weak);
    if (channel == nullptr)
    {
        return;
    }
    if (result.balance >= 0)
    {
        channel->setChannelPointBalance(result.balance);
    }
    else if (fallbackCost > 0)
    {
        const auto balance = channel->channelPointBalance();
        if (balance >= fallbackCost)
        {
            channel->setChannelPointBalance(balance - fallbackCost);
        }
    }
    channel->addSystemMessage(message);
}

void showChannelPointRedeemError(const std::weak_ptr<Channel> &weak,
                                 const QString &context,
                                 const QString &error)
{
    auto *channel = twitchChannelFromWeak(weak);
    if (channel == nullptr)
    {
        return;
    }
    channel->addSystemMessage(
        MoltorinoAuth::normalizeAuthError(context, error));
}

class RewardCardButton final : public QPushButton
{
public:
    RewardCardButton(const GqlChannelPointReward &reward, qint64 balance,
                     QWidget *parent)
        : QPushButton(parent)
        , reward_(reward)
        , image_(rewardImage(reward))
        , balance_(balance)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setMouseTracking(true);
        this->setAttribute(Qt::WA_Hover, true);
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        this->setSizePolicy(policy);
        this->setToolTip(reward.prompt.isEmpty()
                             ? reward.title
                             : QStringLiteral("%1\n%2")
                                   .arg(reward.title, reward.prompt));
        this->setFlat(true);
    }

    void setScale(float scale)
    {
        this->scale_ = scale;
        const auto minimumWidth = rewardCardMinimumWidth(scale);
        this->setMinimumSize(minimumWidth, this->heightForWidth(minimumWidth));
        this->updateGeometry();
    }

    QSize sizeHint() const override
    {
        const auto width = rewardCardPreferredWidth(this->scale_);
        return {width, this->heightForWidth(width)};
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth(int width) const override
    {
        const auto titleHeight = this->titleAreaHeight();
        return std::max(1, width) + titleHeight;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto rect = this->rect();
        bool unavailable =
            !this->reward_.isEnabled || !this->reward_.isInStock ||
            (this->balance_ >= 0 && this->balance_ < this->reward_.cost);
        if (!this->isEnabled())
        {
            unavailable = true;
        }

        const int titleHeight = this->titleAreaHeight();
        const int squareSize = std::max(
            1, std::min(rect.width(), std::max(1, rect.height() - titleHeight)));
        QRect square(rect.left() + (rect.width() - squareSize) / 2, rect.top(),
                     squareSize, squareSize);

        auto base = rewardColor(this->reward_);
        if (this->underMouse() && this->isEnabled())
        {
            base = base.lighter(108);
        }
        if (this->isDown())
        {
            base = base.darker(108);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(base);
        painter.drawRoundedRect(square.adjusted(0, 0, -1, -1), 2, 2);

        const auto imageSize =
            squareSize * rewardImageSizeRatio(this->reward_);
        const QRectF imageRect(square.center().x() - imageSize / 2.0,
                               square.center().y() - imageSize / 2.0,
                               imageSize, imageSize);
        const bool drewImage = drawLoadedImage(painter, imageRect, this->image_);
        if (!drewImage)
        {
            const auto glyphSize = squareSize * 0.50;
            const QRectF glyphRect(square.center().x() - glyphSize / 2.0,
                                   square.center().y() - glyphSize / 2.0,
                                   glyphSize, glyphSize);
            drawRewardGlyph(painter, glyphRect, this->reward_, unavailable);
        }

        if (unavailable)
        {
            painter.setBrush(QColor(0, 0, 0, 120));
            painter.drawRoundedRect(square.adjusted(0, 0, -1, -1), 2, 2);
        }

        auto costFont = this->font();
        costFont.setBold(true);
        costFont.setPointSizeF(std::max(6.5, costFont.pointSizeF() * 0.82));
        painter.setFont(costFont);
        QFontMetrics costMetrics(costFont);
        const auto costText = compactPoints(this->reward_.cost);
        const int pillPaddingX = scaledMetric(this->scale_, 5, 3);
        const int pillHeight = scaledMetric(this->scale_, 17, 12);
        const int pillWidth = std::max(
            scaledMetric(this->scale_, 30, 22),
            costMetrics.horizontalAdvance(costText) + pillPaddingX * 2);
        QRect pill(square.left() + (square.width() - pillWidth) / 2,
                   square.bottom() - pillHeight -
                       scaledMetric(this->scale_, 6, 3),
                   pillWidth, pillHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, unavailable ? 82 : 118));
        painter.drawRoundedRect(pill, 2, 2);

        painter.setPen(QColor(255, 255, 255, unavailable ? 150 : 245));
        painter.drawText(pill, Qt::AlignCenter, costText);

        auto titleFont = this->font();
        titleFont.setBold(true);
        painter.setFont(titleFont);
        auto titleColor = this->palette().color(QPalette::WindowText);
        titleColor.setAlpha(unavailable ? 150 : 245);
        painter.setPen(titleColor);

        const auto titleTop =
            square.bottom() + scaledMetric(this->scale_, 4, 2);
        const QRect titleRect(rect.left() + 2, titleTop,
                              std::max(1, rect.width() - 4),
                              std::max(1, rect.bottom() - titleTop + 1));
        QFontMetrics titleMetrics(titleFont);
        const auto lines =
            twoLineTitle(this->reward_.title, titleMetrics, titleRect.width());
        const int lineHeight = titleMetrics.lineSpacing();
        const int textHeight = lineHeight * lines.size();
        int y = titleRect.top() +
                std::max(0, (titleRect.height() - textHeight) / 2);
        for (const auto &line : lines)
        {
            painter.drawText(QRect(titleRect.left(), y, titleRect.width(),
                                   lineHeight),
                             Qt::AlignCenter, line);
            y += lineHeight;
        }

        if (this->hasFocus())
        {
            painter.setPen(QPen(QColor(255, 255, 255, 210), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(square.adjusted(1, 1, -2, -2), 2, 2);
        }
    }

private:
    int titleAreaHeight() const
    {
        QFont titleFont = this->font();
        titleFont.setBold(true);
        QFontMetrics metrics(titleFont);
        return std::max(rewardTitleMinimumHeight(this->scale_),
                        metrics.lineSpacing() * 2 +
                            scaledMetric(this->scale_, 5, 3));
    }

    GqlChannelPointReward reward_;
    ImagePtr image_;
    qint64 balance_ = -1;
    float scale_ = 1.F;
};

class EmoteTileButton final : public QPushButton
{
public:
    EmoteTileButton(const GqlChannelPointEmote &emote, bool loadImage,
                    QWidget *parent)
        : QPushButton(parent)
        , emote_(emote)
        , image_(loadImage ? emoteImage(emote) : Image::getEmpty())
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::StrongFocus);
        this->setMouseTracking(true);
        this->setAttribute(Qt::WA_Hover, true);
        this->setFlat(true);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->setToolTip(emoteTooltip(emote));
    }

    void setScale(float scale)
    {
        this->scale_ = scale;
        this->setMinimumSize(this->sizeHint());
        this->updateGeometry();
    }

    QSize sizeHint() const override
    {
        return emoteTileSize(this->scale_);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const auto *theme = getApp()->getThemes();
        auto bg = theme->splits.input.background;
        auto text = theme->window.text;
        auto border = theme->splits.header.border;
        if (this->underMouse() && this->isEnabled())
        {
            bg = theme->isLightTheme() ? bg.darker(104) : bg.lighter(108);
            border = theme->splits.header.focusedBorder;
        }
        if (this->isDown())
        {
            bg = theme->isLightTheme() ? bg.darker(110) : bg.lighter(114);
        }

        const auto rect = this->rect().adjusted(0, 0, -1, -1);
        painter.setPen(QPen(border, 1));
        painter.setBrush(bg);
        painter.drawRect(rect);

        const int imageSize = scaledMetric(this->scale_, 28, 18);
        const int textHeight = scaledMetric(this->scale_, 15, 10);
        const int imageTop = scaledMetric(this->scale_, 5, 3);
        const QRect imageRect((this->width() - imageSize) / 2, imageTop,
                              imageSize, imageSize);

        if (!this->image_->isEmpty())
        {
            if (auto pixmap = this->image_->pixmapOrLoad())
            {
                painter.drawPixmap(imageRect, *pixmap, pixmap->rect());
            }
            else
            {
                auto muted = text;
                muted.setAlpha(70);
                painter.setPen(Qt::NoPen);
                painter.setBrush(muted);
                painter.drawRect(imageRect);
            }
        }
        else
        {
            auto placeholder = text;
            placeholder.setAlpha(60);
            painter.setPen(Qt::NoPen);
            painter.setBrush(placeholder);
            painter.drawRect(imageRect);
        }

        auto labelFont = this->font();
        labelFont.setPointSizeF(labelFont.pointSizeF() * 0.86);
        painter.setFont(labelFont);
        QFontMetrics metrics(labelFont);
        const auto labelWidth =
            this->width() - scaledMetric(this->scale_, 10, 6);
        const auto label =
            metrics.elidedText(emoteLabel(this->emote_), Qt::ElideRight,
                               labelWidth);
        QRect labelRect(4, this->height() - textHeight - 3,
                        this->width() - 8, textHeight);
        painter.setPen(text);
        painter.drawText(labelRect, Qt::AlignCenter, label);

        if (this->hasFocus())
        {
            auto focus = text;
            focus.setAlpha(210);
            painter.setPen(QPen(focus, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect.adjusted(1, 1, -1, -1));
        }
    }

private:
    GqlChannelPointEmote emote_;
    ImagePtr image_;
    float scale_ = 1.F;
};

class RewardIconPreview final : public QWidget
{
public:
    RewardIconPreview(const GqlChannelPointReward &reward, QWidget *parent)
        : QWidget(parent)
        , reward_(reward)
        , image_(rewardImage(reward))
    {
        this->setObjectName("ChannelPointsRewardPreview");
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setScale(float scale)
    {
        this->scale_ = scale;
        const int size = std::max(62, int(64 * scale));
        this->setFixedSize(size, size);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const auto rect = this->rect().adjusted(0, 0, -1, -1);
        painter.setPen(Qt::NoPen);
        painter.setBrush(rewardColor(this->reward_));
        painter.drawRoundedRect(rect, 2, 2);

        const auto imageSize =
            rect.width() * rewardImageSizeRatio(this->reward_);
        const QRectF imageRect(rect.center().x() - imageSize / 2.0,
                               rect.center().y() - imageSize / 2.0, imageSize,
                               imageSize);
        if (!drawLoadedImage(painter, imageRect, this->image_))
        {
            const auto glyphSize = rect.width() * 0.50;
            const QRectF glyphRect(rect.center().x() - glyphSize / 2.0,
                                   rect.center().y() - glyphSize / 2.0,
                                   glyphSize, glyphSize);
            drawRewardGlyph(painter, glyphRect, this->reward_, false);
        }
    }

private:
    GqlChannelPointReward reward_;
    ImagePtr image_;
    float scale_ = 1.F;
};

}  // namespace

std::vector<QPointer<ChannelPointsDialog>> ChannelPointsDialog::activeDialogs_;

ChannelPointsDialog::ChannelPointsDialog(TwitchChannel *channel,
                                         SplitInput *input, QWidget *parent)
    : DraggablePopup(true, parent)
    , channel_(channel)
    , input_(input)
{
    this->setAttribute(Qt::WA_DeleteOnClose);
    this->setObjectName("ChannelPointsDialog");
    this->setWindowTitle("Channel Points");
    this->setScaleIndependentSize(DEFAULT_DIALOG_SIZE);
    this->layoutRefreshTimer_.setSingleShot(true);
    QObject::connect(&this->layoutRefreshTimer_, &QTimer::timeout, this,
                     [this] {
                         if (this->contentWidget_ != nullptr)
                         {
                             this->refreshCurrentLayout();
                         }
                     });

    auto *container = this->getLayoutContainer();
    container->setObjectName("ChannelPointsDialogRoot");
    this->mainLayout_ = new QVBoxLayout(container);
    this->mainLayout_->setSpacing(0);

    this->headerWidget_ = new QWidget(container);
    this->headerWidget_->setObjectName("ChannelPointsDialogHeader");
    auto *headerLayout = new QHBoxLayout(this->headerWidget_);
    const int margin = std::max(7, int(8 * this->scale()));
    headerLayout->setContentsMargins(margin, 3, margin, 2);

    auto *headerTextLayout = new QVBoxLayout();
    headerTextLayout->setContentsMargins(0, 0, 0, 0);

    this->backButton_ = new QPushButton("<", this->headerWidget_);
    this->backButton_->setObjectName("ChannelPointsHeaderBackButton");
    this->backButton_->setToolTip("Back to rewards");
    this->backButton_->setCursor(Qt::PointingHandCursor);
    this->backButton_->hide();
    QObject::connect(this->backButton_, &QPushButton::clicked, this,
                     [this] { this->showRewardsView(); });
    headerLayout->addWidget(this->backButton_);

    this->headerTitleLabel_ =
        new QLabel("Channel Rewards", this->headerWidget_);
    this->headerTitleLabel_->setObjectName("ChannelPointsHeaderTitle");
    this->headerSubtitleLabel_ = new QLabel("", this->headerWidget_);
    this->headerSubtitleLabel_->setObjectName("ChannelPointsHeaderSubtitle");
    headerTextLayout->addWidget(this->headerTitleLabel_);
    headerTextLayout->addWidget(this->headerSubtitleLabel_);
    headerLayout->addLayout(headerTextLayout);
    headerLayout->addStretch(1);

    this->pinButton_ = this->createPinButton();
    this->pinButton_->setToolTip("Pin Rewards Popup");
    if (!getSettings()->rewardsCloseOnFocusLoss)
    {
        this->ensurePinned();
    }
    headerLayout->addWidget(this->pinButton_);

    this->closeButton_ = new SvgButton(
        {
            .dark = ":/buttons/cancel.svg",
            .light = ":/buttons/cancelDark.svg",
        },
        this, QSize{3, 3});
    this->closeButton_->setScaleIndependentSize(18, 18);
    this->closeButton_->setToolTip("Close");
    this->closeButton_->setCursor(Qt::PointingHandCursor);
    QObject::connect(this->closeButton_, &Button::leftClicked, this,
                     &QWidget::close);
    headerLayout->addWidget(this->closeButton_);
    this->mainLayout_->addWidget(this->headerWidget_);

    auto *separator = new Line(false);
    separator->setObjectName("ChannelPointsDialogSeparator");
    separator->setFixedHeight(scaledSeparatorHeight(this->scale()));
    this->mainLayout_->addWidget(separator);

    this->scrollArea_ = new QScrollArea(container);
    this->scrollArea_->setObjectName("ChannelPointsScrollArea");
    this->scrollArea_->setFrameShape(QFrame::NoFrame);
    this->scrollArea_->setWidgetResizable(true);
    this->scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->mainLayout_->addWidget(this->scrollArea_, 1);

    this->contentWidget_ = new QWidget();
    this->contentWidget_->setObjectName("ChannelPointsDialogContent");
    this->contentLayout_ = new QVBoxLayout(this->contentWidget_);
    this->contentLayout_->setContentsMargins(8, 7, 8, 8);
    this->contentLayout_->setSpacing(7);
    this->scrollArea_->setWidget(this->contentWidget_);

    this->channelPointsConnection_ =
        this->channel_->channelPointsChanged.connect([this] {
            this->refreshHeader();
        });

    this->refreshHeader();
    this->refreshStyle();
    this->rebuildContent();
}

void ChannelPointsDialog::showDialog(TwitchChannel *channel, SplitInput *input,
                                     QWidget *parent)
{
    if (channel == nullptr)
    {
        return;
    }

    for (auto it = activeDialogs_.begin(); it != activeDialogs_.end();)
    {
        if (it->isNull())
        {
            it = activeDialogs_.erase(it);
            continue;
        }
        if ((*it)->channel_ == channel)
        {
            (*it)->input_ = input;
            (*it)->raise();
            (*it)->activateWindow();
            (*it)->reloadRewards(true);
            return;
        }
        ++it;
    }

    auto *dialog = new ChannelPointsDialog(channel, input, parent);
    activeDialogs_.push_back(dialog);

    QPoint center = QCursor::pos();
    if (parent != nullptr && parent->window() != nullptr)
    {
        center = parent->window()->geometry().center();
    }

    dialog->show();
    const auto size = dialog->size();
    dialog->showAndMoveTo(
        center - QPoint(size.width() / 2, size.height() / 2),
        widgets::BoundsChecking::DesiredPosition);
    dialog->raise();
    dialog->activateWindow();
    dialog->reloadRewards(false);
}

void ChannelPointsDialog::themeChangedEvent()
{
    DraggablePopup::themeChangedEvent();
    this->refreshStyle();
}

void ChannelPointsDialog::scaleChangedEvent(float scale)
{
    DraggablePopup::scaleChangedEvent(scale);
    this->refreshStyle();
    this->applySizeConstraints();
    this->scheduleLayoutRefresh(0);
}

void ChannelPointsDialog::resizeEvent(QResizeEvent *event)
{
    DraggablePopup::resizeEvent(event);
    if (event->oldSize().isValid() &&
        event->size().width() != event->oldSize().width() &&
        (this->view_ == View::Rewards || this->view_ == View::Emotes))
    {
        this->scheduleLayoutRefresh();
    }
}

void ChannelPointsDialog::showEvent(QShowEvent *event)
{
    DraggablePopup::showEvent(event);
    if (!this->initialFetchDone_)
    {
        QTimer::singleShot(0, this, [this] {
            if (!this->initialFetchDone_)
            {
                this->reloadRewards(false);
            }
        });
    }
}

void ChannelPointsDialog::refreshHeader()
{
    const auto balance = this->channel_->channelPointBalance();
    const auto channelName =
        this->channel_->getName().isEmpty() ? QStringLiteral("channel")
                                            : this->channel_->getName();
    if (this->view_ == View::RewardDetail && this->selectedRewardValid_)
    {
        this->headerTitleLabel_->setText(this->selectedReward_.title);
    }
    else if (this->view_ == View::Emotes && this->selectedRewardValid_)
    {
        this->headerTitleLabel_->setText(this->selectedReward_.title);
    }
    else
    {
        this->headerTitleLabel_->setText(
            QStringLiteral("%1's Rewards").arg(channelName));
    }
    this->headerSubtitleLabel_->setText(
        QStringLiteral("Bal: %1").arg(fullPoints(balance)));
    this->backButton_->setVisible(this->view_ != View::Rewards);
}

void ChannelPointsDialog::refreshStyle()
{
    auto *fonts = getApp()->getFonts();
    const auto rawScale = this->scale();
    const auto effectiveScale = contentScale(rawScale);
    const auto headerScale = readableFontScale(rawScale * 1.30F);
    const int radius = std::max(1, int(2 * rawScale));
    const int inputPaddingX = std::max(4, int(5 * effectiveScale));
    const int inputMinHeight = std::max(14, int(20 * effectiveScale));
    this->headerTitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMediumBold, headerScale));
    this->headerSubtitleLabel_->setFont(
        fonts->getFont(FontStyle::UiMedium, headerScale));
    this->headerSubtitleLabel_->setStyleSheet(QString());
    this->backButton_->setFont(
        fonts->getFont(FontStyle::UiMediumBold, headerScale));

    this->headerWidget_->layout()->setContentsMargins(
        std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)),
        std::max(2, int(4 * rawScale)), std::max(2, int(3 * rawScale)));
    this->headerWidget_->layout()->setSpacing(std::max(2, int(3 * rawScale)));
    this->mainLayout_->setContentsMargins(std::max(3, int(5 * rawScale)),
                                          std::max(3, int(5 * rawScale)),
                                          std::max(3, int(5 * rawScale)),
                                          std::max(3, int(5 * rawScale)));
    this->mainLayout_->setSpacing(0);
    this->contentLayout_->setContentsMargins(
        scaledMetric(effectiveScale, 8, 4), scaledMetric(effectiveScale, 7, 4),
        scaledMetric(effectiveScale, 8, 4), scaledMetric(effectiveScale, 8, 4));
    this->contentLayout_->setSpacing(scaledMetric(effectiveScale, 7, 4));
    if (auto *separator =
            this->findChild<QWidget *>("ChannelPointsDialogSeparator"))
    {
        separator->setFixedHeight(scaledSeparatorHeight(rawScale));
    }

    if (this->statusLabel_ != nullptr)
    {
        this->statusLabel_->setFont(
            fonts->getFont(FontStyle::UiMedium,
                           readableFontScale(this->scale())));
    }

    const auto *theme = this->theme;
    auto textColor = theme->window.text;
    auto mutedColor = textColor;
    mutedColor.setAlpha(160);
    const auto bg = theme->window.background.name();
    const auto text = textColor.name(QColor::HexArgb);
    const auto border = theme->splits.header.border.name();
    const auto muted = mutedColor.name(QColor::HexArgb);
    const auto buttonBg = theme->splits.input.background.name();
    const auto fieldBg = theme->splits.input.background.name();
    const auto hoverBg =
        theme->isLightTheme() ? theme->splits.input.background.darker(104).name()
                              : theme->splits.input.background.lighter(108).name();
    const auto focusedBorder = theme->splits.header.focusedBorder.name();

    this->setStyleSheet(QStringLiteral(R"(
        QWidget#ChannelPointsDialogRoot {
            background: %1;
            color: %2;
        }
        QLabel#ChannelPointsHeaderTitle {
            color: %2;
            font-weight: 700;
        }
        QLabel#ChannelPointsHeaderSubtitle {
            color: %4;
        }
        QPushButton#ChannelPointsHeaderBackButton {
            background: transparent;
            color: %2;
            border: 0;
            padding: 0 5px 0 0;
            font-weight: 700;
        }
        QPushButton#ChannelPointsHeaderBackButton:hover {
            color: #ffffff;
        }
        QScrollArea#ChannelPointsScrollArea {
            background: transparent;
            border: 0;
        }
        QScrollArea#ChannelPointsEmoteScrollArea {
            background: transparent;
            border: 0;
        }
        QScrollBar:vertical {
            width: 0px;
            background: transparent;
        }
        QWidget#ChannelPointsDialogContent {
            background: transparent;
            color: %2;
        }
        QWidget#ChannelPointsEmoteGridContent {
            background: transparent;
        }
        QLabel#ChannelPointsStatusLabel {
            color: %4;
        }
        QLineEdit#ChannelPointsEmoteSearch {
            background: %6;
            color: %2;
            border: 1px solid %3;
            border-radius: %7px;
            padding: 0 %8px;
            min-height: %9px;
        }
        QPushButton#ChannelPointsUtilityButton,
        QPushButton#ChannelPointsModifierButton,
        QPushButton#ChannelPointsRedeemButton {
            background: %5;
            color: %2;
            border: 1px solid %3;
            border-radius: %7px;
            padding: 4px 8px;
            text-align: left;
        }
        QPushButton#ChannelPointsModifierButton {
            padding: 4px 5px;
            text-align: center;
        }
        QPushButton#ChannelPointsUtilityButton:hover,
        QPushButton#ChannelPointsModifierButton:hover,
        QPushButton#ChannelPointsRedeemButton:hover {
            background: %10;
            border-color: %11;
        }
        QPushButton#ChannelPointsRedeemButton {
            text-align: center;
            font-weight: 700;
        }
        QPushButton#ChannelPointsRedeemButton:disabled {
            color: %4;
            border-color: %3;
        }
        QPushButton#ChannelPointsModifierButton:checked {
            background: #3b2664;
            border-color: %11;
        }
    )")
                            .arg(bg, text, border, muted, buttonBg, fieldBg)
                            .arg(radius)
                            .arg(inputPaddingX)
                            .arg(inputMinHeight)
                            .arg(hoverBg, focusedBorder));
}

void ChannelPointsDialog::reloadRewards(bool force)
{
    if (this->rewardsLoading_ && !force)
    {
        return;
    }

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        this->initialFetchDone_ = true;
        this->rebuildContent();
        return;
    }

    this->initialFetchDone_ = true;
    this->rewardsLoading_ = true;
    this->setStatus("Loading rewards...");
    this->rebuildContent();

    QPointer<ChannelPointsDialog> self = this;
    TwitchGql::getChannelPointRewards(
        this->channel_->getName(), token,
        [self](GqlChannelPointRewards rewards) {
            if (!self)
            {
                return;
            }
            self->rewardsLoading_ = false;
            self->rewardsChannelId_ = rewards.channelId;
            self->rewards_ = std::move(rewards.rewards);
            self->rewardImageRefreshAttempts_ = 0;
            self->rewardImageRefreshQueued_ = false;
            self->setStatus({});
            if (rewards.balance >= 0)
            {
                self->channel_->setChannelPointBalance(rewards.balance);
            }
            self->refreshHeader();
            self->rebuildContent();
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->rewardsLoading_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "loading channel point rewards", error),
                            true);
            self->rebuildContent();
        });
}

void ChannelPointsDialog::rebuildContent()
{
    if (this->rebuildingContent_ || this->refreshingLayout_)
    {
        this->rebuildQueued_ = true;
        return;
    }

    this->rebuildingContent_ = true;
    this->layoutRefreshTimer_.stop();
    const bool updatesWereEnabled = this->updatesEnabled();
    if (updatesWereEnabled)
    {
        this->setUpdatesEnabled(false);
    }

    this->clearContent();
    this->refreshHeader();
    this->scrollArea_->verticalScrollBar()->setEnabled(this->view_ !=
                                                       View::Emotes);
    if (this->view_ == View::Emotes)
    {
        this->scrollArea_->verticalScrollBar()->setValue(0);
    }
    if (this->view_ == View::Emotes)
    {
        this->rebuildEmotes();
    }
    else if (this->view_ == View::RewardDetail)
    {
        this->rebuildRewardDetail();
    }
    else
    {
        this->rebuildRewards();
    }
    this->applySizeConstraints();

    if (updatesWereEnabled)
    {
        this->setUpdatesEnabled(true);
        this->update();
    }
    this->rebuildingContent_ = false;

    if (this->rebuildQueued_)
    {
        this->rebuildQueued_ = false;
        QTimer::singleShot(0, this, [this] {
            if (this->contentWidget_ != nullptr)
            {
                this->rebuildContent();
            }
        });
        return;
    }
    if (this->layoutRefreshQueued_)
    {
        this->layoutRefreshQueued_ = false;
        this->scheduleLayoutRefresh(0);
    }
}

void ChannelPointsDialog::refreshCurrentLayout()
{
    if (this->contentWidget_ == nullptr)
    {
        return;
    }
    if (this->rebuildingContent_)
    {
        this->layoutRefreshQueued_ = true;
        return;
    }
    if (this->refreshingLayout_)
    {
        return;
    }

    this->refreshingLayout_ = true;
    const bool updatesWereEnabled = this->updatesEnabled();
    if (updatesWereEnabled)
    {
        this->setUpdatesEnabled(false);
    }

    if (this->view_ == View::Emotes)
    {
        this->refreshEmotesLayout();
    }
    else if (this->view_ == View::RewardDetail)
    {
        this->refreshRewardDetailLayout();
    }
    else
    {
        this->refreshRewardsLayout();
    }
    this->applySizeConstraints();

    if (updatesWereEnabled)
    {
        this->setUpdatesEnabled(true);
        this->update();
    }
    this->refreshingLayout_ = false;

    if (this->rebuildQueued_)
    {
        this->rebuildQueued_ = false;
        QTimer::singleShot(0, this, [this] {
            if (this->contentWidget_ != nullptr)
            {
                this->rebuildContent();
            }
        });
        return;
    }
    if (this->layoutRefreshQueued_)
    {
        this->layoutRefreshQueued_ = false;
        this->scheduleLayoutRefresh(0);
    }
}

void ChannelPointsDialog::refreshRewardsLayout()
{
    auto *gridWidget = this->contentWidget_->findChild<QWidget *>(
        QStringLiteral("ChannelPointsRewardGridContent"));
    auto *grid =
        gridWidget == nullptr ? nullptr : dynamic_cast<QGridLayout *>(
                                      gridWidget->layout());
    if (grid == nullptr)
    {
        return;
    }

    const auto cards = directChildWidgets<RewardCardButton>(gridWidget);
    if (cards.isEmpty())
    {
        return;
    }

    const auto effectiveScale = contentScale(this->scale());
    const int horizontalSpacing = scaledMetric(effectiveScale, 5, 3);
    const int verticalSpacing = scaledMetric(effectiveScale, 6, 4);
    grid->setHorizontalSpacing(horizontalSpacing);
    grid->setVerticalSpacing(verticalSpacing);

    const int columns = gridColumnsForWidth(
        availableContentWidth(this->scrollArea_, this->contentLayout_),
        rewardCardMinimumWidth(effectiveScale), horizontalSpacing,
        REWARD_GRID_COLUMNS, cards.size() > 1 ? 2 : 1);
    int row = 0;
    int column = 0;
    for (auto *card : cards)
    {
        card->setScale(effectiveScale);
        card->setFont(getApp()->getFonts()->getFont(
            FontStyle::UiMedium, readableFontScale(effectiveScale)));
        grid->removeWidget(card);
        grid->addWidget(card, row, column);
        column += 1;
        if (column >= columns)
        {
            column = 0;
            row += 1;
        }
    }

    for (int i = 0; i < REWARD_GRID_COLUMNS; ++i)
    {
        grid->setColumnStretch(i, i < columns ? 1 : 0);
    }
    gridWidget->updateGeometry();
}

void ChannelPointsDialog::refreshRewardDetailLayout()
{
    const auto effectiveScale = contentScale(this->scale());
    for (auto *preview : this->contentWidget_->findChildren<QWidget *>(
             QStringLiteral("ChannelPointsRewardPreview")))
    {
        if (auto *rewardPreview = dynamic_cast<RewardIconPreview *>(preview))
        {
            rewardPreview->setScale(effectiveScale);
        }
    }

    for (auto *label : this->contentWidget_->findChildren<QLabel *>(
             QStringLiteral("ChannelPointsRewardPromptLabel")))
    {
        label->setFont(getApp()->getFonts()->getFont(
            FontStyle::UiMediumBold, readableFontScale(effectiveScale)));
    }

    for (auto *button : this->contentWidget_->findChildren<QPushButton *>(
             QStringLiteral("ChannelPointsRedeemButton")))
    {
        button->setFont(getApp()->getFonts()->getFont(
            FontStyle::UiMediumBold, readableFontScale(effectiveScale)));
    }
}

void ChannelPointsDialog::refreshEmotesLayout()
{
    const auto effectiveScale = contentScale(this->scale());
    if (this->emoteSearchInput_ != nullptr)
    {
        this->emoteSearchInput_->setFont(getApp()->getFonts()->getFont(
            FontStyle::UiMedium, readableFontScale(effectiveScale)));
    }

    if (auto *modifierWidget = this->contentWidget_->findChild<QWidget *>(
            QStringLiteral("ChannelPointsModifierRow")))
    {
        if (auto *modifierGrid =
                dynamic_cast<QGridLayout *>(modifierWidget->layout()))
        {
            const int modifierSpacing = scaledMetric(effectiveScale, 5, 3);
            modifierGrid->setHorizontalSpacing(modifierSpacing);
            modifierGrid->setVerticalSpacing(modifierSpacing);
            const auto buttons =
                directChildWidgets<QPushButton>(modifierWidget);
            const int modifierColumns = gridColumnsForWidth(
                availableContentWidth(this->scrollArea_, this->contentLayout_),
                scaledMetric(effectiveScale, 86, 58), modifierSpacing, 3);
            int index = 0;
            for (auto *button : buttons)
            {
                modifierGrid->removeWidget(button);
                modifierGrid->addWidget(button, index / modifierColumns,
                                        index % modifierColumns);
                index += 1;
            }
            for (int i = 0; i < 3; ++i)
            {
                modifierGrid->setColumnStretch(i, i < modifierColumns ? 1 : 0);
            }
            modifierWidget->updateGeometry();
        }
    }

    auto *gridWidget = this->contentWidget_->findChild<QWidget *>(
        QStringLiteral("ChannelPointsEmoteGridContent"));
    auto *grid =
        gridWidget == nullptr ? nullptr : dynamic_cast<QGridLayout *>(
                                      gridWidget->layout());
    if (grid == nullptr)
    {
        return;
    }

    const auto tiles = directChildWidgets<EmoteTileButton>(gridWidget);
    if (tiles.isEmpty())
    {
        return;
    }

    const int gridSpacing = scaledMetric(effectiveScale, 5, 3);
    grid->setHorizontalSpacing(gridSpacing);
    grid->setVerticalSpacing(gridSpacing);
    const int columns = gridColumnsForWidth(
        availableContentWidth(this->scrollArea_, this->contentLayout_),
        emoteTileMinimumWidth(effectiveScale), gridSpacing,
        EMOTE_GRID_COLUMNS);
    int row = 0;
    int column = 0;
    for (auto *tile : tiles)
    {
        tile->setScale(effectiveScale);
        tile->setFont(getApp()->getFonts()->getFont(
            FontStyle::UiMedium, readableFontScale(effectiveScale)));
        grid->removeWidget(tile);
        grid->addWidget(tile, row, column);
        column += 1;
        if (column >= columns)
        {
            column = 0;
            row += 1;
        }
    }
    for (int i = 0; i < EMOTE_GRID_COLUMNS; ++i)
    {
        grid->setColumnStretch(i, i < columns ? 1 : 0);
    }
    gridWidget->updateGeometry();
}

void ChannelPointsDialog::rebuildRewards()
{
    this->statusLabel_ = new QLabel(this->contentWidget_);
    this->statusLabel_->setObjectName("ChannelPointsStatusLabel");
    this->statusLabel_->setWordWrap(true);
    this->statusLabel_->hide();
    this->contentLayout_->addWidget(this->statusLabel_);
    this->setStatus(this->statusText_, this->statusIsError_);

    if (this->rewardsLoading_)
    {
        this->setStatus("Loading rewards...");
        this->contentLayout_->addStretch(1);
        return;
    }

    auto visibleRewards = QVector<GqlChannelPointReward>();
    for (const auto &reward : this->rewards_)
    {
        if (!shouldShowReward(reward))
        {
            continue;
        }
        visibleRewards.push_back(reward);
    }
    std::sort(visibleRewards.begin(), visibleRewards.end(),
              [](const auto &left, const auto &right) {
                  if (left.cost != right.cost)
                  {
                      return left.cost < right.cost;
                  }
                  return left.title.compare(right.title, Qt::CaseInsensitive) <
                         0;
              });

    if (visibleRewards.isEmpty())
    {
        if (!(this->statusIsError_ && !this->statusText_.isEmpty()))
        {
            this->setStatus(this->rewards_.isEmpty()
                                ? QStringLiteral("No rewards loaded yet.")
                                : QStringLiteral("No supported rewards found."));
        }
        auto *refresh = new QPushButton("Refresh", this->contentWidget_);
        refresh->setObjectName("ChannelPointsUtilityButton");
        refresh->setCursor(Qt::PointingHandCursor);
        QObject::connect(refresh, &QPushButton::clicked, this,
                         [this] { this->reloadRewards(true); });
        this->contentLayout_->addWidget(refresh);
        this->contentLayout_->addStretch(1);
        return;
    }

    auto *gridWidget = new QWidget(this->contentWidget_);
    gridWidget->setObjectName("ChannelPointsRewardGridContent");
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    const auto effectiveScale = contentScale(this->scale());
    const int horizontalSpacing = scaledMetric(effectiveScale, 5, 3);
    const int verticalSpacing = scaledMetric(effectiveScale, 6, 4);
    grid->setHorizontalSpacing(horizontalSpacing);
    grid->setVerticalSpacing(verticalSpacing);

    const int columns = gridColumnsForWidth(
        availableContentWidth(this->scrollArea_, this->contentLayout_),
        rewardCardMinimumWidth(effectiveScale), horizontalSpacing,
        REWARD_GRID_COLUMNS, visibleRewards.size() > 1 ? 2 : 1);
    int row = 0;
    int column = 0;
    bool hasRewardImages = false;
    for (const auto &reward : visibleRewards)
    {
        hasRewardImages = hasRewardImages || !reward.imageUrl.isEmpty();
        auto *card = new RewardCardButton(
            reward, this->channel_->channelPointBalance(), gridWidget);
        card->setObjectName("ChannelPointsRewardCardButton");
        card->setScale(effectiveScale);
        card->setFont(getApp()->getFonts()->getFont(
            FontStyle::UiMedium, readableFontScale(effectiveScale)));
        card->setEnabled(!this->actionInFlight_);
        QObject::connect(card, &QPushButton::clicked, this,
                         [this, reward] { this->openRewardDetail(reward); });
        grid->addWidget(card, row, column);
        column += 1;
        if (column >= columns)
        {
            column = 0;
            row += 1;
        }
    }
    for (int i = 0; i < columns; ++i)
    {
        grid->setColumnStretch(i, 1);
    }

    this->contentLayout_->addWidget(gridWidget);
    this->contentLayout_->addStretch(1);
    if (hasRewardImages)
    {
        this->queueRewardImageRefresh();
    }
}

void ChannelPointsDialog::rebuildRewardDetail()
{
    if (!this->selectedRewardValid_)
    {
        this->showRewardsView();
        return;
    }

    this->statusLabel_ = new QLabel(this->contentWidget_);
    this->statusLabel_->setObjectName("ChannelPointsStatusLabel");
    this->statusLabel_->setWordWrap(true);
    this->statusLabel_->setAlignment(Qt::AlignCenter);
    this->contentLayout_->addWidget(this->statusLabel_);
    this->setStatus(this->statusText_, this->statusIsError_);

    const auto effectiveScale = contentScale(this->scale());
    auto *prompt = new QLabel(this->contentWidget_);
    prompt->setObjectName("ChannelPointsRewardPromptLabel");
    prompt->setWordWrap(true);
    prompt->setAlignment(Qt::AlignCenter);
    prompt->setText(this->selectedReward_.prompt);
    prompt->setVisible(!this->selectedReward_.prompt.isEmpty());
    prompt->setFont(getApp()->getFonts()->getFont(FontStyle::UiMediumBold,
                                                  readableFontScale(
                                                      effectiveScale)));
    this->contentLayout_->addWidget(prompt);

    this->contentLayout_->addStretch(1);

    auto *preview = new RewardIconPreview(this->selectedReward_,
                                          this->contentWidget_);
    preview->setScale(effectiveScale);
    this->contentLayout_->addWidget(preview, 0, Qt::AlignCenter);
    if (!this->selectedReward_.imageUrl.isEmpty())
    {
        this->queueRewardImageRefresh();
    }

    this->contentLayout_->addStretch(2);

    const auto balance = this->channel_->channelPointBalance();
    const bool enoughPoints =
        balance < 0 || balance >= this->selectedReward_.cost;
    const bool redeemable = !this->actionInFlight_ &&
                            this->selectedReward_.isEnabled &&
                            this->selectedReward_.isInStock && enoughPoints;

    auto *button = new QPushButton(this->contentWidget_);
    button->setObjectName("ChannelPointsRedeemButton");
    button->setCursor(redeemable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    button->setEnabled(redeemable);
    button->setFont(getApp()->getFonts()->getFont(FontStyle::UiMediumBold,
                                                  readableFontScale(
                                                      effectiveScale)));
    if (this->actionInFlight_)
    {
        button->setText("Redeeming...");
    }
    else if (!this->selectedReward_.isEnabled ||
             !this->selectedReward_.isInStock)
    {
        button->setText("Unavailable");
    }
    else if (!enoughPoints)
    {
        button->setText(QStringLiteral("%1 Required").arg(
            fullPoints(this->selectedReward_.cost)));
    }
    else
    {
        button->setText(QStringLiteral("Redeem %1").arg(
            fullPoints(this->selectedReward_.cost)));
    }
    QObject::connect(button, &QPushButton::clicked, this,
                     [this] { this->activateSelectedReward(); });
    this->contentLayout_->addWidget(button);
}

void ChannelPointsDialog::rebuildEmotes()
{
    const auto effectiveScale = contentScale(this->scale());

    auto *search = new QLineEdit(this->contentWidget_);
    this->emoteSearchInput_ = search;
    search->setObjectName("ChannelPointsEmoteSearch");
    search->setPlaceholderText("Search emotes");
    search->setText(this->emoteSearch_);
    search->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                  readableFontScale(
                                                      effectiveScale)));
    QObject::connect(search, &QLineEdit::textChanged, this,
                     [this](const QString &text) {
                         this->emoteSearch_ = text;
                         this->emoteVisibleLimit_ = EMOTE_GRID_INITIAL_LIMIT;
                         this->emoteScrollValue_ = 0;
                         this->rebuildContent();
                         if (this->emoteSearchInput_ != nullptr)
                         {
                             this->emoteSearchInput_->setFocus(
                                 Qt::OtherFocusReason);
                             this->emoteSearchInput_->setCursorPosition(
                                 this->emoteSearch_.size());
                         }
                     });
    this->contentLayout_->addWidget(search);

    this->statusLabel_ = new QLabel(this->contentWidget_);
    this->statusLabel_->setObjectName("ChannelPointsStatusLabel");
    this->statusLabel_->setWordWrap(true);
    this->contentLayout_->addWidget(this->statusLabel_);
    this->setStatus(this->statusText_.isEmpty() ? this->selectedReward_.prompt
                                                : this->statusText_,
                    this->statusIsError_);

    if (this->selectingModifiedEmote_)
    {
        auto *modifierWidget = new QWidget(this->contentWidget_);
        modifierWidget->setObjectName("ChannelPointsModifierRow");
        auto *modifierGrid = new QGridLayout(modifierWidget);
        modifierGrid->setContentsMargins(0, 0, 0, 0);
        const int modifierSpacing = scaledMetric(effectiveScale, 5, 3);
        modifierGrid->setHorizontalSpacing(modifierSpacing);
        modifierGrid->setVerticalSpacing(modifierSpacing);
        int modifierIndex = 0;
        const int modifierColumns = gridColumnsForWidth(
            availableContentWidth(this->scrollArea_, this->contentLayout_),
            scaledMetric(effectiveScale, 86, 58), modifierSpacing, 3);
        for (const auto &modifier : this->modifiers_)
        {
            const bool hasAvailableEmote =
                hasEmoteForModifier(this->emotes_, modifier.id);
            auto *button = new QPushButton(modifier.title, modifierWidget);
            button->setObjectName("ChannelPointsModifierButton");
            button->setCheckable(true);
            button->setChecked(modifier.id == this->selectedModifierId_);
            button->setEnabled(hasAvailableEmote);
            button->setCursor(hasAvailableEmote ? Qt::PointingHandCursor
                                                : Qt::ArrowCursor);
            button->setMinimumWidth(0);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            QObject::connect(button, &QPushButton::clicked, this,
                             [this, modifier] {
                                 this->selectedModifierId_ = modifier.id;
                                 this->rebuildContent();
                             });
            modifierGrid->addWidget(button, modifierIndex / modifierColumns,
                                    modifierIndex % modifierColumns);
            ++modifierIndex;
        }
        for (int i = 0; i < modifierColumns; ++i)
        {
            modifierGrid->setColumnStretch(i, 1);
        }
        this->contentLayout_->addWidget(modifierWidget);
    }

    if (this->emotesLoading_)
    {
        this->setStatus("Loading emotes...");
        this->contentLayout_->addStretch(1);
        return;
    }

    auto *emoteScrollArea = new QScrollArea(this->contentWidget_);
    emoteScrollArea->setObjectName("ChannelPointsEmoteScrollArea");
    emoteScrollArea->setFrameShape(QFrame::NoFrame);
    emoteScrollArea->setWidgetResizable(true);
    emoteScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    emoteScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    emoteScrollArea->setSizePolicy(QSizePolicy::Expanding,
                                   QSizePolicy::Ignored);
    emoteScrollArea->setMinimumHeight(0);

    auto *gridWidget = new QWidget(emoteScrollArea);
    gridWidget->setObjectName("ChannelPointsEmoteGridContent");
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    const int gridSpacing = scaledMetric(effectiveScale, 5, 3);
    grid->setHorizontalSpacing(gridSpacing);
    grid->setVerticalSpacing(gridSpacing);

    const auto needle = this->emoteSearch_.trimmed();
    int row = 0;
    int column = 0;
    int shown = 0;
    int matched = 0;
    const int columns = gridColumnsForWidth(
        availableContentWidth(this->scrollArea_, this->contentLayout_),
        emoteTileMinimumWidth(effectiveScale), gridSpacing,
        EMOTE_GRID_COLUMNS);
    for (const auto &emote : this->emotes_)
    {
        if (!needle.isEmpty() &&
            !emoteSearchText(emote).contains(needle, Qt::CaseInsensitive))
        {
            continue;
        }
        if (emote.type == "GLOBALS")
        {
            continue;
        }
        if (this->selectingModifiedEmote_ &&
            !this->selectedModifierId_.isEmpty() &&
            findEmoteModification(emote, this->selectedModifierId_) == nullptr)
        {
            continue;
        }

        matched += 1;
        if (shown >= this->emoteVisibleLimit_)
        {
            continue;
        }

        auto *button = new EmoteTileButton(emote, true, gridWidget);
        button->setObjectName("ChannelPointsEmoteButton");
        button->setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium,
                                                      readableFontScale(
                                                          effectiveScale)));
        button->setScale(effectiveScale);
        QObject::connect(button, &QPushButton::clicked, this,
                         [this, emote] { this->unlockSelectedEmote(emote); });
        grid->addWidget(button, row, column);
        shown += 1;
        column += 1;
        if (column >= columns)
        {
            column = 0;
            row += 1;
        }
    }
    for (int i = 0; i < columns; ++i)
    {
        grid->setColumnStretch(i, 1);
    }

    if (shown == 0)
    {
        this->setStatus(needle.isEmpty() ? QStringLiteral("No emotes found.")
                                         : QStringLiteral("No matching emotes."));
    }

    emoteScrollArea->setWidget(gridWidget);
    this->contentLayout_->addWidget(emoteScrollArea, 1);
    auto *scrollBar = emoteScrollArea->verticalScrollBar();
    QObject::connect(scrollBar, &QScrollBar::valueChanged, this,
                     [this, scrollBar, matched](int value) {
                         this->emoteScrollValue_ = value;
                         if (matched <= this->emoteVisibleLimit_)
                         {
                             return;
                         }

                         const int threshold =
                             std::max(48, scrollBar->pageStep() / 3);
                         if (scrollBar->maximum() - value <= threshold)
                         {
                             this->queueEmoteLazyLoad();
                         }
                     });
    QPointer<QScrollArea> weakScrollArea = emoteScrollArea;
    QTimer::singleShot(0, this, [this, weakScrollArea] {
        if (!weakScrollArea)
        {
            return;
        }

        auto *bar = weakScrollArea->verticalScrollBar();
        bar->setValue(std::min(this->emoteScrollValue_, bar->maximum()));
    });
    if (shown > 0)
    {
        this->queueEmoteImageRefresh();
    }
}

void ChannelPointsDialog::clearContent()
{
    this->statusLabel_ = nullptr;
    this->emoteSearchInput_ = nullptr;
    while (auto *item = this->contentLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void ChannelPointsDialog::setStatus(const QString &text, bool error)
{
    this->statusText_ = text;
    this->statusIsError_ = error;

    if (this->statusLabel_ == nullptr)
    {
        return;
    }

    this->statusLabel_->setText(text);
    this->statusLabel_->setVisible(!text.isEmpty());
    auto muted = this->theme->window.text;
    muted.setAlpha(155);
    const auto color =
        error ? QStringLiteral("#ff9e9e") : muted.name(QColor::HexArgb);
    this->statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
}

void ChannelPointsDialog::showRewardsView()
{
    this->view_ = View::Rewards;
    this->selectedRewardValid_ = false;
    this->selectingModifiedEmote_ = false;
    this->setStatus({});
    this->refreshHeader();
    this->rebuildContent();
}

void ChannelPointsDialog::openRewardDetail(
    const GqlChannelPointReward &reward)
{
    if (this->actionInFlight_)
    {
        return;
    }

    this->selectedReward_ = reward;
    this->selectedRewardValid_ = true;
    this->view_ = View::RewardDetail;
    this->setStatus({});
    this->refreshHeader();
    this->rebuildContent();
}

void ChannelPointsDialog::activateSelectedReward()
{
    if (!this->selectedRewardValid_)
    {
        return;
    }

    this->selectReward(this->selectedReward_);
}

void ChannelPointsDialog::selectReward(const GqlChannelPointReward &reward)
{
    if (!this->canRedeem(reward))
    {
        return;
    }

    if (!reward.isAutomatic)
    {
        if (reward.isUserInputRequired)
        {
            if (!this->input_)
            {
                this->setStatus("Open a chat input to redeem this reward.",
                                true);
                return;
            }
            const auto token = this->authTokenOrMessage();
            if (token.isEmpty())
            {
                return;
            }
            auto *channel = this->channel_;
            const auto channelId = this->redeemChannelId();
            const auto weak = channel->weak_from_this();
            this->input_->showChannelPointRewardPrompt(
                QStringLiteral("Redeem %1").arg(reward.title),
                reward.prompt.isEmpty() ? QStringLiteral("Add a message")
                                        : reward.prompt,
                true,
                [weak, channelId, reward, token](const QString &text) {
                    TwitchGql::redeemCustomReward(
                        channelId, reward, text, token,
                        [weak, reward](
                            const GqlChannelPointRedeemResult &result) {
                            applyChannelPointRedeemResult(
                                weak, result,
                                QStringLiteral("Redeemed %1").arg(
                                    reward.title),
                                reward.cost);
                        },
                        [weak](const QString &error) {
                            showChannelPointRedeemError(
                                weak, "redeeming channel point rewards",
                                error);
                        });
                });
            this->close();
            return;
        }

        this->redeemCustomReward(reward, {});
        return;
    }

    if (reward.rewardType == "SINGLE_MESSAGE_BYPASS_SUB_MODE")
    {
        if (!this->input_)
        {
            this->setStatus("Open a chat input to redeem this reward.", true);
            return;
        }
        const auto token = this->authTokenOrMessage();
        if (token.isEmpty())
        {
            return;
        }
        auto *channel = this->channel_;
        const auto channelId = this->redeemChannelId();
        const auto weak = channel->weak_from_this();
        const auto cost = reward.cost;
        const auto successMessage = QStringLiteral("Sub-only message sent");
        this->input_->showChannelPointRewardPrompt(
            reward.title, QStringLiteral("Send a message"), true,
            [weak, channelId, cost, token,
             successMessage](const QString &text) {
                auto success =
                    [weak,
                     successMessage](const GqlChannelPointRedeemResult &result) {
                        applyChannelPointRedeemResult(weak, result,
                                                      successMessage);
                    };
                auto failure = [weak](const QString &error) {
                    showChannelPointRedeemError(
                        weak, "redeeming channel point rewards", error);
                };

                TwitchGql::sendSubOnlyBypassMessage(
                    channelId, cost, text, token, success, failure);
            });
        this->close();
        return;
    }

    if (reward.rewardType == "SEND_HIGHLIGHTED_MESSAGE")
    {
        if (!this->input_)
        {
            this->setStatus("Open a chat input to redeem this reward.", true);
            return;
        }
        const auto token = this->authTokenOrMessage();
        if (token.isEmpty())
        {
            return;
        }
        auto *channel = this->channel_;
        const auto channelId = this->redeemChannelId();
        const auto weak = channel->weak_from_this();
        const auto cost = reward.cost;
        const auto successMessage = QStringLiteral("Highlighted message sent");
        this->input_->showChannelPointRewardPrompt(
            reward.title, QStringLiteral("Send a highlighted message"), true,
            [weak, channelId, cost, token,
             successMessage](const QString &text) {
                auto success =
                    [weak,
                     successMessage](const GqlChannelPointRedeemResult &result) {
                        applyChannelPointRedeemResult(weak, result,
                                                      successMessage);
                    };
                auto failure = [weak](const QString &error) {
                    showChannelPointRedeemError(
                        weak, "redeeming channel point rewards", error);
                };

                TwitchGql::sendHighlightedChatMessage(
                    channelId, cost, text, token, success, failure);
            });
        this->close();
        return;
    }

    if (reward.rewardType == "RANDOM_SUB_EMOTE_UNLOCK")
    {
        this->unlockRandomEmote(reward);
        return;
    }

    if (reward.rewardType == "CHOSEN_SUB_EMOTE_UNLOCK")
    {
        this->openEmotePicker(reward, false);
        return;
    }

    if (reward.rewardType == "CHOSEN_MODIFIED_SUB_EMOTE_UNLOCK")
    {
        this->openEmotePicker(reward, true);
        return;
    }

    this->setStatus("This power-up is not supported by Moltorino yet.", true);
}

void ChannelPointsDialog::redeemCustomReward(
    const GqlChannelPointReward &reward, const QString &prompt)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        return;
    }

    this->actionInFlight_ = true;
    this->rebuildContent();
    QPointer<ChannelPointsDialog> self = this;
    TwitchGql::redeemCustomReward(
        this->redeemChannelId(), reward, prompt, token,
        [self, reward](const GqlChannelPointRedeemResult &result) {
            if (!self)
            {
                return;
            }
            self->applyRedeemResult(
                result, QStringLiteral("Redeemed %1").arg(reward.title));
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "redeeming channel point rewards", error),
                            true);
            self->rebuildContent();
        });
}

void ChannelPointsDialog::unlockRandomEmote(
    const GqlChannelPointReward &reward)
{
    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        return;
    }

    this->actionInFlight_ = true;
    this->rebuildContent();
    QPointer<ChannelPointsDialog> self = this;
    TwitchGql::unlockRandomSubscriberEmote(
        this->redeemChannelId(), reward.cost, token,
        [self](const GqlChannelPointRedeemResult &result) {
            if (!self)
            {
                return;
            }
            const auto message =
                result.emoteToken.isEmpty()
                    ? QStringLiteral("Unlocked a random emote")
                    : QStringLiteral("Unlocked %1").arg(result.emoteToken);
            self->applyRedeemResult(result, message);
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->actionInFlight_ = false;
            self->setStatus(MoltorinoAuth::normalizeAuthError(
                                "unlocking channel point emotes", error),
                            true);
            self->rebuildContent();
        });
}

void ChannelPointsDialog::openEmotePicker(const GqlChannelPointReward &reward,
                                          bool modified)
{
    this->selectedReward_ = reward;
    this->selectedRewardValid_ = true;
    this->selectingModifiedEmote_ = modified;
    this->view_ = View::Emotes;
    this->headerTitleLabel_->setText(reward.title);
    this->emoteSearch_.clear();
    this->emoteVisibleLimit_ = EMOTE_GRID_INITIAL_LIMIT;
    this->emoteScrollValue_ = 0;
    this->emoteImageRefreshAttempts_ = 0;
    this->emoteImageRefreshQueued_ = false;
    this->emoteLazyLoadQueued_ = false;
    this->setStatus({});
    this->loadEmotePickerData();
    this->rebuildContent();
}

void ChannelPointsDialog::loadEmotePickerData()
{
    if (this->emotesLoading_)
    {
        return;
    }

    const bool loadingModifiedEmotes = this->selectingModifiedEmote_;
    const bool needsEmotes =
        this->emotes_.isEmpty() ||
        this->emotesLoadedForModifiedPicker_ != loadingModifiedEmotes;
    if (needsEmotes)
    {
        this->emotes_.clear();
    }
    const bool needsModifiers =
        this->selectingModifiedEmote_ && this->modifiers_.isEmpty();
    if (!needsEmotes && !needsModifiers)
    {
        this->emotesLoading_ = false;
        if (this->selectingModifiedEmote_ &&
            (this->selectedModifierId_.isEmpty() ||
             !hasEmoteForModifier(this->emotes_, this->selectedModifierId_)) &&
            !this->modifiers_.isEmpty())
        {
            this->selectedModifierId_ =
                firstAvailableModifierId(this->modifiers_, this->emotes_);
        }
        return;
    }

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        this->emotesLoading_ = false;
        return;
    }

    this->emotesLoading_ = true;
    QPointer<ChannelPointsDialog> self = this;

    const auto finish = [self](const QString &status, bool error) {
        if (!self)
        {
            return;
        }
        self->emotesLoading_ = false;
        self->setStatus(status, error);
        if (self->view_ == View::Emotes)
        {
            self->rebuildContent();
        }
    };

    const auto loadModifiers = [self, token, finish] {
        TwitchGql::getChannelPointEmoteModifiers(
            token,
            [self, finish](QVector<GqlChannelPointEmoteModifier> modifiers) {
                if (!self)
                {
                    return;
                }
                self->modifiers_ = std::move(modifiers);
                if ((self->selectedModifierId_.isEmpty() ||
                     !hasEmoteForModifier(self->emotes_,
                                          self->selectedModifierId_)) &&
                    !self->modifiers_.isEmpty())
                {
                    self->selectedModifierId_ =
                        firstAvailableModifierId(self->modifiers_,
                                                 self->emotes_);
                }

                if (self->modifiers_.isEmpty())
                {
                    finish(QStringLiteral("No emote modifiers found."), true);
                    return;
                }
                finish({}, false);
            },
            [finish](const QString &error) {
                finish(MoltorinoAuth::normalizeAuthError(
                           "loading emote modifiers", error),
                       true);
            });
    };

    if (!needsEmotes)
    {
        loadModifiers();
        return;
    }

    const auto emoteSuccess =
        [self, loadModifiers, finish,
         needsModifiers,
         loadingModifiedEmotes](QVector<GqlChannelPointEmote> emotes) {
            if (!self)
            {
                return;
            }
            self->emotes_ = std::move(emotes);
            self->emotesLoadedForModifiedPicker_ = loadingModifiedEmotes;
            if (self->selectingModifiedEmote_ && !self->modifiers_.isEmpty() &&
                (self->selectedModifierId_.isEmpty() ||
                 !hasEmoteForModifier(self->emotes_,
                                      self->selectedModifierId_)))
            {
                self->selectedModifierId_ =
                    firstAvailableModifierId(self->modifiers_,
                                             self->emotes_);
            }
            if (self->view_ == View::Emotes &&
                self->selectingModifiedEmote_ != loadingModifiedEmotes)
            {
                self->emotesLoading_ = false;
                self->loadEmotePickerData();
                self->rebuildContent();
                return;
            }
            if (!needsModifiers)
            {
                finish({}, false);
                return;
            }

            loadModifiers();
        };
    const auto emoteFailure = [finish](const QString &error) {
        finish(MoltorinoAuth::normalizeAuthError("loading channel point emotes",
                                                 error),
               true);
    };

    if (loadingModifiedEmotes)
    {
        TwitchGql::getModifiableChannelPointEmotes(
            this->channel_->getName(), token, emoteSuccess, emoteFailure);
        return;
    }

    TwitchGql::getAvailableChannelPointEmotes(
        this->redeemChannelId(), token, emoteSuccess, emoteFailure);
}

void ChannelPointsDialog::unlockSelectedEmote(
    const GqlChannelPointEmote &emote)
{
    if (!this->selectedRewardValid_ || this->actionInFlight_)
    {
        return;
    }

    const auto token = this->authTokenOrMessage();
    if (token.isEmpty())
    {
        return;
    }

    this->actionInFlight_ = true;
    this->rebuildContent();
    QPointer<ChannelPointsDialog> self = this;
    const auto cost = this->selectedReward_.cost;
    auto success = [self, emote](const GqlChannelPointRedeemResult &result) {
        if (!self)
        {
            return;
        }
        self->applyRedeemResult(
            result, QStringLiteral("Unlocked %1").arg(emoteLabel(emote)));
    };
    auto failure = [self](const QString &error) {
        if (!self)
        {
            return;
        }
        self->actionInFlight_ = false;
        self->setStatus(MoltorinoAuth::normalizeAuthError(
                            "unlocking channel point emotes", error),
                        true);
        self->rebuildContent();
    };

    if (this->selectingModifiedEmote_)
    {
        if (this->selectedModifierId_.isEmpty())
        {
            this->actionInFlight_ = false;
            this->setStatus("Choose a modifier first.", true);
            this->rebuildContent();
            return;
        }
        const auto *modification =
            findEmoteModification(emote, this->selectedModifierId_);
        if (modification == nullptr)
        {
            this->actionInFlight_ = false;
            this->setStatus("That modifier is not available for this emote.",
                            true);
            this->rebuildContent();
            return;
        }
        const auto modifiedLabel =
            modification->emoteToken.isEmpty() ? emoteLabel(emote)
                                               : modification->emoteToken;
        auto modifiedSuccess =
            [self, modifiedLabel](const GqlChannelPointRedeemResult &result) {
                if (!self)
                {
                    return;
                }
                self->applyRedeemResult(
                    result, QStringLiteral("Unlocked %1").arg(modifiedLabel));
            };
        TwitchGql::unlockModifiedSubscriberEmote(
            this->redeemChannelId(), modification->emoteId, cost, token,
            modifiedSuccess, failure);
        return;
    }

    TwitchGql::unlockChosenSubscriberEmote(this->redeemChannelId(), emote.id,
                                           cost, token, success, failure);
}

void ChannelPointsDialog::applyRedeemResult(
    const GqlChannelPointRedeemResult &result, const QString &message)
{
    this->actionInFlight_ = false;
    if (result.balance >= 0)
    {
        this->channel_->setChannelPointBalance(result.balance);
    }
    this->channel_->addSystemMessage(message);
    this->refreshHeader();
    if (getSettings()->rewardsCloseAfterRedeem)
    {
        this->close();
        return;
    }

    if (getSettings()->rewardsReturnToListAfterRedeem)
    {
        this->view_ = View::Rewards;
        this->selectedRewardValid_ = false;
        this->selectingModifiedEmote_ = false;
        this->reloadRewards(true);
        return;
    }

    this->setStatus(message);
    this->rebuildContent();
}

QString ChannelPointsDialog::redeemChannelId() const
{
    const auto rewardsChannelId = this->rewardsChannelId_.trimmed();
    if (!rewardsChannelId.isEmpty())
    {
        return rewardsChannelId;
    }

    return this->channel_ == nullptr ? QString() : this->channel_->roomId();
}

void ChannelPointsDialog::queueEmoteImageRefresh()
{
    if (this->emoteImageRefreshQueued_ ||
        this->emoteImageRefreshAttempts_ >= 80)
    {
        return;
    }

    this->emoteImageRefreshQueued_ = true;
    this->emoteImageRefreshAttempts_ += 1;
    QTimer::singleShot(120, this, [this] {
        this->emoteImageRefreshQueued_ = false;
        if (this->view_ != View::Emotes || this->contentWidget_ == nullptr)
        {
            return;
        }

        for (auto *button :
             this->contentWidget_->findChildren<QPushButton *>(
                 QStringLiteral("ChannelPointsEmoteButton")))
        {
            button->update();
        }
        this->queueEmoteImageRefresh();
    });
}

void ChannelPointsDialog::queueRewardImageRefresh()
{
    if (this->rewardImageRefreshQueued_ ||
        this->rewardImageRefreshAttempts_ >= 40)
    {
        return;
    }

    this->rewardImageRefreshQueued_ = true;
    this->rewardImageRefreshAttempts_ += 1;
    QTimer::singleShot(120, this, [this] {
        this->rewardImageRefreshQueued_ = false;
        if (this->contentWidget_ == nullptr ||
            (this->view_ != View::Rewards && this->view_ != View::RewardDetail))
        {
            return;
        }

        for (auto *button :
             this->contentWidget_->findChildren<QPushButton *>(
                 QStringLiteral("ChannelPointsRewardCardButton")))
        {
            button->update();
        }
        for (auto *preview : this->contentWidget_->findChildren<QWidget *>(
                 QStringLiteral("ChannelPointsRewardPreview")))
        {
            preview->update();
        }
        this->queueRewardImageRefresh();
    });
}

void ChannelPointsDialog::queueEmoteLazyLoad()
{
    if (this->emoteLazyLoadQueued_)
    {
        return;
    }

    this->emoteLazyLoadQueued_ = true;
    this->emoteVisibleLimit_ += EMOTE_GRID_LIMIT_STEP;
    QTimer::singleShot(0, this, [this] {
        this->emoteLazyLoadQueued_ = false;
        if (this->view_ != View::Emotes || this->emotesLoading_)
        {
            return;
        }
        this->rebuildContent();
    });
}

void ChannelPointsDialog::scheduleLayoutRefresh(int delayMs)
{
    if (this->contentWidget_ == nullptr)
    {
        return;
    }
    if (this->rebuildingContent_)
    {
        this->layoutRefreshQueued_ = true;
        return;
    }
    if (this->refreshingLayout_)
    {
        return;
    }

    this->layoutRefreshTimer_.start(std::max(0, delayMs));
}

QString ChannelPointsDialog::authTokenOrMessage()
{
    QString authError;
    const auto auth = MoltorinoAuth::resolveCurrentUserToken(&authError);
    if (auth.hasToken())
    {
        return auth.token;
    }

    const auto message =
        authError.isEmpty()
            ? MoltorinoAuth::authRequiredMessage(
                  "using channel point rewards")
            : authError;
    this->setStatus(message, true);
    return {};
}

bool ChannelPointsDialog::canRedeem(const GqlChannelPointReward &reward)
{
    if (this->actionInFlight_)
    {
        return false;
    }
    if (!reward.isEnabled)
    {
        this->setStatus("That reward is disabled.", true);
        return false;
    }
    if (!reward.isInStock)
    {
        this->setStatus("That reward is out of stock or on cooldown.", true);
        return false;
    }
    const auto balance = this->channel_->channelPointBalance();
    if (balance >= 0 && reward.cost > balance)
    {
        this->setStatus(
            QStringLiteral("You need %1 more points.")
                .arg(compactPoints(reward.cost - balance)),
            true);
        return false;
    }
    if (this->redeemChannelId().isEmpty())
    {
        this->setStatus("Channel point rewards are not ready yet.", true);
        return false;
    }
    return true;
}

void ChannelPointsDialog::applySizeConstraints()
{
    const int targetWidth =
        std::max(1, int(DEFAULT_DIALOG_SIZE.width() * this->scale()));
    const int targetHeight =
        std::max(1, int(DEFAULT_DIALOG_SIZE.height() * this->scale()));
    const int minimumWidth = std::min(
        targetWidth, std::max(118, int(220 * this->scale())));
    const int minimumHeight = std::min(
        targetHeight, std::max(120, int(210 * this->scale())));
    this->setMinimumSize(QSize(minimumWidth, minimumHeight));
}

}  // namespace chatterino

#endif
