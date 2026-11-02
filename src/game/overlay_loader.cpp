// Registers code segments with the runtime when the game loads them.
//
// The game's segment loader, func_8003A5F0 (called by func_8003A1D0 with an
// entry of the load table at 0x800597AC, tools/segments.py), copies a
// segment from ROM in 0x1000-byte osPiStartDma chunks. The runtime registers
// a code section's functions (for indirect calls and calls between
// segments) only when one DMA covers the whole section, so the larger
// overlays were never registered. The hook in snp.jp.toml calls this once
// the copy is done instead.
#include <algorithm>
#include <cstdint>

#include "recomp.h"

#include "snp_mouse.h"
#include "snp_recomp_hooks.h"

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);

namespace {
    // The game's load table: {rom, vram, text end, data end, bss end}, ended
    // by an entry with vram 0.
    constexpr int32_t load_table = (int32_t)0x800597ACu;
    constexpr uint32_t load_table_max_entries = 64;

    bool startup_cleanup_done = false;

    // The front-end menus' overlay (title menu, OPT. menu, ...), loaded at the
    // overlay area like each stage's.
    constexpr int32_t overlay_area_start = (int32_t)0x800E4780u;
    constexpr uint32_t front_end_overlay_rom = 0x683640;
    uint32_t overlay_area_rom = 0;
}

bool snp_in_front_end_menus() {
    return overlay_area_rom == front_end_overlay_rom;
}

// args: the loader's request, {rom start, rom end, ram, ...}.
extern "C" void snp_segment_loaded(uint8_t* rdram, int32_t args) {
    if (!startup_cleanup_done) {
        // The runtime starts the game by registering every section in the
        // first MB of ROM as if it were loaded right after main, which puts
        // seg0 and overlays 1-5 at addresses they never run from (between
        // main's text and the overlay area). Drop those before the game
        // loads anything real, or a later unload that only partly covers one
        // is fatal.
        constexpr int32_t main_text_end = (int32_t)0x80056E80u;
        constexpr int32_t overlay_area = (int32_t)0x800E4780u;
        unload_overlays(main_text_end, overlay_area - main_text_end);
        startup_cleanup_done = true;
    }

    uint32_t rom = MEM_W(0x0, args);
    uint32_t rom_end = MEM_W(0x4, args);
    int32_t ram = MEM_W(0x8, args);

    // Only code segments (load table entries with text) need registering;
    // the loader is also used for data. Overlays sharing this address can be
    // larger than this one: unload the whole span any of them covers, so no
    // old section is only partly unloaded.
    bool is_code = false;
    uint32_t span = 0;
    for (uint32_t i = 0; i < load_table_max_entries; i++) {
        int32_t entry = load_table + i * 20;
        uint32_t entry_rom = MEM_W(0x0, entry);
        int32_t entry_vram = MEM_W(0x4, entry);
        int32_t entry_text_end = MEM_W(0x8, entry);
        if (entry_rom == 0 || entry_vram == 0) {
            break;
        }
        if (entry_vram == ram && entry_text_end != entry_vram) {
            span = std::max<uint32_t>(span, entry_text_end - entry_vram);
            if (entry_rom == rom) {
                is_code = true;
            }
        }
    }
    if (!is_code) {
        return;
    }

    if (ram == overlay_area_start) {
        if (rom != overlay_area_rom) {
            // Which overlay is loaded (each stage has its own), for mapping stages to their
            // overlays: the per-stage GameShark codes only mean something in their own stage.
            snp::mouse::aim_log("overlay loaded: ROM 0x%06X", rom);
        }
        overlay_area_rom = rom;
    }
    unload_overlays(ram, span);
    load_overlays(rom, ram, rom_end - rom);
}
