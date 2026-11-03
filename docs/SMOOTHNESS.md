# Smoothness roadmap

Goal: the Windows screen should feel like a monitor on a cable. The mouse and typing feel
instant, and scrolling and video stay smooth on ordinary Wi-Fi.

Zero latency is not possible. At 60 Hz, the Mac waits for its next screen refresh before a frame
exists, and the PC waits for its next refresh to show it: about 16 ms on average before anything
else happens. These are the realistic targets:

| | Target |
|---|---|
| Mouse pointer (PC mouse, F3 on) | as fast as any Windows app: Windows draws it (step 1) |
| Picture, 60 Hz, good 5 GHz Wi-Fi | 30–45 ms average, p95 under 60 ms |
| Picture, 120 Hz | 20–30 ms average |
| Smoothness | 99 % or more of the frames shown, no hitch once a second |

## Where the time goes

Measured stages from the benchmark ([PERFORMANCE.md](PERFORMANCE.md)). The baseline is the
Phase 16 run on Wi-Fi (2026-10-14). The Phase 17 changes (hardware decoding, one-frame queue,
event-driven loop) are in the code but have not been measured on a PC yet: that is step 0.

| Stage | Baseline avg (p95), ms | Target avg, ms | Steps |
|---|---|---|---|
| Capture | 1.0 (2.2) | ≤ 1 | 4 |
| Encode | 8.3 (13.1) | ≤ 6 | 4 |
| Send | 0.4 (0.7) | < 0.5 | — |
| Network | 10.3 (58.3) | ≤ 5 (p95 < 15) | 2, 5, 6, 8 |
| Queue | 15.6 (45.3) | ≤ 2 | Phase 17 |
| Decode | 102.0 (178.1) | ≤ 4 | Phase 17 |
| Render | 6.8 (11.1) | ≤ 2 | Phase 17 |
| Display | 45.2 (49.7) | ≤ 10 | Phase 17, 3 |
| **Total** | **188.5 (259.6)** | **30–45** | |

## Steps

| # | Step | Status | What it changes |
|---|---|---|---|
| 0 | Measure the current build on the PCs | waiting for a Windows run | the "before" numbers for every step |
| 1 | Pointer drawn on the PC | **done here, to test on the PCs** | the mouse feels instant; moving the pointer no longer sends video |
| 2 | Keyframes only when needed | **done here, to test on the PCs** | removes the burst once a second; a lost packet costs a short freeze, not smeared blocks |
| 3 | Display path: DPI-aware, present without waiting | **done here, to test on the PCs** | sharp 1:1 picture on scaled laptops; present at once (F5) up to one refresh sooner |
| 4 | Capture in the encoder's format | **done here, to test on the PCs** | correct colours (the picture was washed out); no conversion pass before encoding |
| 5 | Packet pacing and loss repair | to do | fewer Wi-Fi spikes; no long wait after a lost packet |
| 6 | Bitrate that follows the delay | to do | stays smooth when the Wi-Fi gets worse |
| 7 | 120 Hz | to do | halves every refresh wait |
| 8 | Network setup | checklist (no code) | removes regular spikes the code cannot fix |

Every step is checked in the same way:
1. Automated tests on the Mac, with the Windows tests run under Wine.
2. The loopback test (real Mac sender ↔ real Windows networking code).
3. A benchmark on the laptops, before and after. Results go into
   [PERFORMANCE.md](PERFORMANCE.md).

---

### Step 0 — Measure the current build

On each Windows laptop, using the newest code:
```bat
git pull
cd windows-receiver
cmake -B build && cmake --build build --config Release
.\build\Release\ExternalDisplayReceiver.exe --bench=60
```
Show moving content on the Mac's virtual display (a video, or https://www.testufo.com). Use
5 GHz Wi-Fi. Keep `bench-*-summary.txt`.

**Check:** the summary says `Decoder: hardware`. Display is about one refresh (≤ 17 ms), and
Queue is a few ms.

### Step 1 — Pointer drawn on the PC

**Why.** Until now the Mac drew the pointer into the video (`showsCursor = true`). With F3 on,
each movement of the PC mouse travelled PC → Mac → capture → encode → network → decode →
display before you saw it: the whole picture latency (188 ms in the baseline). Moving the pointer
also made the Mac encode and send new video frames just to move a few pixels.

