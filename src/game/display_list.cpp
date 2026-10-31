// Edits to each frame's display list just before a task thread loads it
// (hooked before each osSpTaskLoad, snp.jp.toml): widescreen, HUD anchoring
// and interpolation IDs.
//
// Widescreen: the game draws every frame inside an inset (scissor 12,8 -
// 308,231), clears only that area and draws a black border around it. RT64
// only widens a 3D view whose scissor spans the screen, so in Expand the inset
// scissor and the fill rectangles spanning it become full screen and the
// border is dropped. Original aspect is left alone.
//
// Diagnostic: with SNP_DL_DIAG set, every SNP_DL_DIAG_INTERVAL-th (default
// 300) graphics task's commands of interest are printed as they're walked.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "recomp.h"

#include "snp_config.h"
#include "snp_mod_api.h"
#include "snp_mouse.h"
#include "snp_recomp_hooks.h"

namespace snp {
    void interp_new_frame(); // interpolation.cpp
    uint32_t interp_matrix_id(uint32_t addr, uint32_t& instance, bool& object);
    void interp_number_instances(const std::vector<uint32_t>& drawn);
    void interp_walk_done(uint32_t untagged, uint32_t reused_entries);
    uint32_t interp_numbered_id(uint32_t id, uint32_t instance);
    void interp_set_object_model(uint32_t instance, uint32_t model);
    void interp_set_object_place(uint32_t instance, const float (&place)[3]);
}

bool snp_in_front_end_menus(); // src/game/overlay_loader.cpp

namespace {
    // G_SETSCISSOR / G_FILLRECT words: coordinates are 10.2 fixed point.
    constexpr uint32_t xy(uint32_t x, uint32_t y) {
        return ((x * 4) << 12) | (y * 4);
    }
    constexpr uint32_t G_NOOP = 0x00, G_VTX = 0x01, G_FILLRECT = 0xF6, G_SETSCISSOR = 0xED;

    // The inset scissor and its clear, and what they become in Expand.
    constexpr uint32_t inset_scissor_w0 = (G_SETSCISSOR << 24) | xy(12, 8);
    constexpr uint32_t inset_scissor_w1 = xy(308, 231);
    constexpr uint32_t full_scissor_w0 = (G_SETSCISSOR << 24) | xy(0, 0);
    constexpr uint32_t full_scissor_w1 = xy(320, 240);
    constexpr uint32_t ground_band_bottom = 220 * 4;

    // The border's four fill rectangles: {w0, w1}.
    constexpr uint32_t border[4][2] = {
        { (G_FILLRECT << 24) | xy(308, 9), xy(12, 7) },
        { (G_FILLRECT << 24) | xy(308, 232), xy(12, 230) },
        { (G_FILLRECT << 24) | xy(13, 231), xy(11, 8) },
        { (G_FILLRECT << 24) | xy(309, 231), xy(307, 8) },
    };

    uint32_t segments[16];
    bool log_frame = false;

    // The frame's first color image command, and whether its first drawing
    // command is a texture rectangle at the screen's top left corner.
    uint32_t first_cimg_w0, first_cimg_w1;
    bool seen_draw;
    bool starts_with_picture;
    bool effect_area_scissor; // the game's own 0,8 - 320,231 scissor was set
    // The fog colour the frame's first triangles are drawn with (0 if none).
    bool seen_triangle;
    uint32_t fog_color;
    uint32_t first_fog_color;

    void set_command(uint8_t* rdram, uint32_t addr, uint32_t w0, uint32_t w1) {
        MEM_W(0, (gpr)(int32_t)addr) = (int32_t)w0;
        MEM_W(4, (gpr)(int32_t)addr) = (int32_t)w1;
    }

    uint32_t resolve(uint32_t addr) {
        if (addr & 0x80000000u) {
            return addr;
        }
        return 0x80000000u | ((segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF);
    }

    // Scratch lists and copies go in RAM the game doesn't use (it only uses
    // the first 4 MB). Each area is used in turn, so a frame the renderer
    // hasn't read yet keeps its copies.
    constexpr uint32_t quad_area = 0x807F0000;
    constexpr uint32_t quad_slots = 64;
    constexpr uint32_t quad_bytes = 4 * 16;
    uint32_t quad_slot = 0;

    // Fades, tints and backdrop bands (the horizon glow behind the Radan
    // fight's skyline) are a 4-vertex quad across the screen's full width
    // (x -160..160, any height) drawn with the 2D projection, which RT64 keeps
    // at 4:3. In Expand the vertex load points at a copy four times as wide.
    void widen_screen_quad(uint8_t* rdram, uint32_t addr, uint32_t w0, uint32_t w1) {
        if (((w0 >> 12) & 0xFF) != 4) {
            return;
        }
        uint32_t src = resolve(w1);
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t xy_word = (uint32_t)MEM_W(i * 16, (gpr)(int32_t)src);
            int16_t x = (int16_t)(xy_word >> 16), y = (int16_t)xy_word;
            if ((x != 160 && x != -160) || y > 120 || y < -120) {
                return;
            }
        }
        uint32_t dst = quad_area + quad_slot * quad_bytes;
        quad_slot = (quad_slot + 1) % quad_slots;
        for (uint32_t off = 0; off < quad_bytes; off += 4) {
            uint32_t word = (uint32_t)MEM_W(off, (gpr)(int32_t)src);
            if (off % 16 == 0) {
                int16_t x = (int16_t)(word >> 16);
                word = ((uint32_t)(uint16_t)(x * 4) << 16) | (word & 0xFFFF);
            }
            MEM_W(off, (gpr)(int32_t)dst) = (int32_t)word;
        }
        set_command(rdram, addr, w0, dst);
    }

    // A prefix that clears the screen to black and then branches to the
    // game's list: for frames that start with a full-screen 4:3 picture (the
    // title backdrop; nothing clears the sides in Expand), and after switching
    // back to Original.
    constexpr uint32_t prefix_area = 0x807F2000;
    constexpr uint32_t prefix_slots = 8;
    constexpr uint32_t prefix_commands = 64; // the branch to the game's list is the last
    constexpr uint32_t prefix_bytes = prefix_commands * 8;

    // The screen-wide color images a frame draws to, in order (in gameplay
    // the first is an effect buffer, not the framebuffer shown).
    constexpr uint32_t max_cimgs = 4;
    uint32_t cimgs[max_cimgs][2];
    uint32_t cimg_count = 0;

    // Textures a frame reads from a 320-wide image before drawing to it: the
    // motion blur keeps the last frame in an effect buffer (0x8011B760) and
    // draws it over the new one, so it mustn't be cleared.
    constexpr uint32_t cimg_bytes = 320 * 240 * 2;
    constexpr uint32_t max_early_reads = 1024;
    uint32_t early_reads[max_early_reads];
    uint32_t early_read_count = 0;

    bool read_before_drawn(uint32_t cimg_w1) {
        uint32_t start = cimg_w1 & 0x1FFFFFFF;
        for (uint32_t i = 0; i < early_read_count; i++) {
            if (early_reads[i] >= start && early_reads[i] < start + cimg_bytes) {
                return true;
            }
        }
        return false;
    }

    uint32_t prefix_slot = 0;

    // all: clear every color image the frame draws to, not just the first.
    uint32_t clear_prefix(uint8_t* rdram, uint32_t dl, bool all) {
        uint32_t prefix = prefix_area + prefix_slot * prefix_bytes;
        prefix_slot = (prefix_slot + 1) % prefix_slots;
        uint32_t commands[prefix_commands][2];
        uint32_t n = 0;
        auto cmd = [&](uint32_t a, uint32_t b) { commands[n][0] = a; commands[n][1] = b; n++; };
        cmd(0xE3000A01, 0x00300000); // cycle type: fill
        cmd(0xF7000000, 0x00010001); // fill color: black, as the game's own clears
        for (uint32_t i = 0; i < (all ? cimg_count : 1); i++) {
            if (read_before_drawn(cimgs[i][1])) {
                continue;
            }
            cmd(cimgs[i][0], cimgs[i][1]);
            cmd(full_scissor_w0, full_scissor_w1);
            cmd((G_FILLRECT << 24) | xy(319, 239), xy(0, 0));
        }
        if (all && cimg_count > 1) {
            cmd(cimgs[0][0], cimgs[0][1]); // the game's list starts on the first
        }
        while (n < prefix_commands - 1) {
            cmd(G_NOOP << 24, 0);
        }
        cmd(0xDE010000, dl); // branch to the game's display list
        for (uint32_t i = 0; i < n; i++) {
            set_command(rdram, prefix + i * 8, commands[i][0], commands[i][1]);
        }
        return prefix;
    }

    // The zoom into the title draws its effect over rows 8 - 231 only; Expand
    // shows all 240 rows, so the rest kept the title picture. Such frames end
    // with rows 0 - 7 and 232 - 239 filled black: the closing full sync is
    // replaced with a branch to a list that fills them, syncs and ends.
    constexpr uint32_t suffix_area = 0x807F3000;
    constexpr uint32_t suffix_slots = 8;
    constexpr uint32_t suffix_bytes = 8 * 8;
    uint32_t suffix_slot = 0;
    constexpr uint32_t effect_scissor_w0 = (G_SETSCISSOR << 24) | xy(0, 8);
    constexpr uint32_t effect_scissor_w1 = xy(320, 231);

    void black_edge_rows(uint8_t* rdram, uint32_t fullsync_addr) {
        uint32_t suffix = suffix_area + suffix_slot * suffix_bytes;
        suffix_slot = (suffix_slot + 1) % suffix_slots;
        const uint32_t commands[][2] = {
            { 0xE3000A01, 0x00300000 }, // cycle type: fill
            { first_cimg_w0, first_cimg_w1 }, // the screen (the frame may end drawing elsewhere)
            { full_scissor_w0, full_scissor_w1 },
            { 0xF7000000, 0x00010001 }, // fill color: black
            { (G_FILLRECT << 24) | xy(319, 7), xy(0, 0) },
            { (G_FILLRECT << 24) | xy(319, 239), xy(0, 232) },
            { 0xE9000000, 0 }, // the game's full sync, moved after the fills
            { 0xDF000000, 0 },
        };
        uint32_t addr = suffix;
        for (const auto& c : commands) {
            set_command(rdram, addr, c[0], c[1]);
            addr += 8;
        }
        set_command(rdram, fullsync_addr, 0xDE010000, suffix);
    }

