# Changelog

## [0.2.9 Beta] - 2026-10-31

### Added
- Windows: Graphics API on the Graphics tab (Auto, D3D12 or Vulkan). Try
  Vulkan if the game crashes or shows nothing with D3D12; it takes effect
  when the game restarts.

### Changed
- Crash reports name the GPU, its driver and the graphics API in use.
- Smooth Aiming is off by default: it now moves the reticle like the rest of
  the picture, which trails your aim slightly. It's greyed out unless the
  Framerate is above Original, where it has no in-between frames to work in.

### Fixed
- Smooth Aiming had no effect while you aimed, so the reticle moved at 30 fps:
  it only recognised the reticle where it stood still. It now moves it
  smoothly between the game's frames, with the crosshair lines kept on it.
- Windows: when the graphics card's D3D12 driver fails, the game no longer
  crashes on its next buffer upload, and the log says why the driver failed.

## [0.2.8 Beta] - 2026-10-31

### Added
- Local Multiplayer (Mods) on the General tab: player 2 gets their own
  controller (Controls → Assign players) for mods that add a second player,
  like the co-op mod. The game itself stays one player.
- For modders: `snp_get_player_count` and `snp_get_player_pad`, to read each
  player's controller from a mod, and `snp_set_live_reticle`, which gives a
  mod's crosshair Smooth Aiming like player 1's (`include/snp_mod_api.h`).
- For modders: `recomp_printf`, printing to `mod_log.txt` next to the game.
- For modders: `snp_draw_overlay`, to draw from a mod after the HUD, with the
  mod's own memory reachable (RT64's extended RDRAM).

### Fixed
- Windows: the game could crash at start on PCs without the latest Visual C++
  runtime. The zip now includes it.
- Android: a black screen in play (sound and touch working) on phones whose
  GPU can't pick textures per pixel, like the Mali-G52: the game's shaders
  didn't compile there. Textures that change detail level across a surface
  may look off on those phones.
- Local multiplayer: a keyboard and mouse player now starts with your keyboard
  bindings instead of none, and players on controllers no longer also respond
  to the keyboard. Reset on a player's keyboard profile restores the default
  keys.

## [0.2.7 Beta] - 2026-10-27

### Fixed
- Android: on phones whose graphics drivers can't compile the renderer's
  framebuffer effects (the Galaxy S24 Ultra), the game tries them again in a
  form those drivers may accept. These effects are used in some demos and
  transitions.

### Changed
- Crash reports also cover aborts (a library, such as the graphics driver,
  giving up), and on Linux and Android they list the call stack.

## [0.2.6 Beta] - 2026-10-27

### Fixed
- Android: the app closed on start on the Galaxy S24 Ultra (Snapdragon 8 Gen 3)
  and possibly other phones with newer Adreno drivers, which fail to compile
  some of the renderer's effects. The game now skips what failed instead of
  crashing, and the log names it.

## [0.2.5 Beta] - 2026-10-27

### Added
- Android: Share log. After a crash the app offers to share the game's log the
  next time it starts, and the app icon's long-press menu has Share log. The
  log has the device's model and graphics driver and the game's messages.

### Fixed
- Android: the game crashed after the Nintendo logo on phones with older
  Adreno GPUs (Adreno 619 and similar), whose drivers can't compile one of the
  renderer's shaders. The game now draws without it.
- Android: the launcher could open in portrait. The app is landscape only now.

## [0.2.4 Beta] - 2026-10-26

### Added
- Music Volume and Sound Effects Volume on the Sound tab, next to Main Volume.
  Voices count as sound effects.
- Texture Filtering and HUD Filtering on the Graphics tab: Sharp draws the
  game's textures, or the crosshair, HUD and menus, with sharp pixels.
- Menus Only for the screen filter on the Graphics tab: the filter in the menus,
  not in play.
- Smooth Aiming on the Controls tab (on by default): the reticle moves at your
  display's framerate, following the mouse, stick, light gun or touch, not just
  the game's 30 frames a second.