**How (implemented).**
- After the connection is encrypted, the receiver says it can draw the pointer (a
  `RECEIVER_CAPS` message). The Mac then captures **without** the pointer and sends:
  - `CURSOR_SHAPE` when the pointer image changes (arrow, I-beam, hand…), as a small bitmap with
    its hot spot. It is read with `NSCursor.currentSystem`, the API that WebRTC's screen capture
    (used by Chrome Remote Desktop) uses on the Mac.
  - `CURSOR_POSITION` when the pointer is moved on the Mac (trackpad or Mac mouse), checked
    120 times per second. Positions caused by the PC's own mouse are not echoed back.
- On the PC:
  - **F3 on and you move the PC mouse:** Windows' own hardware pointer takes the Mac's pointer
    shape. It moves with no added latency, and the Mac's pointer follows.
  - **The pointer is moved on the Mac** (F3 on or off): the PC draws it at the reported position.
    That position skips capture, encode and decode, so it arrives after about one network trip.
- **F4** switches between "drawn on this PC" and "drawn in the video" while running, to compare.
  `--cursor=video` starts in the old mode. An older Mac app ignores `RECEIVER_CAPS` and keeps
  the pointer in the video. The PC then behaves as before, because it never receives a pointer
  image.
- Two fixes found on the way:
  - The Mac app maps input to the captured display's current position. Before, it was wrong
    when a physical display other than the main one was captured, or after the displays were
    rearranged.
  - The 120 Hz limit on mouse moves used to drop the last move. Now the PC sends that held-back
    position too, so the Mac's pointer stops exactly where the PC's did (hover effects and
    menus match what you see).

**Check.**
- Automated, all passing (2026-11-03):
  - Mac: 35 XCTests, 7 of them new. They cover the payload bytes, which are identical to the
    Windows test's bytes, and the bitmap of the real arrow (size, hot spot, which way up). They
    also check that moves the PC caused are not echoed back.
  - Windows: 285 checks on x64 and x86. New: `PointerTests` (70), covering hostile input,
    resampling, placement through the black bars, which pointer shows when, and building a
    real Windows cursor.
  - Loopback, all 5 scenarios: the real image and a scripted 100-point path arrive complete and
    in order over the encrypted connection. Switching off and on again (F4) works mid-session,
    and a receiver that does not ask gets no pointer messages.
- On the PC: see "Checklist for step 1" below.

**Known risk.** Apple marks `NSCursor.currentSystem` as "to be deprecated" (there is no other
public way to read the system-wide pointer image). If it stops working, the Mac app sends the
standard arrow, and F4 / `--cursor=video` bring back the pointer in the video.

#### Checklist for step 1 (on each laptop)
1. Connect, press **F3**, move the mouse quickly in circles. The pointer follows the hand with no
   delay; windows you drag still lag a little, as in any remote desktop.
2. Hover text fields (I-beam), links (hand), window edges (resize arrows). The pointer changes
   shape within a moment.
3. With F3 on, use the **Mac's trackpad** to move the pointer onto the PC screen. The pointer
   appears and moves there. Move the PC mouse again: the pointer jumps to the PC mouse.
4. Press **F4**: the pointer is drawn in the video again (the old way). Compare, then press F4
   again.
5. F11 (window mode), with black bars at the sides or top: the pointer still lands exactly on
   what it clicks.
6. On a still screen, move the pointer around (F3 on) and watch the F2 overlay. The received
   FPS stays near 0: moving the pointer no longer sends video, unless something under it
   highlights. Press F4 and repeat: now every pointer move produces frames.

### Step 2 — Keyframes only when needed

**Why.** The encoder made a keyframe every 60 frames (`keyframeInterval = 60`): once per second
at 60 FPS. The 10-minute soak test had 600 keyframes in 36,000 frames. A keyframe is several
times larger than a normal frame, so once a second the Wi-Fi got a burst that could delay that
frame and the ones after it.

**How (implemented).**
- **Mac:** keyframes only when the PC asks (`KEYFRAME_REQ`). One a minute remains as a safety
  net (`EncoderConfig.keyframeIntervalSeconds = 60`). A static screen sends nothing at all.
  - If a frame ever fails to convert before sending, the Mac makes the next one a keyframe
    itself. The PC could not notice that frame missing.
