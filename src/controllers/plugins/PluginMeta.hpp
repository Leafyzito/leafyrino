// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/PluginPermission.hpp"

#    include <QJsonObject>
#    include <QString>
#    include <semver/semver.hpp>

#    include <vector>

namespace chatterino {

struct PluginMeta {
    QString name;

    QString description;

    std::vector<QString> authors;

    QString license;

    semver::version version;

    QString homepage;

    std::vector<QString> tags;

    std::vector<PluginPermission> permissions;

    std::vector<QString> errors;

    bool isValid() const
    {
        return this->errors.empty();
    }

    explicit PluginMeta(const QJsonObject &obj);

    PluginMeta() = default;
};

}  // namespace chatterino

#endif
