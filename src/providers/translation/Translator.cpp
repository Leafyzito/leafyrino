#include "providers/translation/Translator.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <optional>

namespace chatterino {

namespace {

constexpr int MAX_TRANSLATION_TEXT_LENGTH = 1200;

std::optional<TranslationResult> parseGoogleTranslationResult(
    const NetworkResult &result)
{
    const auto root = result.parseJsonArray();
    const auto segments = root.isEmpty() ? QJsonArray{} : root.at(0).toArray();
    if (segments.isEmpty())
    {
        return std::nullopt;
    }

    QString translated;
    for (const auto &segmentValue : segments)
    {
        const auto segment = segmentValue.toArray();
        if (!segment.isEmpty())
        {
            translated += segment.at(0).toString();
        }
    }
    translated = translated.trimmed();
    if (translated.isEmpty())
    {
        return std::nullopt;
    }

    auto detectedLanguage = root.size() > 2
                                ? normalizedLanguageCode(root.at(2).toString())
                                : QString{};

    return TranslationResult{
        .translatedText = translated,
        .detectedLanguage = detectedLanguage,
    };
}

}  // namespace

QString normalizedLanguageCode(QString language)
{
    language = language.trimmed().toLower();
    language.replace('_', '-');

    return language;
}

const std::vector<TranslationLanguage> &supportedTranslationLanguages()
{
    static const std::vector<TranslationLanguage> languages{
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("es"), QStringLiteral("Spanish")},
        {QStringLiteral("pt"), QStringLiteral("Portuguese")},
        {QStringLiteral("fr"), QStringLiteral("French")},
        {QStringLiteral("de"), QStringLiteral("German")},
        {QStringLiteral("it"), QStringLiteral("Italian")},
        {QStringLiteral("nl"), QStringLiteral("Dutch")},
        {QStringLiteral("pl"), QStringLiteral("Polish")},
        {QStringLiteral("tr"), QStringLiteral("Turkish")},
        {QStringLiteral("ru"), QStringLiteral("Russian")},
        {QStringLiteral("ja"), QStringLiteral("Japanese")},
        {QStringLiteral("ko"), QStringLiteral("Korean")},
        {QStringLiteral("zh-cn"), QStringLiteral("Chinese (Simplified)")},
        {QStringLiteral("zh-tw"), QStringLiteral("Chinese (Traditional)")},
        {QStringLiteral("ar"), QStringLiteral("Arabic")},
        {QStringLiteral("af"), QStringLiteral("Afrikaans")},
        {QStringLiteral("sq"), QStringLiteral("Albanian")},
        {QStringLiteral("am"), QStringLiteral("Amharic")},
        {QStringLiteral("hy"), QStringLiteral("Armenian")},
        {QStringLiteral("az"), QStringLiteral("Azerbaijani")},
        {QStringLiteral("eu"), QStringLiteral("Basque")},
        {QStringLiteral("be"), QStringLiteral("Belarusian")},
        {QStringLiteral("bn"), QStringLiteral("Bengali")},
        {QStringLiteral("bs"), QStringLiteral("Bosnian")},
        {QStringLiteral("bg"), QStringLiteral("Bulgarian")},
        {QStringLiteral("ca"), QStringLiteral("Catalan")},
        {QStringLiteral("ceb"), QStringLiteral("Cebuano")},
        {QStringLiteral("co"), QStringLiteral("Corsican")},
        {QStringLiteral("hr"), QStringLiteral("Croatian")},
        {QStringLiteral("cs"), QStringLiteral("Czech")},
        {QStringLiteral("da"), QStringLiteral("Danish")},
        {QStringLiteral("eo"), QStringLiteral("Esperanto")},
        {QStringLiteral("et"), QStringLiteral("Estonian")},
        {QStringLiteral("fi"), QStringLiteral("Finnish")},
        {QStringLiteral("fy"), QStringLiteral("Frisian")},
        {QStringLiteral("gl"), QStringLiteral("Galician")},
        {QStringLiteral("ka"), QStringLiteral("Georgian")},
        {QStringLiteral("el"), QStringLiteral("Greek")},
        {QStringLiteral("gu"), QStringLiteral("Gujarati")},
        {QStringLiteral("ht"), QStringLiteral("Haitian Creole")},
        {QStringLiteral("ha"), QStringLiteral("Hausa")},
        {QStringLiteral("haw"), QStringLiteral("Hawaiian")},
        {QStringLiteral("iw"), QStringLiteral("Hebrew")},
        {QStringLiteral("hi"), QStringLiteral("Hindi")},
        {QStringLiteral("hu"), QStringLiteral("Hungarian")},
        {QStringLiteral("is"), QStringLiteral("Icelandic")},
        {QStringLiteral("id"), QStringLiteral("Indonesian")},
        {QStringLiteral("ga"), QStringLiteral("Irish")},
        {QStringLiteral("jv"), QStringLiteral("Javanese")},
        {QStringLiteral("kn"), QStringLiteral("Kannada")},
        {QStringLiteral("kk"), QStringLiteral("Kazakh")},
        {QStringLiteral("km"), QStringLiteral("Khmer")},
        {QStringLiteral("ku"), QStringLiteral("Kurdish")},
        {QStringLiteral("ky"), QStringLiteral("Kyrgyz")},
        {QStringLiteral("lo"), QStringLiteral("Lao")},
        {QStringLiteral("la"), QStringLiteral("Latin")},
        {QStringLiteral("lv"), QStringLiteral("Latvian")},
        {QStringLiteral("lt"), QStringLiteral("Lithuanian")},
        {QStringLiteral("lb"), QStringLiteral("Luxembourgish")},
        {QStringLiteral("mk"), QStringLiteral("Macedonian")},
        {QStringLiteral("mg"), QStringLiteral("Malagasy")},
        {QStringLiteral("ms"), QStringLiteral("Malay")},
        {QStringLiteral("ml"), QStringLiteral("Malayalam")},
        {QStringLiteral("mt"), QStringLiteral("Maltese")},
        {QStringLiteral("mi"), QStringLiteral("Maori")},
        {QStringLiteral("mr"), QStringLiteral("Marathi")},
        {QStringLiteral("mn"), QStringLiteral("Mongolian")},
        {QStringLiteral("my"), QStringLiteral("Myanmar (Burmese)")},
        {QStringLiteral("ne"), QStringLiteral("Nepali")},
        {QStringLiteral("no"), QStringLiteral("Norwegian")},
        {QStringLiteral("ps"), QStringLiteral("Pashto")},
        {QStringLiteral("fa"), QStringLiteral("Persian")},
        {QStringLiteral("ro"), QStringLiteral("Romanian")},
        {QStringLiteral("sr"), QStringLiteral("Serbian")},
        {QStringLiteral("si"), QStringLiteral("Sinhala")},
        {QStringLiteral("sk"), QStringLiteral("Slovak")},
        {QStringLiteral("sl"), QStringLiteral("Slovenian")},
        {QStringLiteral("so"), QStringLiteral("Somali")},
        {QStringLiteral("su"), QStringLiteral("Sundanese")},
        {QStringLiteral("sw"), QStringLiteral("Swahili")},
        {QStringLiteral("sv"), QStringLiteral("Swedish")},
        {QStringLiteral("ta"), QStringLiteral("Tamil")},
        {QStringLiteral("te"), QStringLiteral("Telugu")},
        {QStringLiteral("th"), QStringLiteral("Thai")},
        {QStringLiteral("tl"), QStringLiteral("Tagalog")},
        {QStringLiteral("uk"), QStringLiteral("Ukrainian")},
        {QStringLiteral("ur"), QStringLiteral("Urdu")},
        {QStringLiteral("uz"), QStringLiteral("Uzbek")},
        {QStringLiteral("vi"), QStringLiteral("Vietnamese")},
        {QStringLiteral("cy"), QStringLiteral("Welsh")},
        {QStringLiteral("xh"), QStringLiteral("Xhosa")},
        {QStringLiteral("yi"), QStringLiteral("Yiddish")},
        {QStringLiteral("yo"), QStringLiteral("Yoruba")},
        {QStringLiteral("zu"), QStringLiteral("Zulu")},
    };

