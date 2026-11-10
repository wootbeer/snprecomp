# Status log

How each piece of the port was found and fixed, newest last. PROGRESS.md is
the short version.

## Round 1: ROM analysis, symbol file, first recompile, project setup

### The two ROMs

The Japanese ROM and the "(U) [!]" English ROM are 32 MiB each and 62.9%
identical byte for byte. The English one is a fan translation, not an
official build (details in `syms/rom_info.md`): a ROM tool's boot stub at
`0x1FFFF70`, the header changed to `NGUE`, and the text done by swapping
textures in the asset data, which moved everything after the changed files
up by about 155 KB.

All code segments are identical. Outside the entry point (which the boot
stub overwrote and the recompiled build doesn't use), the only code
differences are constants pointing at moved data:

| Function | Instructions | Japanese | English |
|---|---|---|---|
| `func_8002BCE0` | `0x8002BD5C`, `0x8002BD8C`, `0x8002BD98` | `0x802B5D20` | `0x802B5D60` |
| `func_8003C0D0` | lui/addiu at `0x8003C158`, `0x8003C198`, `0x8003C1EC` | `0x00A8CA40` | `0x00A65CF0` |

The second is a ROM offset, the end of the asset data. The asset table at
ROM `0x34DF4` is data, so the game reads the right one from whichever ROM is
loaded.

So both ROMs run one recompiled build, generated from the Japanese ROM.
Six `[[patches.hook]]` entries in `snp.jp.toml` put the English values in
the register after each of those instruction pairs when the English ROM is
running (`src/game/rom_version.cpp`, from the runtime's game id). Both ROMs
are registered in `src/main/main.cpp` with their own game id, so their saves
are separate.

### Memory layout

- IPL3 loads ROM `0x1000` to the entry point `0x80025C00`
  (`rom = vram - 0x80024C00`). The entry point clears BSS
  `0x80063450`-`0x800A7070`, sets the stack and jumps to `0x80025C40`, which
  calls `__osInitialize_common` and starts the first thread
  (`osCreateThread` `0x80047270`, `osStartThread` `0x80047510`).
- Main's text ends at `0x80056E80`. The last function is the Yay0 decoder at
  `0x80056DD0`, so some assets are Yay0-compressed. After it come rspboot
  (`0x80056E80`), the audio microcode (`0x80056F50`) and a graphics
  microcode (`0x80057BB0`), then data.
- The game's own load table, at ROM `0x34BAC` (VRAM `0x800597AC`), lists
  28 segments as {rom start, vram, text end, data end, bss end}: seg0 at
  `0x800A7070` (right after main's BSS), overlays 1-23 all at `0x800E4780`,
  24-27 at `0x802B4000` / `0x802C3B30` (24 has no code). The table ends at
  an entry with vram 0. `tools/segments.py` reads it.

### libultra and the other SDK libraries

[n64sym](https://github.com/shygoo/n64sym)'s built-in signatures name 226
functions in main (`syms/n64sym_jp.txt`): libultra (threads, message queues,
PI/SI/VI/AI managers, EEPROM, Controller Pak, Rumble Pak), libsched's
`osCreateScheduler`/`__sc*`, the `al*` and `n_al*` audio libraries, gu
matrix helpers and libgcc's 64-bit division. All but two land on a function
start spimdisasm found; `__osException`'s tail and `__osCleanupThread` were
added as starts. N64Recomp's built-in lists already cover all of the names
that need replacing or skipping, so `snp.jp.toml` doesn't list any.

The game saves to EEPROM (`osEepromLongRead`/`Write` are linked and
called). The game entries use `SaveType::Eep4k`; whether the game wants 4K
or 16K isn't confirmed yet, check what it does with `osEepromProbe`'s
result.

### Function boundaries

`tools/run_spimdisasm.py` runs spimdisasm's `singleFileDisasm` on each
segment's text (main, seg0, 26 overlays with code) and keeps its function
list (`syms/spimdisasm/*.csv`). `tools/gen_syms_toml.py` turns those and the
n64sym names into `SnPSyms/snp.jp.syms.toml`: 6278 functions, one
`[[section]]` per segment. Overlay functions are named `func_ovlNN_XXXXXXXX`
since overlays share addresses.

N64Recomp can't branch into another function's interior. Hand-written
code does that, for shared tails: `__osException` (its interrupt dispatch
tail was a separate function, kept as one piece by an override) and libgcc's
float conversions at `0x8004BDD0`-`0x8004C2F8`. The generator fixes these:
it splits a function at the target when the code before it never falls
through, compiles a self-contained tail (code up to its first unconditional
exit, with no branches leaving it) as its own small function so branches to
it become tail calls, and otherwise merges the two functions. An entry
point that disappears into a merged function but is called from elsewhere
stays a function of its own, overlapping the merged one and running to its
end; the only one is `func_8004BE70` (called 288 times), whose branch back
to `0x8004BE30` goes to such a tail.

(N64Recomp's `manual_funcs` would be the usual way to add an overlapping
function, but N64Recomp only reads it for ELF input, not with a symbol
file.)

With that, all 6278 functions recompile with no errors. No `jal` between
overlays was ambiguous; indirect calls into overlays use the runtime's
function lookup, which learns an overlay's functions when the game DMAs it
in (`load_overlays`, called by the runtime's `osPiStartDma`).

### Audio microcode

The audio task setup at `func_80052064` passes ucode text `0x80056F50` and
data `0x80062590`. It's the same `n_aspMain` build as BattleTanx: Global
Assault's: the same 0xC60 size and an identical command dispatch table. So
`n_aspMain.jp.toml` is btgarecomp's config with this ROM's offset
(`0x32350`), and RSPRecomp builds `rsp/n_aspMain.cpp` from it.

### Project setup

Copied from btgarecomp and renamed (`snp_`): the CMake build, the patch
pipeline (`patches/`, `patches.toml`), the frontend (`src/main/`: window,
audio, input, config, crash reporting), the UI assets and the submodules at
the same commits, with btgarecomp's RT64 fixes in `lib-patches/` (Expand
view-projection, matching and interpolation fixes). Left out until this
game needs them: BattleTanx's Controller Pak emulation, multiplayer, and its
game-specific timing, widescreen and scheduler fixes.

- `src/main/main.cpp`: two game entries (Japanese and English), EEPROM
  saves, a Rumble Pak option on the General tab, port 1 only for now.
- `patches/recompui_patches.c`: replaces `func_80046B20`, libsched's
  `__scTaskComplete`, with the same logic plus the frontend's per-frame UI
  pump before the frame swap. It's the game's only per-frame
  `osViSwapBuffer` call.
- `src/game/widescreen.cpp`: the Expand scale from the frontend, for the
  widescreen fixes to come.
- UI theme: the frontend's blue with red and blue swapped (orange).

### Known risks for the first run

- The runtime starts the game by copying 1 MB from ROM `0x1000` to the entry
  point and registering every section in that ROM range
  (`librecomp/src/recomp.cpp`, `load_overlays(0x1000, entrypoint, 1MB)`).
  Here that range also holds seg0 and overlays 1-4, which get registered at
  the wrong addresses (as if loaded right after main). The game's own
  loads register them correctly later, but a later unload that partly
  overlaps one of the wrong entries is fatal ("Cannot partially unload
  section"). If that shows up, limit the startup load to main's size.
- If the game DMAs an overlay in pieces rather than in one go, the runtime
  won't register it (`load_overlays` matches whole sections). The game's
  segment loader would then need a hook.
- The cooperative scheduler issues btgarecomp hit (busy-wait loops waiting on
  another thread) may show up here too; its `scheduler_workaround.cpp` is
  the starting point.

## Round 2: first boots

Run headless for testing: Xvfb with Mesa's software Vulkan (lavapipe), the
ROMs stored in a portable config folder as `<game id>.z64`, started with
`--game snp`. (`--game` matches the mod game id, which both ROMs share, so
it starts whichever registered entry comes first; both ROMs are valid
here.) Thread stacks come from gdb attached to the running game.

### Link: SDK functions n64sym missed

Four SDK functions without an n64sym match were recompiled from the ROM,
and their calls into functions N64Recomp replaces didn't link. Named from
what they call (`EXTRA_NAMES` in `tools/gen_syms_toml.py`) so N64Recomp
replaces or skips them too: `osEPiStartDma` (`0x8003EBB0`), `osContInit`
(`0x800433B0`), `viMgrMain` (`0x80048110`), `__osRepairPackId`
(`0x8004ABF8`).

### Missing functions

- `0x80025C40`: the entry stub ends in `jr $t2` to it, and spimdisasm had
  merged the two. Split by an override.
- `0x8004E640`, a thread entry point only reached through a pointer.
  `tools/gen_syms_toml.py` now starts a function at every address the code
  builds with lui/addiu or stores in a segment's data, when it begins a
  stack frame and the code before it never falls through. The first
  version, without the stack-frame rule, split jump tables (their case
  labels are code addresses in data too) and addresses made up by a `lui`
  and an unrelated `addiu`; N64Recomp then failed on the switch statements.
  That adds 8 functions.

### Busy-wait loops

With every function present, the game started its threads and then
stalled: the main thread waited for a retrace message that never came.
The idle thread (`func_80025CA4`, priority 0) ends in a loop polling
`D_800657B0` for a callback, with no OS call. On the N64 an interrupt
preempts it; the runtime only switches threads inside OS calls, and only
delivers VI and other events when a game thread waits for one, so the
spinning idle thread kept everything else waiting.

Two more loops do the same: the main thread polls `D_80068A95`
(`func_80025E44`) and `func_8004D7B0` waits for the job thread
(`func_8004D7C8`) to finish its jobs (`D_8007AB94` counts them).
`tools/find_spin_loops.py` lists short loops that only load a fixed
address and branch; besides these three it finds only hardware register
polls inside SDK functions the runtime replaces.

Each loop head gets a hook calling `snp_spin_yield`
(`src/game/spin_wait.cpp`): wait up to 1 ms for an event, deliver every
pending one, switch to any higher-priority thread that's ready, then drop
the thread to priority 0 and back so lower-priority ready threads (the job
thread, for the main thread) get to run too. The priority drop does nothing
for the idle thread, already at 0: `osSetThreadPri` returns early when the
priority doesn't change, so the explicit switch is needed (the first
version without it stalled again at the same wait).

### Segment loading

The game's segment loader `func_8003A5F0` (called by `func_8003A1D0` with
a load table entry) copies a segment in 0x1000-byte `osPiStartDma`
chunks. The runtime registers a code section only when one DMA covers all
of it, so most overlays would never be registered. A hook after the copy
loop (`src/game/overlay_loader.cpp`) registers the segment once it's in
place, after unloading every section in the span any overlay at that
address covers (overlays sharing an address differ in size; a partial
unload is fatal). On the first load it also drops the sections the
runtime's startup registered at wrong addresses (round 1's first risk).

`osAiGetLength` (`0x8003F350`, between `__osAiDeviceBusy` and
`osAiGetStatus`) was the next unnamed SDK function: recompiled, it read the
AI length register and crashed when audio started. Named too. A scan for
recompiled functions that build hardware register addresses turns up only
`func_8004BA30` (PI registers, only called from `__osInitialize_common`,
which the runtime replaces) and float constants (`lui 0xBF80` is -1.0f).

### Result: the attract mode runs

With these, both ROMs boot into the attract mode: the intro cutscenes in
3D with "PRESS START BUTTON" and subtitles, English with the fan
translation and Japanese with the Japanese ROM. Not checked yet: sound (no
audio device headless), input, the title menu and gameplay.

Some cutscene shots already draw wider than 4:3 on their own (the side
areas in a few shots); to look at with widescreen.

### `--game`

Both ROMs share the mod game id `snp`, so mods apply to either. The
runtime's `--game` option matched only the mod game id and stopped at the
first match, even one without a ROM. A runtime patch
(`lib-patches/N64ModernRuntime/`, applied at configure time like the RT64
fixes) makes it take the first match with a valid ROM and accept a full
game id too: `--game snp.n64.jp.1.0` or `--game snp.n64.en.fan`.

## Round 3: menus and the first stage

Input works headless too (`xdotool` keys into the Xvfb window; the
frontend's keyboard defaults: Return is Start, WASD the stick, Q is Z).
Start on the attract mode gives the title screen and the main menu, which
is driven with the aiming cursor (stick to move, Z to select).

### Division by zero

Selecting START GAME crashed with a host floating point exception in
`func_800A9954`: an unsigned divide by zero (`divu` at `0x800A99B0`,
`0x8000 / x`). The game was compiled without zero-division checks: of its
1111 `div`/`divu`, only 9 are followed by a `break`. On the VR4300 a
division by zero doesn't trap: the quotient is -1 (1 for a negative signed
dividend) and the remainder is the dividend. N64Recomp emitted a plain C
division, which traps on the host.

btgarecomp guarded the few divisions that hit this with per-site hooks;
with 1111 here, N64Recomp itself is fixed instead
(`lib-patches/N64Recomp/`): `div`/`divu` are emitted as `DIV32`/`DIVU32`,
and `DDIV`/`DDIVU` handle zero too, all giving the VR4300's results. Since
it changes the generated code, `RecompiledFuncs/` now has to be generated
with the N64Recomp the main build makes (with the patch applied), and the
build regenerates it when an N64Recomp patch changes. BUILDING.md's steps
changed accordingly.

### Result: the first stage plays

START GAME now runs stage 1: the player, enemies, the HUD (score, time,
life) and the aiming crosshair; shooting scores. The crosshair's two red
guide lines reach past the 4:3 frame into the window's side areas (to look
at with widescreen).

## Round 4: the launcher takes either ROM

First Windows test: the English ROM was refused with "This ROM is not the
correct game". The frontend's default launcher only offers the first
registered game (`supported_games[0]`, the Japanese ROM) and checks a
loaded ROM against that entry's hash alone; the English entry was never
offered. `src/main/launcher.cpp` replaces it: Load ROM tries the file
against each supported game and stores it under the one it matches, and
there's a start option per loaded ROM ("Start Game" with one, "Start Game
(English)" / "Start Game (Japanese)" with both; Load Another ROM goes away
once both are loaded). Controls, Settings, Mods and Exit are the
frontend's own.

