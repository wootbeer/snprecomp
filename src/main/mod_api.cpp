// Functions mods can call (include/snp_mod_api.h). A mod declares them with
// RECOMP_IMPORT("*", ...), as it does the runtime's own (recomp_alloc).
//
// snp_get_player_count: how many players have their own input. 1, or 2 with
// Second Player on Co-op Mod once a second controller has joined
// (src/main/second_player.cpp).
//
// snp_get_player_pad: a player's controller this moment, filled in like the
// game's own OSContPad { u16 button; s8 stick_x; s8 stick_y; u8 errno; },
// with the stick in the same N64 range the game gets. Returns 1, or 0 with an
// empty pad when that player has no input. Player 0 is player 1, without
// Android's touch controls. While one of the frontend's menus has the input
// the pad is empty, as it is for the game.
//
// With Second Player on Co-op Mod, the game sees port 1 only
// (src/main/main.cpp), so a mod is the only thing reading player 2.
//
// snp_set_live_reticle: Smooth Aiming for a mod's crosshair. Once a game
// frame, after moving it, the mod gives its position (where it draws it this
// frame), its movement per game frame and its reach. The display list walker
// (src/game/display_list.cpp) gives a 2D matrix drawn at one of a slot's last
// few positions that slot's live ID, and RT64 then moves it on every displayed
// frame: from the newest position, on at its movement for the part of a game
// frame gone by (src/main/mouse.cpp's live_offset). Slots 1-3; player 1's
// reticle is slot 0. A slot stops after 0.2 s without a call, and does
// nothing with Smooth Aiming off.
//
// snp_draw_overlay: a display list the mod wants drawn at the end of each
// frame in play, after the HUD (src/game/display_list.cpp). It's called with
// extended RDRAM on, so it and everything it points to can be in the mod's
// own memory (K0 addresses, 0x8xxxxxxx; not segments). It must set all the
// state it needs: it comes after whatever the frame left. Give the newest
// list once a game frame and keep it unchanged until the next call (build
// into two buffers in turn); NULL stops it. It also stops after 0.2 s
// without a call.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "librecomp/helpers.hpp"
#include "librecomp/overlays.hpp"
#include "recompinput/players.h"
#include "recompinput/profiles.h"

#include "snp_config.h"
#include "snp_mod_api.h"
#include "snp_mouse.h"
#include "snp_second_player.h"

// ultramodern/src/input.cpp, not declared in a header: the stick conversion
// osContGetReadData uses.
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

namespace {
    // A mod's crosshair (snp_set_live_reticle), under live_mutex.
    struct LiveReticle {
        bool active = false;
        std::array<float, 4> x{}, y{}; // its last positions, newest first
        float velocity_x = 0.0f, velocity_y = 0.0f; // per game frame
        float reach_x = 0.0f, reach_y = 0.0f;
        double time = 0.0; // of the newest position, in seconds
        double period = 1.0 / 30.0; // between the game's frames
    };
    constexpr uint32_t live_slots = 4; // 0 is player 1's (src/main/mouse.cpp)
    std::mutex live_mutex;
    std::array<LiveReticle, live_slots> live;

