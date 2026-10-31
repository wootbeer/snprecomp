// Mouse aiming (include/snp_mouse.h), and the stick's Analog Aim and
// Stick Sensitivity.
//
// The reticle is two floats in the struct func_800AB924 moves it with ($s2 +
// 0x28 and + 0x2C): its offset from the screen centre in 320 x 240 screen
// units, y up. Each frame the game adds a velocity from the stick, then clamps
// it (src/game/widescreen.cpp widens the clamp in Expand). Just before the
// velocity is added, the mouse moves it (snp.jp.toml), only on frames the mouse
// moved, so a controller aims as before in between.
//
// Smooth Aiming: the renderer moves the reticle (and its crosshair lines) on
// every displayed frame to where the latest input puts it (live_offset), from
// where the game drew it on its last frame. Pointers (the mouse in Absolute,
// light guns and Wiimotes acting as one, touch) give the position itself; the
// mouse in Relative adds the motion the game hasn't taken yet, and the stick
// carries on at the game's velocity for the part of a frame gone by.
//
// Mouse buttons are bound like keys on the Controls tab
// (lib-patches/RecompFrontend/0008, defaults in src/main/input_defaults.cpp).
//
// On Android a finger aims the same way (Touch Aim, src/android/touch_controls.cpp):
// Point puts the reticle under it like Absolute, Drag moves it like Relative.
#include "snp_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recompinput/input_events.h"
#include "recompinput/input_state.h"
#include "rt64_live_offset.h"
#include "snp_config.h"
#include "snp_mod_api.h"

extern SDL_Window* window;

namespace {
    std::mutex motion_mutex;
    float relative_x = 0.0f; // pixels moved since the game last took them
    float relative_y = 0.0f;
    float pointer_x = 0.0f;  // the pointer in the window, in pixels
    float pointer_y = 0.0f;
    bool pointer_moved = false;
    int window_w = 1280;
    int window_h = 720;

    // The aiming finger, in the touch view's pixels (guarded by motion_mutex).
    struct {
        float relative_x = 0.0f;
        float relative_y = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float last_x = 0.0f;
        float last_y = 0.0f;
        bool moved = false;
        int view_w = 1280;
        int view_h = 720;
    } touch;
    // The Touch tab's defaults until its settings load.
    std::atomic<snp::mouse::TouchAim> touch_aim{ snp::mouse::TouchAim::Point };
    std::atomic<float> touch_speed{ 100.0f };

    // The reticle as of the game's last frame (under motion_mutex).
    struct LiveReticle {
        bool active = false; // in play, steered by the player
        uint32_t reticle = 0; // its struct
        float velocity_x = 0.0f, velocity_y = 0.0f; // the stick's, per game frame
        double time = 0.0; // when the game moved it, in seconds
        double period = 1.0 / 30.0; // between the game's frames
    } live;

    bool live_offset(uint32_t id, float* dx, float* dy);

    double now_seconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // Smooth Aiming log, for finding why the reticle isn't smooth: SNP_AIM_LOG=1, or a file
    // snp-aim-log in the game's folder (on Android the app's storage), then start the game.
    // Writes snp_aim.log there: each game frame (whether Smooth Aiming steers the reticle, and
    // where it is), each display list (whether the reticle's matrix got its live ID) and each
    // displayed frame (the offset RT64 got). 20 MB at most.
    std::mutex aim_log_mutex;
    FILE* aim_log_file() {
        static FILE* file = [] {
            const char* v = std::getenv("SNP_AIM_LOG");
            bool on = v != nullptr && v[0] == '1';
            if (FILE* flag = std::fopen("snp-aim-log", "r")) {
                std::fclose(flag);
                on = true;
            }
            FILE* f = on ? std::fopen("snp_aim.log", "w") : nullptr;
            if (on) {
                std::fprintf(stderr, "Smooth Aiming log: %s\n", f != nullptr ? "on, snp_aim.log" : "couldn't create snp_aim.log");
            }
            return f;
        }();
        return file;
    }

    int SDLCALL watch_events(void*, SDL_Event* event) {
        switch (event->type) {
            case SDL_MOUSEMOTION: {
                if (recompinput::game_input_disabled()) {
                    break;
                }
#if defined(__ANDROID__)
                // SDL's mouse made from touches; the touch view aims with fingers itself.
                if (event->motion.which == SDL_TOUCH_MOUSEID) {
                    break;
                }
#endif
                int w = 0, h = 0;
                if (window != nullptr) {
                    SDL_GetWindowSize(window, &w, &h);
                }
                std::lock_guard lock(motion_mutex);
                relative_x += (float)event->motion.xrel;
                relative_y += (float)event->motion.yrel;
                pointer_x = (float)event->motion.x;
                pointer_y = (float)event->motion.y;
                pointer_moved = true;
                if (w > 0 && h > 0) {
                    window_w = w;
                    window_h = h;
                }
                break;
            }
            default:
                break;
        }
        return 0;
    }

