#pragma once

// The Second Player option (src/main/second_player.cpp): a second controller
// for the game's own 2-player mode or for mods.
namespace snp::second_player {
    // Once per read of the game's port 1: who player 1's controller is, and
    // whether a controller has joined as player 2.
    void update();
    // A controller has joined as player 2 (and the option is on).
    bool joined();
    // The game's 2-player mode is on and two controllers are connected: the
    // game sees a controller on port 2.
    bool port2_plugged();
}
