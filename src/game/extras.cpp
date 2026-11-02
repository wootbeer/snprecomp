// The Enhancements and Cheats tabs' effects on the game, and the Graphics
// tab's screen filter and bloom (src/main/extras_config.cpp), applied once a
// frame (snp_extras_frame, from the display list hook).
//
// Addresses (the game's own variables, the same for every supported ROM):
//   0x800D5A94  time left (word)
//   0x800D5A98  health (word, 100 full)
//   0x800D5C0E  continues left (halfword)
//   0x800D5AA0  score (word)
//   per stage: each stage's overlay (load table entries 4-13 are stages 0-0 to
//               3-3) is followed by the player's data, whose invincibility
//               flag (byte) is 0x27B and air jump count (byte) 0x25C past the
//               overlay's BSS end: the community's GameShark codes, one per
//               stage, are exactly these. In 3-3, Earth's health (halfword) is
//               at 0x80100E08, in the overlay's BSS.
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
uint32_t snp_overlay_area_rom(); // src/game/overlay_loader.cpp: the loaded overlay's ROM address, or 0

namespace {
    constexpr uint32_t time_addr = 0x800D5A94;
    constexpr uint32_t health_addr = 0x800D5A98;
    constexpr uint32_t continues_addr = 0x800D5C0E;
    constexpr uint32_t score_addr = 0x800D5AA0;
    constexpr uint32_t load_table_addr = 0x800597AC; // {rom, vram, text end, data end, bss end}
    constexpr uint32_t first_stage_entry = 4; // 0-0
    constexpr uint32_t last_stage_entry = 13; // 3-3
    constexpr uint32_t invincible_offset = 0x27B;
    constexpr uint32_t air_jumps_offset = 0x25C;
    constexpr uint32_t earth_health_addr = 0x80100E08;
    constexpr uint16_t earth_health_full = 50000; // the GameShark code's 0xC350
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

    // Invincible was on last frame, in this stage overlay's player data (0: none).
    uint32_t invincible_flag_addr = 0;

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

    // The stage being played: its load table entry, found by the loaded overlay's ROM address.
    uint32_t stage_entry = 0, stage_bss_end = 0;
    if (const uint32_t rom = snp_overlay_area_rom()) {
        for (uint32_t i = first_stage_entry; i <= last_stage_entry; i++) {
            const uint32_t entry = load_table_addr + i * 20;
            if (word_at(rdram, entry) == rom) {
                stage_entry = i;
                stage_bss_end = word_at(rdram, entry + 16);
                break;
            }
        }
    }
    const bool invincible = get_option(Option::Invincible) && stage_entry != 0;
    const uint32_t flag_addr = (invincible && stage_entry != last_stage_entry) ? stage_bss_end + invincible_offset : 0;
    if (invincible_flag_addr != 0 && invincible_flag_addr != flag_addr) {
        byte_at(rdram, invincible_flag_addr) = 0; // turned off, or left the stage: back to normal
    }
    invincible_flag_addr = flag_addr;
    if (flag_addr != 0) {
        byte_at(rdram, flag_addr) = 1;
    }
    if (invincible && stage_entry == last_stage_entry) {
        uint32_t& word = word_at(rdram, earth_health_addr); // the halfword is the word's high half
        word = (word & 0x0000FFFFu) | ((uint32_t)earth_health_full << 16);
    }
    if (get_option(Option::InfiniteAirJumps) && stage_entry != 0) {
        byte_at(rdram, stage_bss_end + air_jumps_offset) = 0;
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
