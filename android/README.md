# Android build

How to build the Android version (status and history in [PLAN.md](PLAN.md)). The app is SDL's `SDLActivity` hosting the game,
built by Android Studio from the repo-root `CMakeLists.txt` as `libmain.so`. Like the
Windows and Linux releases, the APK carries the compiled game code but no ROM: the build
fails if a file in it is named or headed like an N64 ROM.

## One-time setup

1. **Build the desktop version first, with the ROM in place** ([BUILDING.md](../BUILDING.md)).
   The Android build reuses two things from it:
   - the files generated from the ROM: `RecompiledFuncs/`, `RecompiledPatches/` and
     `rsp/n_aspMain.cpp`, in the checkout itself;
   - `file_to_c(.exe)`, a build tool that ends up in the desktop build folder (`build/`).

2. **A second checkout for Android**, so the desktop one stays on `main`:

       git clone -b android https://github.com/wootbeer/snprecomp snprecomp-android
       cd snprecomp-android
       git submodule update --init --recursive

   Then copy `RecompiledFuncs/`, `RecompiledPatches/` and `rsp/` into it from the desktop
   checkout. Copy them again whenever the desktop build regenerates them (after a change to
   `snp.jp.toml`, the symbols or `patches/`, e.g. when main is merged into this branch).

3. **Android Studio, SDK Manager, SDK Tools** (tick "Show Package Details"):
   NDK **28.2.13676358** and CMake **3.31.6**. Gradle tries to install these itself when
   missing, but the SDK Manager is the reliable way.

4. **`android/local.properties`** (Android Studio creates it with `sdk.dir`; it isn't
   committed). Add the desktop checkout's build folder, with forward slashes:

       snp.hostToolsDir=C:/dev/snprecomp/build

   (`local.properties.example` shows the whole file.)

## Build and run

Open the `android/` folder of the Android checkout in Android Studio, let the Gradle sync
finish, pick the device (USB debugging on), press **Run**. The debug build compiles the
game optimised (with debug info), so the first build takes a while.

After that: `git pull` (and `git submodule update --init --recursive` if a submodule moved),
then Run.

From a terminal instead: `gradlew assembleDebug` in `android/` (the APK lands in
`app/build/outputs/apk/debug/`).

Requirements: Android 9 or newer (API 28), 64-bit ARM, Vulkan with descriptor indexing (RT64's game
shaders declare texture arrays of 8192; older Mali GPUs such as the G52 lack it, see PLAN.md).

The ROM is chosen in the game's launcher: **Load ROM** opens the system file picker
(.z64, .v64 or .n64; the Japanese ROM or the English translation, as on desktop). It's
validated like on desktop and stored in the app's private storage, which is excluded from
backups and device transfers. **Load Spanish ROM** (Sound tab) works the same way.

Back (the system one, not a controller's) opens and closes the game's menu; the launcher's
Exit closes the app.

Touch (settings on the **Touch** tab): by default a touch puts the reticle there and
fires (Touch Aim: Point, Fire While Touching), alongside a controller. Drag aims like a
trackpad instead. The on-screen buttons (move, FIRE, JUMP, AIM, START, MENU) show when no
controller is connected, or always or never as set there.

## Release build

Signed with your own release key, so updates install over each other (by hand or with an
updater such as Obtainium, which takes the APK from the GitHub release). Every release must
be signed with the same key and have a higher `versionCode` (`app/build.gradle.kts`, with
`versionName` the same as the PC release's tag).

One-time: make the key with the `keytool` that comes with Android Studio, outside the repo,
and back the `.jks` file and its password up (a lost key means users must uninstall to
update):

    mkdir C:\keys
    & "C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe" -genkeypair -v -keystore C:\keys\snp-release-key.jks -alias snp -keyalg RSA -keysize 4096 -validity 36500

Then copy `keystore.properties.example` to `keystore.properties` here and fill in the
passwords (gitignored).

Each release, in `android/`: `.\gradlew assembleRelease`. The APK is
`app/build/outputs/apk/release/SnPRecompiled-<version>-android.apk`; attach it to the
GitHub release with the PC builds. A debug build (Run) is signed with Android Studio's debug
key, so the release APK won't install over it: uninstall the debug build first (that also
removes its settings, saves and stored ROM).

## Logs

Players can share the game's log without a computer: after a crash the next start asks whether
to share it (before the game starts again, so it works when the game crashes at once), and the
app icon's long-press menu has **Share log**. Both send one text file with the device, the app's
version, and the latest and the previous run's `log.txt` and `crash_log.txt` (`LogFiles.java`).

The game's output also goes to logcat with the tag `SnP`:

    adb logcat -s SnP

A native crash's full report (logcat cuts some of it):

    adb shell dumpsys dropbox --print data_app_native_crash

## Layout

- `app/src/main/java/org/libsdl/app/`: SDL's Java sources, unmodified, from SDL 2.32.10
  (zlib license, `licenses/SDL2.txt`). Must match the SDL version in `cmake/Android.cmake`.
- `app/src/main/java/io/github/wootbeer/snprecomp/SnPActivity.java`: the activity.
- `cmake/Android.cmake`, `cmake/CrossCompile.cmake` (repo root): the Android side of the
  native build. `src/android/`: the Android-only native code.

## Vulkan validation (testing aid)

To have Khronos' Vulkan validation layer check what RT64 sends the GPU driver, add
`snp.vulkanValidation=true` to `android/local.properties` and press Run. The debug APK then
carries the layer (about 22 MB, downloaded once and checked against a fixed SHA-256). Then turn on
Android's GPU debug layers for the app (phone plugged in, `adb` from the SDK's `platform-tools`):

    adb shell settings put global enable_gpu_debug_layers 1
    adb shell settings put global gpu_debug_app io.github.wootbeer.snprecomp
    adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
    adb shell setprop debug.vulkan.khronos_validation.enables VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT

Restart the game; the layer's reports go to logcat (search for `Validation`). It slows the game
down. To turn it off again:

    adb shell settings delete global enable_gpu_debug_layers
    adb shell settings delete global gpu_debug_app
    adb shell settings delete global gpu_debug_layers
    adb shell setprop debug.vulkan.khronos_validation.enables ""

and remove the line from `local.properties`.
