# EncxdedCanvas — Execution Plan

Companion to [Overview.md](Overview.md). Phases are ordered so each one ships something testable on the tablet, and so the risky engine work (strokes, performance) is isolated from the UI work.

## What already exists (from InfiniPaint)

The overhaul builds on these instead of replacing them:

| Area | Existing code | Gap vs. Overview |
|---|---|---|
| Grids | `src/WorldGrid.*`, `src/GridManager.*`; types `CIRCLE_POINTS`, `SQUARE_POINTS`, `SQUARE_LINES`, `HORIZONTAL_LINES`; drawn with Skia runtime shaders | No coarse-level fading when zoomed out, no screen-anchored modes, no width / box-ratio controls in an Ahmni-style panel |
| Still exports | `src/DrawingProgram/Tools/ScreenshotTool.cpp`, `src/WorldScreenshot.*`: SVG, PNG, JPG, WEBP | Only needs re-skinning into the new top-right camera button |
| Strokes | `src/CanvasComponents/BrushComponentCode.cpp`: stroke → polygon (Clipper2) → `SkPath` of line segments | Faceted when zoomed in. Needs curve fitting |
| Screens | `src/Screens/FileSelectScreen.*` (file browser), `CanvasEditorScreen.*` | No main menu with sidebar (Notes / Schedule / Goals / Settings) |
| Config | `src/GlobalConfig.*` (app-wide) | No per-canvas profile that new canvases inherit |
| Video export | none | MP4 + mic is entirely new |

Codebase size: about 44k lines of C++ across 132 `.cpp` files. It uses SDL3, Skia, Vulkan, and Conan.

---

## Phase 0 — Fast dev loop (do first, about 1 day)

3. **Android build.** Upstream says Windows Android builds fail because of Conan packages, and recommends Linux. You already have the **Ubuntu WSL2** distro, the **Android SDK**, and **NDK 30.0.16248370**, so:
   - Try the Windows-native Android build once with Android Studio and `conan/profiles/android`. If it works, use it.
   - If it fails, build the APK inside WSL2 (with a Linux SDK/NDK installed in WSL) and install it from Windows over adb.
4. **Wireless deploy to the tablet** (Android 11+):
   - Tablet: Settings → Developer options → **Wireless debugging** → *Pair device with pairing code*.
   - PC: `adb pair <ip>:<pair-port>` (one time), then `adb connect <ip>:<port>`.
   - Add a `scripts/deploy-android.ps1` that runs `gradlew :app:installDebug` followed by `adb shell am start -n <appId>.debug/.<MainActivity>`, and `adb logcat` filtered to the app.
   - `adb.exe` lives in `%LOCALAPPDATA%\Android\Sdk\platform-tools`, which isn't on PATH yet. The script will add it.
   - Build **only `arm64-v8a`** for dev builds. The ABI filter currently also builds `x86_64`, which roughly doubles native compile time.
5. **Optional:** **scrcpy** mirrors the tablet screen onto the PC so you can record and screenshot sessions. A USB cable is still faster than Wi-Fi for large APKs.
6. Rename `applicationId` from `com.erroratline0.InfiniPaint` to **`com.joshcarter.encxdedcanvas`**. It then installs alongside InfiniPaint instead of colliding with it.

**Status (2026-10-27):** Windows-native Android build works. These fixes were needed on this machine:
- **Conan HTTPS:** an intercepting certificate breaks Conan's SSL. Fixed with `core.net.http:cacert_path` in `~/.conan2/global.conf`, pointing to an export of the Windows root certificates (`~/.conan2/windows-roots.pem`).
- **Run Conan from PowerShell, not Git Bash.** From Git Bash, the build subprocesses can't find `cmake`.
- **Meson:** `~/.conan2/profiles/android` pins Meson 1.4.0 via `[replace_tool_requires]`. Meson 1.10 rejects the `C:/` prefix when cross-compiling.
- **Skia recipe:** chose `bin\gn` vs `bin/gn` using the target OS instead of the build OS. Fixed in `conan/conan-skia/recipes/skia/all/conanfile.py`.
- **Gradle `conanInstall`:** now only installs the requested build type and ABIs, instead of all four combinations on every invocation.
- **JDK:** Gradle 8.13 needs JDK 17/21. The system Java and Android Studio's bundled Java are both 25, so the build uses `~/.jdks/jbr-21.0.11`.
- **CMake:** Gradle expects 3.31.6, which isn't installed. This machine uses system CMake 4.4.2 via `cmakeVersion` in `~/.gradle/gradle.properties` and `cmake.dir` in `local.properties`.
- **Incremental APK build:** about 2 minutes. `scripts/deploy-android.ps1` builds, installs, launches and streams logs.

