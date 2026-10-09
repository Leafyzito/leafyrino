#include "controllers/completion/CommandUsage.hpp"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace chatterino::completion {

namespace {

struct SyntaxDepth {
    QString closers;
    bool quoted = false;
    bool escaped = false;

    bool topLevel() const
    {
        return this->closers.isEmpty() && !this->quoted;
    }

    void consume(QChar character)
    {
        if (this->escaped)
        {
            this->escaped = false;
            return;
        }
        if (this->quoted && character == QChar('\\'))
        {
            this->escaped = true;
            return;
        }
        if (character == QChar('"'))
        {
            this->quoted = !this->quoted;
            return;
        }
        if (this->quoted)
        {
            return;
        }
        const auto opening = QStringLiteral("<[{(").indexOf(character);
        if (opening >= 0)
        {
            this->closers += QStringLiteral(">]})").at(opening);
        }
        else if (!this->closers.isEmpty() && this->closers.back() == character)
        {
            this->closers.chop(1);
        }
    }
};

QStringList splitUsage(const QString &usage, bool pipes)
{
    QStringList fields;
    qsizetype start = 0;
    SyntaxDepth depth;
    for (qsizetype i = 0; i < usage.size(); ++i)
    {
        const auto c = usage.at(i);
        if (depth.topLevel() && (pipes ? c == QChar('|') : c.isSpace()))
        {
            const auto field = usage.mid(start, i - start).trimmed();
            if (pipes || !field.isEmpty())
            {
                fields.push_back(field);
            }
            start = i + 1;
        }
        else
        {
            depth.consume(c);
        }
    }
    const auto field = usage.mid(start).trimmed();
    if (pipes || !field.isEmpty())
    {
        fields.push_back(field);
    }
    return fields;
}

QString unwrapped(QString field)
{
    if (field.startsWith(QChar('[')) && field.endsWith(QChar(']')))
    {
        return field.mid(1, field.size() - 2);
    }
    return field;
}

QString optionName(const QString &field)
{
    static const QRegularExpression option(
        QStringLiteral(R"(^(?:--?)?([a-zA-Z0-9_-]+)(?::|$))"));
    const auto text = unwrapped(field);

    if (!text.contains(QChar(':')) && !text.startsWith(QChar('-')))
    {
        return {};
    }
    return option.match(text).captured(1).toLower();
}

QString optionValueHint(const QString &field)
{
    const auto text = unwrapped(field);
    const auto colon = text.indexOf(QChar(':'));
    return colon < 0 ? QString{} : text.mid(colon + 1);
}

QStringList typedFields(const QString &arguments, bool pipes, bool &openQuote)
{
    QStringList fields;
    qsizetype start = 0;
    bool escaped = false;
    openQuote = false;
    for (qsizetype i = 0; i < arguments.size(); ++i)
    {
        const auto c = arguments.at(i);
        if (escaped)
        {
            escaped = false;
        }
        else if (openQuote && c == QChar('\\'))
        {
            escaped = true;
        }
        else if (c == QChar('"'))
        {
            openQuote = !openQuote;
        }
        else if (!openQuote && (pipes ? c == QChar('|') : c.isSpace()))
        {
            const auto field = arguments.mid(start, i - start).trimmed();
            if (pipes || !field.isEmpty())
            {
                fields.push_back(field);
            }
            start = i + 1;
        }
    }
    const auto field = arguments.mid(start).trimmed();
    if (pipes || !field.isEmpty())
    {
        fields.push_back(field);
    }
    return fields;
}

bool placeholder(const QString &field)
{
    return field.startsWith(QChar('<')) || field.startsWith(QChar('['));
}

}

QStringList splitCommandUsageFields(const QString &usage)
{
    return splitUsage(usage, false);
}

QStringList splitCommandUsageForms(const QString &usage)
{
    return splitUsage(usage, true);
}