    // Mods' overlays (snp_draw_overlay, src/main/mod_api.cpp): a display list
    // a mod gives for the end of each frame in play, after the HUD. The
    // frame's full sync becomes a branch to a suffix: gEXEnable, extended RDRAM
    // on (mods live past the N64's 8 MB, from 0x81000000), a call to the mod's
    // list, extended RDRAM off, then the full sync. Only in frames that loaded
    // the HUD's 2D projection, outside the front end's menus. The mod's list is
    // walked like the frame's own, so its 2D matrices can get a live ID
    // (snp_set_live_reticle).
    constexpr uint32_t overlay_area = 0x807FA000;
    constexpr uint32_t overlay_slots = 8;
    constexpr uint32_t overlay_bytes = 6 * 8;
    uint32_t overlay_slot = 0;
    bool hud_seen = false; // this frame loaded the HUD's 2D projection

    bool in_overlay_area(uint32_t addr) {
        return addr >= overlay_area && addr < overlay_area + overlay_slots * overlay_bytes;
    }

    void add_overlay(uint8_t* rdram, uint32_t fullsync_addr, uint32_t list) {
        uint32_t suffix = overlay_area + overlay_slot * overlay_bytes;
        overlay_slot = (overlay_slot + 1) % overlay_slots;
        const uint32_t commands[][2] = {
            { 0xE0525464, 0x10000064 }, // gEXEnable
            { 0x6400002C, 1 }, // gEXSetRDRAMExtended(1)
            { 0xDE000000, list }, // the mod's list
            { 0x6400002C, 0 }, // gEXSetRDRAMExtended(0)
            { 0xE9000000, 0 }, // the game's full sync, moved after it
            { 0xDF000000, 0 },
        };
        uint32_t addr = suffix;
        for (const auto& c : commands) {
            set_command(rdram, addr, c[0], c[1]);
            addr += 8;
        }
        set_command(rdram, fullsync_addr, 0xDE010000, suffix);
    }

    // Texture Filtering and HUD Filtering: Sharp. A render mode command that
    // sets the texture filter (othermode bits 12 - 13) is the HUD's if the
    // first thing drawn after it is a rectangle or under the HUD's 2D
    // projection (the crosshair), else the scenery's; when that option is on
    // it's set to point sampling. What it was is kept by address: lists the
    // game keeps get it back when the option is turned off.
    constexpr uint32_t texture_filter_bits = 0x3000;
    bool sharp_textures = false;
    bool sharp_hud = false;
    uint32_t pending_filter = 0; // the latest such command, until something is drawn
    struct FilteredMode {
        uint32_t w0, w1; // the game's
        uint32_t sharp_w0, sharp_w1; // as set here
    };
    std::unordered_map<uint32_t, FilteredMode> filtered_modes;

    bool sets_texture_filter(uint32_t w0) {
        if ((w0 >> 24) == 0xEF) { // G_RDPSETOTHERMODE: othermode_H in w0's low 24 bits
            return true;
        }
        if ((w0 >> 24) != 0xE3) {
            return false;
        }
        // G_SETOTHERMODE_H (F3DEX2): w0 holds 32 - shift - length and length - 1.
        uint32_t length = (w0 & 0xFF) + 1;
        uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - length;
        uint32_t mask = (length >= 32 ? 0xFFFFFFFFu : ((1u << length) - 1)) << shift;
        return (mask & texture_filter_bits) != 0;
    }

    void set_texture_filter(uint8_t* rdram, uint32_t addr, bool sharp) {
        uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
        uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
        auto kept = filtered_modes.find(addr);
        if (kept != filtered_modes.end()) {
            if (kept->second.sharp_w0 != w0 || kept->second.sharp_w1 != w1) { // the game wrote something else here
                filtered_modes.erase(kept);
            }
            else {
                if (!sharp) {
                    set_command(rdram, addr, kept->second.w0, kept->second.w1);
                    filtered_modes.erase(kept);
                }
                return;
            }
        }
        if (!sharp) {
            return;
        }
        uint32_t sharp_w0 = w0, sharp_w1 = w1;
        if ((w0 >> 24) == 0xEF) {
            sharp_w0 = w0 & ~texture_filter_bits;
        }
        else {
            sharp_w1 = w1 & ~texture_filter_bits;
        }
        if (sharp_w0 != w0 || sharp_w1 != w1) {
            filtered_modes[addr] = { w0, w1, sharp_w0, sharp_w1 };
            set_command(rdram, addr, sharp_w0, sharp_w1);
        }
    }

    // Smooth Aiming (src/main/mouse.cpp): the reticle's matrix gets a live
    // ID, which RT64 moves on every displayed frame to where the latest input
    // puts it. Its crosshair lines are fill rectangles, which RT64 can't move:
    // each is drawn instead as a quad (the same colour, from the combiner)
    // under an identity matrix in that group, the full-length lines made long
    // enough that moving them never shows an end. The rectangle's command is
    // kept after the list's end, for lists the game keeps.
    uint32_t live_id = 0; // this frame's, or 0
    float live_x = 0.0f, live_y = 0.0f; // the reticle: offset from the centre, y up
    // Where the reticle was at the last few walks, newest first. The game builds a frame's
    // matrices up to two tasks before its display list goes out, so while the reticle moves its
    // matrix holds one of these, not where it is now.
    constexpr uint32_t live_recent_size = 4;
    float live_recent_x[live_recent_size] = {}, live_recent_y[live_recent_size] = {};
    uint32_t live_recent_count = 0;
    // For the Smooth Aiming log: this display list's HUD matrices, how many got the live ID, and
    // the one nearest the reticle.
    uint32_t hud_matrices = 0, hud_live = 0;
    float hud_nearest = -1.0f, hud_nearest_x = 0.0f, hud_nearest_y = 0.0f;
    constexpr uint32_t live_area = 0x807F5000;
    constexpr uint32_t live_banks = 4;
    constexpr uint32_t live_entries = 32;
    constexpr uint32_t live_entry_bytes = 0xC0;
    constexpr uint32_t live_identity = live_area; // an identity Mtx
    constexpr uint32_t live_lists = live_area + 0x40;
    constexpr uint32_t live_kept_command = 8 * 8;
    constexpr uint32_t live_vertices = 12 * 8;
    uint32_t live_bank = 0;
    uint32_t live_entry = 0;

    bool in_live_area(uint32_t addr) {
        return addr >= live_lists && addr < live_lists + live_banks * live_entries * live_entry_bytes;
    }

    uint32_t still_matrix_params();

    // Any other matrix group's ID kept out of the live IDs' range (RT64 would
    // move it with the reticle).
    uint32_t not_live(uint32_t id) {
        return ((id & 0xFF000000u) == 0x5E000000u) ? (id ^ 0x01000000u) : id;
    }

    // A fill rectangle of the crosshair's lines through the reticle (each
    // line is a 1-pixel grey rectangle in a black outline): its quad.
    bool draw_crosshair_rect_live(uint8_t* rdram, uint32_t addr, uint32_t w0, uint32_t w1) {
        if (live_entry >= live_entries) {
            return false;
        }
        auto px = [](uint32_t v) { float p = v / 4.0f; return p >= 640.0f ? p - 1024.0f : p; }; // 12-bit x wraps left of 0
        const float lrx = px((w0 >> 12) & 0xFFF), lry = (w0 & 0xFFF) / 4.0f;
        const float ulx = px((w1 >> 12) & 0xFFF), uly = (w1 & 0xFFF) / 4.0f;
        const float w = lrx - ulx, h = lry - uly;
        // Through one of the reticle's recent positions (the one its frame was built with): its
        // live ID holds that position, as the reticle's does.
        bool across = false, down = false;
        uint32_t lines_id = 0;
        for (uint32_t i = 0; (i < live_recent_count) && (lines_id == 0); i++) {
            const float line_x = 160.0f + live_recent_x[i], line_y = 120.0f - live_recent_y[i];
            across = std::fabs((uly + lry) * 0.5f - line_y) <= 2.5f && h <= 3.0f && (w >= 300.0f || w <= 2.0f);
            down = std::fabs((ulx + lrx) * 0.5f - line_x) <= 2.5f && w <= 3.0f && (h >= 200.0f || h <= 2.0f);
            if (across || down) {
                lines_id = snp::mouse::live_id(0, live_recent_x[i], live_recent_y[i]);
                if (i + 1 < live_recent_count) {
                    snp::mouse::set_live_previous(lines_id, live_recent_x[i + 1], live_recent_y[i + 1]);
                }
            }
        }
        if (lines_id == 0) {
            return false;
        }
        float left = ulx - 160.0f, right = lrx - 160.0f, top = 120.0f - uly, bottom = 120.0f - lry;
        if (across && w >= 300.0f) {
            left = -4000.0f;
            right = 4000.0f;
        }
        if (down && h >= 200.0f) {
            top = 4000.0f;
            bottom = -4000.0f;
        }
        const uint32_t entry = live_lists + (live_bank * live_entries + live_entry) * live_entry_bytes;
        live_entry++;
        const float corners[4][2] = { { left, top }, { left, bottom }, { right, bottom }, { right, top } };
        for (uint32_t i = 0; i < 4; i++) {
            const uint32_t v = entry + live_vertices + i * 16;
            const int16_t x = (int16_t)std::lround(corners[i][0]), y = (int16_t)std::lround(corners[i][1]);
            MEM_W(0, (gpr)(int32_t)v) = (int32_t)(((uint32_t)(uint16_t)x << 16) | (uint16_t)y);
            MEM_W(4, (gpr)(int32_t)v) = 0;
            MEM_W(8, (gpr)(int32_t)v) = 0;
            MEM_W(12, (gpr)(int32_t)v) = (int32_t)0xFFFFFFFF;
        }
        const uint32_t commands[9][2] = {
            { 0x6400000C, lines_id }, // gEXMatrixGroup, pushed
            { still_matrix_params() | 1, 0 },
            { 0xDA380002, live_identity }, // G_MTX: modelview, push, load
            { 0x01004008, entry + live_vertices }, // G_VTX: 4
            { 0x06000204, 0x00040600 }, // G_TRI2: as the reticle's quad
            { 0xD8380002, 0x40 }, // G_POPMTX: 1
            { 0x6400000D, 1 }, // gEXPopMatrixGroup
            { 0xDF000000, 0 },
            { w0, w1 }, // the game's rectangle (live_kept_command)
        };
        for (uint32_t i = 0; i < 9; i++) {
            set_command(rdram, entry + i * 8, commands[i][0], commands[i][1]);
        }
        set_command(rdram, addr, 0xDE000000, entry);
        return true;
    }