Target device: **Galaxy Tab S10 Lite** (arm64, S Pen with pressure). Wireless debugging is already confirmed working through scrcpy.

## Phase 1 — Ahmni-style canvas chrome

Matches `ahmni-example-mm_collage` and `ahmni-example-screenshot (1)`:

- **Top-left cluster** (stacked rows, about 6 cells each):
  1. Back · hide-UI (eye)
  2. Zoom row: fit-to-content · current % · lock-rotation · 50% / 100% / 200% presets
  3. Tool row: tool dropdown ▾ · lasso · eraser · stroke eraser · pen · line
  4. Size row: 6 user-editable size presets
  5. Color row: palette dropdown ▾ · 5 swatches. Long-press edits a swatch.
- **Top-right cluster:** play/record · split view · bookmarks · camera (export) · settings · help.
- **Settings panel** (from `MainSettings-encodedcanvas-idea`): Grid OFF/LINES/DOTS, Width slider, Box Ratio slider, Background presets (Custom / Default / Light / Blueprint).
- Dark navy theme tokens (`CanvasTheme.*`, GUI colors) with rounded tiles.
- Existing InfiniPaint tools that don't fit the bar (shapes, text, image, layers) move behind the ▾ dropdowns rather than being removed.

## Phase 2 — Grids

Extends `WorldGrid` / `GridManager`:

- **Anchor modes:** `CANVAS` (current behaviour) · `SCREEN` (fixed on screen) · `SCREEN_ROTATING` (fixed on screen but follows view rotation).
- **Canvas-mode level of detail:** when on-screen spacing falls below a threshold, fade the grid out and fade in a 4× coarser grid. The multiplier will be configurable. This happens in the shader by blending two levels based on `log4(spacing)`.
- Line width and box ratio (non-square cells) as shader uniforms.
- Preferably serialized per canvas, so it is saved with the canvas profile.

## Phase 3 — Smooth strokes

Currently each stroke is a polygon made of straight segments, which looks faceted when you zoom in.

- Keep the **raw input samples** (position, pressure, time) as the source of truth.
- At render time, fit a centripetal Catmull-Rom spline (or a cubic Bézier fit) to the centerline. Width is interpolated from pressure along the curve, and the outline is generated as `SkPath` cubics. Skia then rasterizes these smoothly at any zoom.
- Tessellate adaptively based on on-screen size, so zooming in adds detail and zooming out costs less.
- Use the existing Clipper2 path only where boolean operations are needed (eraser).
- **Compatibility (decided):** EncxdedCanvas gets its own file format and doesn't need to stay compatible with InfiniPaint, so a format change is fine. It must offer a **one-way import of InfiniPaint `.infpnt` canvases**. Live collaboration with InfiniPaint users is not required.

**Already in place before Phase 3 started (found 2026-10-27):** the `ENCXCV000001` file header (InfiniPaint versions still load, which is the `.infpnt` import), strokes storing their processed pen points (`penPoints`) plus detail-eraser cuts (`eraseOps`), quad-curve rendering through outline midpoints (`smooth_polygon_path`), and the screen-space clipped draw for huge strokes.

**Decisions (2026-10-27, Phase 3 start):**
- **Source of truth: raw samples** (position, pressure, time). Stabilizing, pressure→width and smoothing run when the outline is built.
- **Settings are locked per stroke:** each stroke saves the pressure-curve and stabilizer values it was drawn with, so changing a setting only affects new strokes.
- **Detail eraser cuts the centerline** (Concepts-style): erasing splits the stroke's samples into pieces that stay smooth strokes with caps. A section is cut wherever the eraser **touches the visible ink**, so brushing the edge of a thick stroke cuts it.
- **Caps:** round only (no taper option).
- **Pressure curve:** one soft↔firm slider in tablet settings.
- Imported InfiniPaint strokes from 0.6+ only have outlines, so they stay outline strokes (quad-smoothed, cut by the old outline eraser).

