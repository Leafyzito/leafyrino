#pragma once

#include "controllers/completion/sources/Source.hpp"
#include "controllers/completion/strategies/Strategy.hpp"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace chatterino {
class Channel;
}

namespace chatterino::completion {

struct CommandItem {
    QString name{};
    QString prefix{};
    QString usage{};
};

class CommandSource : public Source
{
public:
    using ActionCallback = std::function<void(const QString &)>;
    using CommandStrategy = Strategy<CommandItem>;

    CommandSource(std::unique_ptr<CommandStrategy> strategy,
                  ActionCallback callback = nullptr,
                  const Channel *channel = nullptr);

    void update(const QString &query) override;
    void addToListModel(GenericListModel &model,
                        size_t maxCount = 0) const override;
    void addToStringList(QStringList &list, size_t maxCount = 0,
                         bool isFirstWord = false) const override;

    const std::vector<CommandItem> &output() const;

private:
    void initializeItems();

    std::unique_ptr<CommandStrategy> strategy_;
    ActionCallback callback_;
    const Channel *channel_ = nullptr;

    std::vector<CommandItem> items_{};
    std::vector<CommandItem> output_{};
};

}  // namespace chatterino::completion
