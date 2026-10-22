# Android: JNI calls from the recompiled-code fiber (save-data crash)

Applies to: the Android frontend (`port/azahar`, pinned Azahar `5f3306d13`) with the
recompiled-code CPU backend (`port/src/arm_recomp.cpp`, `port/src/fiber.cpp`) on
arm64-v8a. First diagnosed against MH4U USA v1.3.0 (`0004000000126300`) while it
formats its save data on first launch.

## Symptom

About five seconds into the game, the APK aborted and the process died with:

```
F libc: Fatal signal 6 (SIGABRT) in tid <NativeEmulation>, pid <azahar>
F DEBUG: Abort message: 'JNI DETECTED ERROR IN APPLICATION:
    JNI GetStringUTFChars called with pending exception
    java.lang.StackOverflowError: stack size 33MB'
F DEBUG:     at void org.citra.citra_emu.NativeLibrary.run(java.lang.String)
F DEBUG:     in call to GetStringUTFChars
F DEBUG:     from void org.citra.citra_emu.NativeLibrary.run(java.lang.String)
```

The save directory was never populated and `data/00000001.metadata` did not exist, so
`ArchiveSource_SDSaveData::Open` kept returning `ResultNotFormatted` and the game kept
asking for `FormatSaveData`. Raising the Java thread stack (1 → 32 MB) and the fiber
stack (8 → 32 MB) changed nothing: this is not a real stack exhaustion.

## Root cause

The port runs recompiled guest code on an `mmap`-allocated fiber stack
(`port/src/fiber.cpp`, AArch64 switch in `port/src/fiber_arm64.S`). ART knows nothing
about that stack, so any JNI call made from the fiber enters Java with an SP outside the
Java thread's stack range. ART's interpreter stack check is

```c
// art/runtime/interpreter/interpreter_common.cc (android14-release)
uint8_t* stack_end = self->GetStackEndForInterpreter(implicit_check);
if (UNLIKELY(__builtin_frame_address(0) < stack_end + frame_size)) {
    ThrowStackOverflowError(self);
}
```

where `stack_end = stack_begin + GetStackOverflowReservedBytes()` is near the bottom of
the Java thread's stack (`thread-inl.h`, `Thread::InitStackHwm`). The aborting thread's
dump showed `stack=0x77d4602000-0x77d4604000 stackSize=33MB`, while the fiber SP was
`0x7746035880` — below that range. The first JNI call from the fiber therefore throws
`StackOverflowError`; the very next JNI call with that pending exception is fatal.

The crash lands in the save-data path because these calls run synchronously on the
guest thread (the fiber), unlike most file I/O which Azahar dispatches to its async FS
worker (a regular thread):

```
FileSys::SaveDataArchive::OpenDirectory
  → PathParser::GetHostStatus
    → FileUtil::IsDirectory
      → AndroidUtils::CanUseRawFS
        → AndroidUtils::GetBuildFlavor
          → _JNIEnv::GetStringUTFChars   // pending StackOverflowError → SIGABRT
```

`FS_USER::FormatSaveData` is also synchronous, which is why earlier runs died there.
On the vanilla Android build `CanUseRawFS()` is true, so every raw file operation goes
through `AndroidUtils::TranslateFilePath` → `GetUserDirectory` (JNI) and
`CanUseRawFS` → `GetBuildFlavor` (JNI). Both are simple JNI lookups; they are the only
Java calls in the raw-FS path.

Two aggravating factors made the symptom misleading:

* `ArchiveSource_SDSaveData::Format` ignored failures and returned `ResultSuccess` even
  when the directory or metadata write failed, so a broken format could look like a
  success and the game would retry forever.
* The abort happened inside the format/open path, which made it look like a file-system
  or SAF bug. The underlying failure was the JNI transition itself.

## Fix

Keep the emulation core out of JNI. `/home/james/Git/azahar` is patched as follows:

* `src/common/android_utils.{h,cpp}`: cache the build flavor in `InitJNI()`
  (`JNI_OnLoad`, a regular thread) and the user directory in a new
  `CacheUserDirectory()`; `CanUseRawFS()` and `GetUserDirectory()` return the cached
  values and only fall back to JNI when nothing is cached.
* `src/android/app/src/main/jni/native.cpp`: `logUserDirectory` (called during
  `DirectoryInitialization.start()`, before any emulation) resolves and caches the
  native user path. The directory cache is refreshed whenever the directory is set up
  again, e.g. after the user changes the storage location.
* `src/core/file_sys/archive_source_sd_savedata.cpp`: `Format` now logs the guest and
  translated host paths, checks `DeleteDirRecursively`/`CreateFullPath`/`Exists`, the
  metadata open/write/flush, and returns `ResultUnknown` on failure instead of an
  unconditional `ResultSuccess`.

This does not remove JNI calls from the raw-FS path in general; it removes them from the
calls reachable from guest execution. Other Android callbacks (applets, camera) may need
the same treatment when the port starts using them.

## Follow-up: applet callbacks and `mh4u.ini` (same branch)

The same rule applies to every JNI callback the emulation core makes. The Android applets
are the next ones:

* the software keyboard (`AndroidKeyboard::Execute`, `AndroidKeyboard::ShowError`),
* the Mii selector (`AndroidMiiSelector::Setup`),
* the camera's file picker and image load (`Camera::StillImage::Factory::Create`,
  `Camera::StillImage::Interface::StartCapture`).

