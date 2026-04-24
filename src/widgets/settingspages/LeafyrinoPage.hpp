#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

namespace chatterino {

class GeneralPageView;

class LeafyrinoPage : public SettingsPage
{
    Q_OBJECT

public:
    LeafyrinoPage();

    bool filterElements(const QString &query) override;

private:
    void initLayout(GeneralPageView &layout);

    GeneralPageView *view_{};
};

}  // namespace chatterino
