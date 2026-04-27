# Diagnostic Toolset

Diagnostic infrastructure for investigating main-thread responsiveness,
playback-timer jitter, and meter pipeline hot paths in Audacity 4. All
the tools below are gated behind CMake options so they compile to no-ops
in builds where they are not enabled.

This document is the surviving record of the investigation that
introduced these tools. The flags it documents have been used in the
field; the *Lessons learned* section at the bottom is the part to read
first if you're about to chase a similar bug.

## CMake flags at a glance

| Flag | Default | Effect |
| --- | --- | --- |
| `AU_PLAYBACK_JITTER_LOG` | ON  | In-memory ring buffer of every `Au3Player::updatePlaybackPosition()` tick + transport state-change events; flushed to CSV on app exit. Used with `tools/perflog/analyze_jitter.py`. |
| `AU_APPLE_SIGNPOSTS` | ON on macOS, OFF elsewhere | Compiles `AU_SP_EVENT(...)` / `AU_SP_SCOPE(...)` macros to `os_signpost_event_emit` / `os_signpost_interval_*`. Visible in Instruments under Points-of-Interest. |
| `AU_DISABLE_METER_SIGNALS` | OFF | A/B switch — short-circuits the single `channel.send(...)` loop in `AudioMeter` that fans meter samples out to QML. Audio engine still runs; UI meters stop updating. Use to confirm whether the meter dispatch path is on a freeze's critical path. |
| `AU_FORCE_QML_GC` | OFF | Schedules `QQmlApplicationEngine::collectGarbage()` once per second on a `QTimer`. Each invocation is wrapped in an `AU_SP_SCOPE("forceQmlGC")` signpost so the duration shows up in Instruments. Use to test whether ad-hoc GC pauses are responsible for an observed stall. |
| `AU_MAIN_THREAD_WATCHDOG` | ON | Background `std::thread` heartbeat watchdog. When the main thread heartbeat goes stale (>500 ms), it emits an `AU_SP_EVENT("mainStallDetected")` signpost and runs `sample(1)` to dump every thread's backtrace to disk. macOS-only; no-op elsewhere. |

Toggle any of them at configure time. Note that CMake's `option()` is
cached on first configure — editing the default in `CMakeLists.txt`
later does *not* change a value already cached. Always pass the value
on the command line:

```sh
cmake -B build/audacity-debug -DAU_DISABLE_METER_SIGNALS=ON .
cmake --build build/audacity-debug
```

## Jitter logger (`AU_PLAYBACK_JITTER_LOG`)

Captures a per-tick record of the 16 ms `QTimer` driving
`Au3Player::updatePlaybackPosition()` plus transport state changes
(`STATE_RUNNING` / `STATE_PAUSED` / `STATE_STOPPED` / two
`TIMER_STOP_*` variants).

- **Source**: `src/playback/internal/playbackjitterlogger.{h,cpp}`,
  hooks in `src/playback/internal/au3/au3player.cpp`.
- **Buffer**: in-memory ring buffer, 32 768 entries (~8.7 min at
  16 ms), single-threaded (Qt main).
- **Flush**: triggered from
  `PlaybackContext::onDeinit()` — writes a CSV at
  `<userAppData>/perf_logs/playback_jitter_<yyyyMMdd_HHmmss>.log`.
- **CSV schema**:
  ```
  # playback_jitter v1
  # steady_start_ns=...  system_start_iso=...  sample_rate=...  capacity=...  overflowed=0|1  total_entries=N
  # wall_ns,dac_ns,event_type,consumed_samples
  <wall_ns>,<dac_ns or empty>,<EVENT_NAME>,<consumed_samples>
  ```
- **Analyzer**: `tools/perflog/analyze_jitter.py`. One-time setup:
  ```sh
  python3 -m venv tools/perflog/.venv
  tools/perflog/.venv/bin/pip install -r tools/perflog/requirements.txt
  ```
  Then run, with the most recent log auto-discovered:
  ```sh
  tools/perflog/.venv/bin/python tools/perflog/analyze_jitter.py
  ```
  Produces three plots in `<log-dir>/out_<ts>/`:
  - **histogram.png** — log-scale distribution of inter-arrival times,
    16 ms target and p50/p95/p99 marked.
  - **timeline.png** — IAT over session time, with vertical lines for
    transport events. **IAT is reset across timer-halt events** so
    idle gaps don't show up as multi-second outliers.
  - **dac_drift.png** — wall-clock minus DAC time over the session.

