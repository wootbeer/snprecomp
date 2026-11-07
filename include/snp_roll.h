#pragma once

#include <cstdint>

namespace snp::roll {
    // Called with each controller read of a player's buttons: while a Roll input
    // (Roll, Roll Left, Roll Right in the controls) has just been pressed, turns
    // the buttons into a double tap of that way's move button.
    void apply(int player_index, uint16_t* buttons);
}