    return languages;
}

QString translationLanguageCodeFromInput(QString language)
{
    const auto normalized = normalizedLanguageCode(language);
    if (normalized.isEmpty())
    {
        return {};
    }

    const auto &languages = supportedTranslationLanguages();
    const auto codeIt =
        std::ranges::find_if(languages, [&](const TranslationLanguage &item) {
            return item.code == normalized;
        });
    if (codeIt != languages.end())
    {
        return codeIt->code;
    }

    const auto nameIt =
        std::ranges::find_if(languages, [&](const TranslationLanguage &item) {
            return item.name.compare(language.trimmed(), Qt::CaseInsensitive) ==
                   0;
        });
    if (nameIt != languages.end())
    {
        return nameIt->code;
    }

    static const QHash<QString, QString> aliases{
        {"zh", QStringLiteral("zh-cn")},
        {"zh-hans", QStringLiteral("zh-cn")},
        {"cn", QStringLiteral("zh-cn")},
        {"chinese", QStringLiteral("zh-cn")},
        {"simplified-chinese", QStringLiteral("zh-cn")},
        {"zh-hant", QStringLiteral("zh-tw")},
        {"tw", QStringLiteral("zh-tw")},
        {"traditional-chinese", QStringLiteral("zh-tw")},
        {"he", QStringLiteral("iw")},
        {"fil", QStringLiteral("tl")},
        {"filipino", QStringLiteral("tl")},
        {"burmese", QStringLiteral("my")},
        {"br", QStringLiteral("pt")},
        {"pt-br", QStringLiteral("pt")},
        {"jp", QStringLiteral("ja")},
        {"kr", QStringLiteral("ko")},
        {"ua", QStringLiteral("uk")},
    };

    return aliases.value(normalized);
}