Use this when the question is "is the playback timer firing on time?"
not "is the UI hanging?".

## Apple Instruments signpost helper (`AU_APPLE_SIGNPOSTS`)

Header-only helper at `src/common/diagnostics/ausignposts.h` that wraps
Apple's `os_signpost` API. Two macros:

- `AU_SP_EVENT("name")` — point-in-time signpost.
- `AU_SP_SCOPE("name")` — RAII; emits a signpost interval whose
  duration is the lifetime of the local variable.

Both compile to no-ops when `AU_APPLE_SIGNPOSTS` is undefined or on
non-Apple platforms.

The helper uses `os_log_create("org.audacityteam.Audacity",
"PointsOfInterest")` so signposts land in the well-known
*PointsOfInterest* category. The CPU Profiler and the *Blank → Time
Profiler + os_signpost* templates auto-display them — no manual
subsystem/category filter required.

Existing call sites (committed alongside the helper) cover the
playback hot paths and both meter pipelines:

- `Au3Player`: state transitions + `play / stop / pause / resume /
  doPlayTracks / updatePlaybackPosition / updatePlaybackState`.
- `PlaybackController`: `togglePlayAction / pauseAction / stopAction`,
  `onPlaybackPositionChanged`.
- `PlaybackMeterPanelModel` (master meter) — channel receive lambda +
  per-pressure/RMS setters.
- `MeterModel` — `fullSteps()` / `smallSteps()` / `setMeterSize()`.
- `WaveTrackItem` (per-track meter) — playback / record / main-input
  channel lambdas + setters.

Add new ones liberally — the macros expand to nothing when the flag is
off, so cost is zero in release.

## Meter-signal disable (`AU_DISABLE_METER_SIGNALS`)

A single `#ifndef` wraps the `channel.send(...)` loop in
`AudioMeter::m_playingTimer`'s callback
(`src/audio/internal/audiometer.cpp`). When the flag is on:

- Audio thread keeps producing meter samples.
- The internal 60 Hz meter timer keeps decaying levels.
- Nothing is dispatched to subscribers (`PlaybackMeterPanelModel`,
  `WaveTrackItem`, the toolbar level items, etc.) — UI meters
  freeze at their initial state.

Use this as an A/B switch: **does the freeze still happen with no
meter dispatch?** If yes, the bottleneck is somewhere else; if it
shrinks/disappears, the meter pipeline is on the critical path.

## Forced QML GC (`AU_FORCE_QML_GC`)

In `GuiApp::doStartupScenario`, when the flag is on, a `QTimer` is
started that calls `QQmlApplicationEngine::collectGarbage()` once per
second on the main thread. Each invocation is wrapped in
`AU_SP_SCOPE("forceQmlGC")` so its duration appears as a bar in
Instruments.

Use this to falsify the GC-pause hypothesis when investigating long
main-thread freezes. Typical signal:

- If forced GC bars are short and steady (~20 ms each), GC is not the
  source of the long freezes.
- If forced GC bars themselves are multi-second, you are the freeze.

In the original investigation that produced this toolset, the forced
GC bars were 14–27 ms — clearly not the cause.

## Main-thread stall watchdog (`AU_MAIN_THREAD_WATCHDOG`)

The most useful tool of the set when chasing UI hangs you can't
reliably catch under Instruments.

- **Source**: `src/common/diagnostics/mainthreadwatchdog.{h,cpp}`,
  started from `GuiApp::doStartupScenario`.
- **Mechanism**:
  1. A 100 ms `QTimer` on the main thread calls `beat()` on every
     fire (single relaxed atomic store).
  2. A background `std::thread` checks every 100 ms; when
     `now - lastBeat > 500 ms` it considers the main thread stalled.
  3. On detection it emits `AU_SP_EVENT("mainStallDetected")` —
     marker visible at the freeze onset on the Instruments timeline.
  4. It then runs `sample <pid> 1 -file <userAppData>/stall_samples/freeze_<ts>_seq<N>_<ms>ms.txt`
     to capture every thread's backtrace over a 1 s window.
  5. After the heartbeat resumes, the watchdog re-arms; the next
     stall increments `<N>` and produces a new file.
