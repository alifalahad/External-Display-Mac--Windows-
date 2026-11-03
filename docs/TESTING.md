# Testing

## Automated tests

| Suite | Runs on | Covers |
|---|---|---|
| `mac-sender/Tests/ExternalDisplayTests` (XCTest, 36 tests) | Mac | wire layout and malformed input, CRC, version; crypto vectors shared with Windows, invalid keys, tampering, nonce counters; pairing storage (0600, reload, forget, v1 removal) and approval rules; adaptive quality (hysteresis, presets, clamping); rolling statistics; input mapping (clamping, key codes, live display bounds); hardware encoder (keyframes, SPS, timing prefix room, no periodic keyframes); pointer: payload bytes shared with Windows, bitmaps from real macOS cursors (size, hot spot, orientation, premultiplied), size cap, which moves are reported (echoes, other displays) |
| `PipelineMetricsTests` (97 checks) | any OS | clock sync, latency stages, percentiles, jitter, rates and drops, keyframe counters, benchmark files, overlay text, payload layout |
| `AppLogicTests` (106 checks) | any OS | command line, status card texts, Mac selection, mouse mapping through letterbox/pillarbox, UTF-8, logging levels/file/rate limit, replay window, keyframe gate (hold, ask, retry, 300 ms limit, late/duplicate frames, renumbering) |
| `SecureChannelTests` (64 checks) | Windows | CNG self-test, HKDF (RFC 5869), HMAC (RFC 4231), ECDH/key schedule/AES-GCM against the Mac's vectors, replay and forgery rejection, DPAPI |
| `PointerTests` (70 checks) | any OS; Windows for the cursor part | pointer payloads (the Mac's bytes), hostile input, resampling, placement through the letterbox, which pointer shows when (F3, F4, Mac or PC moved last), building the Windows cursor |
| `DisplayTests` (27 checks) | any OS; Windows for the DPI part | swap chains tried per renderer mode (tearing, waitable, fallbacks), when a present may tear, per-monitor DPI awareness, scaled lengths/fonts/frames |
| `tests/integration/run-loopback.sh` | Mac + MinGW + Wine | the real Mac sender (synthetic 720p60 frames, real VideoToolbox) ↔ the real Windows receiver networking (Windows build under Wine), on test ports: new pairing, paired reconnect, impostor key, wrong code, rejected pairing; encrypted streaming, timing, start codes, clock sync, statistics; pointer drawn by the receiver (real pointer image, a scripted path checked point by point, switched off and on, nothing sent unless asked); keyframes only on request (a frame dropped on purpose must be noticed and repaired by the Mac's keyframe within 300 ms; no periodic keyframes) |

```bash
# Mac
cd mac-sender && xcodebuild test -scheme ExternalDisplay -destination 'platform=macOS'
# Integration (needs: brew install mingw-w64 wine-stable)
tests/integration/run-loopback.sh 20 x86_64      # or i686 for the 32-bit receiver
```
```bat
:: Windows
cd windows-receiver
cmake -B build -DEXDP_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

The portable C++ suites also build on macOS/Linux:
`c++ -std=c++17 -Isrc tests/AppLogicTests.cpp src/App/{Options,StatusText,Log}.cpp src/Decoder/KeyframeGate.cpp`, and
`c++ -std=c++17 -Isrc tests/PointerTests.cpp src/Input/Pointer.cpp`, and
`c++ -std=c++17 -Isrc tests/DisplayTests.cpp`.

## What only real hardware can check

Graphics drivers, hardware decoding, Wi-Fi behaviour, input feel and long runs. Per release, on
each Windows laptop:

1. **Start-up:** the receiver finds the Mac, pairs (code → Pair), and shows the picture.
   `receiver.log` shows `Decoder ... (hardware ...)` and `Frame latency: 1 frame (waitable ...)`.
2. **Reconnect:** quit and restart the receiver: it connects without a code. Restart the Mac app;
   put the PC to sleep and wake it; switch Wi-Fi off and on. Each time the picture returns
   without user action.
3. **Input:** move the mouse to all four corners of the picture (also in a window, F11) and click
   small targets near the edges. Type in a text field, including Shift/Ctrl/Alt combinations.
4. **Pointer** (F3 on): it follows the mouse with no delay and changes shape over text, links and
   window edges. Move it with the Mac's trackpad: it shows on the PC. F4 switches to the pointer
   in the video for comparison. Details: [SMOOTHNESS.md](SMOOTHNESS.md#checklist-for-step-1-on-each-laptop).
5. **Clipboard:** copy text each way. Then turn "Share clipboard" off and check nothing crosses.
6. **Quality presets:** switch Battery Saver / Balanced / High while streaming. FPS and bitrate
   follow (F2).
7. **Benchmark:** `--bench=60` with moving content; keep `bench-*-summary.txt`
   ([PERFORMANCE.md](PERFORMANCE.md)).

## Stress (30 min / 1 h / 4 h)

Play a video on the virtual display, then on the PC:
```bat
.\build\ExternalDisplayReceiver.exe --bench=1800     :: 30 min (3600 = 1 h, 14400 = 4 h)
```

`bench-*-seconds.csv` has one row per second. Look for:

| Problem | Column(s) | Sign |
|---|---|---|
| memory leak | `pc_ram_mb`, `mac_ram_mb` | steady growth (the summary prints start → end) |
| latency creep | `total_avg_ms`, `total_p95_ms` | trend upward |
| FPS degradation | `presented_fps` | trend downward |
| thermal throttling | `presented_fps` falling with `pc_cpu_pct` / `mac_cpu_pct` rising | late in the run |
| crashes | `receiver-crash.txt`; the run ends early | — |

The benchmark's histograms use fixed memory, so the recorder itself does not grow.

### Results so far

| Date | Run | Result |
|---|---|---|
| 2026-10-23 | Localhost soak, 10 min, encrypted 720p60 (Mac harness ↔ receiver under Wine) passed: 35,986/35,986 frames, 0 lost; Mac memory flat at 16–23 MB (see "Soak" in [PERFORMANCE.md](PERFORMANCE.md)) |
| — | Real hardware 30 min / 1 h / 4 h | not run yet |
