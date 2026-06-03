// SPDX-FileCopyrightText: 2022 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/RapidjsonHelpers.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/serialize.hpp>
#include <QColor>
#include <QString>

#include <optional>

namespace chatterino {

struct UserData {
    std::optional<QColor> color{std::nullopt};
    QString notes;

    bool isEmpty() const
    {
        return !this->color.has_value() && this->notes.isEmpty();
    }
};

}

namespace pajlada {

template <>
struct Serialize<chatterino::UserData> {
    static rapidjson::Value get(const chatterino::UserData &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value obj;
        obj.SetObject();
        if (value.color)
        {
            const auto &color = *value.color;
            chatterino::rj::set(obj, "color",
                                color.name().toUtf8().toStdString(), a);
        }
        if (!value.notes.isEmpty())
        {
            chatterino::rj::set(obj, "notes",
                                value.notes.toUtf8().toStdString(), a);
        }
        return obj;
    }
};

template <>
struct Deserialize<chatterino::UserData> {
    static chatterino::UserData get(const rapidjson::Value &value,
                                    bool *error = nullptr)
    {
        if (!value.IsObject())
        {
            PAJLADA_REPORT_ERROR(error)
            return chatterino::UserData{};
        }

        chatterino::UserData user;

        QString colorString;
        if (chatterino::rj::getSafe(value, "color", colorString))
        {
            QColor color(colorString);
            if (color.isValid())
            {
                user.color = color;
            }
        }

        QString notes;
        if (chatterino::rj::getSafe(value, "notes", notes))
        {
            user.notes = notes;
        }

        return user;
    }
};

}
