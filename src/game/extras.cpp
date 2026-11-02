// The Enhancements and Cheats tabs' effects on the game, and the Graphics
// tab's screen filter and bloom (src/main/extras_config.cpp), applied once a
// frame (snp_extras_frame, from the display list hook).
//
// Addresses (the game's own variables, the same for every supported ROM):
//   0x800D5A94  time left (word)
//   0x800D5A98  health (word, 100 full)
//   0x800D5C0E  continues left (halfword)
//   0x800D5AA0  score (word)
//   0x80075DD0  the progress and settings block saved with the game (20
//               bytes, copied to the save buffer at 0x80075DAC to save);
//               bytes 7 - 9 record clearing the game on Easy, Normal and
//               Hard, which unlock the OPT. menu's options.
#include <cstdint>

#include "recomp.h"
#include "rt64_screen_filter.h"

#include "snp_config.h"
#include "snp_recomp_hooks.h"

bool snp_in_front_end_menus(); // src/game/overlay_loader.cpp

namespace {
    constexpr uint32_t time_addr = 0x800D5A94;
    constexpr uint32_t health_addr = 0x800D5A98;
    constexpr uint32_t continues_addr = 0x800D5C0E;
    constexpr uint32_t score_addr = 0x800D5AA0;
    constexpr uint32_t max_score = 999999999; // the GameShark code's 0x3B9AC9FF
    constexpr uint32_t clear_flags_addr = 0x80075DD7;
    constexpr uint32_t saved_clear_flags_addr = 0x80075DB3;
    constexpr uint32_t clear_flag_count = 3;

    uint8_t& byte_at(uint8_t* rdram, uint32_t addr) {
        return rdram[(addr - 0x80000000u) ^ 3];
    }

    uint32_t& word_at(uint8_t* rdram, uint32_t addr) {
        return *(uint32_t*)(rdram + (addr - 0x80000000u));
    }

    // While the OPT. menu is unlocked: the clear records as the game last set
    // them (from the save, or a new clear), put back into the save.
    bool unlocked = false;
    uint8_t real_clear_flags[clear_flag_count] = {};
}

extern "C" void snp_extras_frame(uint8_t* rdram) {
    using snp::config::Option;
    using snp::config::get_option;

    // Menus Only: the filter in the menus (the front end), not in play or the
    // intro scenes (played like stages).
    bool filter_off = snp::config::get_screen_filter_menus_only() && !snp_in_front_end_menus();
    RT64::setScreenFilter(filter_off ? RT64::ScreenFilter::None : static_cast<RT64::ScreenFilter>(snp::config::get_screen_filter()));
    RT64::setBloom(static_cast<RT64::Bloom>(snp::config::get_bloom()));

    if (get_option(Option::InfiniteHealth)) {
        uint32_t& health = word_at(rdram, health_addr);
        if (health > 0 && health < 100) {
            health = 100;
        }
    }
    if (get_option(Option::InfiniteTime)) {
        uint32_t& time = word_at(rdram, time_addr);
        if (time > 0 && time < 99) {
            time = 99;
        }
    }
    if (get_option(Option::InfiniteContinues)) {
        uint32_t& word = word_at(rdram, continues_addr & ~3u); // the halfword is the word's low half
        if ((word & 0xFFFF) < 9) {
            word = (word & 0xFFFF0000u) | 9;
        }
    }

    if (get_option(Option::MaxScore) && !snp_in_front_end_menus()) {
        word_at(rdram, score_addr) = max_score;
    }

    bool unlock = get_option(Option::UnlockOptions);
    for (uint32_t i = 0; i < clear_flag_count; i++) {
        uint8_t& flags = byte_at(rdram, clear_flags_addr + i);
        if (unlock) {
            if (flags != 0xFF) { // set by the game: loaded from the save, or a clear
                real_clear_flags[i] = flags;
                flags = 0xFF;
            }
        }
        else if (unlocked) {
            flags = real_clear_flags[i];
        }
    }
    unlocked = unlock;
}

// After the game copies the progress block into the save buffer: the real
// clear records go into the save.
extern "C" void snp_extras_saving(uint8_t* rdram) {
    if (!unlocked) {
        return;
    }
    for (uint32_t i = 0; i < clear_flag_count; i++) {
        byte_at(rdram, saved_clear_flags_addr + i) = real_clear_flags[i];
    }
}