    double now_seconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    float read_float(uint8_t* rdram, gpr addr) {
        uint32_t bits = (uint32_t)MEM_W(0, addr);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    bool fresh(const LiveReticle& r, double now) {
        return r.active && now - r.time <= 0.2;
    }

    // The mod's overlay list (snp_draw_overlay), under live_mutex.
    uint32_t overlay_list = 0;
    double overlay_time = 0.0;

    uint32_t player_count() {
        return (snp::config::get_second_player() == snp::config::SecondPlayer::Mods && snp::second_player::joined()) ? 2 : 1;
    }
}

extern "C" void snp_get_player_count(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return(ctx, player_count());
}

extern "C" void snp_get_player_pad(uint8_t* rdram, recomp_context* ctx) {
    int32_t player = _arg<0, int32_t>(rdram, ctx);
    PTR(void) pad = _arg<1, PTR(void)>(rdram, ctx);

    uint16_t buttons = 0;
    int8_t stick_x = 0, stick_y = 0;
    bool has_input = player >= 0 && (uint32_t)player < player_count();
    if (has_input) {
        float x = 0.0f, y = 0.0f;
        recompinput::profiles::get_n64_input(player, &buttons, &x, &y);
        convert_to_n64_range(x, y, stick_x, stick_y);
    }

    if (pad != NULLPTR) {
        gpr addr = (gpr)pad;
        MEM_H(0, addr) = buttons;
        MEM_B(2, addr) = stick_x;
        MEM_B(3, addr) = stick_y;
        MEM_B(4, addr) = has_input ? 0 : 8; // CONT_NO_RESPONSE_ERROR, as osContGetReadData
    }
    _return(ctx, has_input ? 1 : 0);
}

// snp_set_live_reticle(s32 slot, const SnpLiveReticle* reticle), with
// SnpLiveReticle { f32 x, y; f32 velocity_x, velocity_y; f32 reach_x, reach_y; u32 active; }.
extern "C" void snp_set_live_reticle(uint8_t* rdram, recomp_context* ctx) {
    int32_t slot = _arg<0, int32_t>(rdram, ctx);
    PTR(void) reticle = _arg<1, PTR(void)>(rdram, ctx);
    if (slot < 1 || (uint32_t)slot >= live_slots || reticle == NULLPTR) {
        return;
    }
    gpr addr = (gpr)reticle;
    const float x = read_float(rdram, addr), y = read_float(rdram, addr + 4);
    const float vx = read_float(rdram, addr + 8), vy = read_float(rdram, addr + 12);
    const float reach_x = read_float(rdram, addr + 16), reach_y = read_float(rdram, addr + 20);
    const bool active = MEM_W(24, addr) != 0;
    const bool valid = std::isfinite(x) && std::isfinite(y) && std::isfinite(vx) && std::isfinite(vy) &&
        std::isfinite(reach_x) && std::isfinite(reach_y);

    const double now = now_seconds();
    std::lock_guard lock(live_mutex);
    LiveReticle& r = live[slot];
    const double gap = now - r.time;
    if (gap > 0.005 && gap < 0.2) {
        r.period += (gap - r.period) * 0.2;
    }
    r.active = active && valid;
    if (!r.active) {
        return;
    }
    for (size_t i = r.x.size() - 1; i > 0; i--) {
        r.x[i] = r.x[i - 1];
        r.y[i] = r.y[i - 1];
    }
    r.x[0] = x;
    r.y[0] = y;
    r.velocity_x = vx;
    r.velocity_y = vy;
    r.reach_x = std::fabs(reach_x);
    r.reach_y = std::fabs(reach_y);
    r.time = now;
}

uint32_t snp::mod_live_reticle_id(float x, float y) {
    if (!snp::config::get_option(snp::config::Option::SmoothAiming)) {
        return 0;
    }
    const double now = now_seconds();
    std::lock_guard lock(live_mutex);
    for (uint32_t slot = 1; slot < live_slots; slot++) {
        const LiveReticle& r = live[slot];
        if (!fresh(r, now)) {
            continue;
        }
        for (size_t i = 0; i < r.x.size(); i++) {
            if (std::fabs(x - r.x[i]) < 1.0f && std::fabs(y - r.y[i]) < 1.0f) {
                return snp::mouse::live_id(slot, x, y);
            }
        }
    }
    return 0;
}

bool snp::mod_live_offset(uint32_t slot, float game_x, float game_y, float* dx, float* dy) {
    if (slot < 1 || slot >= live_slots) {
        return false;
    }
    const double now = now_seconds();
    std::lock_guard lock(live_mutex);
    const LiveReticle& r = live[slot];
    if (!fresh(r, now)) {
        return false;
    }
    const float part = (float)std::clamp((now - r.time) / r.period, 0.0, 1.0);
    const float x = std::clamp(r.x[0] + r.velocity_x * part, -r.reach_x, r.reach_x);
    const float y = std::clamp(r.y[0] + r.velocity_y * part, -r.reach_y, r.reach_y);
    *dx = x - game_x;
    *dy = y - game_y;
    return true;
}

// snp_draw_overlay(const void* display_list)
extern "C" void snp_draw_overlay(uint8_t* rdram, recomp_context* ctx) {
    uint32_t list = (uint32_t)_arg<0, PTR(void)>(rdram, ctx);
    std::lock_guard lock(live_mutex);
    overlay_list = (list & 0x80000000u) ? list : 0;
    overlay_time = now_seconds();
}

uint32_t snp::mod_overlay_list() {
    std::lock_guard lock(live_mutex);
    if (overlay_list == 0 || now_seconds() - overlay_time > 0.2) {
        return 0;
    }
    return overlay_list;
}

void snp::register_mod_exports() {
    recomp::overlays::register_base_export("snp_get_player_count", snp_get_player_count);
    recomp::overlays::register_base_export("snp_get_player_pad", snp_get_player_pad);
    recomp::overlays::register_base_export("snp_set_live_reticle", snp_set_live_reticle);
    recomp::overlays::register_base_export("snp_draw_overlay", snp_draw_overlay);
    register_printf_export();
}
