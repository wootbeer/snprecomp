#ifndef __SNP_CONFIG_H__
#define __SNP_CONFIG_H__

#include "librecomp/config.hpp"

// The game's own options: General and Controls tabs (src/main/game_config.cpp),
// Graphics, Enhancements and Cheats tabs (src/main/extras_config.cpp).
namespace snp::config {
    // Adds this game's options to the General tab. Call between
    // recompui::config::create_general_tab() and recompui::config::finalize().
    void add_general_options(recomp::config::Config& general);

    bool get_rumble_pak();
    // Local Multiplayer (Mods): player 2 gets their own controller, read by
    // mods through src/main/mod_api.cpp; the game still sees port 1 only.
    bool get_local_multiplayer();

#if defined(__ANDROID__)
    // Fills the Touch tab (Android): touch aiming and the on-screen buttons
    // (src/android/touch_controls.cpp). Call on the tab's Config before
    // recompui::config::finalize().
    void add_touch_options(recomp::config::Config& touch);
#endif

    // Mouse aiming (on the Controls tab; src/main/mouse.cpp).
    enum class MouseAim : uint32_t {
        Off = 0,
        Relative = 1, // the mouse moves the reticle, the cursor held in the window
        Absolute = 2, // the reticle sits where the pointer is
    };
    MouseAim get_mouse_aim();
    float get_mouse_sensitivity(); // percent, for Relative
    float get_stick_sensitivity(); // percent, 100 the game's own speed

    // Screen Filter and Bloom at the end of the Graphics tab (RT64's
    // ScreenFilter and Bloom values; src/main/extras_config.cpp).
    void add_graphics_options(recomp::config::Config& graphics);
    uint32_t get_screen_filter();
    bool get_screen_filter_menus_only(); // Screen Filter: Menus Only
    uint32_t get_bloom();
    bool get_sharp_textures(); // Texture Filtering: Sharp
    bool get_sharp_hud(); // HUD Filtering: Sharp
    // Texture Packs (Graphics tab): loads the texture_packs folder's Rice
    // packs, or none. Call once at startup; the option calls it on changes.
    void apply_texture_packs();
    // Greys out options that only apply before the game starts.
    void lock_game_start_options();

    // The Enhancements and Cheats tabs (src/main/extras_config.cpp). Call
    // before recompui::config::finalize().
    void add_extras_tabs();
    // Music Volume and Sound Effects Volume on the Sound tab (src/main/sound_volumes.cpp).
    void add_sound_volume_options(recomp::config::Config& sound);
    // Spanish Voices on the Sound tab (src/main/spanish_voices.cpp).
    void add_spanish_voices_option(recomp::config::Config& sound);
    void lock_spanish_voices_option();

    enum class Option {
        UnlockOptions,
        AnalogAim,
        SmoothAiming,
        InfiniteHealth,
        InfiniteTime,
        InfiniteContinues,
        MaxScore,
        Invincible,
        InfiniteAirJumps,
    };
    bool get_option(Option option);
}

// Widescreen (src/game/widescreen.cpp): how much wider than 4:3 RT64 is
// drawing full-width 3D views -- the window aspect / (4/3) in Expand mode,
// 1 otherwise. Updated by the frontend every frame.
namespace snp {
    void set_widescreen_scale(float scale);
    float get_widescreen_scale();
}

#endif
