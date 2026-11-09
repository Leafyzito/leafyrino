// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/DebugCount.hpp"

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

}  // namespace chatterino

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
    std::optional<QPixmap> current() const;
    /// Returns the size of the first frame, if one has been loaded.
    std::optional<QSize> frameSize() const;

private:
    friend class chatterino::ImageExpirationPool;
    struct Storage;
    struct CachedFrames;

    int64_t memoryUsage() const;
    std::unique_ptr<Storage> storage_;
};

QList<Frame> readFrames(QImageReader &reader, const Url &url,
                        std::optional<QSize> rescale = std::nullopt);
void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed);

struct ImageBudgetEntry {
    int64_t bytes = 0;
    std::chrono::steady_clock::time_point lastUsed;
    /// Whether the image may be evicted to get back under the budget
    bool evictable = false;
};

/// Picks the entries to evict, least recently used first, once the total
/// exceeds maxBytes. Evicts until the total fits in targetBytes or no
/// evictable entries are left.
///
/// @returns The indices of the entries to evict
std::vector<size_t> pickImageBudgetEvictions(
    const std::vector<ImageBudgetEntry> &entries, int64_t maxBytes,
    int64_t targetBytes);

}  // namespace chatterino::detail

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
    /// @brief The expected size of this image once its loaded.
    ///
    /// This doesn't represent the actual size (it can be different) - it's
    /// just an estimation and provided to avoid (large) layout shifts when
    /// loading images.
    const QSize expectedSize_{16, 16};
    bool useExpectedSizeForRead_{false};
    std::atomic_bool empty_{false};

    bool shouldLoad_{false};

    /// Size this image should take when loaded (in both dimensions).
    ///
    /// This is used for images that have an unknown scale when they're created
    /// (i.e. the scale is only known after the image is loaded).
    ///
    /// Upon creation, only `expectedSize_` is set to `(autoScale, autoScale)`.
    /// When the image is loaded, `scale_` is set to `autoScale / actualSize`.
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

    /// Evicts the least recently used animated images if the decoded frames
    /// of all images use more memory than the budget allows
    void requestBudgetCheck();

    std::vector<ProviderUsage> getProviderUsageSnapshot();

    void freeAll();

    QTimer *freeTimer_;
    std::map<Image *, std::weak_ptr<Image>> allImages_;
    std::mutex mutex_;

private:
    /// Returns strong references to all tracked images. Images must not be
    /// released while mutex_ is held, since ~Image() locks it too.
    std::vector<ImagePtr> snapshotImages();
    void runBudgetCheck();
    /// @returns true if the images are still over the budget
    bool enforceBudget();

    bool budgetCheckQueued_ = false;
};

#endif

}  // namespace chatterino
