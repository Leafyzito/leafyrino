// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <memory>
#include <string>
#include <vector>

namespace chatterino {

class Channel;
class TwitchChannel;

struct DictionaryInfo {

    QString name;

    QString path;

    bool isSymbolicLink;
    bool isSystem;
};

class SpellCheckerPrivate;
class SpellChecker
{
public:
    SpellChecker();
    ~SpellChecker();

    bool isLoaded() const;

    bool check(const QString &word);
    std::vector<std::string> suggestions(const QString &word);

    std::vector<DictionaryInfo> getAvailableDictionaries() const;

private:
    std::unique_ptr<SpellCheckerPrivate> private_;
};

}
