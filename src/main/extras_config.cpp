// The Enhancements and Cheats tabs of the menu, and Screen Filter, Bloom,
// Texture Filtering, HUD Filtering and Texture Packs on the Graphics tab. The
// game reads the tabs through src/game/extras.cpp.
#include "snp_config.h"

#include <filesystem>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recompui/config.h"
#include "ultramodern/config.hpp"
#include "recompui/renderer.h"
#include "rt64_rice_packs.h"
#include "util/file.h"

namespace {
    const std::string enhancements_id = "enhancements";
    const std::string cheats_id = "cheats";

    bool tabs_added = false;
    bool graphics_options_added = false;
    bool smooth_aiming_added = false;

    // Smooth Aiming moves the reticle on RT64's frames between the game's, which there are only
    // with a framerate above the game's 30 (Display, or Manual above 30): otherwise it's greyed
    // out. temporary: the values being edited in the menu.
    void update_smooth_aiming_available(bool temporary) {
        if (!smooth_aiming_added || !graphics_options_added) {
            return;
        }
        recomp::config::Config& graphics = recompui::config::get_graphics_config();
        auto value = [&](const std::string& id) {
            return temporary ? graphics.get_temp_option_value(id) : graphics.get_option_value(id);
        };
        const auto rate = static_cast<ultramodern::renderer::RefreshRate>(std::get<uint32_t>(value(recompui::config::graphics::options::rr_option)));
        const double manual = std::get<double>(value(recompui::config::graphics::options::rr_manual_value));
        const bool above_30 = (rate == ultramodern::renderer::RefreshRate::Display) ||
            (rate == ultramodern::renderer::RefreshRate::Manual && manual > 30.0);
        recompui::config::get_config(enhancements_id).update_option_disabled("smooth_aiming", !above_30);
    }

    const std::string screen_filter_id = "screen_filter";
    const std::vector<recomp::config::ConfigOptionEnumOption> screen_filter_options = {
        {0, "None", "None"},
        {1, "CRT", "CRT"},
        {2, "Scanlines", "Scanlines"},
        {3, "LCD", "LCD"},
    };

    const std::string screen_filter_menus_id = "screen_filter_menus_only";
    const std::string bloom_id = "bloom";
    const std::vector<recomp::config::ConfigOptionEnumOption> bloom_options = {
        {0, "Off", "Off"},
        {1, "Low", "Low"},
        {2, "High", "High"},
    };

    const std::string texture_filter_id = "texture_filter";
    const std::vector<recomp::config::ConfigOptionEnumOption> texture_filter_options = {
        {0, "Smooth", "Smooth"},
        {1, "Sharp", "Sharp"},
    };
    const std::string hud_filter_id = "hud_filter";
    const std::string texture_packs_id = "texture_packs";

    std::filesystem::path texture_packs_path() {
        return recompui::file::get_app_folder_path() / "texture_packs";
    }

    // Opens a folder in the system's file manager.
    void open_folder(const std::filesystem::path& path) {
        std::string url = "file:///";
        for (char c : path.generic_u8string()) {
            if (c == ' ') {
                url += "%20";
            }
            else if (!(url.size() == 8 && c == '/')) { // no fourth slash before a Linux path
                url += c;
            }
        }
        SDL_OpenURL(url.c_str());
    }

    bool get_bool(const std::string& tab, const char* id) {
        if (!tabs_added) {
            return false;
        }
        return std::get<bool>(recompui::config::get_config(tab).get_option_value(id));
    }
}

// At the end of the Graphics tab.
void snp::config::add_graphics_options(recomp::config::Config& graphics) {
    graphics_options_added = true;
    graphics.add_enum_option(screen_filter_id, "Screen Filter", "", screen_filter_options, 0);
    graphics.add_bool_option(
        screen_filter_menus_id,
        "Menus Only",
        "The screen filter in the menus only, not in play or the intro scenes.",
        false
    );
    graphics.add_option_disable_dependency(screen_filter_menus_id, screen_filter_id, 0u);
    graphics.add_enum_option(bloom_id, "Bloom", "", bloom_options, 0);
    graphics.add_enum_option(
        texture_filter_id,
        "Texture Filtering",
        "The game's world: Smooth is the game's own filtering, Sharp draws its textures with sharp pixels.",
        texture_filter_options, 0
    );
    graphics.add_enum_option(
        hud_filter_id,
        "HUD Filtering",
        "The crosshair, HUD and menus: Smooth is the game's own filtering, Sharp draws them with sharp pixels.",
        texture_filter_options, 0
    );
    graphics.add_bool_option(
        texture_packs_id,
        "Texture Packs",
        "Loads the Rice Video texture packs (an emulator's hires_texture packs) in the texture_packs "
        "folder. RT64 packs (.rtz) are turned on and off in the Mods menu instead."
        "<br/><br/>"
        "Can only be changed before the game starts.",
        true
    );
    for (const std::string& rate_id : { recompui::config::graphics::options::rr_option, recompui::config::graphics::options::rr_manual_value }) {
        graphics.add_option_change_callback(rate_id,
            [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
                update_smooth_aiming_available(context == recomp::config::OptionChangeContext::Temporary);
            });
    }
    graphics.add_option_change_callback(texture_packs_id,
        [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            snp::config::apply_texture_packs();
        });
#if !defined(__ANDROID__)
    // On Android the folder is in the app's private storage, which no file manager can open.
    recompui::config::add_option_button(texture_packs_id, "Open Folder", []() {
        std::error_code dir_ec;
        std::filesystem::create_directories(texture_packs_path(), dir_ec);
        open_folder(texture_packs_path());
    });
#endif
}