    recompinput::GameMouse game_mouse() {
        switch (snp::config::get_mouse_aim()) {
            case snp::config::MouseAim::Relative: return recompinput::GameMouse::Captured;
            case snp::config::MouseAim::Absolute: return recompinput::GameMouse::Hidden;
            default: return recompinput::GameMouse::Default;
        }
    }

    float read_float(uint8_t* rdram, uint32_t addr) {
        float f;
        std::memcpy(&f, rdram + (addr & 0x1FFFFFFF), sizeof(f));
        return f;
    }

    void write_float(uint8_t* rdram, uint32_t addr, float f) {
        std::memcpy(rdram + (addr & 0x1FFFFFFF), &f, sizeof(f));
    }

    uint8_t read_byte(uint8_t* rdram, uint32_t addr) {
        return rdram[(addr ^ 3) & 0x1FFFFFFF]; // RDRAM is stored in 32-bit words
    }

    // Lock-on mode (the aim mode's flag 4 at *0x800DA978): func_800AB924 calls
    // func_800AB31C, which moves the reticle with the stick until the stick
    // is pushed hard (a magnitude of 48 of 64), then locks onto the nearest
    // target that way and steers the reticle onto it (the reticle's byte 2
    // set) until it's lost. The reticle's flag 0x20 overrides it.
    bool lock_on_mode(uint8_t* rdram, uint32_t reticle) {
        uint32_t aim_mode = *reinterpret_cast<uint32_t*>(rdram + 0xDA978);
        return !(read_byte(rdram, reticle) & 0x20) && (read_byte(rdram, aim_mode) & 4);
    }

    // The attract mode's How to Play plays back recorded input (func_800A8638
    // reads it while this byte is set): the reticle is left to the recording,
    // or the demo goes off its script.
    bool demo_playing(uint8_t* rdram) {
        return read_byte(rdram, 0x800D4F00) != 0;
    }

    bool locked_on(uint8_t* rdram, uint32_t reticle) {
        return lock_on_mode(rdram, reticle) && read_byte(rdram, reticle + 2) != 0;
    }

    // Window pixels per screen unit: the picture is 240 units high, or 320
    // wide in a window narrower than 4:3 (Expand only widens it).
    float screen_unit(int w, int h) {
        return (w * 3 >= h * 4) ? h / 240.0f : w / 320.0f;
    }

    // Stick Sensitivity: the reticle moves by its velocity times the
    // sensitivity (the game adds the velocity next; this adds the rest), so
    // the game's own speeds and stick handling stay as they are. Not while
    // lock-on steers it (func_800AB924 calls func_800AB31C instead of the
    // stick's func_800AB1C4: the reticle's flag 0x20 clear and the aim mode's
    // flag 4 set).
    void apply_stick_sensitivity(uint8_t* rdram, uint32_t reticle) {
        float sensitivity = snp::config::get_stick_sensitivity() / 100.0f;
        if (sensitivity == 1.0f) {
            return;
        }
        if (lock_on_mode(rdram, reticle)) {
            return;
        }
        for (uint32_t axis = 0; axis < 2; axis++) {
            float velocity = read_float(rdram, reticle + 0x58 + axis * 4);
            float position = read_float(rdram, reticle + 0x28 + axis * 4) + velocity * (sensitivity - 1.0f);
            if (std::isfinite(position)) {
                write_float(rdram, reticle + 0x28 + axis * 4, position);
            }
        }
    }
}

void snp::mouse::set_touch_aim(TouchAim aim) {
    touch_aim.store(aim);
}

void snp::mouse::set_touch_aim_speed(float speed_percent) {
    touch_speed.store(speed_percent);
}

snp::mouse::TouchAim snp::mouse::get_touch_aim() {
    return touch_aim.load();
}

void snp::mouse::touch_aim_down(float x, float y, int view_w, int view_h) {
    std::lock_guard lock(motion_mutex);
    touch.x = touch.last_x = x;
    touch.y = touch.last_y = y;
    touch.view_w = view_w;
    touch.view_h = view_h;
    // Point puts the reticle under the finger at once; Drag waits for it to move.
    if (touch_aim.load() == TouchAim::Point) {
        touch.moved = true;
    }
}

