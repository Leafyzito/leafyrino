// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Version.hpp"

#include <QFileInfo>
#include <QStringBuilder>

#define STRINGIFY(x) #x

#define STRINGIFY2(x) STRINGIFY(x)

namespace chatterino {

using namespace Qt::Literals::StringLiterals;

namespace {

QString makeDefaultInternalVersion(const QString &commitHash, bool isModified,
                                   const QString &dateOfBuild)
{
    QString normalizedDate = dateOfBuild;
    normalizedDate.remove('-');
    if (normalizedDate.isEmpty())
    {
        normalizedDate = "unknown";
    }

    QString shortHash = commitHash.left(7);
    if (shortHash.isEmpty())
    {
        shortHash = "local";
    }

    auto internal = "molto-" + normalizedDate + "-" + shortHash;
    if (isModified)
    {
        internal += "-dirty";
    }
    return internal;
}

}

Version::Version()
    : version_(CHATTERINO_VERSION)
    , commitHash_(QStringLiteral(CHATTERINO_GIT_HASH))
    , isModified_(CHATTERINO_GIT_MODIFIED == 1)
    , dateOfBuild_(QStringLiteral(CHATTERINO_CMAKE_GEN_DATE))
    , isNightly_(CHATTERINO_NIGHTLY_BUILD == 1)
{
    this->fullVersion_ = "Moltorino ";
    if (this->isNightly())
    {
        this->fullVersion_ += "Nightly ";
    }

    this->fullVersion_ += this->version_;

    const auto configuredInternalVersion =
        QStringLiteral(STRINGIFY2(MOLTORINO_INTERNAL_VERSION))
            .trimmed()
            .remove(u'"');
    if (!configuredInternalVersion.isEmpty() &&
        configuredInternalVersion != "__AUTO__")
    {
        this->internalVersion_ = configuredInternalVersion;
    }
    else
    {
        this->internalVersion_ = makeDefaultInternalVersion(
            this->commitHash_, this->isModified_, this->dateOfBuild_);
    }

#ifndef NDEBUG
    this->fullVersion_ += " DEBUG";
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    this->isSupportedOS_ = true;
#else
    this->isSupportedOS_ = false;
#endif

    this->generateBuildString();
    this->generateRunningString();
    this->generateExtraString();

#ifdef Q_OS_WIN

    this->appUserModelID_ = L"MoltoBenne.Moltorino7";
#endif
}

const Version &Version::instance()
{
    static Version instance;
    return instance;
}

const QString &Version::version() const
{
    return this->version_;
}

const QString &Version::internalVersion() const
{
    return this->internalVersion_;
}

const QString &Version::fullVersion() const
{
    return this->fullVersion_;
}

const QString &Version::commitHash() const
{
    return this->commitHash_;
}

const bool &Version::isModified() const
{
    return this->isModified_;
}

const QString &Version::dateOfBuild() const
{
    return this->dateOfBuild_;
}

const bool &Version::isSupportedOS() const
{
    return this->isSupportedOS_;
}

bool Version::isFlatpak() const
{
    return QFileInfo::exists("/.flatpak-info");
}

QStringList Version::buildTags() const
{
    QStringList tags;

    const auto *runtimeVersion = qVersion();
    if (runtimeVersion != QLatin1String{QT_VERSION_STR})
    {
        tags.append(u"Qt "_s QT_VERSION_STR u" (running on " % runtimeVersion %
                    u")");
    }
    else
    {
        tags.append(u"Qt "_s QT_VERSION_STR);
    }

#ifdef _MSC_FULL_VER
    tags.append("MSVC " + QString::number(_MSC_FULL_VER, 10));
#endif
#ifdef CHATTERINO_WITH_CRASHPAD
    tags.append("Crashpad");
#endif

    return tags;
}

const QString &Version::buildString() const
{
    return this->buildString_;
}

const QString &Version::runningString() const
{
    return this->runningString_;
}

const QString &Version::extraString() const
{
    return this->extraString_;
}

bool Version::isNightly() const
{
    return this->isNightly_;
}

void Version::generateBuildString()
{

    auto s = this->fullVersion();

    s +=
        QString(
            R"( (commit <a href="https://github.com/MoltoBenne/Moltorino/commit/%1">%1</a>)")
            .arg(this->commitHash());
    if (this->isModified())
    {
        s += " modified)";
    }
    else
    {
        s += ")";
    }

    s += " built";

    if (this->isNightly())
    {
        s += " on " + this->dateOfBuild();
    }

    s += " with " + this->buildTags().join(", ");
    s += " [internal " + this->internalVersion() + "]";

    this->buildString_ = s;
}

void Version::generateRunningString()
{
    auto s = QString("Running on %1, kernel: %2")
                 .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());

    if (!this->internalVersion().isEmpty())
    {
        s += ", build: " + this->internalVersion();
    }

    if (!this->isSupportedOS())
    {
        s += " (unsupported OS)";
    }

    this->runningString_ = s;
}

void Version::generateExtraString()
{
    this->extraString_ =
        QStringLiteral(STRINGIFY2(CHATTERINO_EXTRA_BUILD_STRING)).trimmed();
}

#undef STRINGIFY2
#undef STRINGIFY

#ifdef Q_OS_WIN
const std::wstring &Version::appUserModelID() const
{
    return this->appUserModelID_;
}
#endif

}