bool isSupportedTranslationLanguage(const QString &language)
{
    return !translationLanguageCodeFromInput(language).isEmpty();
}

QString normalizedTranslationTargetLanguage(QString language)
{
    const auto code = translationLanguageCodeFromInput(std::move(language));
    if (code.isEmpty())
    {
        return QStringLiteral("en");
    }

    return code;
}

QString translationLanguageName(const QString &language)
{
    const auto normalized = translationLanguageCodeFromInput(language);
    if (normalized.isEmpty())
    {
        return {};
    }

    const auto &languages = supportedTranslationLanguages();
    const auto it =
        std::ranges::find_if(languages, [&](const TranslationLanguage &item) {
            return item.code == normalized;
        });
    if (it != languages.end())
    {
        return it->name;
    }

    return normalized.toUpper();
}

QString trimTextForTranslation(QString text)
{
    text = text.trimmed();
    if (text.size() > MAX_TRANSLATION_TEXT_LENGTH)
    {
        text = text.left(MAX_TRANSLATION_TEXT_LENGTH);
    }

    return text;
}

void requestTextTranslation(const QString &text, const QString &targetLanguage,
                            QObject *caller,
                            TranslationSuccessCallback onSuccess,
                            TranslationErrorCallback onError,
                            TranslationFinishedCallback onFinished)
{
    const auto requestText = trimTextForTranslation(text);
    if (requestText.isEmpty())
    {
        if (onError)
        {
            onError(QStringLiteral("There is no text to translate."));
        }
        if (onFinished)
        {
            onFinished();
        }
        return;
    }

    const auto target = normalizedTranslationTargetLanguage(targetLanguage);

    QUrl url(
        QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("tl"), target);
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("q"), requestText);
    url.setQuery(query);

    auto request =
        NetworkRequest(url)
            .timeout(8000)
            .onSuccess([onSuccess, onError](const NetworkResult &result) {
                const auto parsed = parseGoogleTranslationResult(result);
                if (!parsed.has_value())
                {
                    if (onError)
                    {
                        onError(QStringLiteral(
                            "Translation failed: invalid response."));
                    }
                    return;
                }

                if (onSuccess)
                {
                    onSuccess(*parsed);
                }
            })
            .onError([onError](const NetworkResult &result) {
                (void)result;
                if (onError)
                {
                    onError(
                        QStringLiteral("Translation failed: network error."));
                }
            })
            .finally([onFinished] {
                if (onFinished)
                {
                    onFinished();
                }
            });

    if (caller != nullptr)
    {
        std::move(request).caller(caller).execute();
    }
    else
    {
        std::move(request).execute();
    }
}

}  // namespace chatterino
