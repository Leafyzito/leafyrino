// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/Image.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "debug/Benchmark.hpp"
#include "singletons/helper/GifTimer.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/Helpers.hpp"
#include "util/MemoryReclaimer.hpp"
#include "util/PostToThread.hpp"

#include <boost/functional/hash.hpp>
#include <pajlada/signals/scoped-connection.hpp>
#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <utility>

const auto IMAGE_POOL_CLEANUP_INTERVAL = std::chrono::minutes(1);

const auto IMAGE_POOL_IMAGE_LIFETIME = std::chrono::minutes(10);

// once decoded frames use more than IMAGE_POOL_MAX_BYTES, the least recently
// used animated images are evicted until IMAGE_POOL_TARGET_BYTES are left,
// instead of waiting for IMAGE_POOL_IMAGE_LIFETIME
constexpr int64_t IMAGE_POOL_MAX_BYTES = 256LL * 1024 * 1024;
constexpr int64_t IMAGE_POOL_TARGET_BYTES = 192LL * 1024 * 1024;
// images painted this recently are still on screen, never evict them
const auto IMAGE_POOL_ACTIVE_IMAGE_GRACE = std::chrono::seconds(5);
// freeing less than this isn't worth asking the allocator to give memory back
constexpr int64_t IMAGE_POOL_RELIEF_BYTES = 8LL * 1024 * 1024;

namespace {

using namespace chatterino;

std::pair<QSize, bool> limitedAutoScaleSize(
    QSize original, std::optional<uint16_t> optAutoScale)
{
    if (!optAutoScale)
    {
        return {original, false};
    }
    // 2x ought to be enough for everyone.
    int maxAutoScale = *optAutoScale * 2;
    int maxDim = std::max({original.width(), original.height(), 1});
    if (maxDim <= maxAutoScale)
    {
        return {original, false};
    }
    return {
        original.scaled({maxAutoScale, maxAutoScale},
                        Qt::KeepAspectRatioByExpanding),
        true,
    };
}

}  // namespace

namespace chatterino::detail {

struct Frames::Storage {
    Storage() = default;
    virtual ~Storage() = default;
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
    Storage(Storage &&) = delete;
    Storage &operator=(Storage &&) = delete;

    virtual int64_t memoryUsage() const = 0;
    virtual bool empty() const = 0;
    virtual bool animated() const = 0;
    virtual void start(GIFTimer *timer) = 0;
    virtual std::optional<QPixmap> current() const = 0;
    virtual std::optional<QSize> frameSize() const = 0;

    pajlada::Signals::ScopedConnection gifTimerConnection;
};

/// Stores all decoded frames in memory.
struct Frames::CachedFrames : Storage {
    CachedFrames() = default;
    explicit CachedFrames(QList<Frame> frames)
        : items(std::move(frames))
    {
    }
    ~CachedFrames() override = default;
    CachedFrames(const CachedFrames &) = delete;
    CachedFrames &operator=(const CachedFrames &) = delete;
    CachedFrames(CachedFrames &&) = delete;
    CachedFrames &operator=(CachedFrames &&) = delete;

    int64_t memoryUsage() const override
    {
        int64_t usage = 0;
        for (const auto &frame : this->items)
        {
            auto sz = frame.image.size();
            auto area = sz.width() * sz.height();
            auto memory = area * frame.image.depth() / 8;
            usage += memory;
        }
        return usage;
    }

    bool empty() const override
    {
        return this->items.empty();
    }

    bool animated() const override
    {
        return this->items.size() > 1;
    }

    void start(GIFTimer *timer) override
    {
        if (!this->animated())
        {
            return;
        }

        this->gifTimerConnection = timer->signal.connect([this] {
            this->advance();
        });

        const auto totalLength =
            std::accumulate(this->items.begin(), this->items.end(), 0UL,
                            [](auto init, auto &&frame) {
                                return init + frame.duration;
                            });
        if (totalLength == 0)
        {
            this->durationOffset = 0;
        }
        else
        {
            this->durationOffset =
                std::min<int>(int(timer->position() % totalLength), 60000);
        }
        this->processOffset();
    }