- **PC:** without a keyframe every second, a missing frame must be noticed at once, because the
  frames after it build on it. A new *keyframe gate* in front of the decoder checks the Mac's
  frame numbers. It catches frames lost on the network, frames that arrived incomplete, and
  frames trimmed from the PC's own decode queue (that last case went unnoticed before).
  - After a missing frame it **holds the last good picture** instead of showing smeared blocks.
  - It **asks for a keyframe at once**, then every 250 ms until one arrives. Before, the request
    was limited to once per 500 ms, with no retry if that keyframe was lost too.
  - If the keyframe is late, decoding continues after 300 ms (damaged until the keyframe
    repairs it), so the picture never stops for long.
- **F2 overlay:** new line `Keyframe N (asked M) held K frames`. The benchmark files get the
  columns `keyframes`, `keyframe_requests`, `frames_held` per second, and a summary line.

**Check.**
- Automated, all passing (2026-11-03):
  - Mac: the encoder makes keyframes only at frame 0 and where one was asked for (150 frames:
    `[0, 100]`). With the old setting the same test gives `[0, 60, 100]`.
  - Windows: 334 checks on x64 and x86, 49 of them new. They cover the gate (hold, ask, retry,
    300 ms limit, late and duplicate frames, renumbering, older Mac apps) and the new counters,
    overlay line and benchmark columns.
  - Loopback, all scenarios, 64- and 32-bit, with the real VideoToolbox encoder: **2 keyframes
    per 20-second run instead of about 20**. One frame is lost on purpose 12 s in; the gate
    noticed it, and the Mac's keyframe arrived **8–27 ms** after the request. 1–3 frames were
    held meanwhile (≤ 1/20 s).
- On the PC: see "Checklist for step 2" below.

#### Checklist for step 2 (on each laptop)
1. Stream something moving and press **F2**. The `Keyframe` line counts up only when something
   happens (connecting, a lost packet), not every second.
2. Walk away from the router or start a big download, so that some packets get lost (the `Loss`
   line). You see short freezes instead of smeared or grey blocks, and the picture is sharp
   again right away.
3. Run `--bench=60` and compare with the step 0 run: Network p95/p99 and the frame drops should
   be lower. The summary has a new `Keyframes` line.

### Step 3 — Display path