### Fixed
- The city stage's cones sliding left and right with ghosting at framerates
  above 30.
- Black areas at the sides of the warehouse in the intro in widescreen. The
  room never had side walls; they're now drawn with its own wall textures.

## [0.2.3 Beta] - 2026-10-25

### Fixed
- Linux: Load ROM could crash before the file picker opened. The picker is now
  the desktop's own (xdg-desktop-portal), not GTK's.

## [0.2.2 Beta] - 2026-10-24

### Added
- Spanish Voices on the Sound tab: the Spanish dub's voices with the English
  or Japanese text. Press Load Spanish ROM once and pick the dubbed Spanish
  ROM. (The Spanish translation itself can't run, it changes the game's code.)
- Stick Sensitivity on the Controls tab, how fast the stick moves the reticle.
- 8X MSAA, greyed out if your GPU doesn't support it.

### Fixed
- Motion blur in widescreen, broken in 0.2.1.
- Black gaps at the bottom of the screen in widescreen when aiming high
  (from 0.2.1).
- Lock-on didn't work with mouse aim. In lock-on mode, flick the mouse toward
  an enemy to lock on, and flick again to switch targets.
- The reticle drifted off its crosshair and stuttered at framerates above 30.
- Black patches in the corners in widescreen when aiming to the far left or
  right (stage 1 and 2-2).
- The mouse moved the reticle during the How to Play demo, so the demo went
  off script and Saki died.
- Jittering objects at framerates above 30, like the flying debris in 2-2.
- Saki snapping into place over and over when strafing at framerates above 30.
- Crates in the intro flickering at framerates above 30.
- A band along the top and bottom of the screen in widescreen during motion
  blur.
- The flashing box around the LIFE gauge in the tutorial came apart in
  widescreen.
- Linux: Load ROM could crash on a Wayland desktop (Bazzite) before the file
  picker opened.

## [0.2.1 Beta] - 2026-10-24

### Fixed
- Start Game didn't show after loading a ROM until the app was restarted.
- Flickering, red or smeared bits of background at the sides in widescreen.
  Places with no scenery past the 4:3 view are black now.
- The sky's horizon glow in the Radan fight stopped at the 4:3 edge in
  widescreen.

### Changed
- The translated ROM's Start Game is labeled Translation, not English (Spanish
  and other translations go there too).

## [0.2.0 Beta] - 2026-10-23

### Added
- Mouse aim: Absolute (the reticle follows the pointer) or Relative, with
  sensitivity. Left click fires, right click jumps, middle click switches the
  aim mode.
- Mouse buttons can be rebound, side buttons too.
- Analog Aim option, the stick moves the reticle as far as it's pushed.
- Rice texture packs from the texture_packs folder, with an Open Folder
  button and an on / off option on the Graphics tab.
- Any dump of the Japanese ROM works, and English translations of it, like
  Zoinkity's v1.0.
- Change ROM on the launcher.
- Unlock Everything on the new Enhancements tab.
- Infinite health, time and continues on the new Cheats tab.
- CRT, Scanlines and LCD screen filters, and bloom.
- Add to Steam on the launcher, with artwork.
- F1 opens the settings, as well as Esc.
- crash_log.txt shows more about where a crash happened.

### Changed
- Mouse Aim, Mouse Sensitivity, Joystick Deadzone and Analog Aim are on the
  Controls tab.
- Controls and Mods moved off the launcher, they're in Settings.

### Fixed
- OPT. menu's top row pulled to the edges in widescreen.
- "Fatal error in mod" message when exiting from the launcher.

## [0.1.0] - 2026-10-22

### Added
- Widescreen, HUD anchored to the screen edges and the reticle reaches the
  full width (experimental)
- Framerates above 30 fps (experimental)
- Higher resolutions and anti-aliasing
- Twin-stick controller layout by default, fully remappable
- Supports the original Japanese ROM and the English fan translation
- crash_log.txt generates upon crash
- Linux / Steam Deck build, completely untested, let me know.