**Status (2026-10-28): implemented and deployed to the tablet, not yet tested by hand.**
- **Format:** `ENCXCV000002` (1.1.0). A stroke saves either its raw samples plus `StrokeParams`, or processed points (line tool and older strokes), followed by `penWidthScale`, `keptRanges` and the legacy `eraseOps`. The outline (`meshPath`) is still saved, so loading doesn't rebuild every stroke. 1.0 and InfiniPaint files still load.
- **Raw capture:** `BrushTool` records `RawSample {pos, pressure, time}` on every motion and pressure event, and live drawing runs the same `process_raw_samples` that rebuilds saved strokes, so the two always match. The stabilizer became a pure function (`add_motion_point`), with its pixel distances scaled by `StrokeParams::unitScale` after normalization.
- **Centerline:** `stroke_centerline` (centripetal Catmull-Rom) subdivides each piece by how much it turns: 3 to 32 pieces, about 4.5° each. Rendering is unchanged: quads through the outline midpoints, plus the screen-space clipped draw.
- **Centerline eraser:** `centerline_ranges_touching` walks the centerline in quarter-width steps and bisects each boundary. `keptRanges` are arc-length intervals, and slivers shorter than 0.1× the width are dropped. `attempt_split` turns each range into its own stroke (each keeps the full raw samples).
- **Thickness changes** (`scale_stroke_thickness`) now set `penWidthScale`, so the centerline and its ranges never move.
- **Pressure curve:** tablet setting `brushPressureCurve` (−1 firm … 1 soft), giving `pressure^(2^-curve)`.
- **Test on the tablet:** draw at 10,000% zoom and compare with old strokes. Try partial erasing (brushing the edge of a thick stroke should cut it, and each piece should then be selectable on its own), thickness scaling after erasing, undo of an erase, save and reload, and the pressure curve slider.

## Phase 4 — Performance

Profile before changing anything:

- Add **Tracy** (or Perfetto on Android) behind a CMake option and capture traces on the tablet. Check frame time while drawing, panning, and zooming over a dense canvas.
- Likely areas to examine: `DrawingProgramCache` invalidation, per-frame path rebuilding, input-to-photon latency (SDL event batching, presenting with FIFO vs. MAILBOX in `VulkanContext`), and Android pen sampling.
- Set target numbers once there's a baseline, for example a 120 Hz frame budget and pen latency comparable to Concepts.

**Decisions (2026-10-27):**
- **Zoom lag (seen on the tablet whenever drawing, zooming or rotating; worse the deeper you zoom), decided: general fix.** When a stroke is much larger than the screen, clip it to the view first. Off-screen curve pieces become their chord, which is invisible and keeps the fill correct. Visible pieces are subdivided according to their on-screen bend, and the result is drawn in screen space with no CDT. Edge tolerance is ¼ px, or adaptive (coarse while moving) if that gives a large gain. **Implemented** in `MeshCanvasComponent::get_screen_space_path`. It's used when a stroke is more than 2× the view, and always past the old 16384× "accurate" threshold, where it replaces the CDT path. ¼ px was chosen because adaptive only saves about 1.4×. Waiting on tablet feedback.
- **Priorities:** drawing on dense canvases ≥ pan/zoom smoothness > pen latency. Pen latency can give way to smoothness, as long as it doesn't get too bad. A Concepts-style "low latency mode" is welcome.
- **Workflow:** deploy to the tablet freely. **Never commit.** The user handles git.

## Phase 5 — Canvas profiles

- Split settings into **app settings** (GlobalConfig) and **canvas settings**: grid, background, palette, size presets, tool defaults.
- Store canvas settings inside the canvas file.
- **Concepts-style default:** a new canvas copies the canvas settings of the last opened canvas.
- **Named profiles:** save the current canvas settings as a profile and apply one to any canvas.

## Phase 6 — Main menu

Replaces `FileSelectScreen` with a sidebar shell (`ahmni-example-screenshot (3)`, `folders3`). Sidebar order: Notes · Schedule · Goal Cycle — divider — Sessions · Backup · Settings · Trash. The sidebar collapses with the top-left button.

- **Notes / Canvases** (`folders3`, decided): a **list**, not a thumbnail grid. Rows for folders and canvases with an optional emoji prefix, file size and a ••• menu. Breadcrumbs, back/forward, a new-folder button, a **Focus** button that opens Sessions, and a yellow floating + button for a new canvas.
- **Schedule:** kanban board with columns (1 Day / 1 Week / 1 Month Retrieval, user-editable, each with an editable interval in days). Cards link to a canvas, are draggable between columns, and have a Templates tab.
  - **Number (decided):** days relative to the due date (negative = overdue). Due date = when the card entered its column + that column's interval.
  - **Colour (decided):** green = not due, yellow = due today or soon, red = overdue.
  - **Mark retrieved (decided):** in the card's ••• menu. It moves the card to the next column and resets the due date. Dragging a card also resets the due date.
