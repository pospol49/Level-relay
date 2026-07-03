#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>

using namespace geode::prelude;

namespace relay {
    void loadSaved();
    void reqswitch();
}

$on_mod(Loaded) {
    relay::loadSaved();
}

$on_game(Loaded) {
    listenForKeybindSettingPresses(
        "switchKeybind",
        [](Keybind const&, bool down, bool repeat, double) {
            if (down && !repeat && Mod::get()->getSettingValue<bool>("enableSwitchKeybind")) {
                relay::reqswitch();
            }
        }
    );
}