Still to check: with the keyboard, the highlight starts on Controls rather
than the start option.

## Round 5: widescreen (Expand) part 1; the real frame swap; green theme

Windows reports: in Expand, old frames and bright red stripes showed beside
the picture in cutscenes, the title screen and gameplay; the crosshair has
thin lines across the screen; models jitter in cutscenes (interpolation,
see Next).

### What the game draws

A display list diagnostic (`src/game/display_list.cpp`, `SNP_DL_DIAG=1`)
walks each frame's graphics task and logs color images, scissors,
viewports, fill and texture rectangles and render modes. It's hooked right
before the game's task threads (`func_8004E640`, `func_8004E4A0`) call
`osSpTaskLoad`, with the `OSTask` in `$a0`.

- Every frame is drawn inside an inset: scissor 12,8 - 308,231, cleared
  only there (fill 12,8 - 307,231). At the end the game opens the scissor
  to the full screen and draws a black border around the inset (four fill
  rectangles). The viewport is the full screen.
- RT64 widens a 3D view in Expand only when its scissor spans the full
  screen width, so the picture stayed 4:3, and the side areas, which
  nothing clears, kept old frames.
- The crosshair's lines are the game's own: full-width and full-height fill
  rectangles through the crosshair, built like its HUD boxes (outline and
  fill), drawn translucent (1-cycle, alpha blend, primitive alpha) and
  clipped to the inset. They're faint in Original aspect, at native
  resolution too, so they're likely on hardware as well (not yet compared).
  In Expand RT64 extends full-width rectangles past the scissor, and with
  nothing clearing the side areas each frame's translucent line blended
  over the last into bright stripes.

