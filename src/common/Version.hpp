// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#ifdef Q_OS_WIN
#    include <string>
#endif

namespace chatterino {

/**
 * Valid version formats, in order of latest to oldest
 *
 * Stable:
 *  - 2.4.0
 *
 * Release candidate:
 *  - 2.4.0-rc.3
 *  - 2.4.0-rc.2
 *  - 2.4.0-rc
 *
 * Beta:
 *  - 2.4.0-beta.3
 *  - 2.4.0-beta.2
 *  - 2.4.0-beta
 *
 * Alpha:
 *  - 2.4.0-alpha.3
 *  - 2.4.0-alpha.2
 *  - 2.4.0-alpha
 **/
inline const QString CHATTERINO_VERSION = QStringLiteral("2.5.5");

class Version
{
public:
    static const Version &instance();

    const QString &version() const;
    const QString &commitHash() const;

    const bool &isModified() const;

    const QString &dateOfBuild() const;

    const QString &fullVersion() const;
    const bool &isSupportedOS() const;
    bool isFlatpak() const;

    QStringList buildTags() const;

    const QString &buildString() const;

    const QString &runningString() const;

    const QString &extraString() const;

    bool isNightly() const;

#ifdef Q_OS_WIN

    const std::wstring &appUserModelID() const;
#endif

private:
    Version();

    QString version_;
    QString commitHash_;
    bool isModified_{false};
    QString dateOfBuild_;
    QString fullVersion_;
    bool isSupportedOS_;

    QString buildString_;

    void generateBuildString();

    QString runningString_;

    void generateRunningString();

    QString extraString_;

    void generateExtraString();

    bool isNightly_;

#ifdef Q_OS_WIN
    std::wstring appUserModelID_;
#endif
};

};  // namespace chatterino
