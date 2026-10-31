#ifndef __SNP_MOUSE_H__
#define __SNP_MOUSE_H__

#include <cstdint>

// Mouse aiming (src/main/mouse.cpp).
namespace snp::mouse {
    // Starts tracking the mouse. Call once SDL is up.
    void init();

    // Touch aiming (Android, from src/android/touch_controls.cpp): Point puts the
    // reticle under the finger, Drag moves it by the finger's movement, at
    // speed_percent (100: with the finger). Positions in the pixels of the touch
    // view, view_w x view_h, which covers the game's window.
    enum class TouchAim : uint32_t {
        Off = 0,
        Point = 1,
        Drag = 2,
    };
    void set_touch_aim(TouchAim aim);
    void set_touch_aim_speed(float speed_percent);
    TouchAim get_touch_aim();
    void touch_aim_down(float x, float y, int view_w, int view_h);
    void touch_aim_move(float x, float y);

    // Smooth Aiming: the RT64 live offset ID for the reticle this frame, with
    // its position (x, y: offset from the screen centre, y up), or 0.
    uint32_t live_reticle_id(uint8_t* rdram, float& x, float& y);

    // A crosshair's live ID: RT64's tag, then where it was drawn (x in 12
    // bits, y in 10, in quarter units) and its slot (2 bits): 0 is player 1's
    // reticle, 1-3 mods' crosshairs (src/main/mod_api.cpp).
    uint32_t live_id(uint32_t slot, float x, float y);
    void decode_live_id(uint32_t id, uint32_t& slot, float& x, float& y);

    // The Smooth Aiming log (SNP_AIM_LOG=1 or a file snp-aim-log; src/main/mouse.cpp), a line
    // with the time; does nothing when it's off.
    void aim_log(const char* format, ...);
}

#endif