- **Goal Cycle** (`313x235bb`, `ahmni-example-screenshot (2)`), decided: **multiple goals**. A kanban of goal cards (Todo / In Cycle / Monitoring / Out) with progress rings and a Board / Templates tab. Tapping a goal opens the detail view: goal title, progress slider and ring, entries with a checkbox, title, subtitle and date, plus *Add Canvas* / *Add Entry*, with Done to go back.
- **Sessions** (`focus_cobalt`, `ahmni-example-screenshot (4)`): a "New Session" popup opened from Focus or the sidebar. Before / Focused / After tabs along the bottom. Item types: checkbox, text field (Study Goal), 1–5 rating (Rate Energy) and timer (Pomodoro minutes slider / Flowmodoro stopwatch). Add Item and Hide.
  - **Focused phase (decided):** Start Session hides the popup and shows a **floating timer pill everywhere, including inside canvases**. When the timer ends or you press stop, the After checklist opens.
- **Backup (decided):** export and import one `.zip` with all canvases, folders and menu data (schedule, goals, sessions, settings). No automatic backups.
- **Trash:** deleting a canvas moves it to Trash. From there you can restore it, permanently delete it, or empty the whole Trash. Nothing is removed from disk without an explicit action.
- **Settings:** InfiniPaint settings, plus Concepts features added over time, plus a **theme picker**.
- **Themes (decided, `themes_no_bg`):** all 5 now, applied to the menu and canvas chrome: Charcoal/Blue, Teal, Navy/Yellow (**default**), Light/Blue and Light/Navy. Canvas background presets stay separate.
- *Library is skipped for now.*
- Data is stored as a single local JSON file in the app data directory.

**Status (2026-10-27, later):** steps 1–4 below are done. All menu files compile and are in CMake, `MainMenuScreen` has replaced `FileSelectScreen`, and it runs on the tablet. Backups use the native save/open dialog. On Android, SDL's dialog had no file-name parameter, so `SDLActivity.setSaveDialogSuggestedName` was added (it's in proguard). Safety backups go to `backups/` (newest 3 kept), and a **Roll Back** button on each one restores it with the backup winning instead of merging. The file_paths.xml change (step 2) wasn't needed. **Remaining:** step 5, testing on the tablet.

**Handoff (2026-10-27), Phase 6 status:**
- **Done and compiling** (in `CMakeLists.txt`): `src/Encxded/MenuData.*` (data model, saved to `encxdedMenu.json`), `MiniZip.*` (store-only zip), `MenuWidgets.*` (buttons, round checks, ring, modal `Dialogs`, `popup_menu`), `SessionsGUI.*` (popup and timer pill), `EncxdedApp.*` (`Encxded::app(main)` singleton). Themes are in `GUIStuffHelpers.cpp`; icons are in `assets/data/icons/encxded/`.
- **Written but never compiled** (not in CMake yet): `MainMenuScreen.hpp/.cpp`, `MainMenuNotes.cpp`, `MainMenuBoards.cpp`, `FsUtil.hpp`.
- **Next:**
  1. Write `MainMenuBackup.cpp`. Add the `PendingFileChoice` struct (mutex) to `MainMenuScreen.hpp` for file-dialog results, which can arrive on another thread, and handle them in `update()`. Zip `saves/**`, `encxdedMenu.json`, `config.json`, `palettes.json` and `themes/**`. Before restoring, save a safety backup to `backups/` and merge rather than delete, then `app.store.load()` and `main.load_config()`. On Android, share via `shareInternalFiles({"backups/x.zip"}, "application/zip", true)`.
  2. Add `<files-path name="encxded_backups" path="backups/" />` to `android-project/.../res/xml/file_paths.xml`.
  3. Add the four menu `.cpp` files to CMake. Replace `FileSelectScreen` with `Encxded::MainMenuScreen` in `main.cpp:592` and `CanvasEditorScreen.cpp` (2 places).
  4. In `CanvasEditorScreen`, call `Encxded::app(main).update()` in `update()` and `.overlay_layout()` at the end of `main_display()`, and route back presses through `app.input_global_back_button()`.
  - **Backup decisions (2026-10-27):** on restore, when a canvas or folder path clashes, the **newer file wins** (by modification time). Menu data is **merged**: the backup's schedule cards, goals and templates are added alongside the current ones, and settings come from the backup. A backup item with the same ID and title as an existing one is skipped, so the current version is kept. A card or goal whose column doesn't exist goes to the column with the same name, or the backup's column is appended. **Create backup asks for a destination** with a system save dialog (on Android too). Low latency mode: decide after profiling.
  5. Compile, fix errors, and test on the tablet. Check especially: drag (long press 350 ms on touch), the `•••` menu anchors, and the modal "tap outside to close".