    void advance()
    {
        this->durationOffset += GIF_FRAME_LENGTH;
        this->processOffset();
    }

    void processOffset()
    {
        if (this->items.isEmpty())
        {
            return;
        }

        while (true)
        {
            this->index %= this->items.size();
            if (this->durationOffset > this->items.at(this->index).duration)
            {
                this->durationOffset -= this->items.at(this->index).duration;
                this->index = (this->index + 1) % this->items.size();
            }
            else
            {
                break;
            }
        }
    }

    std::optional<QPixmap> current() const override
    {
        if (this->empty())
        {
            return std::nullopt;
        }
        return this->items[this->index].image;
    }

    std::optional<QSize> frameSize() const override
    {
        if (this->empty())
        {
            return std::nullopt;
        }
        return this->items.front().image.size();
    }

    QList<Frame> items;
    QList<Frame>::size_type index{0};
    int durationOffset{0};
};

Frames::Frames()
    : storage_(std::make_unique<CachedFrames>())
{
    DebugCount::increase(DebugObject::Image);
}

Frames::Frames(QList<Frame> &&frames)
    : storage_(std::make_unique<CachedFrames>(std::move(frames)))
{
    assertInGuiThread();
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        qCDebug(chatterinoImage)
            << "Frames constructor called while app is shutting down";
        return;
    }

    DebugCount::increase(DebugObject::Image);
    if (!this->empty())
    {
        DebugCount::increase(DebugObject::LoadedImage);
    }

    if (this->animated())
    {
        DebugCount::increase(DebugObject::AnimatedImage);

        this->storage_->start(app->getEmotes()->getGIFTimer());
    }

    DebugCount::increase(DebugObject::BytesImageCurrent, this->memoryUsage());
    DebugCount::increase(DebugObject::BytesImageLoaded, this->memoryUsage());
}

Frames::~Frames()
{
    assertInGuiThread();
    DebugCount::decrease(DebugObject::Image);
    if (!this->empty())
    {
        DebugCount::decrease(DebugObject::LoadedImage);
    }

    if (this->animated())
    {
        DebugCount::decrease(DebugObject::AnimatedImage);
    }
    DebugCount::decrease(DebugObject::BytesImageCurrent, this->memoryUsage());
    DebugCount::increase(DebugObject::BytesImageUnloaded, this->memoryUsage());
}

int64_t Frames::memoryUsage() const
{
    return this->storage_->memoryUsage();
}

void Frames::clear()
{
    assertInGuiThread();
    if (!this->empty())
    {
        DebugCount::decrease(DebugObject::LoadedImage);
    }
    DebugCount::decrease(DebugObject::BytesImageCurrent, this->memoryUsage());
    DebugCount::increase(DebugObject::BytesImageUnloaded, this->memoryUsage());

    this->storage_ = std::make_unique<CachedFrames>();
}

bool Frames::empty() const
{
    return this->storage_->empty();
}

bool Frames::animated() const
{
    return this->storage_->animated();
}

std::optional<QPixmap> Frames::current() const
{
    return this->storage_->current();
}

std::optional<QSize> Frames::frameSize() const
{
    return this->storage_->frameSize();
}

