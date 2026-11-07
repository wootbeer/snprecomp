// One-button rolls (issue 21): the game rolls on a quick double tap of a move
// button (C Left / C Right, or the D-Pad in the left-handed control type). For
// light guns with pedals that's awkward, so the Roll, Roll Left and Roll Right
// inputs play that double tap into the game's controller reads.
//
// Counted in controller reads, not time, so it's the same at any frame rate:
// the move button is let go, pressed, let go and pressed again, each for a few
// reads, then the player's own buttons take over again (a held pedal keeps
// Saki moving that way after the roll).
#include <array>

#include "recompinput/profiles.h"

#include "snp_roll.h"

namespace {
    constexpr uint16_t c_left = 0x0002, c_right = 0x0001;
    constexpr uint16_t dpad_left = 0x0200, dpad_right = 0x0100;

    // Reads in each step: released, pressed, released, pressed.
    constexpr int step_reads = 2;
    constexpr int steps = 4;

    struct State {
        bool roll_held = false;
        bool left_held = false;
        bool right_held = false;
        uint16_t button = 0;   // the move button being tapped, 0 when not rolling
        int read = 0;          // reads since the roll started
        bool last_left = false; // the way last moved, for Roll while standing
        bool last_dpad = false; // whether the player last moved with the D-Pad
    };
    std::array<State, 4> states;

    bool pressed(bool now, bool& was) {
        const bool edge = now && !was;
        was = now;
        return edge;
    }
}

void snp::roll::apply(int player_index, uint16_t* buttons) {
    if (player_index < 0 || player_index >= (int)states.size()) {
        return;
    }
    State& s = states[player_index];
    using recompinput::GameInput;
    namespace profiles = recompinput::profiles;

    const bool held_left = (*buttons & (c_left | dpad_left)) != 0;
    const bool held_right = (*buttons & (c_right | dpad_right)) != 0;
    if (held_left != held_right) {
        s.last_left = held_left;
        s.last_dpad = (*buttons & (dpad_left | dpad_right)) != 0 && (*buttons & (c_left | c_right)) == 0;
    }

    const bool roll = pressed(profiles::get_extra_input(player_index, GameInput::ROLL), s.roll_held);
    const bool roll_left = pressed(profiles::get_extra_input(player_index, GameInput::ROLL_LEFT), s.left_held);
    const bool roll_right = pressed(profiles::get_extra_input(player_index, GameInput::ROLL_RIGHT), s.right_held);
    if (s.button == 0 && (roll || roll_left || roll_right)) {
        const bool left = roll_left ? true : roll_right ? false : s.last_left;
        if (s.last_dpad) {
            s.button = left ? dpad_left : dpad_right;
        }
        else {
            s.button = left ? c_left : c_right;
        }
        s.read = 0;
    }
    if (s.button == 0) {
        return;
    }

    // Neither way's move buttons while rolling, but the tapped one when its step is "pressed".
    const int step = s.read / step_reads;
    *buttons &= (uint16_t)~(c_left | c_right | dpad_left | dpad_right);
    if (step % 2 == 1) {
        *buttons |= s.button;
    }
    if (++s.read >= steps * step_reads) {
        s.button = 0;
    }
}
