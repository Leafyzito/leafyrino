// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/RapidjsonHelpers.hpp"

#include <pajlada/serialize.hpp>
#include <QString>

namespace chatterino {

/// A 7TV paint the user chose not to render.
struct SeventvHiddenPaint {
    QString id;
    /// Display name at the time the paint was hidden.
    QString name;

    bool operator==(const SeventvHiddenPaint &other) const
    {
        return this->id == other.id;
    }
};

}  // namespace chatterino

namespace pajlada {

template <>
struct Serialize<chatterino::SeventvHiddenPaint> {
    static rapidjson::Value get(const chatterino::SeventvHiddenPaint &value,
                                rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value ret(rapidjson::kObjectType);

        chatterino::rj::set(ret, "id", value.id, a);
        chatterino::rj::set(ret, "name", value.name, a);

        return ret;
    }
};

template <>
struct Deserialize<chatterino::SeventvHiddenPaint> {
    static chatterino::SeventvHiddenPaint get(const rapidjson::Value &value,
                                              bool *error = nullptr)
    {
        if (!value.IsObject())
        {
            PAJLADA_REPORT_ERROR(error);
            return {};
        }

        chatterino::SeventvHiddenPaint paint;
        if (!chatterino::rj::getSafe(value, "id", paint.id) ||
            paint.id.isEmpty())
        {
            PAJLADA_REPORT_ERROR(error);
            return {};
        }
        chatterino::rj::getSafe(value, "name", paint.name);

        return paint;
    }
};

}  // namespace pajlada