QList<Frame> readFrames(QImageReader &reader, const Url &url,
                        std::optional<QSize> rescale)
{
    QList<Frame> frames;
    frames.reserve(reader.imageCount());

    for (int index = 0; index < reader.imageCount(); ++index)
    {
        auto pixmap = QPixmap::fromImageReader(&reader);
        if (!pixmap.isNull())
        {
            int duration = reader.nextImageDelay();
            if (duration <= 10)
            {
                duration = 100;
            }
            duration = std::max(20, duration);

            if (rescale)
            {
                assert(!isGuiThread());
                // We're not on the main thread - put some effort into scaling.
                pixmap = pixmap.scaled(*rescale, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
            }

            frames.append(Frame{
                .image = std::move(pixmap),
                .duration = duration,
            });
        }
    }

    if (frames.empty())
    {
        qCDebug(chatterinoImage) << "Error while reading image" << url.string
                                 << ": '" << reader.errorString() << "'";
    }

    return frames;
}

void assignFrames(std::weak_ptr<Image> weak, QList<Frame> parsed)
{
    static bool isPushQueued;

    auto cb = [parsed = std::move(parsed), weak = std::move(weak)]() mutable {
        auto shared = weak.lock();
        if (!shared)
        {
            return;
        }
        shared->frames_ = std::make_unique<detail::Frames>(std::move(parsed));
        if (shared->autoScale_)
        {
            // FIXME: We should actually scale the pixmaps. However, we'd also
            //        need to cache that.
            auto frameSize = shared->frames_->frameSize();
            if (frameSize)
            {
                shared->scale_ =
                    static_cast<qreal>(*shared->autoScale_) /
                    std::max(std::min(frameSize->width(), frameSize->height()),
                             1);
            }
        }

        if (!isPushQueued)
        {
            isPushQueued = true;

            QMetaObject::invokeMethod(
                qApp,
                [] {
                    isPushQueued = false;
                    auto *app = tryGetApp();
                    if (app != nullptr)
                    {
                        app->getWindows()->forceLayoutChannelViews();
                    }
                },
                Qt::QueuedConnection);
        }

#ifndef DISABLE_IMAGE_EXPIRATION_POOL
        ImageExpirationPool::instance().requestBudgetCheck();
#endif
    };

    postToGuiThread(cb);
}

std::vector<size_t> pickImageBudgetEvictions(
    const std::vector<ImageBudgetEntry> &entries, int64_t maxBytes,
    int64_t targetBytes)
{
    assert(targetBytes <= maxBytes);

    int64_t totalBytes = 0;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        totalBytes += entries[i].bytes;
        if (entries[i].evictable && entries[i].bytes > 0)
        {
            candidates.push_back(i);
        }
    }

    if (totalBytes <= maxBytes)
    {
        return {};
    }

    std::ranges::stable_sort(candidates, [&](size_t left, size_t right) {
        return entries[left].lastUsed < entries[right].lastUsed;
    });

    std::vector<size_t> evictions;
    for (auto index : candidates)
    {
        if (totalBytes <= targetBytes)
        {
            break;
        }
        evictions.push_back(index);
        totalBytes -= entries[index].bytes;
    }

    return evictions;
}

}  // namespace chatterino::detail

namespace chatterino {

Image::~Image()
{
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
    ImageExpirationPool::instance().removeImagePtr(this);
#endif

    if (this->empty_ && !this->frames_)
    {
        return;
    }

    if (isAppAboutToQuit())
    {
        if (this->frames_)
        {
            std::ignore = this->frames_.release();
        }
        return;
    }

    if (!isGuiThread())
    {
        postToThread([frames = this->frames_.release()]() {
            delete frames;
        });
    }
}

ImagePtr Image::fromUrl(const Url &url, qreal scale, QSize expectedSize)
{
    struct CacheKey {
        Url url;
        qreal scale;
        QSize expectedSize;
        bool useExpectedSizeForRead;

        bool operator==(const CacheKey &other) const
        {
            return this->url == other.url && this->scale == other.scale &&
                   this->expectedSize == other.expectedSize &&
                   this->useExpectedSizeForRead == other.useExpectedSizeForRead;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey &key) const
        {
            size_t seed = qHash(key.url.string);
            boost::hash_combine(seed, key.scale);
            boost::hash_combine(seed, key.expectedSize.width());
            boost::hash_combine(seed, key.expectedSize.height());
            boost::hash_combine(seed, key.useExpectedSizeForRead);
            return seed;
        }
    };

    static std::unordered_map<CacheKey, std::weak_ptr<Image>, CacheKeyHash>
        cache;
    static std::mutex mutex;

    const CacheKey key{
        .url = url,
        .scale = scale,
        .expectedSize =
            expectedSize.isValid() ? expectedSize : (QSize(16, 16) * scale),
        .useExpectedSizeForRead = expectedSize.isValid(),
    };

    std::lock_guard<std::mutex> lock(mutex);

    auto shared = cache[key].lock();

    if (!shared)
    {
        cache[key] = shared = ImagePtr(new Image(url, scale, expectedSize));
    }

    return shared;
}

ImagePtr Image::fromAutoscaledUrl(const Url &url, uint16_t autoScale)
{
    auto shared = Image::fromUrl(url, 1.0, {autoScale, autoScale});
    shared->autoScale_ = autoScale;

    return shared;
}

ImagePtr Image::fromResourcePixmap(const QPixmap &pixmap, qreal scale)
{
    using key_t = std::pair<const QPixmap *, qreal>;
    static std::unordered_map<key_t, std::weak_ptr<Image>, boost::hash<key_t>>
        cache;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);

