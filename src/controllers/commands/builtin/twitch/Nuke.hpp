#pragma once

#include <QSet>
#include <QString>

#include <memory>

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
struct CommandContext;

namespace commands {

struct NukePreview {
    bool active = false;
    QSet<QString> messageIDs;
    QString statusText;
};

NukePreview buildNukePreview(const QString &input, const ChannelPtr &channel);

QString sendNuke(const CommandContext &ctx);
QString sendSpam(const CommandContext &ctx);
QString sendPyramid(const CommandContext &ctx);

}  // namespace commands

}  // namespace chatterino