### Fix (Expand only)

`snp_display_list_hook` rewrites the frame's display list before it's
loaded, when the frontend's widescreen scale is above 1 (Expand with a
wider window):
- the inset scissor becomes the full screen, so RT64 widens the view;
- fill rectangles spanning the inset's width (x 12 - 307: the clear, fades,
  tints, the backdrop band behind the grass in stage 1) span the full
  width, and the full screen height too when they span the inset's height
  (y 8 - 231), or the rows above and below the inset keep old frames;
- the border's four rectangles become no-ops.

Result: cutscenes and stage 1 fill the window in Expand, with nothing stale
beside them. Still 4:3 in Expand: the game's framebuffer effects, which
redraw the previous frame as a texture over the inset (scene cross-fades,
the motion-blurred zoom behind the title logo, some tinted shots). The HUD
isn't anchored to the edges yet.

### The frame swap and the UI pump

The diagnostic was first called from the round 1 patch of libsched's
`__scTaskComplete` and never ran: the game links libsched but runs its own
scheduler. Its frame swap is `func_8004D748`, a job for the job thread
(`func_8004D7C8`) that calls `osViSwapBuffer` with the job's framebuffer
(offset 0x0C). The UI pump patch (`patches/recompui_patches.c`) now
replaces that function instead, so the frontend's per-frame UI callbacks
actually run in game.

