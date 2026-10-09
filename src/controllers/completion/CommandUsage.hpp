#pragma once

#include <QStringList>

#include <vector>

namespace chatterino::completion {

struct CommandUsageOption {
    QString name;
    QString valueHint;
    QStringList aliases;

    bool operator==(const CommandUsageOption &) const = default;
};

struct CommandUsage {
    enum class Kind {
        Positional,
        Pipes,
        Named,
        Alternatives,
        Static,
    };

    Kind kind = Kind::Positional;
    QString text;
    QStringList fields;
    std::vector<CommandUsage> forms;
    std::vector<CommandUsageOption> options;

    bool operator==(const CommandUsage &) const = default;
};

QStringList splitCommandUsageFields(const QString &usage);
QStringList splitCommandUsageForms(const QString &usage);
CommandUsage compileCommandUsage(const QString &usage,
                                 bool alternatives = false,
                                 bool staticUsage = false);
QString remainingCommandUsage(const CommandUsage &usage,
                              const QString &arguments,
                              bool *appendDirectly = nullptr);

}
