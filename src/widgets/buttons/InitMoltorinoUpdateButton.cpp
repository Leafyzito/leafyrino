#include "widgets/buttons/InitMoltorinoUpdateButton.hpp"

// #include "providers/moltorino/MoltorinoPresence.hpp"
#include "widgets/buttons/PixmapButton.hpp"

namespace chatterino {

void initMoltorinoUpdateButton(
    PixmapButton &button, const std::function<void()> & /*relayout*/,
    pajlada::Signals::SignalHolder & /*signalHolder*/)
{
    button.hide();

    // QObject::connect(&button, &Button::leftClicked, [] {
    //     getMoltorinoPresence()->installAvailableUpdate();
    // });
    //
    // auto updateButton = [&button, relayout] {
    //     auto *presence = getMoltorinoPresence();
    //     button.setVisible(presence->shouldShowUpdateButton());
    //     button.setPixmap(QPixmap(presence->isUpdateError()
    //                                  ? ":/buttons/updateError.png"
    //                                  : ":/buttons/update.png"));
    //     button.setDim(presence->isUpdateBusy() ? DimButton::Dim::Lots
    //                                            : DimButton::Dim::Some);
    //     relayout();
    // };
    //
    // updateButton();
    // signalHolder.managedConnect(getMoltorinoPresence()->updateStateChanged,
    //                             [updateButton] {
    //                                 updateButton();
    //                             });
}

}  // namespace chatterino
