// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#define QT_NO_CAST_FROM_ASCII
#include "common/LinkParser.hpp"

#include "common/QLogging.hpp"
#include "util/QCompareTransparent.hpp"

#include <QFile>
#include <QString>
#include <QStringView>
#include <QTextStream>

#include <set>

namespace {

using namespace chatterino;

using TldSet = std::set<QString, QCompareCaseInsensitive>;

TldSet &tlds()
{
    static TldSet tlds = [] {
        QFile file(QStringLiteral(":/tlds.txt"));
        bool ok = file.open(QFile::ReadOnly);
        if (!ok)
        {
            assert(false && "Resources not available");
            qCWarning(chatterinoApp) << "Resources not available";
        }
        QTextStream stream(&file);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

#else
        stream.setCodec("UTF-8");
#endif

        TldSet set;

        while (!stream.atEnd())
        {
            set.emplace(stream.readLine());
        }

        return set;
    }();
    return tlds;
}

bool isValidTld(QStringView tld)
{
    return tlds().contains(tld);
}

bool isValidIpv4(QStringView host)
{

    char16_t sectionValue = 0;
    uint8_t octetNumber = 0;
    uint8_t sectionDigits = 0;
    bool lastWasDot = true;

    for (auto c : host)
    {
        char16_t current = c.unicode();
        if (current == '.')
        {
            if (lastWasDot || octetNumber == 3)
            {
                return false;
            }
            lastWasDot = true;
            octetNumber++;
            sectionValue = 0;
            sectionDigits = 0;
            continue;
        }
        lastWasDot = false;

        if (current > u'9' || current < u'0')
        {
            return false;
        }

        sectionValue = sectionValue * 10 + (current - u'0');
        sectionDigits++;
        if (sectionValue >= 256 || sectionDigits > 3)
        {
            return false;
        }
    }

    return octetNumber == 3 && !lastWasDot;
}

bool startsWithPort(QStringView string)
{
    for (qsizetype i = 0; i < std::min<qsizetype>(5, string.length()); i++)
    {
        char16_t c = string[i].unicode();
        if (i >= 1 && (c == u'/' || c == u'?' || c == u'#'))
        {
            return true;
        }

        if (!string[i].isDigit())
        {
            return false;
        }
    }
    return true;
}

void strip(QStringView &source)
{
    while (!source.isEmpty())
    {
        auto c = source.first();
        if (c == u'<' || c == u'*' || c == u'_' || c == u'~' || c == u'(')
        {
            source = source.mid(1);
            continue;
        }
        break;
    }

    while (!source.isEmpty())
    {
        auto c = source.last();
        if (c == u'>' || c == u'?' || c == u'!' || c == u'.' || c == u',' ||
            c == u':' || c == u'*' || c == u'~' || c == u')')
        {
            source.chop(1);
            continue;
        }
        break;
    }
}

Q_ALWAYS_INLINE bool isValidDomainChar(char16_t c)
{
    return c >= 0x80 || (u'0' <= c && c <= u'9') || (u'A' <= c && c <= u'Z') ||
           (u'a' <= c && c <= u'z') || c == u'_' || c == u'-' || c == u'.';
}

}

namespace chatterino::linkparser {

std::optional<Parsed> parse(QStringView source) noexcept
{
    using SizeType = QString::size_type;

    std::optional<Parsed> result;

    QStringView link{source};
    strip(link);

    QStringView remaining = link;
    QStringView protocol;

    if (remaining.startsWith(u"http", Qt::CaseInsensitive) &&
        remaining.length() >= 4 + 3 + 1)
    {

        auto withProto = remaining.mid(4);

        if (withProto[0] == QChar(u's') || withProto[0] == QChar(u'S'))
        {
            withProto = withProto.mid(1);
        }

        if (withProto.startsWith(u"://"))
        {

            remaining = withProto.mid(3);
            protocol = {link.begin(), remaining.begin()};
        }
    }

    QStringView host = remaining;
    QStringView rest;
    bool lastWasDot = true;
    SizeType lastDotPos = -1;
    SizeType nDots = 0;

    for (SizeType i = 0; i < remaining.size(); i++)
    {
        char16_t currentChar = remaining[i].unicode();
        if (currentChar == u'.')
        {
            if (lastWasDot)
            {
                return result;
            }
            lastDotPos = i;
            lastWasDot = true;
            nDots++;
        }
        else
        {
            lastWasDot = false;
        }

        if (currentChar == u':')
        {
            host = remaining.mid(0, i);
            rest = remaining.mid(i);
            remaining = remaining.mid(i + 1);

            if (!startsWithPort(remaining))
            {
                return result;
            }

            break;
        }

        if (currentChar == u'/' || currentChar == u'?' || currentChar == u'#')
        {
            host = remaining.mid(0, i);
            rest = remaining.mid(i);
            break;
        }

        if (!isValidDomainChar(currentChar))
        {
            return result;
        }
    }

    if (lastWasDot || lastDotPos <= 0)
    {
        return result;
    }

    if ((nDots == 3 && isValidIpv4(host)) ||
        isValidTld(host.mid(lastDotPos + 1)))
    {

        if (link.end() != source.end() && !rest.empty())
        {
            size_t nestingLevel = 0;

            const auto *lastClose = link.end();

            for (const auto *it = rest.begin(); it < source.end(); it++)
            {
                if (it->unicode() == u'(')
                {
                    nestingLevel++;
                    continue;
                }

                if (nestingLevel != 0 && it->unicode() == u')')
                {
                    nestingLevel--;
                    if (nestingLevel == 0)
                    {
                        lastClose = it + 1;
                    }
                }
            }
            link = QStringView{link.begin(), std::max(link.end(), lastClose)};
            rest = QStringView{rest.begin(), std::max(rest.end(), lastClose)};
        }
        result = Parsed{
            .protocol = protocol,
            .host = host,
            .rest = rest,
            .link = link,
        };
    }

    return result;
}

}