- **Filename convention** encodes both the stall duration and the
  sequence number so a quick `ls -t` shows you what happened in
  what order.

`sample(1)` works on same-user processes without sudo. Linux/Windows
builds compile the watchdog but the capture call is a no-op.

To read a capture: open the `freeze_*.txt` and jump to the
`Thread_*: com.apple.main-thread (serial)` block. The call tree
shows where every sampling moment landed during the 1 s window;
the deepest non-system frame is what the main thread was busy with.

## Worked example — what a stall capture looks like

Excerpt from a real `freeze_*.txt` captured during the original
investigation (~5 s freeze):

```
    691 Thread_…   DispatchQueue_1: com.apple.main-thread  (serial)
    + 691 start  (in dyld) + …
    +   691 main  (in audacity) + … main.cpp:198
    +     691 QCoreApplication::exec()  …
    +       691 QEventLoop::exec(…) …
    +         691 QCocoaEventDispatcher::processEvents(…) …
    +           691 -[NSApplication run]  …
    +             …
    +                   ! :         |     +                 !   :   |
                          +                 !   :   |
                          + 99 QQuickWindow::event(QEvent*)  …
    +                                                 99 QSGThreadedRenderLoop::handleUpdateRequest(…)
    +                                                 ! 68 QSGThreadedRenderLoop::polishAndSync(…)
    +                                                 ! ! 58 QQuickCanvasItem::updatePolish(…)
    +                                                 ! !   …raster paint chain…
```

The numbers on each line are how many samples landed at that depth
during the 1 s capture window. In this case 75 % of the time was
`mach_msg` (idle in kernel) and 14 % was QML Canvas paint via
`QQuickCanvasItem::updatePolish`. Reading the deepest non-system
frame in the busy branch is the fastest way to a hypothesis.

## Lessons learned (read this first)

The original 4.8 s freeze that triggered this whole investigation
turned out to be a **measurement artifact**, not a real Audacity bug.
The freezes only appeared when the app was running under **Xcode
Instruments with the Allocations instrument enabled**. The Allocations
instrument injects `liboainject.dylib` into the target process, which
interposes every `malloc` / `realloc` / `free` and walks the stack
on each call.

Audacity's QML Canvas-based meter widgets allocate aggressively during
paint (paths, font glyphs, span buffers). Without instrumentation a
paint cycle takes ~50 ms; with `liboainject` capturing a stack on
every alloc it balloons to ~5 s — and the periodic ~17 s rhythm of
the long freezes matches Allocations' internal buffer flush cadence.

**Implications**:

1. **Don't trust freeze numbers measured under Instruments-Allocations.**
   Re-confirm any hang reproducibly *without* Instruments before
   spending time on it. The watchdog above is the right tool for that
   — it captures live with no instrumentation overhead.
2. The underlying Audacity behaviour (QML Canvas paint of meters) is
   on the order of ~50 ms / paint cycle, which produces ~500 ms
   stalls every 20–30 s in a stress scenario. Whether that's worth
   fixing depends on the user-perceived UX; the structural fix would
   replace the QML `Canvas { onPaint: ... }` widgets with C++
   `QQuickPaintedItem` subclasses.

## Pointers

- Diagnostic helpers: `src/common/diagnostics/`
- Jitter logger sources + Python analyzer: `src/playback/internal/playbackjitterlogger.{h,cpp}`,
  `tools/perflog/`
- CMake flag definitions: top-level `CMakeLists.txt` (search for `AU_`
  prefixes); per-flag effects are localised to:
  - `AU_PLAYBACK_JITTER_LOG` — `src/playback/CMakeLists.txt`
  - `AU_APPLE_SIGNPOSTS` — top-level only
  - `AU_DISABLE_METER_SIGNALS` — `src/audio/internal/audiometer.cpp`
  - `AU_FORCE_QML_GC` — `src/app/guiapp.cpp`
  - `AU_MAIN_THREAD_WATCHDOG` — `src/app/guiapp.cpp` +
    `src/common/diagnostics/mainthreadwatchdog.{h,cpp}`