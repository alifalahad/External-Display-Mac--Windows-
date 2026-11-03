# Performance

How the pipeline is measured, how to run a benchmark, and what has been measured so far.
Every number in this file and on the diagnostics overlay is a measurement. Nothing is estimated
or filled in.

## What is measured

Each video frame carries timestamps from capture on the Mac to scan-out on the PC.

| Stage    | From → to | Where |
|----------|-----------|-------|
| Capture  | frame on the Mac's screen (ScreenCaptureKit `displayTime`) → handed to VideoToolbox | Mac |
| Encode   | handed to VideoToolbox → encoder output callback | Mac |
| Send     | encoder output → UDP packetization starts (AVCC→Annex B, queue hop) | Mac |
| Network  | packetization starts → last fragment received | Mac→PC (clock-synced) |
| Queue    | frame complete → decode starts (waits for the render loop) | PC |
| Decode   | decode starts → NV12 picture out of Media Foundation | PC |
| Render   | NV12 out → `Present()` returned (CPU copy, texture upload, draw, present) | PC |
| Display  | `Present()` returned → vblank where the frame was scanned out (DXGI frame statistics) | PC |
| Total    | Mac `displayTime` → scan-out on the PC (to `Present()` if the driver gives no frame statistics) | clock-synced |

**Wire format.** Video packets have the `timing` flag (bit 1). The reassembled frame then starts
with a 40-byte `FrameTimingPrefix`: frame number, flags, capture, encode start, encode end and
send times, all in Mac host-clock µs. The receiver strips it before decoding. A receiver also
accepts frames without the prefix, so it still works with an older Mac build.

**Clock sync.** Network and Total compare Mac and PC timestamps. The receiver sends a PING once
per second (five at 200 ms intervals when a stream starts). The PING carries its QPC time; the
Mac's PONG returns the Mac clock and echoes the PING time. Of the last 8 round trips, the one with
the smallest RTT gives the offset, as in NTP's clock filter. The offset error is at most ±RTT/2
of that sample, and the overlay shows this bound as `clock sync ±x ms`.

**Rates and drops.**
- *Expected* frames come from the Mac's frame numbers, so frames whose every fragment was lost
  still count.
- *Dropped %* = (expected − shown) / expected over the last second.
- *Lost in network* = expected − received.
- *Skipped here* = received but never shown (decode queue trimmed, or a newer decoded frame
  replaced it before upload).

**Jitter.** RFC 3550 interarrival jitter, computed from Mac capture times and PC arrival times.
Clock offset cancels out, so it needs no sync.

**Process usage.**
- PC: CPU from `GetProcessTimes`, as % of all cores (Task Manager scale); working set and
  private bytes; GPU from the PDH `GPU Engine` counters for this PID (3D and VideoDecode engines,
  WDDM 2.x).
