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
#include "util/PostToThread.hpp"

#include <boost/functional/hash.hpp>
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

const auto IMAGE_POOL_CLEANUP_INTERVAL = std::chrono::minutes(1);

const auto IMAGE_POOL_IMAGE_LIFETIME = std::chrono::minutes(10);

namespace chatterino::detail {

Frames::Frames()
{
    DebugCount::increase(DebugObject::Image);
}

Frames::Frames(QList<Frame> &&frames)
    : items_(std::move(frames))
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

        this->gifTimerConnection_ =
            app->getEmotes()->getGIFTimer()->signal.connect([this] {
                this->advance();
            });

        auto totalLength =
            std::accumulate(this->items_.begin(), this->items_.end(), 0UL,
                            [](auto init, auto &&frame) {
                                return init + frame.duration;
                            });

        if (totalLength == 0)
        {
            this->durationOffset_ = 0;
        }
        else
        {
            this->durationOffset_ = std::min<int>(
                int(app->getEmotes()->getGIFTimer()->position() % totalLength),
                60000);
        }
        this->processOffset();
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

    this->gifTimerConnection_.disconnect();
}

int64_t Frames::memoryUsage() const
{
    int64_t usage = 0;
    for (const auto &frame : this->items_)
    {
        auto sz = frame.image.size();
        auto area = sz.width() * sz.height();
        auto memory = area * frame.image.depth() / 8;

        usage += memory;
    }
    return usage;
}

void Frames::advance()
{
    this->durationOffset_ += GIF_FRAME_LENGTH;
    this->processOffset();
}

void Frames::processOffset()
{
    if (this->items_.isEmpty())
    {
        return;
    }

    while (true)
    {
        this->index_ %= this->items_.size();

        if (this->durationOffset_ > this->items_[this->index_].duration)
        {
            this->durationOffset_ -= this->items_[this->index_].duration;
            this->index_ = (this->index_ + 1) % this->items_.size();
        }
        else
        {
            break;
        }
    }
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

    this->items_.clear();
    this->index_ = 0;
    this->durationOffset_ = 0;
    this->gifTimerConnection_.disconnect();
}

bool Frames::empty() const
{
    return this->items_.empty();
}

bool Frames::animated() const
{
    return this->items_.size() > 1;
}

std::optional<QPixmap> Frames::current() const
{
    if (this->items_.empty())
    {
        return std::nullopt;
    }

    return this->items_[this->index_].image;
}

std::optional<QPixmap> Frames::first() const
{
    if (this->items_.empty())
    {
        return std::nullopt;
    }

    return this->items_.front().image;
}

QList<Frame> readFrames(QImageReader &reader, const Url &url)
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
            auto firstFrame = shared->frames_->first();
            if (firstFrame)
            {
                auto actualSize = firstFrame->size();
                shared->scale_ =
                    static_cast<qreal>(*shared->autoScale_) /
                    std::max({actualSize.width(), actualSize.height(), 1});
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
    };

    postToGuiThread(cb);
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
    static std::unordered_map<Url, std::weak_ptr<Image>> cache;
    static std::mutex mutex;

    std::lock_guard<std::mutex> lock(mutex);

    auto shared = cache[url].lock();

    if (!shared)
    {
        cache[url] = shared = ImagePtr(new Image(url, scale, expectedSize));
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

    if (auto pixmap = this->frames_->first())
    {
        return static_cast<int>(pixmap->width() * this->scale_);
    }

    return static_cast<int>(this->expectedSize_.width() * this->scale_);
}

int Image::height() const
{
    assertInGuiThread();

    if (auto pixmap = this->frames_->first())
    {
        return static_cast<int>(pixmap->height() * this->scale_);
    }

    return static_cast<int>(this->expectedSize_.height() * this->scale_);
}

QSizeF Image::size() const
{
    assertInGuiThread();

    if (auto pixmap = this->frames_->first())
    {
        return pixmap->size().toSizeF() * this->scale_;
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

        if (shared->useExpectedSizeForRead_)
        {
            reader.setScaledSize(shared->expectedSize_);
        }

        if (double(size.width()) * double(size.height()) *
                double(reader.imageCount()) * 4.0 >
            double(Image::maxBytesRam))
        {
            qCDebug(chatterinoImage) << "image too large in RAM";

            shared->empty_ = true;
            return;
        }

        auto parsed = detail::readFrames(reader, shared->url());

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

void ImageExpirationPool::freeAll()
{
    {
        std::lock_guard<std::mutex> lock(this->mutex_);
        for (auto it = this->allImages_.begin(); it != this->allImages_.end();)
        {
            auto img = it->second.lock();
            img->expireFrames();
            it = this->allImages_.erase(it);
        }
    }
    this->freeOld();
}

void ImageExpirationPool::freeOld()
{
    std::lock_guard<std::mutex> lock(this->mutex_);

    size_t numExpired = 0;
    size_t eligible = 0;

    auto now = std::chrono::steady_clock::now();
    for (auto it = this->allImages_.begin(); it != this->allImages_.end();)
    {
        auto img = it->second.lock();
        if (!img)
        {
            it = this->allImages_.erase(it);
            continue;
        }

        if (img->frames_->empty())
        {
            ++it;
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
            img->expireFrames();

            it = this->allImages_.erase(it);
            continue;
        }

        ++it;
    }

#    ifndef NDEBUG
    qCDebug(chatterinoImage) << "freed frame data for" << numExpired << "/"
                             << eligible << "eligible images";
#    endif
    DebugCount::set(DebugObject::LastImageGcExpired, numExpired);
    DebugCount::set(DebugObject::LastImageGcEligible, eligible);
    DebugCount::set(DebugObject::LastImageGcLeft, this->allImages_.size());
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

    std::lock_guard<std::mutex> lock(this->mutex_);
    for (auto it = this->allImages_.begin(); it != this->allImages_.end();)
    {
        auto img = it->second.lock();
        if (!img)
        {
            it = this->allImages_.erase(it);
            continue;
        }

        if (img->frames_->empty())
        {
            ++it;
            continue;
        }

        const auto bytes = img->frames_->memoryUsage();
        if (bytes <= 0)
        {
            ++it;
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

        ++it;
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
