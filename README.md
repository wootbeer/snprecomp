# Sin and Punishment (2000 N64) Recompiled and Port

You will need the game file from a licensed copy in order to play it.

A native PC and Android port of Sin and Punishment (Tsumi to Batsu: Hoshi no Keishousha),
made by statically recompiling the N64 game with
[N64Recomp](https://github.com/N64Recomp/N64Recomp) and running it on
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) with
[RT64](https://github.com/rt64/rt64) for rendering.

Unofficial, and not affiliated with the rights holders. **No game data is
included.** You need your own dump of the game to build or play this.

## Features
- Runs natively on Windows and Android. Linux and Steam Deck builds are untested.
- Widescreen, with the HUD at the screen edges and the reticle reaching the
  full width (experimental).
- High resolutions and framerates above 30 fps (experimental).
- Mouse aim, Absolute (the reticle follows the pointer) or Relative.
- Analog Aim for smoother aiming with a stick.
- Smooth Aiming: the reticle moves at your display's framerate.
- Twin-stick controller layout by default, fully remappable, and keyboard.
- Plays the Japanese ROM or an English translation of it.
- Texture packs: RT64 packs and Rice packs made for emulators.
- CRT, Scanlines and LCD screen filters, and bloom.
- Sharp texture filtering for the game and the HUD, each on its own.
- Separate Music and Sound Effects volume.
- Unlocks and cheats in the settings, no codes needed.
- Add to Steam from the launcher, with artwork.
- Android: plays with a controller or a handheld's own controls, and touch
  controls (experimental): touch to aim and shoot, and on-screen buttons.

## Game Files
You need your own data file: the Japanese release, or an English translation
patched onto it. Known good:

| ROM | SHA-1 |
|---|---|
| Tsumi to Batsu - Hoshi no Keishousha (Japan) | `581297b9d5c3a4c33169ae0aae218c742cd9cbcf` |
| Sin and Punishment - Successor of the Earth (U), English translation | `5945ff3935d9d9531bbb5cfcabed7c198e2b2f41` |
| Sin and Punishment - Successor of the Earth, English v1.0 [Zoinkity] | `e3906c919236dac6eba020b4b8c1df91fd725c5d` |

Other dumps work too: `.z64`, `.n64` and `.v64`, and other translation
patches as long as they only change text and graphics.

The Spanish translation changes the game's code, so it doesn't run, but its
Spanish voices can be used: on the Sound tab, press **Load Spanish ROM**,
choose the Spanish dub (the Japanese ROM patched with
`SAPESP.10.DOBLADO.xdelta`) and turn on **Spanish Voices**. The text stays the
loaded ROM's.

Start the game, pick **Load ROM** and select your file. It only has to be
done once. You can keep one Japanese and one English ROM, each with its own
saves, and pick either one with **Start Game**. **Change ROM** swaps one out.

Add a blank portable.txt in the application's folder to make it portable,
if you want to save settings and saves locally.

## Download
Get the latest build from the [Releases](https://github.com/wootbeer/snprecomp/releases) page.
- **Windows:** unzip it anywhere and run `SnPRecompiled.exe`.
- **Linux / Steam Deck (untested):** extract the `.tar.gz` and run
  `SnPRecompiled.sh`. Settings and saves go in `~/.config/snprecomp`.

- **Android:** install `SnPRecompiled-<version>-android.apk` (allow installs
  from your browser or file manager when asked). Needs Android 9 or newer, a
  64-bit ARM device and Vulkan with descriptor indexing: older Mali GPUs, like
  the Mali-G52, lack it and show a black screen. On first start, **Load ROM**
  opens the system file picker; the ROM is copied into the app, so it's needed
  only once.

On Steam Deck, in Desktop Mode, use **Add to Steam** on the launcher (or "Add a
Non-Steam Game" in Steam), then play it from Gaming Mode.

## Controls
F1 or Esc opens the settings. Change the controls on the Controls tab.

| | Mouse and keyboard | Controller |
|---|---|---|
| Move | Arrow keys | Left stick |
| Aim | Mouse, or W A S D | Right stick |
| Fire | Left click / Q | Right trigger |
| Jump | Right click / R | Left trigger |
| Aim mode (lock-on / free) | Middle click / Space | A |
| Start | Enter | Start |

Mouse Aim, Mouse Sensitivity, Joystick Deadzone, Analog Aim and Stick
Sensitivity are at the top
of the Controls tab. Mouse buttons, side buttons too, can be bound like keys
on the keyboard side of the Controls tab.

Rolling is a quick double tap of left or right. **Roll**, **Roll Left** and
**Roll Right** (unbound at first, at the end of the Controls tab's list) roll
with one button instead, handy on a light gun with pedals: Roll goes the way
you're moving, or the way you last moved.

**Android:** a controller or a handheld's controls work as above. Back opens
the settings. Touch controls (experimental) are set on the **Touch** tab:
- **Touch Aim**: Point (the reticle goes where you touch, the default), Drag
  (like a trackpad) or Off. It works together with a controller.
- **Fire While Touching** (on by default): fires while your finger is down,
  so with Point a touch shoots where it lands.
- **On-screen Buttons**: a stick to move, FIRE, JUMP, AIM, START and MENU.
  Auto shows them only when no controller is connected.

## Texture Packs
- **RT64 packs** (`.rtz`): install them from the Mods tab.
- **Rice packs** (emulator `hires_texture` packs): put the pack's folder in the
  `texture_packs` folder. Graphics > Texture Packs > **Open Folder** opens it.
  Texture Packs can be turned off there too, before the game starts.

Tested with Vanit's English translation pack on the Japanese ROM. If both
kinds of pack replace the same texture, the RT64 one is used.

On Android, Rice packs can't be installed yet (the app's folder can't be
opened from a file manager).

## Mods
Install mods from the Mods tab. To make your own, start from the
[mod template](https://github.com/wootbeer/snprecomp-mod-template).

## Issues and Limitations

Known:
- Beta.
- Widescreen and framerates above 30 fps are experimental. If something looks
  wrong, check it at Original aspect ratio and Original framerate.
- A few scenes show sky or nothing at the sides in widescreen, where the game
  has no scenery past the original 4:3 view.
- Linux and Steam Deck builds are untested.
- Android: tested on a Retroid Pocket 6. Touch controls are experimental.

Else:
- Please see the "Issues" section in GitHub. Screenshots or a short video
  help a lot.
- If the game crashes, attach `crash_log.txt` from the game's folder.
- Android: after a crash, the app offers to share its log the next time it
  starts. Or long-press the app icon and choose **Share log** at any time.

## Changes
See [CHANGELOG.md](CHANGELOG.md).

## Building
See [BUILDING.md](BUILDING.md), and [android/README.md](android/README.md) for
Android.

## Credits
- [N64Recomp and N64ModernRuntime](https://github.com/N64Recomp) by Mr-Wiseguy
  and contributors, which this port is built with
- [RT64](https://github.com/rt64/rt64) by Dario and contributors, the renderer
- [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) by the N64Recomp
  contributors, the launcher and input layer
- Zoinkity, for the English translation of the game's text
- [Mouse Injector](https://github.com/garungorp/MouseInjectorDolphinDuck) by
  Carnivorous, whose Sin and Punishment driver pointed to the reticle in memory
- The CRT filter is based on Timothy Lottes' CRT shader, and the LCD filter on
  LCD3x by Gigaherz (both public domain)
- The Rice texture pack hashing follows RT64's texture_hasher, based on
  GlideN64 and Rice Video (GPL)
- Steam artwork from [SteamGridDB](https://www.steamgriddb.com/), downloaded
  when you use Add to Steam
- [spimdisasm and rabbitizer](https://github.com/Decompollaborate) for finding
  function boundaries, and [n64sym](https://github.com/shygoo/n64sym) by shygoo
  for identifying the N64 SDK's functions
- The app icon's 罪 is drawn with [Dela Gothic One](https://github.com/syakuzen/DelaGothic)
  (SIL Open Font License 1.1)

## License
The project's own code is GPL-3.0; see [COPYING](COPYING). The game itself
remains the property of its rights holders.