## Phase 7 — Exports

- **Stills:** re-skin the existing SVG / PNG / JPG export into the camera button.
- **MP4 with optional microphone (decided: replay render):**
  - **While recording:** log a timeline of every edit, camera move, tool or UI state change, and pointer position, all timestamped. Microphone audio is recorded through SDL3 audio capture into the same session with matching timestamps. Live drawing does no video encoding, so it isn't slowed down.
  - **On export:** replay the timeline offscreen in the background at the chosen resolution and frame rate, and mux it with the recorded audio.
  - **Export option:** *include UI* (toolbar, cursor and pen indicator are drawn from the logged UI state) or *canvas only*.
  - **Recording controls (decided):** the top-right record button starts recording. A red pill shows the elapsed time with pause, stop and a mic on/off toggle (the mic setting is remembered). Stop saves the recording and offers *Export now*.
  - **Storage (decided):** recordings are **separate files beside the canvas**, not inside it. The camera/export panel gets a **Video** tab that lists the canvas's recordings (name, length, date) with Export MP4 / Rename / Delete.
  - **Export options (decided):** resolution 720p / 1080p (default) / 1440p / 4K at the recording's aspect ratio. 30 (default) or 60 fps. Include UI on/off. No trimming or speed change.
  - **Encoding:** use the platform encoder (MediaCodec on Android, Media Foundation on Windows). This avoids FFmpeg's size and licensing questions.
- **Handoff (2026-10-27), Phase 7 status:** only the export-mode exit below is done. Stills already work through the camera button. MP4 hasn't been started. The icons are ready (`encxded/record.svg`, `mic.svg`, `mic-off.svg`, `video.svg`). `SessionsGUI::play_chime` shows how to start SDL audio on demand (`SDL_InitSubSystem(SDL_INIT_AUDIO)`); the mic capture will need the same. Suggested order:
  1. Timeline recorder: serialize edits, camera and pointer with timestamps.
  2. Mic capture to a sidecar file next to the canvas.
  3. Video tab in the export panel.
  4. Offscreen replay renderer.
  5. Encoders: MediaCodec via JNI, then Media Foundation.
- **Decisions (2026-10-27, Phase 7 start):**
  - **Include UI = exact UI.** A snapshot of the GUI layer (`gui.io.surface`, which only redraws when the UI changes) is saved whenever it changes, and replay composites it on top.
  - **Camera:** an *Exact / Smoothed* choice in the export options.
  - **Interruptions:** going to the background pauses the recording, and paused time is cut. Leaving the canvas stops and saves it.
  - **Encoders:** Android MediaCodec first, Windows Media Foundation afterwards.
  - **The recording pill is hidden in videos.** Its rectangle is cut out of the UI snapshots, and it sits where no other UI normally is.
  - **MP4 output:** Android saves to the Gallery (`Movies/EncxdedCanvas`, through MediaStore).
  - **Export runs in the background with a progress pill and Cancel.** It renders gently while a canvas is open and at full speed on the main menu.
  - **Storage:** a hidden `<canvas>.recordings/` folder next to the canvas holds `<name>.encrec` (zstd timeline), `<name>.wav` and `<name>.json`. Recordings follow the canvas when it's renamed, moved or trashed.
  - **Known gap:** tool overlays drawn inside the world (selection boxes, lasso path) aren't in the replay. The UI layer and a pen/cursor indicator are.
  - **Approach:** a local canvas is a network server, so the recording acts like a virtual client. The server's outgoing messages (initial state plus every update) are saved with timestamps. Replay builds an offscreen client world and feeds it those messages.