    auto it = cache.find({&pixmap, scale});
    if (it != cache.end())
    {
        auto shared = it->second.lock();
        if (shared)
        {
            return shared;
        }

        cache.erase(it);
    }

    auto newImage = ImagePtr(new Image(scale));

    newImage->setPixmap(pixmap);

    cache.insert({{&pixmap, scale}, std::weak_ptr<Image>(newImage)});

    return newImage;
}

ImagePtr Image::getEmpty()
{
    static auto empty = ImagePtr(new Image);
    return empty;
}

ImagePtr getEmptyImagePtr()
{
    return Image::getEmpty();
}

Image::Image()
    : empty_(true)
    , frames_(std::make_unique<detail::Frames>())
{
}

Image::Image(const Url &url, qreal scale, QSize expectedSize)
    : url_(url)
    , scale_(scale)
    , expectedSize_(expectedSize.isValid() ? expectedSize
                                           : (QSize(16, 16) * scale))
    , useExpectedSizeForRead_(expectedSize.isValid())
    , shouldLoad_(true)
    , frames_(std::make_unique<detail::Frames>())
{
}

Image::Image(qreal scale)
    : scale_(scale)
    , frames_(std::make_unique<detail::Frames>())
{
}

void Image::setPixmap(const QPixmap &pixmap)
{
    auto setFrames = [shared = this->shared_from_this(), pixmap]() {
        shared->frames_ = std::make_unique<detail::Frames>(
            QList<detail::Frame>{detail::Frame{pixmap, 1}});
    };

    if (isGuiThread())
    {
        setFrames();
    }
    else
    {
        postToThread(setFrames);
    }
}

const Url &Image::url() const
{
    return this->url_;
}

bool Image::loaded() const
{
    assertInGuiThread();

    return this->frames_->current().has_value();
}

std::optional<QPixmap> Image::pixmapOrLoad() const
{
    assertInGuiThread();

    this->lastUsed_ = std::chrono::steady_clock::now();

    this->load();

    return this->frames_->current();
}

void Image::load() const
{
    assertInGuiThread();

    if (this->shouldLoad_)
    {
        Image *this2 = const_cast<Image *>(this);
        this2->shouldLoad_ = false;
        this2->actuallyLoad();
#ifndef DISABLE_IMAGE_EXPIRATION_POOL
        ImageExpirationPool::instance().addImagePtr(this2->shared_from_this());
#endif
    }
}

qreal Image::scale() const
{
    return this->scale_;
}

bool Image::isEmpty() const
{
    return this->empty_;
}

bool Image::animated() const
{
    assertInGuiThread();

    return this->frames_->animated();
}

void Image::setFrameCacheLifetime(std::chrono::milliseconds lifetime)
{
    this->frameCacheLifetimeMs_.store(std::max<int64_t>(0, lifetime.count()),
                                      std::memory_order_relaxed);
}

int Image::width() const
{
    assertInGuiThread();

    if (!this->frames_)
    {
        return 0;
    }

    if (auto size = this->frames_->frameSize())
    {
        return static_cast<int>(size->width() * this->scale_);
    }

    return static_cast<int>(this->expectedSize_.width() * this->scale_);
}