void snp::mouse::touch_aim_move(float x, float y) {
    std::lock_guard lock(motion_mutex);
    touch.relative_x += x - touch.last_x;
    touch.relative_y += y - touch.last_y;
    touch.x = touch.last_x = x;
    touch.y = touch.last_y = y;
    touch.moved = true;
}

void snp::mouse::init() {
    // The Smooth Aiming log's file appears now, not at its first line in play.
    snp::mouse::aim_log("Smooth Aiming log on (game, list and shown lines start in a level)");
    SDL_AddEventWatch(watch_events, nullptr);
    recompinput::set_game_mouse_callback(game_mouse);
    RT64::setLiveOffsetCallback(live_offset);
}

static void move_reticle(uint8_t* rdram, uint32_t reticle);

namespace {
    // Smooth Aiming: what the reticle did this game frame, once the game's
    // and the mouse's changes are in (the velocity is added after the hook).
    void note_reticle(uint8_t* rdram, uint32_t reticle) {
        const bool option = snp::config::get_option(snp::config::Option::SmoothAiming);
        const bool demo = demo_playing(rdram);
        const bool lock_on = lock_on_mode(rdram, reticle);
        const bool steered = option && !demo && !lock_on;
        const double now = now_seconds();
        snp::mouse::aim_log("game  steered %d (option %d, demo %d, lock-on %d) at %.2f %.2f, velocity %.2f %.2f",
            steered, option, demo, lock_on, read_float(rdram, reticle + 0x28), read_float(rdram, reticle + 0x2C),
            read_float(rdram, reticle + 0x58), read_float(rdram, reticle + 0x5C));
        std::lock_guard lock(motion_mutex);
        const double gap = now - live.time;
        if (gap > 0.005 && gap < 0.2) {
            live.period += (gap - live.period) * 0.2;
        }
        live.active = steered;
        live.reticle = reticle;
        live.velocity_x = read_float(rdram, reticle + 0x58);
        live.velocity_y = read_float(rdram, reticle + 0x5C);
        if (!std::isfinite(live.velocity_x) || !std::isfinite(live.velocity_y)) {
            live.velocity_x = live.velocity_y = 0.0f;
        }
        live.time = now;
    }

    // The reticle's reach (func_800AB924 clamps it to this after moving it).
    float clamp_x(float x) {
        const float limit = 160.0f * snp::get_widescreen_scale() - 16.0f;
        return std::clamp(x, -limit, limit);
    }

    float clamp_y(float y) {
        return std::clamp(y, -104.0f, 104.0f);
    }

    // A coordinate in quarter units, signed, in the given number of bits.
    uint32_t encode_quarter(float v, int bits) {
        const int32_t half = 1 << (bits - 1);
        return (uint32_t)std::clamp((int32_t)std::lround(v * 4.0f), -half, half - 1) & ((1u << bits) - 1);
    }

    float decode_quarter(uint32_t v, int bits) {
        const int32_t half = 1 << (bits - 1);
        int32_t q = (int32_t)(v & ((1u << bits) - 1));
        return (q >= half ? q - 2 * half : q) / 4.0f;
    }

