# Testing

## Automated tests

| Suite | Runs on | Covers |
|---|---|---|
| `mac-sender/Tests/ExternalDisplayTests` (XCTest, 63 tests) | Mac | wire layout and malformed input, CRC, version; crypto vectors shared with Windows, invalid keys, tampering, nonce counters; pairing storage (0600, reload, forget, v1 removal) and approval rules; adaptive quality (hysteresis, presets, clamping); rolling statistics; input mapping (clamping, key codes, live display bounds); hardware encoder (keyframes, SPS, timing prefix room, no periodic keyframes, full-range BT.709 output); capture settings (NV12, BT.709, queue depth); loss repair (parity bytes against vectors computed independently, every single loss rebuilt, parity header layout, pacing schedule); bitrate that follows the delay (cut to 85 % of what a full link carried, only its video share, no cut for a queue that emptied again, the floor, slow and fast rises only while the encoder uses its bitrate, heavy loss, limits, the RATE_FEEDBACK layout shared with Windows, the switch from the tiers); frame rate (120 FPS only for High, a 120 Hz PC screen, a 120 Hz display and an encoder that keeps up; the window's reason; a slow encoder judged over two seconds with the M2's measured times; RECEIVER_CAPS bytes shared with Windows; an encoder switched to 120 without a new session); pointer: payload bytes shared with Windows, bitmaps from real macOS cursors (size, hot spot, orientation, premultiplied), size cap, which moves are reported (echoes, other displays) |
| `PipelineMetricsTests` (102 checks) | any OS | clock sync, latency stages, percentiles, jitter, rates and drops, keyframe counters, queuing delay, benchmark files, overlay text (with the screen's refresh rate), payload layout |
| `AppLogicTests` (119 checks) | any OS | command line, status card texts, Mac selection, mouse mapping through letterbox/pillarbox, UTF-8, logging levels/file/rate limit, replay window, keyframe gate (hold, ask, retry, 300 ms limit, late/duplicate frames, renumbering) |
| `SecureChannelTests` (64 checks) | Windows | CNG self-test, HKDF (RFC 5869), HMAC (RFC 4231), ECDH/key schedule/AES-GCM against the Mac's vectors, replay and forgery rejection, DPAPI |
| `PointerTests` (71 checks) | any OS; Windows for the cursor part | pointer payloads (the Mac's bytes), hostile input, resampling, placement through the letterbox, which pointer shows when (F3, F4, Mac or PC moved last), building the Windows cursor |
| `NetworkTests` (106 checks) | any OS | loss repair: parity bytes identical to the Mac's, frame assembly (any order, duplicates), repair of every single loss, bursts up to the group count, the last fragment, unrepairable groups, hostile headers, late frames, numbering restarts, the loss simulator; the delay monitor (queuing delay whatever the clock offset, overuse/normal/hold, a draining queue is not overuse, the link's capacity only while it was full, a keyframe's burst, spikes filtered, a lasting route change, the rate at a stream start, loss, the RATE_FEEDBACK layout); the link simulator (rate, queue, tail drop, packets held until they leave the link, in order) |
| `DisplayTests` (56 checks) | any OS; Windows for the DPI and refresh-rate part | swap chains tried per renderer mode (tearing, waitable, fallbacks), when a present may tear, per-monitor DPI awareness, scaled lengths/fonts/frames; stream colours: SPS parsing (the Mac encoder's real bytes and constructed variants), YCbCr→RGB per range and matrix |
| `tests/integration/run-loopback.sh` | Mac + MinGW + Wine | the real Mac sender (synthetic 720p60 frames, real VideoToolbox) ↔ the real Windows receiver networking (Windows build under Wine), on test ports: new pairing, paired reconnect, impostor key, wrong code, rejected pairing; encrypted streaming, timing, start codes, clock sync, statistics; pointer drawn by the receiver (real pointer image, a scripted path checked point by point, switched off and on, nothing sent unless asked); keyframes only on request (a frame dropped on purpose must be noticed and repaired by the Mac's keyframe within 300 ms; no periodic keyframes); colours (the harness feeds full-range NV12 like the app; every keyframe's SPS must say full-range BT.709); loss repair: `lossy` (1 % of the packets dropped on purpose; parity must rebuild ≥ 80 % of them and ≥ 97 % of the frames arrive) and `lossy-nofec` (the same without parity or pacing, for comparison); the Mac's pacing spread reported in SENDER_STATS; bitrate that follows the delay: `narrow` (an 8 Mbps link with a 200 ms queue in front of the receiver, the Mac starting at 20 Mbps: it must go below the link within 3 s, keep the queue under 40 ms on average and deliver ≥ 90 % of the frames) and `narrow-tiers` (the same with the old loss/RTT tiers, for comparison); frame rate: `fps120` (the receiver says its screen runs at 120 Hz: ~120 FPS, captured 7–9.5 ms apart, the Mac capturing and encoding at 100+ FPS), and every other run says 60 Hz and must stay at ~60 FPS |

```bash
# Mac
cd mac-sender && xcodebuild test -scheme ExternalDisplay -destination 'platform=macOS'
# Integration (needs: brew install mingw-w64 wine-stable)
tests/integration/run-loopback.sh 20 x86_64      # or i686 for the 32-bit receiver
EXDP_MODES="narrow narrow-tiers" tests/integration/run-loopback.sh 20   # only some runs
# Logs per run in the work directory it prints: harness-<n>-<mode>.log, client-…, sender-…
```
```bat
:: Windows
cd windows-receiver
cmake -B build -DEXDP_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

The portable C++ suites also build on macOS/Linux:
`c++ -std=c++17 -Isrc tests/AppLogicTests.cpp src/App/{Options,StatusText,Log}.cpp src/Decoder/KeyframeGate.cpp`, and
`c++ -std=c++17 -Isrc tests/PointerTests.cpp src/Input/Pointer.cpp`, and
`c++ -std=c++17 -Isrc tests/DisplayTests.cpp src/Decoder/H264Sps.cpp`, and
`c++ -std=c++17 -Isrc tests/NetworkTests.cpp src/Network/{FrameAssembler,DelayMonitor}.cpp`.

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