int Image::height() const
{
    assertInGuiThread();

    if (!this->frames_)
    {
        return 0;
    }

    if (auto size = this->frames_->frameSize())
    {
        return static_cast<int>(size->height() * this->scale_);
    }

    return static_cast<int>(this->expectedSize_.height() * this->scale_);
}

QSizeF Image::size() const
{
    assertInGuiThread();

    if (!this->frames_)
    {
        return {0, 0};
    }

    if (auto size = this->frames_->frameSize())
    {
        return size->toSizeF() * this->scale_;
    }

    return this->expectedSize_.toSizeF() * this->scale_;
}

void Image::actuallyLoad()
{
    auto weak = weakOf(this);
    auto onSuccess = [weak](const auto &result) {
        auto shared = weak.lock();
        if (!shared)
        {
            return;
        }

        assert(!isAppAboutToQuit());

        QBuffer buffer;
        buffer.setData(result.getData());
        QImageReader reader(&buffer);

        if (!reader.canRead())
        {
            qCDebug(chatterinoImage)
                << "Error: image cant be read " << shared->url().string;
            shared->empty_ = true;
            return;
        }

        const auto size = reader.size();
        if (size.isEmpty())
        {
            shared->empty_ = true;
            return;
        }

        if (reader.imageCount() <= 0)
        {
            qCDebug(chatterinoImage)
                << "Error: image has less than 1 frame " << shared->url().string
                << ": " << reader.errorString();
            shared->empty_ = true;
            return;
        }

        // Autoscaled images only use expectedSize_ as a layout placeholder.
        // Decoding at that size throws away the source pixels (JilChat badges
        // are 128px and would become an 18px bitmap).
        if (shared->useExpectedSizeForRead_ && !shared->autoScale_)
        {
            reader.setScaledSize(shared->expectedSize_);
        }

        auto [limitedSize, needsLimit] =
            limitedAutoScaleSize(size, shared->autoScale_);

        // use "double" to prevent int overflows
        if (double(limitedSize.width()) * double(limitedSize.height()) *
                double(reader.imageCount()) * 4.0 >
            double(Image::maxBytesRam))
        {
            qCDebug(chatterinoImage) << "image too large in RAM";

            shared->empty_ = true;
            postToGuiThread([] {
                if (auto *app = tryGetApp())
                {
                    app->getWindows()->forceLayoutChannelViews();
                }
            });
            return;
        }

        auto parsed = detail::readFrames(
            reader, shared->url(),
            makeConditionedOptional(needsLimit, limitedSize));

        assignFrames(shared, parsed);
    };
    auto onError = [weak](const auto &) {
        auto shared = weak.lock();
        if (!shared)
        {
            return false;
        }

        shared->empty_ = true;

        return true;
    };

    if (this->url_.string.startsWith(u":/"))
    {
        QThreadPool::globalInstance()->start([onSuccess = std::move(onSuccess),
                                              onError = std::move(onError),
                                              url = this->url_.string] {
            QByteArray data;
            {
                QFile file(url);
                if (!file.open(QFile::ReadOnly))
                {
                    onError(NetworkResult{
                        NetworkResult::NetworkError::ContentNotFoundError,
                        404,
                        {},
                    });
                    return;
                }
                data = file.readAll();
            }
            onSuccess(NetworkResult{
                NetworkResult::NetworkError::NoError,
                200,
                std::move(data),
            });
        });
    }
    else
    {
        NetworkRequest(this->url().string)
            .concurrent()
            .cache()
            .onSuccess(std::move(onSuccess))
            .onError(std::move(onError))
            .execute();
    }
}

void Image::expireFrames()
{
    assertInGuiThread();
    this->frames_->clear();
    this->shouldLoad_ = true;
}

#ifndef DISABLE_IMAGE_EXPIRATION_POOL

ImageExpirationPool::ImageExpirationPool()
    : freeTimer_(new QTimer)
{
    QObject::connect(this->freeTimer_, &QTimer::timeout, [this] {
        if (isGuiThread())
        {
            this->freeOld();
        }
        else
        {
            postToThread([this] {
                this->freeOld();
            });
        }
    });

    this->freeTimer_->start(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            IMAGE_POOL_CLEANUP_INTERVAL));
}

