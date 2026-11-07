// Game-specific options on the General tab, and on Android the Touch Controls
// tab. See include/snp_config.h.
#include "snp_config.h"
#include "snp_android.h"
#include "snp_mouse.h"

#include "recompui/config.h"

namespace {
    const std::string rumble_pak_id = "rumble_pak";
    const std::string mouse_aim_id = "mouse_aim";
    const std::string mouse_sensitivity_id = "mouse_sensitivity_relative";
    const std::string second_player_id = "second_player";

    const std::vector<recomp::config::ConfigOptionEnumOption> mouse_aim_options = {
        {0, "Off", "Off"},
        {1, "Relative", "Relative"},
        {2, "Absolute", "Absolute"},
    };

    const std::vector<recomp::config::ConfigOptionEnumOption> second_player_options = {
        {0, "Off", "Off"},
        {1, "Game", "Game's 2-Player Mode"},
        {2, "Mods", "Co-op Mod"},
    };

    // Looked up by id on each read: the Config& create_general_tab() returns
    // lives in a container that moves when later tabs are created, so a
    // pointer kept from it dangles.
    bool options_added = false;
}

void snp::config::add_general_options(recomp::config::Config& general) {
    options_added = true;

    general.add_bool_option(
        rumble_pak_id,
        "Rumble Pak",
        "Plugs a Rumble Pak into the controller, so the game can vibrate it."
        "<br/><br/>"
        "Changing this is like swapping paks: the game notices the next time it checks.",
        true
    );

    general.add_enum_option(
        mouse_aim_id,
        "Mouse Aim",
        "How the mouse moves the reticle. The mouse buttons are bound below, on the "
        "keyboard's bindings (left click fires, right click jumps, middle click switches "
        "the aim mode by default)."
        "<br/><br/>"
        "<b>Absolute</b>: the reticle is where the mouse pointer is, like a light gun."
        "<br/>"
        "<b>Relative</b>: the mouse moves the reticle by Mouse Sensitivity, and the "
        "pointer stays in the window."
        "<br/><br/>"
        "A controller works as before either way.",
        mouse_aim_options,
        2
    );
    general.add_percent_number_option(
        mouse_sensitivity_id,
        "Mouse Sensitivity",
        "How far the reticle moves for the mouse with Mouse Aim on Relative. "
        "At 50% it moves with the pointer.",
        50.0
    );

    general.add_enum_option(
        second_player_id,
        "Second Player",
        "A second player on a second controller: the next controller that presses a button "
        "after player 1's joins as player 2, with the same controller bindings. Player 1 keeps "
        "the keyboard, the mouse and every other controller."
        "<br/><br/>"
        "<b>Game's 2-Player Mode</b>: the game's own, where player 1 moves and player 2 aims "
        "and shoots. The game sees a second controller plugged in while two are connected."
        "<br/>"
        "<b>Co-op Mod</b>: player 2 goes to a mod that adds a second player (like the co-op "
        "mod); the game itself stays one player."
        "<br/><br/>"
        "Player 2 leaves when this is turned off or their controller is disconnected.",
        second_player_options,
        0
    );

    // Set once the mouse buttons were added to saved keyboard bindings
    // (src/main/input_defaults.cpp).
    general.add_bool_option("mouse_buttons_bound", "", "", false, true);

    // Shown on the Controls tab, above the mappings, instead
    // (lib-patches/RecompFrontend/0002).
    recompui::config::show_option_on_controls_tab(recompui::config::general::id, mouse_aim_id);
    recompui::config::show_option_on_controls_tab(recompui::config::general::id, mouse_sensitivity_id);
    recompui::config::show_option_on_controls_tab(recompui::config::general::id, recompui::config::general::options::joystick_deadzone);
}

snp::config::MouseAim snp::config::get_mouse_aim() {
    if (!options_added) {
        return MouseAim::Off;
    }
    return (MouseAim)std::get<uint32_t>(recompui::config::get_general_config().get_option_value(mouse_aim_id));
}

float snp::config::get_mouse_sensitivity() {
    if (!options_added) {
        return 50.0f;
    }
    return (float)std::get<double>(recompui::config::get_general_config().get_option_value(mouse_sensitivity_id));
}

snp::config::SecondPlayer snp::config::get_second_player() {
    if (!options_added) {
        return SecondPlayer::Off;
    }
    return (SecondPlayer)std::get<uint32_t>(recompui::config::get_general_config().get_option_value(second_player_id));
}

