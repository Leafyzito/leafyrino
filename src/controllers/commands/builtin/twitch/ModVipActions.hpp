#pragma once

#include <QString>

namespace chatterino {

struct CommandContext;

namespace commands {

enum class ModVipAction {
    AddModerator,
    RemoveModerator,
    AddVIP,
    RemoveVIP,
    AddLeadModerator,
    RemoveLeadModerator,
    AddEditor,
    RemoveEditor,
};

bool tryRunModVipActionWithLeadModGql(const CommandContext &ctx,
                                      const QString &target,
                                      ModVipAction action);

QString addLeadModerator(const CommandContext &ctx);

QString removeLeadModerator(const CommandContext &ctx);

QString addEditor(const CommandContext &ctx);

QString removeEditor(const CommandContext &ctx);

}  // namespace commands
}  // namespace chatterino