    // RT64's live offset callback (any thread): where the latest input puts
    // the reticle, from where the game drew it (the ID's position).
    bool compute_live_offset(uint32_t id, float* dx, float* dy, const char*& why) {
        uint32_t slot;
        float game_x, game_y;
        snp::mouse::decode_live_id(id, slot, game_x, game_y);
        if (std::fabs(game_x) > 160.0f * snp::get_widescreen_scale() || std::fabs(game_y) > 105.0f) {
            why = "outside the reticle's reach";
            return false; // not where the reticle can be
        }
        if (slot != 0) {
            why = "a mod's crosshair";
            return snp::mod_live_offset(slot, game_x, game_y, dx, dy); // a mod's (src/main/mod_api.cpp)
        }
        const snp::config::MouseAim aim = snp::config::get_mouse_aim();
        const float sensitivity = snp::config::get_mouse_sensitivity();
        const float stick = snp::config::get_stick_sensitivity() / 100.0f;
        const double now = now_seconds();
        float x = game_x, y = game_y;
        {
            std::lock_guard lock(motion_mutex);
            if (!live.active || now - live.time > 0.2) {
                why = !live.active ? "not steered" : "no game frame for 0.2 s";
                return false;
            }
            const float unit = screen_unit(window_w, window_h);
            const snp::mouse::TouchAim finger_aim = touch_aim.load();
            const float touch_unit = screen_unit(touch.view_w, touch.view_h);
            // A finger first, as in move_reticle: Point like the mouse in Absolute, Drag like
            // Relative.
            if (finger_aim == snp::mouse::TouchAim::Point && touch.moved) {
                why = "touch point";
                x = (touch.x - touch.view_w * 0.5f) / touch_unit;
                y = (touch.view_h * 0.5f - touch.y) / touch_unit;
            }
            else if (finger_aim == snp::mouse::TouchAim::Drag && (touch.relative_x != 0.0f || touch.relative_y != 0.0f)) {
                why = "touch drag";
                const float scale = touch_speed.load() / 100.0f / touch_unit;
                x = game_x + touch.relative_x * scale;
                y = game_y - touch.relative_y * scale;
            }
            else if (aim == snp::config::MouseAim::Absolute && pointer_moved) {
                why = "mouse absolute";
                x = (pointer_x - window_w * 0.5f) / unit;
                y = (window_h * 0.5f - pointer_y) / unit;
            }
            else if (aim == snp::config::MouseAim::Relative && (relative_x != 0.0f || relative_y != 0.0f)) {
                why = "mouse relative";
                const float scale = sensitivity / 50.0f / unit;
                x = game_x + relative_x * scale;
                y = game_y - relative_y * scale;
            }
            else {
                why = "stick";
                const float part = (float)std::clamp((now - live.time) / live.period, 0.0, 1.0);
                x = game_x + live.velocity_x * stick * part;
                y = game_y + live.velocity_y * stick * part;
            }
        }
        x = clamp_x(x);
        y = clamp_y(y);
        if (!std::isfinite(x) || !std::isfinite(y)) {
            why = "not a number";
            return false;
        }
        *dx = x - game_x;
        *dy = y - game_y;
        return true;
    }

    bool live_offset(uint32_t id, float* dx, float* dy) {
        const char* why = "";
        const bool moved = compute_live_offset(id, dx, dy, why);
        uint32_t slot;
        float game_x, game_y;
        snp::mouse::decode_live_id(id, slot, game_x, game_y);
        if (moved) {
            snp::mouse::aim_log("shown drawn at %.2f %.2f (slot %u), moved %.2f %.2f (%s)", game_x, game_y, slot, *dx, *dy, why);
        }
        else {
            snp::mouse::aim_log("shown drawn at %.2f %.2f (slot %u), not moved (%s)", game_x, game_y, slot, why);
        }
        return moved;
    }
}

void snp::mouse::aim_log(const char* format, ...) {
    FILE* file = aim_log_file();
    if (file == nullptr) {
        return;
    }
    static const double start = now_seconds();
    std::lock_guard lock(aim_log_mutex);
    if (std::ftell(file) > 20L * 1024 * 1024) {
        return;
    }
    std::fprintf(file, "%9.4f ", now_seconds() - start);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

// Smooth Aiming, for the display list (src/game/display_list.cpp): the live ID
// for the reticle this frame (its position in it), or 0 outside play.
uint32_t snp::mouse::live_reticle_id(uint8_t* rdram, float& x, float& y) {
    uint32_t reticle;
    {
        std::lock_guard lock(motion_mutex);
        if (!live.active || now_seconds() - live.time > 0.2) {
            return 0;
        }
        reticle = live.reticle;
    }
    x = read_float(rdram, reticle + 0x28);
    y = read_float(rdram, reticle + 0x2C);
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return 0;
    }
    return snp::mouse::live_id(0, x, y);
}

uint32_t snp::mouse::live_id(uint32_t slot, float x, float y) {
    return RT64::LiveOffsetIdTag | (encode_quarter(x, 12) << 12) | ((slot & 3) << 10) | encode_quarter(y, 10);
}

void snp::mouse::decode_live_id(uint32_t id, uint32_t& slot, float& x, float& y) {
    x = decode_quarter(id >> 12, 12);
    slot = (id >> 10) & 3;
    y = decode_quarter(id, 10);
}

// func_800AB924, before the reticle's velocity is added: the reticle struct.
extern "C" void snp_mouse_reticle(uint8_t* rdram, uint32_t reticle) {
    move_reticle(rdram, reticle);
    note_reticle(rdram, reticle);
}