bool snp::config::get_rumble_pak() {
    if (!options_added) {
        return false;
    }
    return std::get<bool>(recompui::config::get_general_config().get_option_value(rumble_pak_id));
}

#if defined(__ANDROID__)
// Touch aiming (src/main/mouse.cpp) and the on-screen buttons (src/android/touch_controls.cpp),
// which read these through the change callbacks (also run when the saved settings load).
void snp::config::add_touch_options(recomp::config::Config& touch) {
    static const std::string aim_id = "touch_aim";
    static const std::string speed_id = "touch_aim_speed";

    touch.add_enum_option(
        aim_id,
        "Touch Aim",
        "How a finger on the screen moves the reticle. It works together with a controller, and "
        "whether or not the on-screen buttons show."
        "<br/><br/>"
        "<b>Point</b>: the reticle goes where you touch, like a light gun."
        "<br/>"
        "<b>Drag</b>: dragging a finger anywhere moves the reticle, like a trackpad, by Touch Aim "
        "Speed."
        "<br/><br/>"
        "In lock-on mode a swipe locks onto the next target that way.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            {(uint32_t)snp::mouse::TouchAim::Off, "Off", "Off"},
            {(uint32_t)snp::mouse::TouchAim::Point, "Point", "Point"},
            {(uint32_t)snp::mouse::TouchAim::Drag, "Drag", "Drag"},
        },
        snp::mouse::TouchAim::Point
    );
    touch.add_number_option(
        speed_id,
        "Touch Aim Speed",
        "How far the reticle moves for a drag with Touch Aim on Drag. At 100% it moves with the "
        "finger.",
        25, 300, 5, 0, true, 100
    );
    touch.add_option_hidden_dependency(speed_id, aim_id, snp::mouse::TouchAim::Off, snp::mouse::TouchAim::Point);
    touch.add_option_change_callback(aim_id, [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
        snp::mouse::set_touch_aim((snp::mouse::TouchAim)std::get<uint32_t>(value));
    });
    touch.add_option_change_callback(speed_id, [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
        snp::mouse::set_touch_aim_speed((float)std::get<double>(value));
    });

    touch.add_bool_option(
        "touch_fire",
        "Fire While Touching",
        "Fires for as long as the aiming finger is on the screen: with Touch Aim on Point, touch "
        "where to shoot. Off, fire with the FIRE button or the controller.",
        true
    );
    touch.add_option_hidden_dependency("touch_fire", aim_id, snp::mouse::TouchAim::Off);
    touch.add_option_change_callback("touch_fire", [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
        snp::android::set_touch_fire_while_touching(std::get<bool>(value));
    });

    touch.add_enum_option(
        "touch_buttons",
        "On-screen Buttons",
        "The buttons for playing without a controller: a stick to move, FIRE, JUMP, AIM (the aim "
        "mode), START and MENU."
        "<br/><br/>"
        "<b>Auto</b>: shown unless a controller is connected (a handheld's own controls count)."
        "<br/>"
        "<b>Always</b>, <b>Never</b>: whether or not one is.",
        std::vector<recomp::config::ConfigOptionEnumOption>{
            {(uint32_t)snp::android::TouchButtons::Auto, "Auto", "Auto"},
            {(uint32_t)snp::android::TouchButtons::Always, "Always", "Always"},
            {(uint32_t)snp::android::TouchButtons::Never, "Never", "Never"},
        },
        snp::android::TouchButtons::Auto
    );
    touch.add_option_change_callback("touch_buttons", [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
        snp::android::set_touch_buttons((snp::android::TouchButtons)std::get<uint32_t>(value));
    });

    touch.add_number_option(
        "touch_size",
        "Button Size",
        "The size of the on-screen buttons.",
        90, 180, 5, 0, true, 100
    );
    touch.add_option_change_callback("touch_size", [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
        snp::android::set_touch_size(std::get<double>(value));
    });

    touch.add_number_option(
        "touch_opacity",
        "Button Opacity",
        "How solid the on-screen buttons are drawn.",
        30, 100, 5, 0, true, 30
    );
    touch.add_option_change_callback("touch_opacity", [](recomp::config::ConfigValueVariant value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
        snp::android::set_touch_opacity(std::get<double>(value));
    });
}
#endif