**Why.**
- **DPI awareness.** The receiver did not declare itself DPI-aware. On a laptop at 125–150 %
  scaling, Windows rendered its window at a lower resolution and stretched it. On a 1920×1080
  panel at 150 % the video was drawn at 1280×720, then enlarged again, so the picture was
  blurry. The compositor also had to scale every frame, which rules out "independent flip"
  (frames going from the GPU straight to the screen, without the compositor's extra frame).
- **Waiting for the refresh.** `Present(1, 0)` waits for the next refresh: half a refresh on
  average, up to a full one.

**How (implemented).**
- **Per-monitor DPI awareness** (v2 on Windows 10 1703+, with fallbacks for older versions),
  turned on before any window exists.
  - The swap chain now has the panel's real pixels, so a 1920×1080 stream shows 1:1 on a
    1920×1080 panel.
  - The status card and the F2 overlay are laid out at 100 % and drawn at the screen's scale.
    Their text is sharp, because DirectWrite text is vector.
  - The pairing dialog scales its layout and font to the screen.
  - The F11 window opens at 1280×720 × scale.
  - Moving the window to a screen with another scale (or changing the scale) is followed.
  - `receiver.log` says `DPI awareness: per-monitor v2, screen at 150 %`.
- **Present at once, as an option** (`--present=immediate` or **F5** while running; off by
  default).
  - Where Windows and the driver support it (DXGI 1.5, Windows 10 1607+), the swap chain is
    created with "allow tearing". With the option on, frames are presented without waiting for
    the refresh.
  - That is up to one refresh sooner, but the picture can tear (a visible line) during fast
    scrolling, which is why it is off by default.
  - Without driver support, F5 logs that and nothing changes.
  - The order the swap chains are tried in is now a tested plan (`SwapChainPlan`). A refused
    tearing chain falls back to the waitable chain first, before the plainer ones.
- **F2 overlay:** a second line reads `window 1920x1080 at 150%`, and then
  `Pointer drawn here (F4)  present on refresh (F5)`. Benchmark summaries record the window
  size, scale and present mode, so runs can be compared.

**Check.**
- Automated, all passing (2026-11-03):
  - Windows: 364 checks on x64 and x86. New `DisplayTests` (27): the swap chain plan for every
    renderer mode, when a present may tear, per-monitor DPI awareness on Windows (Wine
    reports `per-monitor v2`), scaled lengths, fonts and window frames. Option parsing for
    `--present`.
  - The full receiver builds with the tearing and DPI APIs (MinGW, 64- and 32-bit).
- **Not testable here:** how the picture looks and how much latency it saves. Wine cannot run
  Direct3D on this Mac, so this needs the laptops. Note: DXGI's frame statistics are tied to
  refreshes, so with tearing the benchmark's Display stage may not show the whole gain.
  PresentMon (free, github.com/GameTechDev/PresentMon) shows the real present mode
  (`Hardware: Independent Flip` vs `Composed: Flip`).

#### Checklist for step 3 (on each laptop)
1. Check the scale: Settings → System → Display → Scale (often 125 % or 150 % on laptops).
2. Start the receiver. Text on the Mac's screen should look as sharp as on the Mac itself; a
   1920×1080 stream on a 1920×1080 panel is now 1:1. The F2 overlay says `at 150%` (or
   whatever the scale is).
3. The status card, the F2 text and the pairing dialog are a normal size: not tiny, not blurry.
4. F11 into a window, drag it to another monitor with a different scale (if there is one): it
   resizes and stays sharp.
5. Benchmark `--bench=60` (on refresh), then `--bench=60 --present=immediate`. Compare the
   Display and Total stages with each other and with the step 0/2 runs. Scroll a web page with
   F5 on and off: does the tearing bother you?

### Step 4 — Capture in the encoder's format

**Why.**
- The Mac captured BGRA (`kCVPixelFormatType_32BGRA`), but H.264 needs YUV 4:2:0, so every frame
  was converted before encoding.
- **Found while checking this:** VideoToolbox converts BGRA to *video range* (16–235) and writes
  "unspecified" colours into the stream. The receiver's shader assumed *full range* for every
  stream. So on the PC **black showed as dark grey (6 %) and white as light grey (92 %)**: the
  whole picture was washed out.

**How (implemented).**
- **Mac:** capture `420f` (full-range NV12) with the BT.709 matrix and sRGB (= BT.709)
  primaries. That is the encoder's own input format, so no conversion pass is needed. The encoder
  also writes BT.709 into the stream explicitly.
- **PC:** the receiver reads the colour description from each keyframe's SPS (range and
  matrix), and the shader converts with the matching constants: full or video range, BT.709 or
  BT.601. Streams from an older Mac app (video range) also show correct black and white now.
  The F2 overlay names the colours, for example `1920x1080, full-range BT.709`, and
  `receiver.log` says `Video colours: ...`.
- `queueDepth` stays at 3. ScreenCaptureKit's documentation says fewer frames "may stall the
  display stream". It is a pool of surfaces, not a delay, and the app holds one surface for
  keyframe re-encodes. Lowering it (the earlier plan) could only cause stalls.
- Later, for quality rather than speed: capture runs at the display's size in points, so on a
  Retina-style virtual display text is captured at 1×. Capturing at pixel size would be sharper
  and cost more bitrate.