Their Java side now runs on a small **JNI executor thread** (`AndroidUtils::RunOnJniThread`):
a native thread created at `JNI_OnLoad`, attached to the JVM on first use, with the fiber
enqueueing a task and waiting for it; the applet callbacks are rare, so the thread and the
hand-off cost nothing noticeable. `RunOnJniThread` runs inline when called from the
executor thread itself, and the whole JNI body (local references included) stays on the
thread that runs it. A blocking dialog (the software keyboard) blocks only the executor
thread while the fiber waits — the same flow as before, on another thread.

The PC port's second-screen layout is shared core code (`Layout::ScreenOverlay`), but its
options file was not reachable on Android: `Port::Hooks::LoadOptions` opened `mh4u.ini`
with `std::ifstream`, and on Android the user folder is not the host's root. It now reads
through `FileUtil::IOFile`, so `<user folder>/mh4u.ini` works on Android as on desktop;
its `touch_overlay`/`quest_overlay`/`target_overlay` keys then drive the overlay layout
when the Android layout is set to *Custom Layout*.

Device verification of the applet dialogs and the overlay is pending (the build is in
`app-vanilla-debug.apk`; the plan is `.opencode/android_next_plan.md`). The device-free
guards are in `tests/test_android_port.py` (the patch routes the applet JNI through the
executor and `LoadOptions` uses FileUtil; `MH4U_AZAHAR_DIR` also checks the patch matches
the tree) and `tests/test_android_apk.py` (the packaged core is the freshly built one, by
ELF build ID, and the code library in the APK matches the one given to Gradle).

## The overlay's boot order (device, October 2026)

First device run: the overlay did not appear at boot. The layout is worked out when the
Android surface changes (~3.9 s in the log), and `Hooks::Install` reads `mh4u.ini` and
calls `Layout::SetScreenOverlay` a moment later (~4.1 s), on the Java thread that loads
the code library; nothing asked the renderer for the layout again, so it stayed on the
plain Custom Layout until the screen was resized (rotating the device showed the overlay
immediately, confirming it). The port now notes at install time that the boot layout was
computed before the overlay (`relayout_for_overlay`, with the portrait flag read there,
where JNI is safe) and works the layout out again on the first `sMhMain::frame` hook,
which runs with the renderer up. `ApplyQuestOverlay` already recomputes when the quest's
map/target views change.

## Widescreen that matches the screen, and the first file on Android

The PC packages write an `mh4u.ini` on first run (60 fps, 16:9, the touch screen in a
corner); the Android app had no such file, so it started at the port's built-in defaults
(native 5:3, the game's own 30 fps). It now writes the template on the first game start,
with one change: `widescreen = match`, which makes the top screen take the *frontend's*
shape. The size cannot be read from the renderer at install time (it does not exist yet
then, and the layout and the code's size tunables are fixed before the game's first area),
so the Android frontend passes the surface size with `Port::SetWindowSize` before
`PrepareBackendFor`; `Install` resolves `match` to that ratio and leaves it off for a
portrait window. The first-frame relayout above then applies it. On the desktop, `match`
stays off for frontends that never pass a size; the PC app keeps writing the fixed 16:9
template.

## Verification (device: Xiaomi M2102K1AC, Android 14, arm64-v8a)

1. Deleted the stale `data/00000001/` directory so the game would truly format.
2. `libcitra-android.so` in the installed APK has BuildId
   `c17ca1fc83d877cba19f56e9dc8fad5a50bbe2cd` (the previous build was `55be3714…`).
3. Azahar log:

   ```
   [  4.728078] Frontend <Info> main/jni/native.cpp:RunCitra:332:
                running recompiled code from libmh4u_code.so (40572 functions)
   [ 10.146076] Service.FS <Info> archive_source_sd_savedata.cpp:94:
                FormatSaveData 0004000000126300: dir guest=/sdmc/Nintendo 3DS/…/title/00040000/00126300/data/00000001/
                host=/storage/emulated/0/Games/sdmc/Nintendo 3DS/…/title/00040000/00126300/data/00000001/
   [ 10.150705] Service.FS <Info> archive_source_sd_savedata.cpp:130:
                FormatSaveData 0004000000126300: wrote 16 bytes to …/data/00000001.metadata
   ```

4. On device: `00000001.metadata` exists (16 bytes) next to the recreated `00000001/`.
5. No `StackOverflowError`, `JNI DETECTED ERROR` or `Fatal signal` in logcat; the game
   reached the title screen / Main Menu with the recompiled backend loaded.

## Commands

```bash
# build (Azahar tree with the port patch applied)
cd /home/james/Git/azahar/src/android
./gradlew assembleVanillaDebug \
  -Pmh4uPortDir=/home/james/Git/MH4UltimateDC/port \
  -Pmh4uCodeLibs=/home/james/Git/MH4UltimateDC/build/android_jniLibs \
  --no-daemon --console=plain

adb install -r -t src/android/app/build/outputs/apk/vanilla/debug/app-vanilla-debug.apk
adb shell 'rm -rf /sdcard/Games/sdmc/…/title/00040000/00126300/data/00000001*'   # force a real format
adb shell am start -n org.azahar_emu.azahar.debug/org.citra.citra_emu.ui.main.MainActivity
# tap the game card, wait, then
adb pull /sdcard/Games/log/azahar_log.txt

# regenerate the port's Azahar patch and check it reverses cleanly
cd /home/james/Git/azahar
# git diff leaves out files git does not track yet: mark the ones the patch creates first
git add -N src/core/frontend/top_screen.h src/video_core/renderer_vulkan/vk_overlay.h
git -c color.ui=never --no-pager diff --no-ext-diff --no-color --abbrev=7 \
  > /home/james/Git/MH4UltimateDC/port/azahar/azahar.patch
git apply --check --reverse /home/james/Git/MH4UltimateDC/port/azahar/azahar.patch
```