ImageExpirationPool &ImageExpirationPool::instance()
{
    static auto *instance = new ImageExpirationPool;
    return *instance;
}

void ImageExpirationPool::addImagePtr(ImagePtr imgPtr)
{
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->allImages_.emplace(imgPtr.get(), std::weak_ptr<Image>(imgPtr));
}

void ImageExpirationPool::removeImagePtr(Image *rawPtr)
{
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->allImages_.erase(rawPtr);
}

std::vector<ImagePtr> ImageExpirationPool::snapshotImages()
{
    std::vector<ImagePtr> images;

    std::lock_guard<std::mutex> lock(this->mutex_);
    images.reserve(this->allImages_.size());
    for (auto it = this->allImages_.begin(); it != this->allImages_.end();)
    {
        if (auto img = it->second.lock())
        {
            images.push_back(std::move(img));
            ++it;
        }
        else
        {
            it = this->allImages_.erase(it);
        }
    }

    return images;
}

void ImageExpirationPool::freeAll()
{
    assertInGuiThread();

    for (const auto &img : this->snapshotImages())
    {
        img->expireFrames();
        this->removeImagePtr(img.get());
    }
    this->freeOld();
}

void ImageExpirationPool::freeOld()
{
    assertInGuiThread();

    size_t numExpired = 0;
    size_t eligible = 0;
    int64_t freedBytes = 0;

    auto now = std::chrono::steady_clock::now();
    for (const auto &img : this->snapshotImages())
    {
        if (img->frames_->empty())
        {
            continue;
        }

        ++eligible;

        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - img->lastUsed_);
        const auto customLifetimeMs =
            img->frameCacheLifetimeMs_.load(std::memory_order_relaxed);
        const auto lifetime =
            customLifetimeMs > 0
                ? std::chrono::milliseconds{customLifetimeMs}
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      IMAGE_POOL_IMAGE_LIFETIME);
        if (diff > lifetime)
        {
            ++numExpired;
            freedBytes += img->frames_->memoryUsage();
            img->expireFrames();
            this->removeImagePtr(img.get());
        }
    }

#    ifndef NDEBUG
    qCDebug(chatterinoImage) << "freed frame data for" << numExpired << "/"
                             << eligible << "eligible images";
#    endif
    DebugCount::set(DebugObject::LastImageGcExpired, numExpired);
    DebugCount::set(DebugObject::LastImageGcEligible, eligible);
    {
        std::lock_guard<std::mutex> lock(this->mutex_);
        DebugCount::set(DebugObject::LastImageGcLeft, this->allImages_.size());
    }

    if (freedBytes >= IMAGE_POOL_RELIEF_BYTES)
    {
        requestMemoryPressureRelief();
    }
}

void ImageExpirationPool::requestBudgetCheck()
{
    assertInGuiThread();

    if (this->budgetCheckQueued_ ||
        DebugCount::get(DebugObject::BytesImageCurrent) <= IMAGE_POOL_MAX_BYTES)
    {
        return;
    }

    // images often load in bursts, check once they're all in
    this->budgetCheckQueued_ = true;
    QTimer::singleShot(0, qApp, [this] {
        this->runBudgetCheck();
    });
}

void ImageExpirationPool::runBudgetCheck()
{
    assertInGuiThread();

    if (this->enforceBudget())
    {
        // whatever is left was in use, check again once it might not be
        QTimer::singleShot(IMAGE_POOL_ACTIVE_IMAGE_GRACE, qApp, [this] {
            this->runBudgetCheck();
        });
        return;
    }

    this->budgetCheckQueued_ = false;
}