    // The warehouse in the intro (the soldiers among the crates) has its two
    // end walls, floor and ceiling but no side walls: a wider view sees past
    // the ends of the back wall, into black. Its list is called with a list
    // that calls it and then draws the side walls with the end walls' own
    // textures (loaded by copies of its own commands), shading and texture
    // scale, continuing them around the corners.
    constexpr uint32_t warehouse_bytes = 0x880; // to its end command, included
    constexpr uint64_t warehouse_hash = 0x1c09034e8e83b85full;
    constexpr uint32_t warehouse_upper_load = 0x4B8; // the end walls' upper band (y 100 - 300)
    constexpr uint32_t warehouse_lower_load = 0x7B8; // their lower band (y 0 - 100)
    constexpr uint32_t warehouse_load_commands = 14;
    constexpr uint32_t walls_area = 0x807F4000;
    constexpr uint32_t walls_vertices = walls_area + 0x200;

    bool is_warehouse(uint8_t* rdram, uint32_t list) {
        if ((uint32_t)MEM_W(0, (gpr)(int32_t)list) != 0xFD100000 ||
            (uint32_t)MEM_W(warehouse_bytes - 8, (gpr)(int32_t)list) != 0xDF000000) {
            return false;
        }
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t off = 0; off < warehouse_bytes; off += 4) {
            h = (h ^ (uint32_t)MEM_W(off, (gpr)(int32_t)list)) * 0x100000001b3ull;
        }
        return h == warehouse_hash;
    }

    uint32_t warehouse_with_side_walls(uint8_t* rdram, uint32_t list) {
        // The room is x -450 - 450, y 0 - 300, z -300 - 300 in its own space.
        // Along the end walls s is 14.9 per unit, -1024 at x -450 and 3072 at
        // x 450; t 40.96 per unit of height. The corners are shaded darker.
        constexpr float s_per_unit = (1210.0f + 1024.0f) / 150.0f;
        constexpr uint32_t edge = 0x00141EFF, wall = 0x001E28FF;
        uint32_t v = walls_vertices;
        auto vertex = [&](int x, int y, int z) {
            float s = x < 0 ? -1024.0f - (z + 300) * s_per_unit : 3072.0f + (z + 300) * s_per_unit;
            int t = y * 4096 / 100;
            uint32_t color = (y == 0 || y == 300) ? edge : wall;
            MEM_W(0, (gpr)(int32_t)v) = (int32_t)(((uint32_t)(uint16_t)x << 16) | (uint16_t)y);
            MEM_W(4, (gpr)(int32_t)v) = (int32_t)((uint32_t)(uint16_t)z << 16);
            MEM_W(8, (gpr)(int32_t)v) = (int32_t)(((uint32_t)(uint16_t)(int16_t)s << 16) | (uint16_t)t);
            MEM_W(12, (gpr)(int32_t)v) = (int32_t)color;
            v += 16;
        };
        // A quad's corners: near (z 300) and far (z -300) at the bottom (y0) and top (y1).
        const uint32_t lower = v;
        for (int x : { -450, 450 }) {
            vertex(x, 0, -300); vertex(x, 0, 300); vertex(x, 100, 300); vertex(x, 100, -300);
        }
        const uint32_t upper = v;
        for (int x : { -450, 450 }) {
            vertex(x, 100, -300); vertex(x, 100, 300); vertex(x, 200, 300); vertex(x, 200, -300);
            vertex(x, 300, 300); vertex(x, 300, -300);
        }
        auto tri2 = [](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
            return std::pair<uint32_t, uint32_t>(0x06000000 | (a * 2) << 16 | (b * 2) << 8 | c * 2, (d * 2) << 16 | (e * 2) << 8 | f * 2);
        };
        // Facing into the room: far bottom, far top, near top, then far bottom, near top, near bottom
        // on the left wall; the other way round on the right.
        auto quad_left = [&](uint32_t fb, uint32_t nb, uint32_t nt, uint32_t ft) { return tri2(fb, ft, nt, fb, nt, nb); };
        auto quad_right = [&](uint32_t fb, uint32_t nb, uint32_t nt, uint32_t ft) { return tri2(fb, nt, ft, fb, nb, nt); };

        uint32_t addr = walls_area;
        auto cmd = [&](uint32_t w0, uint32_t w1) { set_command(rdram, addr, w0, w1); addr += 8; };
        auto copy_load = [&](uint32_t from) {
            for (uint32_t i = 0; i < warehouse_load_commands; i++) {
                cmd((uint32_t)MEM_W(from + i * 8, (gpr)(int32_t)list), (uint32_t)MEM_W(from + i * 8 + 4, (gpr)(int32_t)list));
            }
        };
        cmd(0xDE000000, list);
        copy_load(warehouse_lower_load);
        cmd(0x01008010, lower); // 8 vertices
        auto q = quad_left(0, 1, 2, 3); cmd(q.first, q.second);
        q = quad_right(4, 5, 6, 7); cmd(q.first, q.second);
        copy_load(warehouse_upper_load);
        cmd(0x0100C018, upper); // 12 vertices
        q = quad_left(0, 1, 2, 3); cmd(q.first, q.second);
        q = quad_left(3, 2, 4, 5); cmd(q.first, q.second);
        q = quad_right(6, 7, 8, 9); cmd(q.first, q.second);
        q = quad_right(9, 8, 10, 11); cmd(q.first, q.second);
        cmd(0xDF000000, 0);
        return walls_area;
    }

    // The motion blur blends the effect buffer onto the frame, then copies the
    // frame into it, both in strips of 6 rows from row 8 to 231: the rows
    // the N64's border covers were left out. Expand shows them, unblurred,
    // as a band offset from the rest of the picture at the top and bottom.
    // In Expand the first and last strips of each pass are replaced with a
    // call to a list that also draws strips for rows 0 - 7 or 232 - 239, made
    // from that strip's own commands: its texture image and rows moved.
    constexpr uint32_t blur_buffer = 0x0011B760;
    constexpr uint32_t blur_area = 0x80740000;
    constexpr uint32_t blur_banks = 4;
    constexpr uint32_t blur_bank_bytes = 0x1000;
    constexpr uint32_t blur_entry_commands = 64;
    constexpr uint32_t blur_entry_bytes = blur_entry_commands * 8;
    constexpr uint32_t blur_entries = blur_bank_bytes / blur_entry_bytes;
    constexpr uint32_t blur_strip_max = 12; // commands, from its texture image to its rectangle's last word
    constexpr uint32_t row_bytes = 320 * 2;
    uint32_t blur_bank = 0;
    uint32_t blur_entry = 0;

    bool in_blur_area(uint32_t addr) {
        return addr >= blur_area && addr < blur_area + blur_banks * blur_bank_bytes;
    }

    // addr: a G_SETTIMG. If it starts a strip of the motion blur (a texture
    // rectangle across the screen, its image the rows it's drawn on of a
    // 320-wide one) that is the first or last of a pass, the strip is moved
    // into an entry with the extra strips. An entry starts with two no-ops
    // holding the strip's length and where it is in the entry, for putting it
    // back in a list the game draws again.
    bool extend_blur_strip(uint8_t* rdram, uint32_t addr) {
        if (blur_entry >= blur_entries) {
            return false;
        }
        uint32_t words = 0;
        uint32_t rect = 0;
        for (uint32_t i = 1; i < blur_strip_max; i++) {
            uint32_t op = (uint32_t)MEM_W(0, (gpr)(int32_t)(addr + i * 8)) >> 24;
            if (op == 0xE4) {
                rect = addr + i * 8;
                words = i + 3;
                break;
            }
            if (op != 0xF5 && op != 0xF4 && op != 0xF3 && op != 0xF2 && op != 0xE6 && op != 0xE7 && op != 0xE8) {
                return false;
            }
        }
        if (rect == 0) {
            return false;
        }
        uint32_t rw0 = (uint32_t)MEM_W(0, (gpr)(int32_t)rect), rw1 = (uint32_t)MEM_W(4, (gpr)(int32_t)rect);
        uint32_t ulx = (rw1 >> 12) & 0xFFF, uly = rw1 & 0xFFF, lrx = (rw0 >> 12) & 0xFFF, lry = rw0 & 0xFFF;
        if (ulx != 0 || lrx != 320 * 4 || (uly & 3) != 0 || lry <= uly) {
            return false;
        }
        bool first = (uly == 8 * 4);
        bool last = (uly <= 231 * 4 && lry >= 231 * 4);
        if (!first && !last) {
            return false;
        }
        // The image the strip reads, row 0 of it: the effect buffer or a 320-wide color image.
        uint32_t timg = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
        uint32_t base = (resolve(timg) & 0x1FFFFFFF) - (uly / 4) * row_bytes;
        bool known = (base == blur_buffer);
        for (uint32_t i = 0; i < cimg_count; i++) {
            known = known || (base == (cimgs[i][1] & 0x1FFFFFFF));
        }
        if (!known) {
            return false;
        }
        uint32_t entry = blur_area + blur_bank * blur_bank_bytes + blur_entry * blur_entry_bytes;
        blur_entry++;
        uint32_t commands[blur_entry_commands][2];
        uint32_t n = 0;
        auto cmd = [&](uint32_t a, uint32_t b) { commands[n][0] = a; commands[n][1] = b; n++; };
        // A copy of the strip on rows from top, reading those rows, rows
        // high. Whether the rectangle's lower edge is drawn (copy mode) or
        // not shows in where the game's strip ends: on its last row (13 or
        // 231) or after it.
        const int32_t edge = (int32_t)lry - (first ? 14 * 4 : 232 * 4);
        auto strip = [&](uint32_t top, uint32_t rows) {
            for (uint32_t i = 0; i < words; i++) {
                uint32_t a = (uint32_t)MEM_W(0, (gpr)(int32_t)(addr + i * 8));
                uint32_t b = (uint32_t)MEM_W(4, (gpr)(int32_t)(addr + i * 8));
                if (i == 0) {
                    b = 0x80000000u | (base + top * row_bytes);
                }
                else if (addr + i * 8 == rect) {
                    uint32_t y0 = top * 4;
                    uint32_t y1 = (uint32_t)((int32_t)(y0 + rows * 4) + edge);
                    a = (a & 0xFFFFF000) | (y1 & 0xFFF);
                    b = (b & 0xFFFFF000) | (y0 & 0xFFF);
                }
                cmd(a, b);
            }
        };
        cmd(G_NOOP << 24, words);
        cmd(G_NOOP << 24, 0);
        if (first) {
            strip(0, 2);
            strip(2, 6);
        }
        commands[1][1] = n;
        for (uint32_t i = 0; i < words; i++) {
            cmd((uint32_t)MEM_W(0, (gpr)(int32_t)(addr + i * 8)), (uint32_t)MEM_W(4, (gpr)(int32_t)(addr + i * 8)));
        }
        if (last) {
            strip(232, 6);
            strip(238, 2);
        }
        cmd(0xDF000000, 0);
        for (uint32_t i = 0; i < n; i++) {
            set_command(rdram, entry + i * 8, commands[i][0], commands[i][1]);
        }
        set_command(rdram, addr, 0xDE000000, entry);
        for (uint32_t i = 1; i < words; i++) {
            set_command(rdram, addr + i * 8, G_NOOP << 24, 0);
        }
        return true;
    }

    bool is_prefix(uint32_t addr) {
        return addr >= prefix_area && addr < prefix_area + prefix_slots * prefix_bytes;
    }

    // Interpolation IDs (src/game/interpolation.cpp). Every modelview load is
    // replaced with a call to a short list holding a matrix group (its ID, or
    // automatic) and the original load, so no load inherits another's group.
    constexpr uint32_t interp_area = 0x80700000;
    constexpr uint32_t interp_banks = 4;
    constexpr uint32_t interp_bank_bytes = 0x10000;
    constexpr uint32_t interp_entry_bytes = 5 * 8;
    constexpr uint32_t interp_entries = interp_bank_bytes / interp_entry_bytes;
    constexpr uint32_t G_EX_ID_AUTO = 0xFFFFFFFF;
    uint32_t interp_bank = 0;
    uint32_t interp_entry = 0;

    bool in_interp_area(uint32_t addr) {
        return addr >= interp_area && addr < interp_area + interp_banks * interp_bank_bytes;
    }

    // The game's matrix load each tagged command replaced, by the command's address. Read back
    // from here, not from the tag entry: a list the game keeps but doesn't draw for a few
    // frames (a stage's scenery while the game is left and the stage started again) still
    // calls an entry that a later frame has since filled with another load, and restoring that
    // drew alike models with each other's matrices (the city stage's cones traded places).
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> tagged_loads;

    // gEXMatrixGroup parameters. Skeleton (limb) matrices always interpolate
    // and are blended entry by entry: the game's bone matrices are skewed, and
    // decomposing them warped limbs on in-between frames. A numbered object
    // (one told apart from the others of its kind) always interpolates too:
    // RT64's automatic position stops interpolating on any frame where the
    // movement reverses or jumps tenfold, so things the game shakes (the
    // flying debris in stage 2-2) flipped between smoothed and snapped
    // frames. Everything else keeps RT64's automatic settings. A numbered ID
    // is unique in the frame and is paired directly (G_EX_ORDER_LINEAR),
    // otherwise by closeness.
    uint32_t matrix_group_params(bool skeleton, bool numbered) {
        constexpr uint32_t interpolate = 1, automatic = 2;
        const uint32_t motion = (skeleton || numbered) ? interpolate : automatic;
        const uint32_t order = numbered ? 0u : 1u;
        return (0u << 0) |   // no push
            (0u << 1) |      // modelview
            ((skeleton ? 0u : 1u) << 2) | // decompose
            (motion << 3) |  // position
            (motion << 5) |  // rotation
            (motion << 7) |  // scale
            (motion << 9) |  // skew
            (motion << 11) | // perspective
            (0u << 13) |     // vertices: skip
            (motion << 15) | // tiles
            (order << 17) |
            (0u << 19) |     // not editable
            (0u << 20) |     // aspect: auto
            (0u << 22) |     // texture coordinates: skip
            (automatic << 24); // look-at
    }

    // Every component skipped: the matrix as the game set it, every frame.
    uint32_t still_matrix_params() {
        constexpr uint32_t automatic = 2;
        return (1u << 2) |        // decompose
            (1u << 17) |          // order: auto
            (automatic << 24);    // look-at
    }

    // Matrix loads this frame that belong to an instance (a skeleton or an
    // object). Their IDs get the instance's number after the walk.
    struct InstanceTag {
        uint32_t entry;
        uint32_t id;
        uint32_t instance;
        bool object;
    };
    std::vector<InstanceTag> instance_tags;
    uint32_t interp_untagged = 0; // matrix loads this frame past the tag area (for the log)
    uint32_t interp_reused_entries = 0; // tagged loads whose entry held another load (the log)

    // The model a matrix load draws: the first vertices or display list
    // after it, before the next matrix (0 if none). Loads tagged in an earlier
    // frame are calls into the tag area.
    uint32_t drawn_model(uint8_t* rdram, uint32_t addr) {
        for (int n = 0; n < 64; n++, addr += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
            uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
            switch (w0 >> 24) {
                case G_VTX:
                    return resolve(w1);
                case 0xDE: // G_DL
                    return in_interp_area(w1) ? 0 : resolve(w1);
                case 0xDA: case 0xDF: // G_MTX, G_ENDDL
                    return 0;
                case 0xE4: // G_TEXRECT and its two halves
                    addr += 16;
                    break;
                default:
                    break;
            }
        }
        return 0;
    }

    // The RSP's modelview matrix stack as the walk goes (row vectors, as the N64's: a load with
    // G_MTX_MUL puts the new matrix in front of the top). Only for telling apart alike objects at
    // the same place in their own matrix (instances of one model under different parents).
    struct Matrix {
        float m[4][4];
    };
    constexpr int modelview_depth = 32;
    Matrix modelview[modelview_depth];
    int modelview_top = 0;

    Matrix identity_matrix() {
        Matrix r{};
        for (int i = 0; i < 4; i++) {
            r.m[i][i] = 1.0f;
        }
        return r;
    }

    // An N64 Mtx (s15.16: 16 integer halves, then 16 fraction halves).
    Matrix read_mtx(uint8_t* rdram, uint32_t mtx) {
        Matrix r;
        for (int i = 0; i < 16; i++) {
            int32_t hi = (int16_t)((uint32_t)MEM_W((i / 2) * 4, (gpr)(int32_t)mtx) >> ((i & 1) ? 0 : 16));
            uint32_t lo = (uint16_t)((uint32_t)MEM_W(32 + (i / 2) * 4, (gpr)(int32_t)mtx) >> ((i & 1) ? 0 : 16));
            r.m[i / 4][i % 4] = (int32_t)(((uint32_t)hi << 16) | lo) / 65536.0f;
        }
        return r;
    }

    Matrix multiply(const Matrix& a, const Matrix& b) {
        Matrix r{};
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                for (int k = 0; k < 4; k++) {
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
                }
            }
        }
        return r;
    }

    // A modelview G_MTX (F3DEX2: its parameters with G_MTX_PUSH inverted).
    void modelview_load(uint8_t* rdram, uint32_t w0, uint32_t w1) {
        const uint32_t params = (w0 & 0xFF) ^ 1;
        Matrix m = read_mtx(rdram, resolve(w1));
        if ((params & 1) && modelview_top + 1 < modelview_depth) { // G_MTX_PUSH
            modelview[modelview_top + 1] = modelview[modelview_top];
            modelview_top++;
        }
        modelview[modelview_top] = (params & 2) ? m : multiply(m, modelview[modelview_top]); // G_MTX_LOAD or MUL
    }

    // hud: loaded under the HUD's 2D projection (the reticle): drawn where the
    // game put it each frame, not interpolated, as the crosshair lines through
    // it are fill rectangles RT64 doesn't move between frames (and shots go
    // where the game has it).
    void tag_matrix(uint8_t* rdram, uint32_t addr, uint32_t w0, uint32_t w1, bool hud) {
        if (interp_entry >= interp_entries) {
            interp_untagged++;
            return;
        }
        uint32_t instance = 0;
        bool object = false;
        uint32_t id = not_live(snp::interp_matrix_id(resolve(w1), instance, object));
        if (hud) {
            id = G_EX_ID_AUTO;
            instance = 0;
            // The reticle (Smooth Aiming): its matrix puts it where the game has it.
            const float (&row)[4] = modelview[modelview_top].m[3];
            hud_matrices++;
            if (live_id != 0) {
                const float distance = std::fmax(std::fabs(row[0] - live_x), std::fabs(row[1] - live_y));
                if (hud_nearest < 0.0f || distance < hud_nearest) {
                    hud_nearest = distance;
                    hud_nearest_x = row[0];
                    hud_nearest_y = row[1];
                }
            }
            int32_t reticle = -1; // the recent position it's drawn at
            for (uint32_t i = 0; (live_id != 0) && (i < live_recent_count) && (reticle < 0); i++) {
                if (std::fabs(row[0] - live_recent_x[i]) < 1.0f && std::fabs(row[1] - live_recent_y[i]) < 1.0f) {
                    reticle = (int32_t)i;
                }
            }
            if (reticle >= 0) {
                // Its ID holds where it's drawn; the live offset moves it there from where it was
                // drawn the frame before (the next older position).
                id = snp::mouse::live_id(0, row[0], row[1]);
                if ((uint32_t)reticle + 1 < live_recent_count) {
                    snp::mouse::set_live_previous(id, live_recent_x[reticle + 1], live_recent_y[reticle + 1]);
                }
                hud_live++;
            }
            // A mod's crosshair (src/main/mod_api.cpp).
            else if (uint32_t mod_id = snp_in_front_end_menus() ? 0 : snp::mod_live_reticle_id(row[0], row[1])) {
                id = mod_id;
            }
        }
        uint32_t entry = interp_area + interp_bank * interp_bank_bytes + interp_entry * interp_entry_bytes;
        const uint32_t commands[5][2] = {
            { interp_entry == 0 ? 0xE0525464u : G_NOOP << 24, interp_entry == 0 ? 0x10000064u : 0u }, // gEXEnable, once a frame
            { 0x6400000C, id }, // gEXMatrixGroup
            { hud ? still_matrix_params() : matrix_group_params(id != G_EX_ID_AUTO && !object, false), 0 },
            { w0, w1 }, // the game's matrix load
            { 0xDF000000, 0 },
        };
        for (uint32_t i = 0; i < 5; i++) {
            set_command(rdram, entry + i * 8, commands[i][0], commands[i][1]);
        }
        if (instance != 0 && object) {
            snp::interp_set_object_model(instance, drawn_model(rdram, addr + 8));
            const float (&row)[4] = modelview[modelview_top].m[3];
            const float place[3] = { row[0], row[1], row[2] };
            snp::interp_set_object_place(instance, place);
        }
        if (instance != 0) {
            instance_tags.push_back({ entry, id, instance, object });
        }
        interp_entry++;
        tagged_loads[addr] = { w0, w1 };
        set_command(rdram, addr, 0xDE000000, entry);
    }

    // After the walk: number the instances this display list draws (all at
    // once) and put the numbers into their matrices' IDs.
    void number_tagged_instances(uint8_t* rdram) {
        snp::interp_walk_done(interp_untagged, interp_reused_entries);
        interp_untagged = 0;
        interp_reused_entries = 0;
        if (instance_tags.empty()) {
            return;
        }
        std::vector<uint32_t> drawn;
        for (const InstanceTag& tag : instance_tags) {
            drawn.push_back(tag.instance);
        }
        std::sort(drawn.begin(), drawn.end());
        drawn.erase(std::unique(drawn.begin(), drawn.end()), drawn.end());
        snp::interp_number_instances(drawn);
        for (const InstanceTag& tag : instance_tags) {
            set_command(rdram, tag.entry + 8, 0x6400000C, not_live(snp::interp_numbered_id(tag.id, tag.instance)));
            set_command(rdram, tag.entry + 16, matrix_group_params(!tag.object, true), 0);
        }
        instance_tags.clear();
    }

    // HUD text is drawn as 1:1 texture rectangles starting exactly on a texel.
    // Upscaled, edge pixels can round to the texel before it and show thin
    // lines from the neighbouring glyph; 1:1 rectangles start 1/32 texel later,
    // which samples the same texels at the N64's resolution.
    void nudge_rect_start(uint8_t* rdram, uint32_t addr) {
        uint32_t h0 = (uint32_t)MEM_W(0, (gpr)(int32_t)(addr + 8));  // RDPHALF_1: s, t (S10.5)
        uint32_t h1 = (uint32_t)MEM_W(4, (gpr)(int32_t)(addr + 8));
        uint32_t h2 = (uint32_t)MEM_W(4, (gpr)(int32_t)(addr + 16)); // RDPHALF_2: dsdx, dtdy (S5.10)
        int32_t s = (int16_t)(h1 >> 16), t = (int16_t)h1;
        if ((h2 >> 16) != 1 << 10 || (h2 & 0xFFFF) != 1 << 10 || (s & 31) != 0 || (t & 31) != 0) {
            return; // scaled, or already off a texel boundary (including a rectangle nudged before)
        }
        s += 1;
        t += 1;
        set_command(rdram, addr + 8, h0, ((uint32_t)(uint16_t)s << 16) | (uint16_t)t);
    }

    // HUD anchoring in Expand. RT64 keeps 2D rectangles in the centred 4:3
    // area unless they're anchored to an edge (gEXSetRectAlign). The HUD is
    // drawn after the game loads its 2D projection (0x802C3AE0); its top band
    // has the score on the left, TIME in the middle and LIFE on the right.
    // Elements there are anchored to the nearer edge, 16 pixels further out
    // (the original keeps a TV margin): each rectangle is replaced with a call
    // to a list that sets the alignment and a whole-window scissor, draws it
    // and restores both. Everything lower is left alone.
    constexpr uint32_t hud_projection = 0x802C3AE0;
    constexpr uint32_t hud_area = 0x807A0000;
    constexpr uint32_t hud_banks = 4;
    constexpr uint32_t hud_bank_bytes = 0x14000; // 640 entries; the banks end at 0x807F0000
    constexpr uint32_t hud_entry_max = 16; // commands; a texture rectangle needs 13
    constexpr uint32_t hud_entry_bytes = hud_entry_max * 8;
    constexpr uint32_t hud_entries = hud_bank_bytes / hud_entry_bytes;
    constexpr uint32_t hud_band_bottom = 40 * 4; // 10.2
    constexpr uint32_t hud_element_bottom = 80 * 4;
    constexpr int32_t hud_margin = 16 * 4;
    constexpr int32_t right_origin_shift = -320 * 4; // RT64 adds the width to right-anchored coordinates
    constexpr uint32_t origin_left = 0x0, origin_right = 0x400, origin_none = 0x800;
    bool in_hud = false;
    // The front-end menus draw with the same projection, and the OPT. menu's
    // top row is in the band: no anchoring there.
    bool front_end = false;
    uint32_t hud_bank = 0;
    uint32_t hud_entry = 0;

    bool in_hud_area(uint32_t addr) {
        return addr >= hud_area && addr < hud_area + hud_banks * hud_bank_bytes;
    }

    // The edge an element (bounds in 10.2) is anchored to, or false to leave
    // it centred. It must start in the top band and end near it (the
    // tutorial's box around the life gauge ends at about y 50). The side goes
    // by where it starts or ends when that's clear of the middle (a shrinking
    // bar's centre moves), otherwise by its centre.
    bool hud_origin(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry, uint32_t& origin, int32_t& offset) {
        if (uly >= hud_band_bottom || lry >= hud_element_bottom) {
            return false;
        }
        uint32_t centre = (ulx + lrx) / 2;
        if (ulx >= 176 * 4 || centre > 192 * 4) {
            origin = origin_right;
            offset = right_origin_shift + hud_margin;
            return true;
        }
        if (lrx <= 144 * 4 || centre < 128 * 4) {
            origin = origin_left;
            offset = -hud_margin;
            return true;
        }
        return false;
    }

    // addr: a G_TEXRECT (3 commands) or G_FILLRECT (1 command).
    void anchor_rect(uint8_t* rdram, uint32_t addr, uint32_t words, uint32_t origin, int32_t offset) {
        if (hud_entry >= hud_entries) {
            return;
        }
        uint32_t entry = hud_area + hud_bank * hud_bank_bytes + hud_entry * hud_entry_bytes;
        hud_entry++;
        uint32_t commands[hud_entry_max][2];
        uint32_t n = 0;
        auto cmd = [&](uint32_t a, uint32_t b) { commands[n][0] = a; commands[n][1] = b; n++; };
        cmd(0xE0525464, 0x10000064); // gEXEnable
        cmd(0x64000017, 0);          // gEXPushScissor
        cmd(0x64000005, (origin_left << 2) | (origin_right << 14)); // gEXSetScissor: the whole window
        cmd(0, 240 * 4);
        cmd(0x64000006, origin | (origin << 12)); // gEXSetRectAlign
        cmd((uint32_t)(uint16_t)offset << 16, (uint32_t)(uint16_t)offset << 16);
        for (uint32_t i = 0; i < words; i++) {
            cmd((uint32_t)MEM_W(0, (gpr)(int32_t)(addr + i * 8)), (uint32_t)MEM_W(4, (gpr)(int32_t)(addr + i * 8)));
        }
        cmd(0x64000006, origin_none | (origin_none << 12)); // back to unanchored
        cmd(0, 0);
        cmd(0x64000018, 0);          // gEXPopScissor
        cmd(0xDF000000, 0);
        for (uint32_t i = 0; i < n; i++) {
            set_command(rdram, entry + i * 8, commands[i][0], commands[i][1]);
        }
        set_command(rdram, addr, 0xDE000000, entry);
        for (uint32_t i = 1; i < words; i++) {
            set_command(rdram, addr + i * 8, G_NOOP << 24, 0);
        }
    }

    // A prefix that clears only the strips beside the 4:3 picture (where the
    // N64 shows nothing) to fill (black, or a fog colour), then branches to
    // the game's list: in Expand the scenery doesn't always reach past the
    // 4:3 edge (gaps in a far skyline), and the game never clears its
    // framebuffer, so the strips kept old frames. The 4:3 picture is left as
    // the game draws it: rows the scenery misses there keep the last frames
    // as on the N64 (the grass's gaps at the bottom), and the motion blur
    // draws over the last frame.
    uint32_t side_clear_prefix(uint8_t* rdram, uint32_t dl, uint32_t fill) {
        uint32_t prefix = prefix_area + prefix_slot * prefix_bytes;
        prefix_slot = (prefix_slot + 1) % prefix_slots;
        uint32_t commands[prefix_commands][2];
        uint32_t n = 0;
        auto cmd = [&](uint32_t a, uint32_t b) { commands[n][0] = a; commands[n][1] = b; n++; };
        // Each strip's width in 4:3 units (RT64 widens the picture by the
        // window's aspect over 4:3), and a pixel more.
        uint32_t strip = std::min<uint32_t>((uint32_t)(160.0f * (snp::get_widescreen_scale() - 1.0f) * 4.0f) + 4, 320 * 4);
        cmd(0xE0525464, 0x10000064); // gEXEnable
        cmd(0xE3000A01, 0x00300000); // cycle type: fill
        cmd(0xF7000000, fill);
        cmd(0x64000017, 0);          // gEXPushScissor
        cmd(0x64000005, (origin_left << 2) | (origin_right << 14)); // gEXSetScissor: the whole window
        cmd(0, 240 * 4);
        for (uint32_t i = 0; i < cimg_count; i++) {
            if (read_before_drawn(cimgs[i][1])) {
                continue;
            }
            cmd(cimgs[i][0], cimgs[i][1]);
            cmd(0x64000006, origin_left | (origin_left << 12)); // gEXSetRectAlign: from the left edge
            cmd(0, 0);
            cmd((G_FILLRECT << 24) | ((strip & 0xFFF) << 12) | (239 * 4), 0);
            cmd(0x64000006, origin_right | (origin_right << 12)); // from the right edge
            cmd((uint32_t)(uint16_t)right_origin_shift << 16, (uint32_t)(uint16_t)right_origin_shift << 16);
            cmd((G_FILLRECT << 24) | ((319 * 4) << 12) | (239 * 4), ((320 * 4 - strip) & 0xFFF) << 12);
        }
        cmd(0x64000006, origin_none | (origin_none << 12)); // back to unanchored
        cmd(0, 0);
        cmd(0x64000018, 0);          // gEXPopScissor
        cmd(cimgs[0][0], cimgs[0][1]); // the game's list starts on the first
        while (n < prefix_commands - 1) {
            cmd(G_NOOP << 24, 0);
        }
        cmd(0xDE010000, dl); // branch to the game's display list
        for (uint32_t i = 0; i < n; i++) {
            set_command(rdram, prefix + i * 8, commands[i][0], commands[i][1]);
        }
        return prefix;
    }

    // A HUD element is a run of rectangles drawn back to back (only state
    // changes between them) that touch, directly or through others: a string's
    // glyphs, the life bar's frame and fill. Each is anchored as a whole by its
    // bounds. The life bar's highlight is drawn in 1-pixel columns, so runs
    // reach about 85 rectangles; a run that fills up is anchored in parts.
    constexpr uint32_t max_fill_run = 256;
    struct HudRect {
        uint32_t addr, words, ulx, uly, lrx, lry;
    };
    HudRect fill_run[max_fill_run];
    uint32_t fill_run_count = 0;

    // The crosshair's vertical line when the reticle is past the 4:3 picture
    // (Expand): beyond x 320 the game's scissor cuts it off, and left of 0 its
    // 12-bit x wraps to near 1024. It's drawn with the whole window as the
    // scissor, a wrapped one moved back by 1024.
    void place_crosshair_line(uint8_t* rdram, const HudRect& r) {
        if (r.words != 1 || r.lry - r.uly < 200 * 4 || r.lrx - r.ulx > 8 * 4) {
            return;
        }
        if (r.ulx >= 640 * 4) {
            anchor_rect(rdram, r.addr, 1, origin_none, -1024 * 4);
        }
        else if (r.lrx > 320 * 4) {
            anchor_rect(rdram, r.addr, 1, origin_none, 0);
        }
    }

    // A box drawn as bars around a HUD element (the tutorial's flashing
    // frame, zooming in onto the LIFE gauge): two horizontal bars and two
    // vertical ones meeting at their ends, starting in the top band. Too big
    // while it zooms to be one element, each bar was placed on its own and
    // the box came apart. The whole box is moved as one, by part of its
    // side's anchoring shift: none while it's screen-sized and in the middle,
    // all of it once it's down to the element's size at the side (64 rows),
    // so it ends around the element without a jump on the way.
    // framed: set for the run's rectangles that are the box's. False if none.
    bool place_hud_box(uint8_t* rdram, uint32_t n, bool* framed) {
        constexpr uint32_t thin = 6 * 4, long_bar = 16 * 4, slack = 4 * 4;
        auto horizontal = [&](const HudRect& r) { return r.lry - r.uly <= thin && r.lrx - r.ulx >= long_bar; };
        auto vertical = [&](const HudRect& r) { return r.lrx - r.ulx <= thin && r.lry - r.uly >= long_bar; };
        auto near = [&](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= slack; };
        uint32_t L = 0, T = 0, R = 0, B = 0;
        bool found = false;
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = 0; j < n; j++) {
                const HudRect& top = fill_run[i];
                const HudRect& bottom = fill_run[j];
                if (!horizontal(top) || !horizontal(bottom) || bottom.uly < top.lry + long_bar ||
                    !near(top.ulx, bottom.ulx) || !near(top.lrx, bottom.lrx) || top.uly >= hud_band_bottom) {
                    continue;
                }
                uint32_t l = std::min(top.ulx, bottom.ulx), r = std::max(top.lrx, bottom.lrx);
                bool left_side = false, right_side = false;
                for (uint32_t k = 0; k < n; k++) {
                    const HudRect& v = fill_run[k];
                    if (vertical(v) && v.uly <= top.uly + thin && v.lry + thin >= bottom.lry) {
                        left_side = left_side || near(v.ulx, l);
                        right_side = right_side || near(v.lrx, r);
                    }
                }
                if (left_side && right_side && (!found || bottom.lry - top.uly > B - T)) {
                    found = true;
                    L = l;
                    R = r;
                    T = top.uly;
                    B = bottom.lry;
                }
            }
        }
        if (!found) {
            return false;
        }
        const float extra = 160.0f * (snp::get_widescreen_scale() - 1.0f) * 4.0f + hud_margin;
        const float size = std::clamp((240.0f * 4 - (B - T)) / ((240.0f - 64.0f) * 4), 0.0f, 1.0f);
        const float side = std::clamp(((L + R) / 2.0f - 160.0f * 4) / (64.0f * 4), -1.0f, 1.0f);
        const int32_t offset = (int32_t)std::lround(extra * size * side);
        for (uint32_t i = 0; i < n; i++) {
            const HudRect& r = fill_run[i];
            framed[i] = r.ulx + slack >= L && r.lrx <= R + slack && r.uly + slack >= T && r.lry <= B + slack &&
                (r.lry - r.uly <= thin || r.lrx - r.ulx <= thin);
            if (framed[i]) {
                anchor_rect(rdram, r.addr, r.words, origin_none, offset);
            }
        }
        return true;
    }

    void end_fill_run(uint8_t* rdram) {
        uint32_t n = fill_run_count;
        fill_run_count = 0;
        uint32_t shape[max_fill_run];
        bool framed[max_fill_run] = {};
        place_hud_box(rdram, n, framed);
        for (uint32_t i = 0; i < n; i++) {
            shape[i] = i;
        }
        auto find = [&](uint32_t i) { while (shape[i] != i) i = shape[i] = shape[shape[i]]; return i; };
        constexpr uint32_t touch = 1 * 4;
        // The crosshair's full-height and full-width lines join nothing, or
        // crossing the HUD they'd make it one element too big to anchor.
        auto joins = [&](const HudRect& r) {
            return r.uly < hud_band_bottom && r.lry < hud_element_bottom && r.lrx - r.ulx < 240 * 4;
        };
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
                const HudRect& a = fill_run[i];
                const HudRect& b = fill_run[j];
                if (!framed[i] && !framed[j] && joins(a) && joins(b) && a.ulx <= b.lrx + touch && b.ulx <= a.lrx + touch && a.uly <= b.lry + touch && b.uly <= a.lry + touch) {
                    shape[find(i)] = find(j);
                }
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!framed[i]) {
                place_crosshair_line(rdram, fill_run[i]);
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (framed[i] || find(i) != i) {
                continue;
            }
            uint32_t ulx = 0xFFF, uly = 0xFFF, lrx = 0, lry = 0;
            for (uint32_t j = 0; j < n; j++) {
                if (find(j) == i) {
                    ulx = std::min(ulx, fill_run[j].ulx);
                    uly = std::min(uly, fill_run[j].uly);
                    lrx = std::max(lrx, fill_run[j].lrx);
                    lry = std::max(lry, fill_run[j].lry);
                }
            }
            uint32_t origin;
            int32_t offset;
            if (!hud_origin(ulx, uly, lrx, lry, origin, offset)) {
                continue;
            }
            for (uint32_t j = 0; j < n; j++) {
                if (find(j) == i) {
                    anchor_rect(rdram, fill_run[j].addr, fill_run[j].words, origin, offset);
                }
            }
        }
    }

    void add_hud_rect(uint8_t* rdram, uint32_t addr, uint32_t w0, uint32_t w1, uint32_t words) {
        if (fill_run_count >= max_fill_run) {
            end_fill_run(rdram);
        }
        fill_run[fill_run_count++] = { addr, words, (w1 >> 12) & 0xFFF, w1 & 0xFFF, (w0 >> 12) & 0xFFF, w0 & 0xFFF };
    }

    // Commands that may come between an element's rectangles.
    bool is_fill_run_state(uint32_t op) {
        switch (op) {
            case 0xE2: case 0xE3: case 0xE6: case 0xE7: case 0xE8: // othermode, syncs
            case 0xF7: case 0xFA: case 0xFB: case 0xFC:              // colours, combine
            case 0xFD: case 0xF5: case 0xF4: case 0xF3: case 0xF2: case 0xF0: // textures, tiles
            case 0xE1: case 0xF1:                                   // texture rectangle halves
                return true;
            default:
                return false;
        }
    }

    void log_command(int depth, uint8_t* rdram, uint32_t w0, uint32_t w1) {
        auto rect = [&](const char* what, uint32_t ul, uint32_t lr) {
            printf("[DL] %*s%s %.2f,%.2f - %.2f,%.2f\n", depth, "", what,
                ((ul >> 12) & 0xFFF) / 4.0, (ul & 0xFFF) / 4.0, ((lr >> 12) & 0xFFF) / 4.0, (lr & 0xFFF) / 4.0);
        };
        switch (w0 >> 24) {
            case 0xDC:
                if ((w0 & 0xFF) == 8) { // G_MV_VIEWPORT
                    uint32_t vp = resolve(w1);
                    int32_t scale = MEM_W(0, (gpr)(int32_t)vp), trans = MEM_W(8, (gpr)(int32_t)vp);
                    printf("[DL] %*sviewport scale %.2f,%.2f trans %.2f,%.2f\n", depth, "",
                        (int16_t)(scale >> 16) / 4.0, (int16_t)scale / 4.0, (int16_t)(trans >> 16) / 4.0, (int16_t)trans / 4.0);
                }
                break;
            case 0xFF: printf("[DL] %*scolor image %08X width %u\n", depth, "", w1, (w0 & 0xFFF) + 1); break;
            case 0xED: rect("scissor", w0, w1); break;
            case 0xF7: printf("[DL] %*sfill color %08X\n", depth, "", w1); break;
            case 0xF6: rect("fill rect", w1, w0); break;
            case 0xE4: rect("tex rect", w1, w0); break;
            case 0xFC: printf("[DL] %*scombine %08X %08X\n", depth, "", w0, w1); break;
            case 0xDE: printf("[DL] %*sdisplay list %08X %08X\n", depth, "", w0, w1); break;
            case 0xFD: printf("[DL] %*stexture image %08X fmt %u siz %u width %u\n", depth, "", w1, (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 & 0xFFF) + 1); break;
            case 0xF5: printf("[DL] %*ssettile %08X %08X\n", depth, "", w0, w1); break;
            case 0xF4: printf("[DL] %*sloadtile %08X %08X\n", depth, "", w0, w1); break;
            case 0x64: printf("[DL] %*sextended %08X %08X\n", depth, "", w0, w1); break;
            case 0xE2: printf("[DL] %*sothermode_L %08X %08X\n", depth, "", w0, w1); break;
            case 0xDB: printf("[DL] %*smoveword %08X %08X\n", depth, "", w0, w1); break;
            case 0xE3: printf("[DL] %*sothermode_H %08X %08X\n", depth, "", w0, w1); break;
            case 0xFA: printf("[DL] %*sprim color %08X\n", depth, "", w1); break;
            case 0xFB: printf("[DL] %*senv color %08X\n", depth, "", w1); break;
            case 0x01: {
                printf("[DL] %*svertices %08X %08X\n", depth, "", w0, w1);
                uint32_t count = (w0 >> 12) & 0xFF;
                if (count <= 4) {
                    uint32_t v = resolve(w1);
                    for (uint32_t i = 0; i < count; i++, v += 16) {
                        uint32_t a = (uint32_t)MEM_W(0, (gpr)(int32_t)v), b = (uint32_t)MEM_W(4, (gpr)(int32_t)v), c = (uint32_t)MEM_W(12, (gpr)(int32_t)v);
                        printf("[DL] %*s  v %d %d %d rgba %08X\n", depth, "", (int16_t)(a >> 16), (int16_t)a, (int16_t)(b >> 16), c);
                    }
                }
                break;
            }
            case 0x05: case 0x06: printf("[DL] %*striangles %08X %08X\n", depth, "", w0, w1); break;
            case 0xDA: {
                printf("[DL] %*smatrix %08X %08X\n", depth, "", w0, w1);
                uint32_t m = resolve(w1);
                printf("[DL] %*s  m", depth, "");
                for (int i = 0; i < 16; i++) {
                    int32_t hi = (int16_t)((uint32_t)MEM_W((i / 2) * 4, (gpr)(int32_t)m) >> ((i & 1) ? 0 : 16));
                    uint32_t lo = (uint16_t)((uint32_t)MEM_W(32 + (i / 2) * 4, (gpr)(int32_t)m) >> ((i & 1) ? 0 : 16));
                    printf(" %.4f", (int32_t)(((uint32_t)hi << 16) | lo) / 65536.0);
                }
                printf("\n");
                break;
            }
            default: break;
        }
    }

    // top: the list is the frame's own, or one it branched to (its end ends the frame).
    void walk(uint8_t* rdram, uint32_t addr, int depth, bool widen, bool top) {
        for (int n = 0; n < 200000; n++, addr += 8) {
            uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
            uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
            if ((w0 >> 24) == 0xDE && in_hud_area(w1)) {
                // A HUD rectangle anchored in an earlier frame (a list the game
                // keeps): back to the game's command(s), anchored again below.
                // They're at entry + 6 commands (anchor_rect).
                uint32_t entry = w1 + 6 * 8;
                uint32_t words = ((uint32_t)MEM_W(0, (gpr)(int32_t)entry) >> 24) == 0xE4 ? 3 : 1;
                for (uint32_t i = 0; i < words; i++) {
                    set_command(rdram, addr + i * 8, (uint32_t)MEM_W(0, (gpr)(int32_t)(entry + i * 8)), (uint32_t)MEM_W(4, (gpr)(int32_t)(entry + i * 8)));
                }
                w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
                w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
            }
            if ((w0 >> 24) == 0xDE && in_blur_area(w1)) {
                // A blur strip extended in an earlier frame: back to the
                // game's commands, extended again below.
                uint32_t words = (uint32_t)MEM_W(4, (gpr)(int32_t)w1);
                uint32_t at = w1 + (uint32_t)MEM_W(12, (gpr)(int32_t)w1) * 8;
                for (uint32_t i = 0; i < words && i < blur_strip_max; i++) {
                    set_command(rdram, addr + i * 8, (uint32_t)MEM_W(0, (gpr)(int32_t)(at + i * 8)), (uint32_t)MEM_W(4, (gpr)(int32_t)(at + i * 8)));
                }
                w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
                w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
            }
            if ((w0 >> 24) == 0xDE && in_live_area(w1)) {
                // A crosshair rectangle drawn live in an earlier frame: back to
                // the game's command, drawn live again below.
                w0 = (uint32_t)MEM_W(live_kept_command, (gpr)(int32_t)w1);
                w1 = (uint32_t)MEM_W(live_kept_command + 4, (gpr)(int32_t)w1);
                set_command(rdram, addr, w0, w1);
            }
            if ((w0 >> 24) == 0xDE && in_overlay_area(w1)) {
                // A full sync turned into a mod's overlay in an earlier frame (a
                // list the game keeps): the full sync again, and the overlay
                // added again below if it's still wanted.
                set_command(rdram, addr, w0 = 0xE9000000, w1 = 0);
            }
            if ((w0 >> 24) == 0xDE && in_interp_area(w1)) {
                // A matrix load tagged in an earlier frame (a list the game
                // keeps): back to the game's command, tagged again below.
                uint32_t entry = w1;
                w0 = (uint32_t)MEM_W(3 * 8, (gpr)(int32_t)entry);
                w1 = (uint32_t)MEM_W(3 * 8 + 4, (gpr)(int32_t)entry);
                auto original = tagged_loads.find(addr);
                if (original != tagged_loads.end()) {
                    if (original->second.first != w0 || original->second.second != w1) {
                        interp_reused_entries++;
                    }
                    w0 = original->second.first;
                    w1 = original->second.second;
                }
                set_command(rdram, addr, w0, w1);
            }
            if (widen) {
                if (w0 == inset_scissor_w0 && w1 == inset_scissor_w1) {
                    set_command(rdram, addr, w0 = full_scissor_w0, w1 = full_scissor_w1);
                }
                else if ((w0 >> 24) == G_FILLRECT && ((w0 >> 12) & 0xFFF) == 307 * 4 && ((w1 >> 12) & 0xFFF) == 12 * 4) {
                    // A fill rectangle across the inset's full width (the clear, fades
                    // and tints, a backdrop band): full screen width.
                    w0 = (w0 & 0xFF000FFF) | ((319 * 4) << 12);
                    w1 = (w1 & 0xFF000FFF) | (0 << 12);
                    // Covering the inset's full height too: the full screen height,
                    // or the rows above and below the inset, only ever covered by
                    // the border, keep old frames.
                    if ((w0 & 0xFFF) == 231 * 4 && (w1 & 0xFFF) == 8 * 4) {
                        w0 = (w0 & 0xFFFFF000) | (239 * 4);
                        w1 = (w1 & 0xFFFFF000) | 0;
                    }
                    // A band from the inset's top or reaching near the bottom
                    // is a backdrop colour drawn under the scenery (stage 1's
                    // grass, 2-2's sea); the scenery doesn't reach the corners
                    // of a wider view, and the rows the border covered were
                    // left black there: to the top or bottom row.
                    else {
                        if ((w1 & 0xFFF) == 8 * 4) {
                            w1 = (w1 & 0xFFFFF000) | 0;
                        }
                        if ((w0 & 0xFFF) >= ground_band_bottom) {
                            w0 = (w0 & 0xFFFFF000) | (239 * 4);
                        }
                    }
                    set_command(rdram, addr, w0, w1);
                }
                else if ((w0 >> 24) == 0xFD && seen_draw && !starts_with_picture && !in_blur_area(addr) &&
                    extend_blur_strip(rdram, addr)) {
                    w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)addr);
                    w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)addr);
                }
                else if (w0 == effect_scissor_w0 && w1 == effect_scissor_w1 && seen_draw && !starts_with_picture) {
                    // The blur's scissor, without the border's rows.
                    set_command(rdram, addr, w0 = full_scissor_w0, w1 = full_scissor_w1);
                }
                else if ((w0 >> 24) == G_VTX) {
                    widen_screen_quad(rdram, addr, w0, w1);
                }
                else {
                    for (const auto& b : border) {
                        if (w0 == b[0] && w1 == b[1]) {
                            set_command(rdram, addr, w0 = G_NOOP << 24, w1 = 0);
                        }
                    }
                }
            }
            switch (w0 >> 24) {
                case 0xFF: // G_SETCIMG
                    if (first_cimg_w0 == 0) {
                        first_cimg_w0 = w0;
                        first_cimg_w1 = w1;
                    }
                    if ((w0 & 0xFFF) + 1 == 320) {
                        bool known = false;
                        for (uint32_t i = 0; i < cimg_count; i++) {
                            known = known || cimgs[i][1] == w1;
                        }
                        if (!known && cimg_count < max_cimgs) {
                            cimgs[cimg_count][0] = w0;
                            cimgs[cimg_count][1] = w1;
                            cimg_count++;
                        }
                    }
                    break;
                case 0xFD: { // G_SETTIMG: a read of an image not drawn to yet this frame?
                    uint32_t image = resolve(w1) & 0x1FFFFFFF;
                    bool drawn = false;
                    for (uint32_t i = 0; i < cimg_count; i++) {
                        uint32_t start = cimgs[i][1] & 0x1FFFFFFF;
                        drawn = drawn || (image >= start && image < start + cimg_bytes);
                    }
                    for (uint32_t i = 0; i < early_read_count && !drawn; i++) {
                        drawn = early_reads[i] == image; // already listed
                    }
                    if (!drawn && early_read_count < max_early_reads) {
                        early_reads[early_read_count++] = image;
                    }
                    break;
                }
                case 0xE4: // G_TEXRECT
                    if (!seen_draw) {
                        starts_with_picture = (w1 & 0x00FFFFFF) == 0;
                    }
                    seen_draw = true;
                    break;
                case 0xF8: // G_SETFOGCOLOR
                    fog_color = w1;
                    break;
                case 0x05: case 0x06: case 0x07: // G_TRI1, G_TRI2, G_QUAD
                    if (!seen_triangle) {
                        seen_triangle = true;
                        first_fog_color = fog_color;
                    }
                    seen_draw = true;
                    break;
                case 0xF6: // G_FILLRECT
                    seen_draw = true;
                    break;
                default:
                    break;
            }
            if (sets_texture_filter(w0)) {
                pending_filter = addr;
            }
            else if (pending_filter != 0) {
                const uint32_t op = w0 >> 24;
                const bool rect = op == 0xE4 || op == 0xE5;
                if (rect || op == 0x05 || op == 0x06 || op == 0x07) {
                    set_texture_filter(rdram, pending_filter, (rect || in_hud) ? sharp_hud : sharp_textures);
                    pending_filter = 0;
                }
            }
            if (log_frame) {
                log_command(depth, rdram, w0, w1);
            }
            if (fill_run_count > 0 && (w0 >> 24) != G_FILLRECT && (w0 >> 24) != 0xE4 && !is_fill_run_state(w0 >> 24)) {
                end_fill_run(rdram);
            }
            switch (w0 >> 24) {
                case 0xDE: { // G_DL
                    bool branch = ((w0 >> 16) & 0xFF) != 0; // no return
                    if (!branch && (addr < walls_area || addr >= walls_vertices) && is_warehouse(rdram, resolve(w1))) {
                        set_command(rdram, addr, w0, w1 = warehouse_with_side_walls(rdram, resolve(w1)));
                    }
                    if (depth < 10) {
                        walk(rdram, resolve(w1), depth + 1, widen, top && branch);
                    }
                    if (branch) {
                        return;
                    }
                    break;
                }
                case 0xDF: // G_ENDDL
                    return;
                case 0xE9: // G_RDPFULLSYNC: the end of the frame's drawing
                    if (top && widen && starts_with_picture && effect_area_scissor && first_cimg_w0 != 0 &&
                        !(addr >= suffix_area && addr < suffix_area + suffix_slots * suffix_bytes)) {
                        black_edge_rows(rdram, addr);
                        return;
                    }
                    if (top && hud_seen && !front_end && !in_overlay_area(addr)) {
                        uint32_t list = snp::mod_overlay_list();
                        if (list != 0) {
                            add_overlay(rdram, addr, list);
                            if (depth < 10) {
                                walk(rdram, list, depth + 1, widen, false);
                            }
                            return;
                        }
                    }
                    break;
                case 0xED: // G_SETSCISSOR
                    if (w0 == effect_scissor_w0 && w1 == effect_scissor_w1) {
                        effect_area_scissor = true;
                    }
                    break;
                case 0xDB: // G_MOVEWORD
                    if (((w0 >> 16) & 0xFF) == 6) { // G_MW_SEGMENT
                        segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
                    }
                    break;
                case 0xE4: // G_TEXRECT: two more words follow (RDPHALF_1, RDPHALF_2)
                    nudge_rect_start(rdram, addr);
                    if (widen && in_hud && !front_end) {
                        add_hud_rect(rdram, addr, w0, w1, 3);
                    }
                    addr += 16;
                    break;
                case 0xF6: // G_FILLRECT
                    if (in_hud && live_id != 0 && draw_crosshair_rect_live(rdram, addr, w0, w1)) {
                        break;
                    }
                    if (widen && in_hud && !front_end) {
                        add_hud_rect(rdram, addr, w0, w1, 1);
                    }
                    break;
                case 0xDA: // G_MTX
                    if ((((w0 & 0xFF) ^ 1) & 4) != 0) { // projection
                        in_hud = (w1 == hud_projection);
                        hud_seen = hud_seen || in_hud;
                    }
                    else { // modelview
                        modelview_load(rdram, w0, w1);
                        tag_matrix(rdram, addr, w0, w1, in_hud);
                    }
                    break;
                case 0xD8: // G_POPMTX: w1 / 64 matrices
                    modelview_top = std::max(0, modelview_top - (int)(w1 / 64));
                    break;
                default:
                    break;
            }
        }
    }
}

