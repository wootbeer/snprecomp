// Second Player (General tab): with it on, the next controller that presses a
// button after player 1's joins as player 2. Player 1 keeps the keyboard, the
// mouse and every other controller; player 2 reads that one controller with
// the same controller bindings (lib-patches/RecompFrontend/0021). Nothing to
// assign: the frontend stays in single player mode, and its Controls tab is
// the usual one.
//
// Game's 2-Player Mode: the game sees port 2 plugged in while two controllers
// are connected (as on the console, where its 2-player mode starts when a
// second controller is in), and reads player 2 there. Co-op Mod: port 2 stays
// empty and mods read player 2 (src/main/mod_api.cpp).
#include <atomic>

#include "recompinput/input_state.h"

#include "snp_config.h"
#include "snp_second_player.h"

namespace {
    // Player 1's controller: the first one to press a button (-1 until then).
    SDL_JoystickID player1 = -1;
    std::atomic<SDL_JoystickID> player2 = -1;
}

void snp::second_player::update() {
    const snp::config::SecondPlayer mode = snp::config::get_second_player();
    SDL_JoystickID p2 = player2.load();

    if (p2 >= 0 && (mode == snp::config::SecondPlayer::Off || !recompinput::is_controller_connected(p2))) {
        p2 = -1;
    }
    if (player1 >= 0 && !recompinput::is_controller_connected(player1)) {
        player1 = -1;
    }
    // Player 1's controller is the first to press a button, but never player 2's.
    if (player1 < 0) {
        player1 = recompinput::get_controller_pressing_button(p2);
    }
    // Player 2 is another one pressing a button once player 1 has one.
    if (mode != snp::config::SecondPlayer::Off && p2 < 0 && player1 >= 0 && !recompinput::game_input_disabled()) {
        p2 = recompinput::get_controller_pressing_button(player1);
    }

    if (p2 != player2.load()) {
        player2.store(p2);
        recompinput::set_second_player_controller(p2);
    }
}

bool snp::second_player::joined() {
    return player2.load() >= 0 && snp::config::get_second_player() != snp::config::SecondPlayer::Off;
}

bool snp::second_player::port2_plugged() {
    return snp::config::get_second_player() == snp::config::SecondPlayer::Game &&
        recompinput::get_connected_controller_count() >= 2;
}