bool ImageExpirationPool::enforceBudget()
{
    assertInGuiThread();

    const auto images = this->snapshotImages();
    const auto now = std::chrono::steady_clock::now();

    std::vector<detail::ImageBudgetEntry> entries;
    entries.reserve(images.size());
    int64_t totalBytes = 0;
    for (const auto &img : images)
    {
        const auto bytes = img->frames_->memoryUsage();
        totalBytes += bytes;
        entries.push_back({
            .bytes = bytes,
            .lastUsed = img->lastUsed_,
            // static images are small and may still be drawn in a message
            // buffer without being painted again, so leave them to freeOld()
            .evictable = img->frames_->animated() &&
                         now - img->lastUsed_ >= IMAGE_POOL_ACTIVE_IMAGE_GRACE,
        });
    }

    const auto evictions = detail::pickImageBudgetEvictions(
        entries, IMAGE_POOL_MAX_BYTES, IMAGE_POOL_TARGET_BYTES);

    int64_t freedBytes = 0;
    for (auto index : evictions)
    {
        const auto &img = images[index];
        freedBytes += entries[index].bytes;
        img->expireFrames();
        this->removeImagePtr(img.get());
    }

    if (!evictions.empty())
    {
        qCDebug(chatterinoImage)
            << "image budget evicted" << evictions.size() << "images,"
            << freedBytes << "of" << totalBytes << "bytes";
    }

    if (freedBytes >= IMAGE_POOL_RELIEF_BYTES)
    {
        requestMemoryPressureRelief();
    }

    return totalBytes - freedBytes > IMAGE_POOL_MAX_BYTES;
}

std::vector<ImageExpirationPool::ProviderUsage>
    ImageExpirationPool::getProviderUsageSnapshot()
{
    const auto providerForUrl = [](const QString &url) {
        if (url.isEmpty())
        {
            return QStringLiteral("internal");
        }
        if (url.startsWith(u":/"))
        {
            return QStringLiteral("resources");
        }

        const QUrl parsed(url);
        const auto host = parsed.host().toLower();
        const auto path = parsed.path().toLower();
        if (host.isEmpty())
        {
            return QStringLiteral("unknown");
        }

        if (host.contains(u"chatterinohomies.com") ||
            host == u"itzalex.github.io")
        {
            return QStringLiteral("Homies");
        }
        if (host.contains(u"7tv"))
        {
            if (path.contains(u"/emote/"))
            {
                return QStringLiteral("7TV emotes");
            }
            if (path.contains(u"/paint/"))
            {
                return QStringLiteral("7TV paints");
            }
            if (path.contains(u"/badge/"))
            {
                return QStringLiteral("7TV badges");
            }
            if (path.contains(u"/cosmetic/"))
            {
                return QStringLiteral("7TV cosmetics");
            }
            return QStringLiteral("7TV");
        }
        if (host.contains(u"jtvnw.net") || host.contains(u"ttvnw.net") ||
            host.contains(u"twitch.tv"))
        {
            return QStringLiteral("Twitch");
        }
        if (host.contains(u"betterttv") || host.contains(u"bttv"))
        {
            return QStringLiteral("BTTV");
        }
        if (host.contains(u"frankerfacez") || host.contains(u"ffzap"))
        {
            return QStringLiteral("FFZ");
        }
        if (host.contains(u"chatterino"))
        {
            return QStringLiteral("Chatterino");
        }

        return host;
    };

    std::map<QString, ProviderUsage> providers;

    for (const auto &img : this->snapshotImages())
    {
        if (img->frames_->empty())
        {
            continue;
        }

        const auto bytes = img->frames_->memoryUsage();
        if (bytes <= 0)
        {
            continue;
        }

        const auto provider = providerForUrl(img->url_.string);
        auto &usage = providers[provider];
        usage.provider = provider;
        usage.bytes += bytes;
        usage.images += 1;
        if (img->frames_->animated())
        {
            usage.animatedImages += 1;
        }
    }

    std::vector<ProviderUsage> result;
    result.reserve(providers.size());
    for (auto &[_, usage] : providers)
    {
        result.push_back(std::move(usage));
    }

    std::ranges::sort(result, [](const auto &a, const auto &b) {
        return a.bytes > b.bytes;
    });

    return result;
}

#endif

}  // namespace chatterino