// task: the OSTask about to be loaded.
extern "C" void snp_display_list_hook(uint8_t* rdram, int32_t task) {
    if (MEM_W(0x0, task) != 1) { // only graphics tasks (M_GFXTASK)
        return;
    }
    static const char* diag = std::getenv("SNP_DL_DIAG");
    static int interval = [] { const char* v = std::getenv("SNP_DL_DIAG_INTERVAL"); return v ? std::atoi(v) : 300; }();
    static int frame = 0;
    log_frame = diag != nullptr && (frame++ % interval) == 0;

    uint32_t dl = (uint32_t)MEM_W(0x30, task); // OSTask.data_ptr
    if (is_prefix(dl)) { // a task loaded again: the game's list is the branch target
        dl = (uint32_t)MEM_W((prefix_commands - 1) * 8 + 4, (gpr)(int32_t)dl);
        MEM_W(0x30, task) = (int32_t)dl;
    }
    snp_extras_frame(rdram);
    sharp_textures = snp::config::get_sharp_textures();
    sharp_hud = snp::config::get_sharp_hud();
    if (live_id != 0 || hud_matrices != 0) {
        if (live_id != 0 && hud_nearest >= 0.0f) {
            snp::mouse::aim_log("list  reticle at %.2f %.2f: %u HUD matrices, %u with its live ID, nearest at %.2f %.2f (%.2f off)",
                live_x, live_y, hud_matrices, hud_live, hud_nearest_x, hud_nearest_y, hud_nearest);
        }
        else {
            snp::mouse::aim_log("list  %s: %u HUD matrices", live_id != 0 ? "reticle live but no HUD matrix" : "no live ID", hud_matrices);
        }
    }
    hud_matrices = 0;
    hud_live = 0;
    hud_nearest = -1.0f;
    live_id = snp_in_front_end_menus() ? 0 : snp::mouse::live_reticle_id(rdram, live_x, live_y);
    if (live_id != 0) {
        for (uint32_t i = live_recent_size - 1; i > 0; i--) {
            live_recent_x[i] = live_recent_x[i - 1];
            live_recent_y[i] = live_recent_y[i - 1];
        }
        live_recent_x[0] = live_x;
        live_recent_y[0] = live_y;
        live_recent_count = std::min(live_recent_count + 1, live_recent_size);
    }
    else {
        live_recent_count = 0;
    }
    live_bank = (live_bank + 1) % live_banks;
    live_entry = 0;
    if (live_id != 0) {
        // The identity matrix (s15.16: integer halves, then fractions).
        const uint32_t identity[16] = { 0x00010000, 0, 0x00000001, 0, 0, 0x00010000, 0, 0x00000001, 0, 0, 0, 0, 0, 0, 0, 0 };
        for (uint32_t i = 0; i < 16; i++) {
            MEM_W(i * 4, (gpr)(int32_t)live_identity) = (int32_t)identity[i];
        }
    }
    pending_filter = 0;
    bool widen = snp::get_widescreen_scale() > 1.0f;
    interp_bank = (interp_bank + 1) % interp_banks;
    interp_entry = 0;
    snp::interp_new_frame();
    if (log_frame) {
        static const auto start = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        printf("[DL] ---- frame %d at %.1f s, display list %08X%s\n", frame - 1, t, dl, widen ? " (widened)" : "");
    }
    for (uint32_t& s : segments) {
        s = 0;
    }
    first_cimg_w0 = 0;
    cimg_count = 0;
    early_read_count = 0;
    seen_draw = false;
    starts_with_picture = false;
    effect_area_scissor = false;
    seen_triangle = false;
    fog_color = 0;
    first_fog_color = 0;
    in_hud = false;
    hud_seen = false;
    front_end = snp_in_front_end_menus();
    hud_bank = (hud_bank + 1) % hud_banks;
    blur_bank = (blur_bank + 1) % blur_banks;
    blur_entry = 0;
    hud_entry = 0;
    fill_run_count = 0;
    instance_tags.clear();
    modelview_top = 0;
    modelview[0] = identity_matrix();
    walk(rdram, dl, 0, widen, true);
    end_fill_run(rdram);
    number_tagged_instances(rdram);
    // After switching from Expand back to Original, the strips outside the
    // inset keep the wide picture (the game never clears them): the first
    // frames clear every color image they draw to, enough for all three
    // framebuffers.
    static bool was_widened = false;
    static int clear_after_switch = 0;
    if (was_widened && !widen) {
        clear_after_switch = 6;
    }
    was_widened = widen;
    bool clear_all = clear_after_switch > 0;
    if (clear_all) {
        clear_after_switch--;
    }
    if (first_cimg_w0 != 0) {
        if ((widen && starts_with_picture) || clear_all) {
            MEM_W(0x30, task) = (int32_t)clear_prefix(rdram, dl, clear_all);
        }
        else if (widen && cimg_count > 0) {
            // The fog colour the frame's scenery starts with (black if none):
            // the far scenery fades into it, and it shows where the scenery
            // doesn't reach the strips (past the edge of 2-2's sea in the
            // corners of a rolled wide view, between its clouds and its sky
            // band).
            uint32_t fill = 0x00010001;
            if ((first_fog_color & 0xFFFFFF00) != 0) {
                uint32_t r = first_fog_color >> 24, g = (first_fog_color >> 16) & 0xFF, b = (first_fog_color >> 8) & 0xFF;
                uint32_t c = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
                fill = (c << 16) | c;
            }
            MEM_W(0x30, task) = (int32_t)side_clear_prefix(rdram, dl, fill);
        }
    }
    if (log_frame) {
        fflush(stdout);
    }
}