CommandUsage compileCommandUsage(const QString &usage, bool alternatives,
                                 bool staticUsage)
{
    CommandUsage result;
    result.text = usage;
    if (staticUsage)
    {
        result.kind = CommandUsage::Kind::Static;
        return result;
    }
    const auto forms = splitCommandUsageForms(usage);
    if (forms.size() > 16)
    {
        result.kind = CommandUsage::Kind::Static;
        return result;
    }
    if (alternatives)
    {
        result.kind = CommandUsage::Kind::Alternatives;
        for (const auto &form : forms)
        {
            result.forms.push_back(compileCommandUsage(form));
        }
    }
    else if (forms.size() > 1 &&
             std::ranges::all_of(forms, [](const QString &form) {
                 return placeholder(form) &&
                        splitCommandUsageFields(form).size() == 1;
             }))
    {
        result.kind = CommandUsage::Kind::Pipes;
        result.fields = forms;
    }
    else if (forms.size() > 1)
    {
        result.kind = CommandUsage::Kind::Static;
    }
    else
    {
        result.fields = splitCommandUsageFields(usage);
        if (result.fields.size() > 32)
        {
            result.kind = CommandUsage::Kind::Static;
            result.fields.clear();
            return result;
        }
        if (!result.fields.isEmpty() &&
            std::ranges::all_of(result.fields, [](const QString &field) {
                return !optionName(field).isEmpty();
            }))
        {
            result.kind = CommandUsage::Kind::Named;
        }
    }
    return result;
}

QString remainingCommandUsage(const CommandUsage &usage,
                              const QString &arguments, bool *appendDirectly)
{
    if (appendDirectly)
    {
        *appendDirectly = false;
    }
    if (usage.kind == CommandUsage::Kind::Static)
    {
        return usage.text;
    }
    bool openQuote = false;
    const auto typed = typedFields(
        arguments, usage.kind == CommandUsage::Kind::Pipes, openQuote);
    if (openQuote)
    {
        return {};
    }
    if (usage.kind == CommandUsage::Kind::Pipes)
    {
        const auto field = typed.size() - 1;
        if (field >= usage.fields.size())
        {
            return {};
        }
        if (typed.back().isEmpty())
        {
            return usage.fields.mid(field).join(QStringLiteral(" | "));
        }
        const auto next =
            usage.fields.mid(field + 1).join(QStringLiteral(" | "));
        return next.isEmpty() ? QString{} : QStringLiteral("| ") + next;
    }
    if (usage.kind == CommandUsage::Kind::Alternatives)
    {
        if (typed.isEmpty())
        {
            return usage.text;
        }
        const CommandUsage *generic = nullptr;
        int genericCount = 0;
        const CommandUsage *selected = nullptr;
        qsizetype selectedLength = 0;
        for (const auto &form : usage.forms)
        {
            if (form.fields.isEmpty() || placeholder(form.fields.front()) ||
                !optionName(form.fields.front()).isEmpty())
            {
                generic = &form;
                ++genericCount;
                continue;
            }
            qsizetype matched = 0;
            while (matched < form.fields.size() && matched < typed.size() &&
                   typed.at(matched).compare(form.fields.at(matched),
                                             Qt::CaseInsensitive) == 0)
            {
                ++matched;
            }
            if (matched > selectedLength)
            {
                selected = &form;
                selectedLength = matched;
            }
        }
        if (!selected && genericCount == 1)
        {
            selected = generic;
        }
        return selected
                   ? remainingCommandUsage(*selected, arguments, appendDirectly)
                   : usage.text;
    }

    QSet<QString> supplied;
    qsizetype positional = 0;
    QString currentValueHint;
    for (const auto &token : typed)
    {
        auto name = optionName(token);
        QString valueHint;
        bool known = false;
        for (const auto &field : usage.fields)
        {
            if (!name.isEmpty() && optionName(field) == name)
            {
                valueHint = optionValueHint(field);
                known = true;
                break;
            }
        }
        for (const auto &option : usage.options)
        {
            if (!name.isEmpty() &&
                (option.name == name || option.aliases.contains(name)))
            {
                name = option.name;
                if (valueHint.isEmpty())
                {
                    valueHint = option.valueHint;
                }
                known = true;
                break;
            }
        }
        if (known)
        {
            supplied.insert(name);
            if (token == typed.back() && token.endsWith(QChar(':')) &&
                !arguments.isEmpty() && !arguments.back().isSpace())
            {
                currentValueHint = valueHint;
            }
        }
        else if (usage.kind != CommandUsage::Kind::Named)
        {
            ++positional;
        }
    }
    QStringList remaining;
    for (const auto &field : usage.fields)
    {
        const auto name = optionName(field);
        if (!name.isEmpty())
        {
            if (!supplied.contains(name))
            {
                remaining.push_back(field);
            }
        }
        else if (positional > 0)
        {
            --positional;
        }
        else
        {
            remaining.push_back(field);
        }
    }
    if (!currentValueHint.isEmpty())
    {
        remaining.prepend(currentValueHint);
        if (appendDirectly)
        {
            *appendDirectly = true;
        }
    }
    return remaining.join(QChar(' '));
}

}