### Theme

Green instead of orange, after the title logo's black, green and white
(the frontend's blue palette with green and blue swapped).

## Round 6: audio crackle; quitting on Windows

### Constant crackle

`SNP_AUDIO_DIAG=1` (in `queue_samples`, `src/main/main.cpp`) logs per
second the audio the game delivered, chunk sizes, the queue's range and
drops. Run headless with SDL's real-time `disk` audio driver
(`SDL_AUDIODRIVER=disk`), it showed the game delivering about 28,500 frames
a second for a 22,050 Hz output: always 920-frame chunks, 30 a second. The
queue sat at the 200 ms latency cap and about 6 chunks a second were
dropped, each an audible skip.

The game's audio thread (`func_80052064`, libmus) sizes each frame from
`osAiGetLength` with hysteresis: frame 920 (184-sample steps, the
`func_800521DC` sizing), 736 or 1104 to correct, but a correction only
after the length was in its target band (220 - 404 samples) the frame
before (`func_80052268`, flag `D_800622B0`). And it skips a frame while
`osAiGetStatus` says the AI FIFO is full. On hardware `AI_LEN` is what's
left of the buffer playing now (0 - 920 samples) and the FIFO holds two
buffers. The runtime reports the whole queue as the length and never full,
so the length jumped past the band, no correction ever started, and the
game wrote 25% too much. (Not btgarecomp's under-reported "headroom", which
this game doesn't use: removing it changed nothing.)

Fix: two hooks in `func_80052064` replace the results of its
`osAiGetStatus` and `osAiGetLength` calls with hardware-like ones from the
frontend, which tracks the chunks it queued: the length is what's left of
the chunk playing now, and the FIFO is full while two chunks wait behind it
(one more than hardware, for host thread wakeup jitter). Measured: 22,050
frames a second on average in 736/920/1104 chunks, queue 32 - 90 ms, no
drops, never empty after startup.

(The game's `__osInitialize_common` does the `osClockRate * 3 / 4` step
that btgarecomp round 138 had to redo, but nothing else in this game reads
`osClockRate`, so nothing runs slow here.)

### Quitting on Windows: "stopped interacting with Windows"

Quitting from the in-game menu ended in a Windows cross-thread hang report.
First seen on a build from before round 5, whose UI pump never ran; still
there after it. Quitting exits cleanly here on Linux. On quit, the runtime
joins the game, graphics and event threads before `recomp::start` returns;
when that takes a while the window stops pumping messages and Windows
closes the program as hung. A watchdog in `main.cpp` ends the process 3
seconds after quitting starts (the runtime's `exited` flag) if it's still
running. Saves are written as the game goes, so nothing needs the orderly
shutdown.

## Round 7: widescreen (Expand) part 2: fades, tints, the title

In Expand, scene fades and tints (the red flash in the intro, the eyes
shot's black, cross-fades) showed as a 4:3 box over a widescreen picture,
and the title screen's sides kept old frames.

### What it wasn't

The game has a framebuffer feedback effect (motion trails): each frame,
6-row strips of an off-screen buffer at `0x8011B760` are blended over the
frame, then the frame is copied back into it. That looked like the cause,
but RT64 handles it: its copies of that buffer are at the wide resolution,
and the strips span the scissor, so RT64 stretches them; the trails
already reach the screen edges. Nothing in the game's RAM rewrites that
buffer either (checked with temporary RT64 logging).

### Fades and tints: a full-screen quad in the 2D projection

The tint is a 4-vertex quad, -160..160 by -120..120, drawn after the game
loads its 2D projection (an orthographic 320x240). RT64 keeps 2D
projections at 4:3 so the HUD isn't stretched, so the quad covered the
middle 4:3 only; its color then went into the feedback buffer, which kept
the box going for a while after.

Fix (`widen_screen_quad`, `src/game/display_list.cpp`): in Expand, a
4-vertex load whose corners are exactly at +-160, +-120 is pointed at a copy
four times as wide, made every frame from the game's vertices (which carry
that frame's fade color) and clipped to the screen by the scissor. The
copies go in RAM the game doesn't use (from `0x807F0000`; the game lives in
the first 4 MB, its last framebuffer ending at `0x80400000`, and nothing in
it addresses higher), used in turn so a frame the renderer hasn't read yet
keeps its copies. The command is in the frame's own display list, rebuilt
every frame, so nothing needs restoring.

### The title: a 4:3 picture with no clear

The title screen is a 320x240 picture drawn as 64x32 tiles, and its
frames don't clear the screen (the picture covers it on the N64). In Expand
the tiles stay 4:3, which is right for the art, but the sides kept what the
cutscene before had drawn there. Fix (`clear_prefix`): in Expand, a frame
whose first drawing command is a texture rectangle at the top left corner
gets a short display list in front of it that clears the whole screen to
black, then branches to the game's list (the task's `data_ptr` is pointed
at it). The title now has black side bars that match the art.

### Left as is

- The zoom into the title (about a second) is drawn from buffers the CPU
  writes (`0x800E52B0`, `0x8011C470`), so RT64 can only show what the CPU
  made, at 4:3: a 4:3 box for that second.
- The CPU writes a few dozen pixels into the feedback buffer's RAM in some
  scenes (it shares memory with something else); RT64 copies those in, at
  4:3 positions. Not visible in the scenes checked.

## Round 8: interpolation IDs, default controls, culling at the screen edges

### Limbs jumping at high framerates

Characters are drawn limb by limb: a modelview matrix load, then the limb's
display list. The limbs are drawn from depth-sorted buckets
(`func_80030448`), so their order changes from frame to frame, and RT64
pairs matrices between frames by guessing, so limbs jumped on the
in-between frames (fine at Original framerate, the owner confirmed).

The first fix took the ID from the limb's display list and vertex data.
That made it worse: several soldiers share one model, so two soldiers' arms
had the same ID apart from a draw-order count, and when the order changed
an arm flew between soldiers on the in-between frames (the owner's video).

Now the ID comes from the game's skeleton: `func_80032F2C` walks a
character's joints with `$s2` pointing at the character's pose entry for the
joint (0x20 bytes each, unique per character and joint, the same every
frame), and each mesh's matrix is built in `func_80032AA0` (`guMtxF2L` at
`0x80032C88`). Hooks record the matrix with its joint and mesh
(`src/game/interpolation.cpp`); the display list walker puts a
`gEXMatrixGroup` with that ID in front of the matrix's load, and RT64's
automatic matching in front of every other modelview load. A frame's
matrices are built two graphics tasks before its display list goes out, and
the pools are reused, so a recorded matrix counts for two tasks. About a
third to most of the loads in character scenes get an ID. `SNP_INTERP_TAGS=0`
turns the tagging off for comparison.

That still left the jitter (the owner: same with the IDs off). Logging RT64's
matcher showed why: in the soldier scene only 11 of 67 transforms had an ID.
The game has a second skeleton walker, `func_80033758`, for animated
characters (it applies animation channels, `$s0`, on top of the pose entry,
`$s1`), and it drew most of the soldiers; their limbs, all from one shared
model, were still matched by RT64's guessing, with several pairs a frame
jumping between pieces (rotation entries changing by 0.7 - 1.5 between the
paired matrices). It's hooked the same way (key: both addresses), as is the
matrix built by `func_80034054` (another mesh path) when it's part of a
joint: now 56 of 67 have IDs. Character matrices are also blended entry by
entry rather than decomposed: about half are strongly skewed in the
convention RT64 decomposes them in.

Owner: fixed.

### Default controller layout

The owner's bindings made the default (`src/main/input_defaults.cpp`):
twin-stick, the C buttons (moving) on the left stick, the stick (aiming) on
the right stick, Z on the right trigger, R on the left. The keyboard keeps
the frontend's defaults.

### Culling at the screen edges

In Expand, a piece of wall at the right edge was missing (the owner's video,
stage 2). `func_80029070` is the game's visibility test: the object's
position in camera space (the matrix stack top times the camera's view
matrix, camera at `*0x80068180`) against near/far (`+0x4C0`, `+0x4C4`),
top/bottom (`+0x4AC * z + 0x4B0`) and left/right (`+0x4B4 * z + 0x4B8`)
limits, each plus the object's radius. Found with a gdb read watchpoint on
the camera's combined matrix (`0x80067CD0`), whose only reader is the
`guMtxCatF` in this function. A hook scales the left/right limit by the
widescreen scale. It's the only code reading those camera fields.

### Shading flicker at high framerates (RT64 patch 0016)

With the limbs paired right, the shading on the characters (the commander's
coat most of all) still flickered at the display framerate, not at Original.
The game gives every object its own set of lights each frame (`G_MOVEMEM` to
the light slots, different directions per object). RT64 lights a vertex with
the current frame's lights and the transform it's drawing with, which on an
in-between frame is the interpolated one: the next frame's lights on a pose
halfway there, so the light on a surface jumped back and forth every other
frame. `lib-patches/rt64/0016-shading-from-frame-matrix.patch` gives the RSP
shader the frame's own (uninterpolated) matrices too, in a new buffer, and
uses them for lighting and texgen: the shading follows the game's 30 fps
frames while the geometry moves smoothly. Positions are unchanged.

## Round 9: widescreen HUD; edges after switching back to Original

### HUD anchoring

As btgarecomp does it: in Expand, RT64 keeps 2D rectangles in the centred
4:3 area unless the display list anchors them to a screen edge
(`gEXSetRectAlign`). The HUD is drawn after the game loads its fixed 2D
projection (`0x802C3AE0`); its top band (y < 40) has the score on the left,
TIME in the middle, LIFE and its bar on the right. The walker
(`src/game/display_list.cpp`) collects each run of rectangles drawn back to
back (texture and fill rectangles with only state changes between them),
splits it into elements (rectangles touching or overlapping, directly or
through others: a string's glyphs, the life bar's frame and fill, the
tutorial's box around it), and anchors an element that starts in the band by
its centre: left of x 128 to the left edge, right of 192 to the right, both
16 pixels further out (the original layout keeps a TV margin). Each anchored
rectangle becomes a call to a short list that sets the alignment and a
whole-window scissor, draws it, and restores both. Centred text (PRESS START
BUTTON, TIME) and everything lower (crosshair lines, subtitles, tutorial
panels) stay as they are.

Some HUD rectangles are in a list the game keeps between frames, so the
walker restores an earlier frame's redirect before anchoring again (the
first version crashed RT64 when its entries overflowed and chained into
stale ones).

### Switching back to Original

After Expand, the strips outside the game's 4:3 inset kept pieces of the
wide picture (the game only clears the inset; on the N64 they're black from
boot). The first six frames after switching back clear the whole screen.

### Enemies of one kind jittering (stage 2's winged insects)

Enemies of one kind can share an animation pose, so their joints share
IDs, and RT64 paired repeats of an ID in drawing order: when the depth sort
swapped two insects, their wings were paired with each other's on in-between
frames. Tagged groups now use `G_EX_ORDER_AUTO`: repeats of an ID are paired
by closeness (different IDs still never). Measured with temporary RT64
logging in that scene: paired matrices changing a lot between frames went
from about 50 a frame (the worst entry change 1.18) to about 18 (0.72, the
fast-flapping wings themselves); the soldier scene is unchanged.

Closeness alone still let pieces fly off the small insects in a swarm (25 of
one kind sharing a pose): an insect's body was sometimes paired with the one
beside it, about 130 units away, and shot down on the in-between frames.
Each skeleton drawn is now an instance (hooks where the walkers start:
func_80033128, func_80033170, func_80033A40, func_80033AC4), placed by
its first matrix's translation, and numbered by matching the frame's
instances of a kind to last frame's, closest pairs first (within 256 units);
the number goes into its IDs, so repeats no longer happen. Taking them one at
a time in draw order wasn't enough: a newly spawned insect took the number of
the one beside it. Logged in that scene: an instance's match is now its own
last position (about 17 units a frame against 130+ to the next nearest),
apart from insects flying in even lines, which all shift together.

Numbered IDs are now paired directly (`G_EX_ORDER_LINEAR`); only un-numbered
ones keep `G_EX_ORDER_AUTO`. Automatic ordering pairs a matrix only with one
whose draw call looks the same in both frames, and with unique IDs that left
whole insects unpaired, snapping to their new place on in-between frames.
Logged in the swarm: tagged matrices left unpaired went from about 48 of 104
a frame to about 3 of 97. (`SNP_INTERP_INSTANCES=0` turns the numbering off
for comparing; `SNP_PACING_LOG=1` writes `snp_pacing.log` and
`snp_present.log` with frame pacing and present times.)

Two more causes of whole insects jumping: an instance's kind was the key of
its first matrix, and which mesh comes first changes from frame to frame (the
same insect showed up as three kinds in three frames, getting a new number
each time); kinds now come from the walker entry points' skeleton and
animation arguments. And instances were batched by the task they were built
in, which can hold two frames' builds; now, once a display list has been
walked, exactly the instances it draws are numbered and their IDs rewritten in
the scratch entries. Logged in the swarm: tagged IDs missing from the previous
frame went from about 4 a frame to about 0.6 (enemies appearing). With the
pacing log a user's 144 Hz machine showed a steady 30 fps, 5-5-5-5-4
in-between frames and presents 6.95 ms apart; the cutscene camera shake is
there at Original too (the game's own).

The crosshair's vertical line runs the full height, so it started in the
HUD band and was pinned to an edge when the reticle was off centre; HUD
elements must now also end above y 80.

### Reticle reach in Expand

The reticle's offset from the screen centre is kept within -144 .. 144 (320
units) by func_800AB924 (struct at $s2: x at +0x28, y at +0x2C within
-104 .. 104), so in Expand enemies at the sides could be seen but not aimed
at. In Expand the side limits are now 16 short of the wider screen's edges
(hooks in snp.jp.toml, src/game/widescreen.cpp); Original is unchanged. Shots
go to the reticle out there as well (the game turns its screen offset into a
direction with its own projection, which Expand only shows more of). The
crosshair's vertical line past the 4:3 picture is drawn beyond x 320 (cut by
the game's scissor) or, on the left, at a negative x that wraps to near 1024:
it's drawn with the whole window as scissor, a wrapped one moved back by 1024.

### Identical objects flickering (the city stage's cones)

The cones aren't skeletons, so they had no IDs and RT64 paired them by its
guesses: on single in-between frames a cone vanished or showed 20-50 pixels
off (paired with another cone). guMtxF2L now records every matrix the joint
hooks didn't as an object of its own, of the kind its caller ($ra) makes,
placed by its translation, and numbered with the instances (closest pairs
first, per display list). Objects keep RT64's automatic component settings and
decomposition; only their pairing is by ID (G_EX_ORDER_LINEAR). An object's ID
is used only with its number (with SNP_INTERP_INSTANCES=0 they're automatic as
before). Numbering costs about 0.1 ms a frame (stage 2, up to 121 instances).

### Fast rows of alike objects (a boss's fire wave)

Numbering paired instances by where they were: the fire wave after a boss is
a row of alike panels rushing at the camera, each moving further in a frame
than half the gap to the next, so a panel was paired with the one behind it
and flickered back on in-between frames. Each numbered instance now carries
its last movement, and pairs are taken closest to where that movement would
take it (still within 256 units of where it was).

Objects are also matched by how they're turned (RT64's measure, 1 - dot per
axis, weighted 64 units each): a boss's energy fountains are alike spray
layers in one place turned differently, and by place alone a layer could be
paired with another and swirl on in-between frames. guMtxF2L's hook reads the
float matrix's translation and axes itself.

### Cones shifting with ghosting (the city stage, found on Android)

The cones still shifted left and right with ghosting at Display rate, in some
sessions and not others, on PC too; fine at Original. They stand in four spots
drawn with two models (two frames of the cone, swapped from frame to frame),
and a spot changes model between frames, so an object of one model was paired
with its own model's object in another spot, 50-150 units away, and slid
across on in-between frames. Kinds whose objects take over each other's exact
spot (within 1 unit, with none of their own kind there) three times are now
joined into one family, numbered and given IDs as one kind, so a cone keeps
its number with either model, standing or kicked into the air; until then
such an object gets a new number and is drawn where it is.

Also found on the way (each harmless when it isn't the cause):
- Equal distances in the pairing sort went by the library's order (std::sort;
  Android's libc++ orders ties differently): now std::stable_sort.
- A display list the game keeps had its tagged matrix loads restored from the
  tag entries, which are reused every four graphics tasks: a kept list not
  drawn for that long (the stage while its menu is up) got another load back.
  The original loads are now kept by command address.
- Objects at exactly the same place in their own matrix are told apart by
  where the display list puts them (the walk keeps the modelview stack).

`SNP_INTERP_LOG=1` writes snp_interp.log: per display list, the matrix
lookups by outcome and how each kind of object with more than one drawn was
numbered (on Android, the debug.snp.interplog property or a file
snp-interp-log in the app's storage).

### Explosions pulsing (issue 19)

Seen in a 60 fps recording of a stage with the rolling mines: an explosion's
spheres first showed up huge and shrank over a frame or two, then grew with
their size going big, small, big on alternating displayed frames. An
explosion is alike sphere layers at one place, each its own size, and
objects were paired by place and turn only, so layers swapped numbers. The
guMtxF2L hook now also records how much a matrix scales (its axes' mean
length), and pairs are weighed by how far that is from where the last growth
would take it (64 units a doubling); a pair more than 3 times larger or
smaller isn't made at all (a new explosion where one just ended). The interp
log shows each object's scale.

## Round 10: a second player for mods

For the co-op mod (wootbeer/snprecomp-mod-coop): player 2 flies a small drone
with their own crosshair, alongside player 1. The game's own 2-player mode is
different (player 2 aims and shoots for player 1's character), and it starts
when a second controller is plugged in, so the game must keep seeing port 1
only.

- Local Multiplayer (Mods), a General tab option read at startup, puts
  recompinput in multi-player mode for two players: each picks a controller
  or the keyboard with Assign players, and player 1's no longer drives
  anything else.
- Ports 2-4 stay unplugged in the game's controller status as before, and
  in multi-player mode they don't answer reads either (`get_n64_input`,
  src/main/main.cpp), so player 2 can't reach the game.
- Mods read the players through two exports (src/main/mod_api.cpp,
  `RECOMP_IMPORT("*", ...)`): `snp_get_player_count` and
  `snp_get_player_pad`, which fills an OSContPad with the stick in the range
  the game gets.

Smooth Aiming for a mod's crosshair: player 2's moves at the game's 30 fps
otherwise, since 2D matrices aren't interpolated. RT64's live offset already
takes any ID with its tag; only snprecomp assumed one reticle. A live ID now
holds a slot (2 bits) between the position's x (12 bits) and y (10 bits,
plenty for the reach of -104 .. 104): slot 0 is player 1's reticle, as
before, and 1-3 are mods'. Once a game frame a mod calls
`snp_set_live_reticle` with its crosshair's position, movement per game frame
and reach. The walker gives a 2D matrix at one of a slot's last four
positions that slot's ID (after checking player 1's), and RT64's callback
moves it from the newest position on at its movement for the part of a game
frame gone by. Only with Smooth Aiming on; a slot stops 0.2 s after the
mod's last call. Mods' crosshairs get no full-screen lines.

`recomp_printf` for mods (src/main/mod_printf.cpp), the name other recomps'
mods use: to stdout, and on PC to mod_log.txt in the working directory,
since Windows release builds have no console. It reads the arguments as an
o32 variadic call passes them (checked against clang's output for the mod
template's flags: doubles and 64-bit integers in an aligned pair of slots,
high word first, past a3 on the caller's stack) and was tested on the host
with every conversion.

An overlay for mods (`snp_draw_overlay`): a display list a mod gives once a
game frame, drawn at the end of each frame in play, after the HUD. Mods are
loaded from 0x81000000, past the N64's 8 MB, where an N64 address wraps back
to the start of RAM; RT64's extended RDRAM (`gEXSetRDRAMExtended`) takes
0x8xxxxxxx addresses as offsets into all of it instead. So the walker turns
the frame's full sync into a branch to a suffix (0x807FA000, 8 slots used in
turn): gEXEnable, extended RDRAM on, a call to the mod's list, extended RDRAM
off, the full sync. Only in frames that loaded the HUD's 2D projection, outside
the front end's menus, and only while the mod keeps calling (0.2 s). The
mod's list is walked like the frame's own, so its 2D matrices get Smooth
Aiming through `snp_set_live_reticle`. A full sync redirected in a list the
game keeps is put back at the next walk.

Not yet tested in game: Assign players with two controllers, the keyboard as
player 2 (multi-player keyboard bindings start empty in recompinput), the
overlay, and a mod's live crosshair (the co-op mod draws both). Player 1's live ID
changed layout; Smooth Aiming should be checked again with the mouse, the
stick and touch.


### Second Player instead of assigning players

Multi-player mode made the Controls tab a per-player page that needed Assign
players before any binding could be seen, and gave player 1 a separate
keyboard profile. The frontend now always stays in single player mode, and
the Second Player option (General tab: Off, Game's 2-Player Mode, Co-op Mod)
gives a second controller to player 2 (src/main/second_player.cpp,
lib-patches/RecompFrontend/0021): player 1's controller is the first to press
a button, player 2's the next other one to, and leaves when disconnected or
the option is turned off. Player 1 reads the keyboard, the mouse and every
controller but player 2's; player 2 only theirs, with the same controller
bindings. Game's 2-Player Mode plugs port 2 in for the game while two
controllers are connected, so its own 2-player mode can start; Co-op Mod
keeps port 2 empty and gives player 2 to mods (snp_get_player_count is 2 once
one has joined).

## Round 11: CPU use while waiting

BattleTanx GA Recompiled's main game thread spun a whole core at all times
(its round 188): its yield hook returned at once, so the polling loops called
it back to back. Checked here the same way: headless (Xvfb, lavapipe, disk
audio), per-thread CPU with `top -H` on the attract mode and the title screen,
and gdb backtraces of every thread, five samples.

No game thread spins. On the title screen the busiest, the main thread
("Game 3"), used 1-2% of a core; every other game thread was asleep in
`run_next_thread_and_wait` or `resume_thread_and_wait`. The main thread was
always inside `snp_spin_yield` from `func_80025E44` (its wait on
D_80068A95), asleep in `wait_for_external_message_timed`:
`src/game/spin_wait.cpp` has waited up to 1 ms for an external message on
every call since round 2, so none of the three hooked loops spins. The busy
threads in the sandbox were lavapipe's (software Vulkan), which a real GPU
doesn't have. Nothing changed.