- Mac: CPU from `getrusage`, as % of all cores (multiply by the core count for Activity
  Monitor's scale); memory is `phys_footprint`.
- macOS has no public per-process GPU utilization API, so Mac GPU % is not reported.

## Where to see it

- **Windows overlay (F2).** FPS at every stage (capture, encode, receive, decode, shown), whether
  decoding runs on the GPU or CPU, presents per second and render CPU time, bitrate, RTT, jitter,
  drops, packet loss and lost packets rebuilt from parity, the Mac's pacing spread, how long
  packets waited in queues on the way (`Net wait`, with the verdict the Mac's bitrate follows),
  keyframes (received / asked for / frames held waiting for one), per-stage
  latency (avg / p95 / max over the last ~2 s), Mac and PC CPU/RAM, and GPU usage.
- **Windows console.** A two-line summary every 5 s.
- **Mac window.** Capture FPS and latency, encoder FPS, latency and bitrate (actual / target),
  send rate, RTT, end-to-end latency, packet loss, and this process's CPU and memory. Updated once
  per second; the RTT, end-to-end latency and loss come from the receiver.

## Running a benchmark

```powershell
cd windows-receiver
cmake -B build
cmake --build build --config Release
.\build\Release\ExternalDisplayReceiver.exe --bench=60      # Visual Studio generator
# or .\build\ExternalDisplayReceiver.exe --bench=60         # single-config generators
```

Recording starts with the first displayed frame and runs for 60 s. The receiver then prints a
summary and exits; ESC stops early and keeps what was recorded. Files are written to the current
directory:

| File | Content |
|------|---------|
| `bench-<time>-frames.csv`  | one row per displayed frame: every stage in ms (empty = not measurable) |
| `bench-<time>-seconds.csv` | one row per second: FPS per stage, bitrate, loss, drops, jitter, RTT, PC CPU/RAM/GPU, Mac CPU/RAM/FPS, keyframes / keyframe requests / frames held / packets rebuilt, the queuing delay on the way and its verdict (normal / hold / overuse) in that second |
| `bench-<time>-summary.txt` | avg / p50 / p95 / p99 / max per stage, averages of the per-second values, RAM start → end |

Percentiles come from fixed-size histograms (0.1 ms bins), so memory stays flat even in
multi-hour runs. The seconds file shows leaks (RAM trend) and slow FPS or latency degradation.

For comparable numbers:
- Build the Mac app in **Release** (Xcode: Product → Scheme → Edit Scheme → Run → Build
  Configuration → Release). Debug Swift is several times slower in tight loops.
- Show moving content (a video, or a 60 FPS animation such as https://www.testufo.com). A static
  screen produces no frames, since ScreenCaptureKit only delivers changes.
- Record which network you used (5 GHz Wi-Fi, 2.4 GHz, Ethernet) and the distance to the access
  point.

## Unit tests

The latency arithmetic, clock sync, percentiles, jitter, rates, benchmark files and payload
layout are tested by `PipelineMetricsTests` (plain C++, any OS). The full test list is in
[TESTING.md](TESTING.md).

## Results so far

### Localhost loopback (synthetic frames, no network hardware)

Run during development on an Apple M2 (2026-10-13). This used the real Mac `StreamSender` and
VideoToolbox encoder (1280×720 synthetic frames at 60 FPS), and the real C++ `StreamReceiver`
(MinGW build under Wine) over 127.0.0.1. It validates the protocol and the measurements; it is
**not** a LAN result.

| | |
|---|---|
| Frames | 1,192 received / 1,192 expected, 0 packets lost, all with timing prefix |
| Encode (VideoToolbox, 720p) | avg 6.0 ms, p95 7.3 ms |
| Send | avg 0.07 ms |
| Network (localhost + Wine) | avg 2.2 ms, p95 2.7 ms |
| Clock sync | RTT ≈ 0.7–1.4 ms, offset error bound ±0.25 ms |

### Mac sender optimizations (Phase 16)

Same synthetic 60 FPS stream (~22 Mbps), measured before/after on an M2:

| Change | Before | After |
|--------|--------|-------|
| Sender process, Debug build, 26 s run | 8.56 s CPU (≈ 5.3 % of all cores) | 7.60 s CPU (≈ 3.4–4.1 %) |
| CRC-32 per UDP packet, 1 s of 40 Mbps (Debug / Release) | 69 / 19 ms CPU | 24 / 2.4 ms CPU (slicing-by-8) |
| Preview, 2560×1600 source | full size, 30 FPS: 78 ms CPU/s, 492 MB/s GPU→CPU readback | 960 px, ≤ 10 FPS: 12 ms CPU/s, 23 MB/s (0 with Preview off) |

Other changes (not separately benchmarked):
- Network counters are no longer pushed to the main thread per UDP packet (~3,600 dispatches/s at
  40 Mbps, each one a SwiftUI update). The UI now pulls them once per second.
- The capture callback no longer publishes stats to SwiftUI on every frame.
- TCP_NODELAY on the Mac control socket.
- QUALITY_REPORT carries the real RTT. It used to be 0, which kept adaptive bitrate from ever
  reacting to queueing delay.

### Real LAN results

#### Phase 16 baseline — 2026-10-14, Wi-Fi, 60 s (`bench-20261014-103135`)

| Stage | avg | p50 | p95 | p99 | max (ms) |
|-------|----:|----:|----:|----:|----:|
| Capture | 1.0 | 0.05 | 2.2 | 21.2 | 171 |
| Encode | 8.3 | 7.4 | 13.1 | 15.8 | 71 |
| Send | 0.4 | 0.2 | 0.7 | 3.8 | 107 |
| Network | 10.3 | 4.5 | 58.3 | 80.5 | 169 |
| Queue | 15.6 | 11.5 | 45.3 | 58.3 | 79 |
| **Decode** | **102.0** | 74.5 | 178.1 | 554.5 | 2635 |
| Render | 6.8 | 7.2 | 11.1 | 11.8 | 24 |
| **Display** | **45.2** | 49.5 | 49.7 | 49.8 | 50 |
| **Total** | **188.5** | 157.6 | 259.6 | 648.1 | 2698 |

Other numbers from the same run:
- 30.6 FPS shown of 39.9 received; 20.6 % of frames dropped, 564 of them on the PC.
- 8.6 Mbps, RTT 9.7 ms, jitter 9.1 ms, packet loss 0.011 %.
- PC CPU 9.5 %, GPU 3D 8.5 %, **GPU video decode 0.0 %**. Mac CPU 3.8 %.

What it showed:
1. **Decoding ran in software on the CPU.** The GPU's video engine sat at 0 %. Decoded pictures
   were also only collected when the *next* frame was submitted, so each picture waited for the
   frame behind it. When the decoder was full, the input frame was thrown away, corrupting the
   picture until the next keyframe.
2. **Display ≈ 3 refreshes** (49.5 ms, nearly constant): DXGI's default queue of up to 3
   presented frames.
3. **Queue**: frames waited for the vsync-paced render loop before decoding started.

#### Phase 17 / version 2 — awaiting measurement

Same benchmark after the changes below. Compare against the table above. The benchmark summary
now records the decoder (hardware/software), the renderer mode and 32/64-bit, so runs on
different laptops can be compared.

### Bitrate that follows the delay (smoothness step 6)

Loopback, 2026-11-03: the real Mac sender and VideoToolbox encoder against the real Windows
receiver networking (64- and 32-bit), 720p60, 20 s per run. The Mac starts at 20 Mbps, behind
an 8 Mbps link with a 200 ms queue (`--simulate-link=8000`):

| | Follows the delay | Loss/RTT tiers (`--adapt=tiers`) |
|---|---|---|
| Below the link after | under 1 s (first cut at 0.1 s) | 12.4–12.7 s |
| Queue on the link, last 10 s, average | 2.4–3.2 ms | 32–77 ms |
| Frames that arrived | 99.4–99.5 % | 16.6–19.5 % |
| Packets lost after the first 2 s | 0 | ~8,700–8,900 |

Cost: one 16-byte message every 100 ms from the PC. The video bitrate settles at 5.5–6.5 Mbps on
the 8 Mbps link (with parity and headers, about 80–90 % of it). It rises slowly until a small
queue appears, then cuts by 15 %.

On Wi-Fi, compare `--bench=60` with `--bench=60 --adapt=tiers`. The summary's `Net wait` line
gives the time spent in overuse.

### Loss repair and pacing (smoothness step 5)

Loopback, 2026-11-03: the real Mac sender and VideoToolbox encoder against the real Windows
receiver networking (64- and 32-bit), 720p60, 20 s per run, 1 % of the packets dropped on
purpose (`--simulate-loss=1`):

| | Parity + pacing | Neither (as before) |
|---|---|---|
| Frames that arrived | 98.7 % | 73.5–74.2 % |
| Lost packets rebuilt | 329–336 | 0 |
| Keyframes | 22 | 117 |

Cost:
- About 3 parity packets per frame at this size (≈10 % more data).
- Pacing spreads a large frame's datagrams over about 1.05 ms (measured on the Mac, up to about
  2 ms when it is busy). Frames of up to 8 packets are not paced.

On Wi-Fi, compare `--bench=60` with `--pacing=off` and `--fec=off`.

### Capture in the encoder's format (smoothness step 4)

M2, synthetic 1080p at 60 FPS, the app's encoder settings (2026-11-03):

| Encoder input | Stream says | Encode avg | Encode p95 |
|---|---|---|---|
| BGRA (before) | video range, colours unspecified | 7.95 ms | 10.77 ms |
| `420f` + BT.709 tags (now) | full range, BT.709 | 8.32 ms | 9.27 ms |

The average is unchanged; the slow frames are about 1.5 ms faster. The capture side (one
conversion instead of two) shows in the benchmark's Capture/Encode stages on real hardware. The
larger effect of this step is correct colours.

### Display path (smoothness step 3) — awaiting measurement

Per-monitor DPI awareness and the optional present-at-once mode change the Display stage.
Compare `--bench=60` with `--bench=60 --present=immediate` on each laptop; the summary records
window size, scale and present mode. DXGI's frame statistics are tied to refreshes, so with
tearing the Display stage may not show the whole gain. PresentMon shows the real present mode.

### Keyframes only on request (smoothness step 2)

Loopback, 2026-11-03 (real VideoToolbox encoder, 720p60, 64- and 32-bit receiver):

| | Before | After |
|---|---|---|
| Keyframes in a 20 s stream | ~20 (one per second) | 2 (stream start, one repair) |
| A frame missing (dropped on purpose in the test) | decoded damaged until a keyframe: the requested one after packet loss, or the next periodic one (≤ 1 s) when the frame went missing on the PC | noticed at once, the picture held; keyframe 8–27 ms after the request; 1–3 frames held |

On real Wi-Fi the benchmark's Network p95/p99 and frame drops show the effect of the missing
bursts (step 0 vs. step 2 runs).

### Encryption cost (protocol v2)

AES-256-GCM over every video packet, measured on an M2 with CryptoKit (Release build). Packets
are 1,348-byte payloads:

| Stream | Packets/s | CPU for encryption |
|---|---:|---:|
| 25 Mbps | 2,250 | 4.2 ms/s (0.4 % of one core) |
| 40 Mbps | 3,600 | 8.0 ms/s (0.8 % of one core) |

Adds 24 bytes per datagram (8-byte counter + 16-byte tag): 1.7 % of a full packet. Windows
decryption uses CNG (AES-NI); it is not measured here, because Wine's numbers would not represent
real Windows. The benchmark's PC CPU column shows it on real hardware.

### Soak: 10 minutes, encrypted, localhost

2026-10-23, Apple M2: the Mac sender harness (real StreamSender, VideoToolbox, AES-GCM; 720p60
synthetic frames) streamed to the Windows receiver networking code (MinGW build under Wine)
for 10 minutes. The Mac sender's memory was sampled every 30 s:

| Time | 0 s | 1 min | 2 min | 3 min | 4 min | 5 min | 6 min | 7 min | 8 min | 9 min | 10 min |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Mac RSS (MB) | 47.2 (start-up) | 22.5 | 22.1 | 18.7 | 22.1 | 17.7 | 21.2 | 16.3 | 16.2 | 16.3 | 19.8 |
| Mac CPU (% of one core) | 26 | 23 | 25 | 18 | 13 | 16 | 23 | 25 | 24 | 28 | 29 |

Result: **35,986 of 35,986 frames received, 0 lost, 0 bad start codes, 600 keyframes**, every
frame with timing. Encode 5.8 ms avg, network 1.1 ms avg, RTT 0.7 ms, clock sync ± 0.23 ms.
Memory stays between 16 and 23 MB after start-up with no upward trend: no leak in the sender,
encryption or statistics paths. Most of the CPU is the harness drawing its synthetic frames.

This covers the code, not the hardware: the GPU, decoder and Wi-Fi need the real-hardware
stress runs in [TESTING.md](TESTING.md#stress-30-min--1-h--4-h).

### Phase 17 changes (power and latency)

| Change | Why (from the baseline) |
|--------|-------------------------|
| Hardware H.264 decoding: the decoder gets the renderer's D3D11 device (`IMFDXGIDeviceManager`); pictures stay on the GPU and are copied GPU→GPU into an NV12 texture sampled as R8 + R8G8. Software decoding is the automatic fallback. | Decode 102 ms avg on the CPU, video engine 0 %. Also removes the per-frame CPU copy and texture upload. |
| Decoder: full output drained after every input and when frames are pending; on `MF_E_NOTACCEPTING` the input is resubmitted, not dropped. | Pictures waited for the next frame; dropped inputs corrupted the picture. |
| Swap chain: frame-latency waitable object, maximum frame latency 1. The loop waits for the slot, *then* decodes whatever arrived and presents the newest picture. | Display ≈ 49.5 ms (3-frame queue). |
| Event-driven render loop: sleeps in `MsgWaitForMultipleObjectsEx` until a frame arrives (event set by the network thread), a window message, or a timer. It renders only when the picture or overlay changed. | Rendered 60× per second even on a static screen; frames waited for the next vsync before decoding. |
| Mac: VideoToolbox low-latency rate control with the Constrained High profile, hardware required (falls back to the normal encoder if refused). | Measured on M2 at 1080p: 9.2 → 6.7 ms avg, p95 12.3 → 7.0 ms, same bitrate, same CPU. Forced keyframes verified. |
| Mac: preview paused while streaming (toggle to keep it). | Each preview is a GPU→CPU readback; the picture is on the PC anyway. |
| Mac: `ProcessInfo.beginActivity(.userInitiatedAllowingIdleSystemSleep)` while capturing. | App Nap would throttle the sender while its window is hidden, which is exactly when the PC display is in use. Idle sleep stays allowed. |

## Known limitations

- **Capture** starts at ScreenCaptureKit's `displayTime`, so WindowServer compositing before that
  moment is not included.
- **Display** needs DXGI frame statistics for the flip-model swap chain. If the driver doesn't
  provide them (or numbers them differently), the receiver logs it once; Display then shows
  `n/a` and Total ends at `Present()`. The vblank wait (0–16.7 ms at 60 Hz) is then missing from
  Total.
- **Network and Total** are accurate to ± the clock sync bound shown on the overlay. A sample
  slightly below zero (within the bound) is shown as 0; one further below is discarded.
- **Mac GPU %** is not available (no public API).
- **Software decoding fallback**: used when the GPU has no D3D11 video support, cannot sample
  NV12, or rejects the stream. The console says `Decoder ... (SOFTWARE — CPU decoding)` and the
  overlay shows `decode ... (CPU)`. Benchmark summaries record which decoder ran.
- **Rendered FPS** (overlay, `rendered_fps` column) counts presents. A static screen with the
  overlay hidden presents nothing, which is the intended power saving, not a stall.