void snp::config::apply_texture_packs() {
    std::error_code ec;
    std::filesystem::create_directories(texture_packs_path(), ec);
    bool enabled = !graphics_options_added
        || std::get<bool>(recompui::config::get_graphics_config().get_option_value(texture_packs_id));
    RT64::setRiceTextureDirectories(enabled ? std::vector<std::filesystem::path>{ texture_packs_path() } : std::vector<std::filesystem::path>{});
    recompui::renderer::trigger_texture_pack_update();
}

void snp::config::lock_game_start_options() {
    snp::config::lock_spanish_voices_option();
    if (graphics_options_added) {
        recompui::config::get_graphics_config().update_option_disabled(texture_packs_id, true);
    }
}

uint32_t snp::config::get_screen_filter() {
    if (!graphics_options_added) {
        return 0;
    }
    return std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(screen_filter_id));
}

bool snp::config::get_screen_filter_menus_only() {
    if (!graphics_options_added) {
        return false;
    }
    return std::get<bool>(recompui::config::get_graphics_config().get_option_value(screen_filter_menus_id));
}

bool snp::config::get_sharp_textures() {
    if (!graphics_options_added) {
        return false;
    }
    return std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(texture_filter_id)) == 1;
}

bool snp::config::get_sharp_hud() {
    if (!graphics_options_added) {
        return false;
    }
    return std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(hud_filter_id)) == 1;
}

uint32_t snp::config::get_bloom() {
    if (!graphics_options_added) {
        return 0;
    }
    return std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(bloom_id));
}

void snp::config::add_extras_tabs() {
    tabs_added = true;

    recomp::config::Config& enhancements = recompui::config::create_config_tab("Enhancements", enhancements_id, false);
    enhancements.add_bool_option(
        "unlock_options",
        "Unlock Everything",
        "Everything clearing the game on each difficulty unlocks: the main menu's OPT. menu with "
        "BGM and Alert Sound (Easy), Subtitles (Normal), Frame Skip and Game Speed (Hard), "
        "and every stage in Scene Select."
        "<br/><br/>"
        "Your save isn't changed: turning this off shows your own progress again.",
        false
    );
    enhancements.add_bool_option(
        "analog_aim",
        "Analog Aim",
        "Moves the reticle as far as the stick is pushed. The game takes the stick in steps, "
        "five speeds each way with a wide dead zone; with this on it takes every position, "
        "for finer aiming.",
        false
    );
    enhancements.add_number_option(
        "stick_sensitivity",
        "Stick Sensitivity",
        "How fast the stick moves the reticle. 100% is the game's own speed. "
        "Works best with Analog Aim on.",
        50.0, 200.0, 5.0, 0, true, 100.0
    );
    enhancements.add_bool_option(
        "smooth_aiming",
        "Smooth Aiming",
        "Moves the reticle smoothly at your display's framerate, like the rest of the picture, "
        "instead of 30 times a second. It follows your aim slightly behind, as everything else "
        "on screen does; turn it off for the most direct aim."
        "<br/><br/>"
        "Needs a Framerate above Original on the Graphics tab.",
        false // off by default: the slight lag is felt aiming with a mouse
    );
    // On the Controls tab instead (lib-patches/RecompFrontend/0002).
    recompui::config::show_option_on_controls_tab(enhancements_id, "smooth_aiming");
    smooth_aiming_added = true;
    update_smooth_aiming_available(false);
    recompui::config::show_option_on_controls_tab(enhancements_id, "analog_aim");
    recompui::config::show_option_on_controls_tab(enhancements_id, "stick_sensitivity");

    recomp::config::Config& cheats = recompui::config::create_config_tab("Cheats", cheats_id, false);
    cheats.add_bool_option("infinite_health", "Infinite Health", "", false);
    cheats.add_bool_option("infinite_time", "Infinite Time", "", false);
    cheats.add_bool_option("infinite_continues", "Infinite Continues", "", false);
}

float snp::config::get_stick_sensitivity() {
    if (!tabs_added) {
        return 100.0f;
    }
    return (float)std::get<double>(recompui::config::get_config(enhancements_id).get_option_value("stick_sensitivity"));
}

bool snp::config::get_option(Option option) {
    switch (option) {
        case Option::UnlockOptions: return get_bool(enhancements_id, "unlock_options");
        case Option::AnalogAim: return get_bool(enhancements_id, "analog_aim");
        case Option::SmoothAiming: return get_bool(enhancements_id, "smooth_aiming");
        case Option::InfiniteHealth: return get_bool(cheats_id, "infinite_health");
        case Option::InfiniteTime: return get_bool(cheats_id, "infinite_time");
        case Option::InfiniteContinues: return get_bool(cheats_id, "infinite_continues");
    }
    return false;
}
