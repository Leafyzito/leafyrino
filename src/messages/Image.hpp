// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/DebugCount.hpp"

#include <boost/variant.hpp>
#include <pajlada/signals/signal.hpp>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace chatterino {

class Image;
class ImageExpirationPool;

}

namespace chatterino::detail {

struct Frame {
    QPixmap image;
    int duration;
};

class Frames
{
public:
    Frames();
    Frames(QList<Frame> &&frames);
    ~Frames();

    Frames(const Frames &) = delete;
    Frames &operator=(const Frames &) = delete;

    Frames(Frames &&) = delete;
    Frames &operator=(Frames &&) = delete;

    void clear();
    bool empty() const;
    bool animated() const;
    void advance();
    std::optional<QPixmap> current() const;
    std::optional<QPixmap> first() const;

private:
    friend class chatterino::ImageExpirationPool;

    int64_t memoryUsage() const;
    void processOffset();
    QList<Frame> items_;
    QList<Frame>::size_type index_{0};
    int durationOffset_{0};
    pajlada::Signals::Connection gifTimerConnection_;
};

QList<Frame> readFrames(QImageReader &reader, const Url &url);
void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed);

}

namespace chatterino {

class Image;
using ImagePtr = std::shared_ptr<Image>;

class Image : public std::enable_shared_from_this<Image>
{
public:

    static constexpr int maxBytesRam = 20 * 1024 * 1024;

    ~Image();

    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;

    Image(Image &&) = delete;
    Image &operator=(Image &&) = delete;

    static ImagePtr fromUrl(const Url &url, qreal scale = 1,
                            QSize expectedSize = {});
    static ImagePtr fromResourcePixmap(const QPixmap &pixmap, qreal scale = 1);
    static ImagePtr getEmpty();

    static ImagePtr fromAutoscaledUrl(const Url &url, uint16_t autoScale);

    const Url &url() const;
    bool loaded() const;

    std::optional<QPixmap> pixmapOrLoad() const;
    void load() const;
    qreal scale() const;
    bool isEmpty() const;
    int width() const;
    int height() const;
    QSizeF size() const;
    bool animated() const;
    void setFrameCacheLifetime(std::chrono::milliseconds lifetime);

    bool operator==(const Image &image) = delete;
    bool operator!=(const Image &image) = delete;

private:
    Image();
    Image(const Url &url, qreal scale, QSize expectedSize);
    Image(qreal scale);

    void setPixmap(const QPixmap &pixmap);
    void actuallyLoad();
    void expireFrames();

    const Url url_{};
    qreal scale_{1};

    const QSize expectedSize_{16, 16};
    std::atomic_bool empty_{false};

    bool shouldLoad_{false};

    std::optional<uint16_t> autoScale_;
    std::atomic<int64_t> frameCacheLifetimeMs_{0};

    mutable std::chrono::time_point<std::chrono::steady_clock> lastUsed_;

    std::unique_ptr<detail::Frames> frames_;

    friend class ImageExpirationPool;
    friend void detail::assignFrames(std::weak_ptr<Image>,
                                     QList<detail::Frame>);
};

ImagePtr getEmptyImagePtr();

#ifndef DISABLE_IMAGE_EXPIRATION_POOL

class ImageExpirationPool
{
public:
    struct ProviderUsage {
        QString provider;
        int64_t bytes = 0;
        size_t images = 0;
        size_t animatedImages = 0;
    };

    ImageExpirationPool();
    static ImageExpirationPool &instance();

    void addImagePtr(ImagePtr imgPtr);
    void removeImagePtr(Image *rawPtr);

    void freeOld();

    std::vector<ProviderUsage> getProviderUsageSnapshot();

    void freeAll();

    QTimer *freeTimer_;
    std::map<Image *, std::weak_ptr<Image>> allImages_;
    std::mutex mutex_;
};

#endif

}