**Check.**
- Measured on this M2 (synthetic 1080p frames at 60 FPS, the app's encoder settings):

  | Encoder input | Stream says | Encode avg | Encode p95 |
  |---|---|---|---|
  | BGRA (before) | video range, colours unspecified | 7.95 ms | 10.77 ms |
  | `420f` + BT.709 tags (now) | full range, BT.709 | 8.32 ms | 9.27 ms |

  The average encode time is unchanged and the slow frames are about 1.5 ms faster. The
  capture-side saving cannot be measured here (this shell has no Screen Recording permission).
  The benchmark's Capture and Encode stages on your setup will show it.
- Automated, all passing (2026-11-03):
  - Mac: 38 XCTests. New: the capture settings (`420f`, BT.709, sRGB, queue depth), and
    VideoToolbox's own reading of the stream it wrote (full range, BT.709 primaries, transfer
    and matrix).
  - Windows: 392 checks on x64 and x86. `DisplayTests` (55) parses the Mac encoder's real SPS
    bytes from both formats, plus constructed SPS variants (no VUI, aspect ratio, scaling
    lists, picture-order types, cropping, Baseline, escape bytes). It also checks the shader's
    arithmetic: black and white for each range, pure red, and the old washed-out case.
  - Loopback, 64- and 32-bit: the harness now feeds full-range NV12, like the app. Every keyframe
    carries an SPS that says full-range BT.709.

#### Checklist for step 4 (on each laptop and the Mac)
1. Update **both** apps (the Mac app changes how it captures).
2. Open a page with pure black and pure white on the Mac's virtual display. On the PC, black is
   black and white is white, not grey. Compare with the Mac's own screen.
3. F2: the first line shows `full-range BT.709`.
4. `--bench=60`: compare the Capture and Encode stages with the earlier runs.

### Step 5 — Packet pacing and loss repair

**Why.** A frame's packets leave in one burst, and on Wi-Fi a burst can overflow queues. A lost
packet ruins the frame until a keyframe arrives (at least one round trip, plus a big frame).

**How.**
- Spread each frame's packets over 1–2 ms.
- Add parity packets (XOR or Reed–Solomon) so the PC can rebuild one or two lost packets per
  frame.
- Ask for a keyframe only when the repair fails (step 2's gate already does the asking).
- Option: VideoToolbox long-term reference frames (`kVTCompressionPropertyKey_EnableLTR`, macOS
  12+). The PC acknowledges frames; after a loss the Mac refers back to an acknowledged frame
  (`ForceLTRRefresh`) instead of sending a full keyframe.

**Check.** A loopback test that drops 1–5 % of packets on purpose: frames are rebuilt and none
are lost. Then the benchmark's Network p95 and frame drops on Wi-Fi.

### Step 6 — Bitrate that follows the delay

**Why.** Adaptive quality reacts to loss above 3 %, RTT above 50 ms, drops or a growing queue. By
then the picture already stutters. Wi-Fi shows rising delay before it loses packets.

**How.** Use the per-frame network delay that is already measured. Lower the bitrate within about
100 ms when the delay keeps rising, and raise it slowly when the delay is stable (as WebRTC's
congestion control does).

**Check.** Limit the network with Network Link Conditioner (Xcode Additional Tools). The bitrate
drops quickly and the picture stays smooth (no frozen frames).

### Step 7 — 120 Hz

**Why.** Every wait for a refresh (capture on the Mac, display on the PC) is about 8 ms on average
at 60 Hz and 4 ms at 120 Hz.

**How.**
- Virtual display at 120 Hz.
- Capture `minimumFrameInterval` 1/120.
- Offer a 120 FPS quality preset when the PC's screen runs at 120 Hz (the receiver reports its
  refresh rate).

The encoder does twice the work, so enable it only when both sides support it.

**Check.** The benchmark at 120 Hz: Capture + Display stages, and the encoder keeps up.

### Step 8 — Network setup (no code)

- Use 5 GHz or 6 GHz Wi-Fi, close to the router, or Ethernet on one side.
- On the Mac, turn off AirDrop and Handoff while streaming. They use AWDL, which regularly
  switches the Wi-Fi radio to another channel and causes latency spikes at regular intervals.
- On the PC, set the Wi-Fi adapter's power saving to maximum performance (Power Options →
  Wireless Adapter Settings).
- Check: the benchmark's Network p95 before and after each change.

---

## After the smoothness steps: making it feel native

| # | Step | Needs |
|---|---|---|
| N1 | Mac menu-bar app: share / stop, choose the PC, quality | — |
| N2 | Start at login, connect to the paired PC on its own | the `.app` bundle (`SMAppService`) |
| N3 | Control Center button (macOS 26+) | the `.app` bundle and a widget extension |
| N4 | Windows: tray icon, start with Windows, connect on its own | — |
| N5 | `.app` / `.dmg` and a Windows installer | planned separately |