static void move_reticle(uint8_t* rdram, uint32_t reticle) {
    snp::config::MouseAim aim = snp::config::get_mouse_aim();
    snp::mouse::TouchAim finger_aim = touch_aim.load();

    float dx, dy, px, py, tdx, tdy, tx, ty;
    int w, h, tw, th;
    bool moved, touched;
    {
        std::lock_guard lock(motion_mutex);
        dx = relative_x;
        dy = relative_y;
        relative_x = relative_y = 0.0f;
        px = pointer_x;
        py = pointer_y;
        moved = pointer_moved;
        pointer_moved = false;
        w = window_w;
        h = window_h;
        tdx = touch.relative_x;
        tdy = touch.relative_y;
        touch.relative_x = touch.relative_y = 0.0f;
        tx = touch.x;
        ty = touch.y;
        touched = touch.moved;
        touch.moved = false;
        tw = touch.view_w;
        th = touch.view_h;
    }
    if (demo_playing(rdram)) {
        return;
    }
    // A finger wins over the mouse in a frame both moved in.
    bool use_touch = finger_aim != snp::mouse::TouchAim::Off && touched;
    bool use_mouse = !use_touch && aim != snp::config::MouseAim::Off && moved;
    if (!use_touch && !use_mouse) {
        apply_stick_sensitivity(rdram, reticle);
        return;
    }

    // Locked on, the game steers the reticle; the mouse picks targets
    // (snp_aim_stick).
    if (locked_on(rdram, reticle)) {
        return;
    }
    float x, y;
    if (use_touch) {
        float unit = screen_unit(tw, th);
        if (finger_aim == snp::mouse::TouchAim::Drag) {
            // At 100% the reticle moves with the finger.
            float scale = touch_speed.load() / 100.0f / unit;
            x = read_float(rdram, reticle + 0x28) + tdx * scale;
            y = read_float(rdram, reticle + 0x2C) - tdy * scale;
        }
        else {
            x = (tx - tw * 0.5f) / unit;
            y = (th * 0.5f - ty) / unit;
        }
    }
    else {
        float unit = screen_unit(w, h);
        if (aim == snp::config::MouseAim::Relative) {
            float scale = snp::config::get_mouse_sensitivity() / 50.0f / unit;
            x = read_float(rdram, reticle + 0x28) + dx * scale;
            y = read_float(rdram, reticle + 0x2C) - dy * scale;
        }
        else {
            x = (px - w * 0.5f) / unit;
            y = (h * 0.5f - py) / unit;
        }
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    write_float(rdram, reticle + 0x28, x);
    write_float(rdram, reticle + 0x2C, y);
    // The game clamps the position next; the stick's velocity is let go.
    write_float(rdram, reticle + 0x58, 0.0f);
    write_float(rdram, reticle + 0x5C, 0.0f);
}

// func_800AB924, with the stick's offsets as read and as the game stepped them
// (x, y): with Analog Aim on, the offsets as read, within the same -64 .. 64.
// In lock-on mode with the stick at rest, a mouse flick (a fast move since
// the last frame) is a full push of the stick its way, so the game locks onto
// the next target in that direction, as it does for a flick of the stick.
extern "C" void snp_aim_stick(uint8_t* rdram, uint32_t reticle, int32_t raw_x, int32_t raw_y, int32_t* x, int32_t* y) {
    if (demo_playing(rdram)) {
        return;
    }
    if (snp::config::get_option(snp::config::Option::AnalogAim)) {
        *x = std::clamp(raw_x, -64, 64);
        *y = std::clamp(raw_y, -64, 64);
    }
    bool mouse_aims = snp::config::get_mouse_aim() != snp::config::MouseAim::Off;
    bool finger_aims = touch_aim.load() != snp::mouse::TouchAim::Off;
    if ((!mouse_aims && !finger_aims) || !lock_on_mode(rdram, reticle) || *x != 0 || *y != 0) {
        return;
    }
    // In screen units; a finger's swipe flicks the same way.
    float dx = 0.0f, dy = 0.0f;
    {
        std::lock_guard lock(motion_mutex);
        if (mouse_aims) {
            float unit = screen_unit(window_w, window_h);
            dx += relative_x / unit;
            dy += relative_y / unit;
        }
        if (finger_aims) {
            float unit = screen_unit(touch.view_w, touch.view_h);
            dx += touch.relative_x / unit;
            dy += touch.relative_y / unit;
        }
    }
    constexpr float flick = 8.0f; // screen units in a frame
    float distance = std::sqrt(dx * dx + dy * dy);
    if (distance < flick) {
        return;
    }
    *x = (int32_t)std::lround(dx / distance * 64.0f);
    *y = (int32_t)std::lround(-dy / distance * 64.0f);
}