- **Status (2026-10-27, end of session):** recording through Android MP4 export works end to end on the tablet. Tested: record, draw, stop, Video tab, 720p export, Gallery. The pulled MP4 showed replayed strokes, the UI layer, the pen indicator, and no recording pill.
  - **Engine taps:**
    - `NetServer::add_recording_client` / `recordingSink`
    - `NetClient(ReplayTag)` / `push_replay_message`
    - `World::start_recording_client`, private `recordServer`, `broadcast_server()`, `isReplay`
    - `GUIManager::take_redrawn_area`
  - **New files:**
    - `src/Encxded/`: `Recording.hpp` (format), `Recorder.*`, `Replayer.*` (`RecordingReader` + offscreen replay with `takingScreenshot`), `VideoEncoder.*` (NDK `AMediaCodec`/`AMediaMuxer`, AAC from the WAV), `VideoExport.*` (job), `VideoTabGUI.*`
    - `src/Screens/CanvasEditorUI/CanvasEditorRecording.cpp` (record button and pill)
    - Java: `InfiniPaint.saveVideoToGallery`
  - **Not yet tested:** the microphone (permission prompt, sync), Smoothed camera, rotating the tablet mid-recording, 1440p and 4K, 60 fps, hosting while recording, and recordings cut off by a crash.
  - **Remaining:**
    1. Windows Media Foundation encoder. Desktop currently says it's unsupported.
    2. Async UI readback. `readPixels` is synchronous now, so opening a menu while recording can cause a hitch.
    3. Tool overlays in replays (the known gap above).
    4. Remember the export options in MenuData. They're per session now.
- **Decisions (2026-10-27, finishing Phase 7):**
  - **Windows build:** set up a real Windows desktop build so the Media Foundation encoder is compiled and tested here.
  - **Windows output:** MP4s save automatically to `Videos\EncxdedCanvas`, with a notification giving the path.
  - **Tool overlays:** recorded whenever they change, as a screen-space vector picture (`SkPicture`), and composited in replay (transformed to follow the Smoothed camera). They are **part of Include UI**: canvas-only videos leave them out.
  - **Export options:** remembered **globally** in MenuData.
- **Status (2026-10-27, Phase 7 finished in code; needs testing on the tablet):** all four remaining items are done. The user mainly uses Android, so Windows only gets a compile check.
  1. **Windows Media Foundation encoder** (`VideoEncoder.cpp`, `_WIN32` branch). It's an `IMFSinkWriter` with NV12 input to H.264 and PCM to AAC, and MP4s save to `Videos\EncxdedCanvas`. A Windows Release build now works (`build-x86_64/`, Conan profile `win-x86_64`, `cmake --preset conan-default` then `cmake --build --preset conan-release`). The encoder compiles but hasn't been run.
  2. **Async readback:** recorder UI patches and export frames use `asyncRescaleAndReadPixels` (Ganesh), with up to 3 export frames in flight, encoded in order. A synchronous fallback remains.
  3. **Tool overlays:** new record types `OVERLAY` (serialized `SkPicture` of `DrawingProgram::draw_overlays`) and `SELECTION` (selected IDs plus their draw camera). The replay hides selected components from the layers (`DrawData::hiddenComponents`) and draws them with the recorded camera, so drags, scales and rotates show live. Overlays map through the recorded→output camera transform, so they follow Smoothed.
  4. **Export options** are saved in `MenuData::videoExport`.
  - **Test on the tablet:** select and drag strokes while recording, lasso while recording, opening menus while recording (should no longer hitch), and export speed.
- **Export mode exit (done):** the first tap outside the export rectangle clears it. A tap on the dimmed screen with no rectangle returns to the previous tool and closes the export panel. Dragging still draws a rectangle, and a second finger cancels the tap.

## Phase 8 — Staying close to upstream

- Add `upstream` → `ErrorAtLine0/infinipaint` as a git remote.
- Keep new features in new files and directories (`src/Encxded/...`) where possible, and keep edits to upstream files small and marked.
- Merge upstream periodically (for example monthly) and record conflicts in this file.

## Suggested order and checkpoints

Priority: new look and smooth strokes first, speed later.

| Order | Phase | Deliverable you can test on the tablet |
|---|---|---|
| 1 | 0 | Wireless one-command deploy of the renamed app |
| 2 | 1 | New toolbar plus settings panel on top of existing tools |
| 3 | 3 | Smooth strokes (new format plus `.infpnt` importer), compared before/after at 10,000% zoom |
| 4 | 2 | Grid modes and fading |
| 5 | 6 | Main menu: Canvases + Trash → Schedule → Goal Cycle → Sessions |
| 6 | 5 | Canvas profiles |
| 7 | 7 | Replay MP4 export with mic and a UI on/off option |
| 8 | 4 | Profiling numbers, then targeted fixes |
