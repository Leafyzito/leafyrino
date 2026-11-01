#include "controllers/highlights/HighlightMatch.hpp"

#include "singletons/Settings.hpp"

namespace chatterino {

QColor defaultAutoModMatchColor()
{
    return {255, 177, 64, 128};
}

HighlightMatchStyle autoModMatchStyle()
{
    const auto configured =
        getSettings()->automodMatchHighlightStyle.getValue();
    if (configured < static_cast<int>(HighlightMatchStyle::None) ||
        configured > static_cast<int>(HighlightMatchStyle::Underline))
    {
        return HighlightMatchStyle::Fill;
    }
    return static_cast<HighlightMatchStyle>(configured);
}

QColor autoModMatchColor()
{
    QColor configured(getSettings()->automodMatchHighlightColor.getValue());
    if (!configured.isValid())
    {
        return defaultAutoModMatchColor();
    }
    return configured;
}

}  // namespace chatterino
