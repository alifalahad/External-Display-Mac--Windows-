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
| 2 | Keyframes only when needed | to do | removes the burst once a second |
| 3 | Display path: DPI-aware, present without waiting | to do | up to one refresh less; sharper picture |
| 4 | Capture in the encoder's format | to do | 1–3 ms less, less GPU work on the Mac |
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

**Why.** The encoder makes a keyframe every 60 frames (`EncoderConfig.keyframeInterval = 60`):
once per second at 60 FPS. The 10-minute soak test had 600 keyframes in 36,000 frames. A keyframe
is several times larger than a normal frame, so once a second the Wi-Fi gets a burst that can
delay that frame and the ones after it.

**How.**
- Make the keyframe interval very long (VideoToolbox `MaxKeyFrameInterval` and
  `MaxKeyFrameIntervalDuration`).
- Send keyframes only on request. That path already exists: decoder start, lost packets, and
  rate-limited `KEYFRAME_REQ`.
- Step 5's loss repair makes requests rare.

**Check.** Count keyframes per second (add the count to `SENDER_STATS` and the overlay). Then
compare the Network p95/p99 and the frame drops before and after.

### Step 3 — Display path

**Why.**
- **DPI awareness.** The receiver does not declare itself DPI-aware. On a laptop at 125–150 %
  scaling, Windows renders its window at a lower resolution and stretches it. That makes the
  picture blurry, makes the compositor scale every frame, and rules out "independent flip" (the
  GPU sending our frames straight to the screen).
- **Waiting for vblank.** `Present(1, 0)` waits for the next refresh: half a refresh on average,
  up to a full one.

**How.**
- Declare per-monitor DPI awareness (manifest or `SetProcessDpiAwarenessContext`). Map mouse
  coordinates with real pixels.
- Offer presenting without waiting: `DXGI_PRESENT_ALLOW_TEARING` on the flip-discard swap chain,
  in borderless full screen. It can tear during fast scrolling, so make it a switch and measure
  both.

**Check.** The Display stage in the benchmark, and how sharp text looks at 125 % or 150 %
scaling.

### Step 4 — Capture in the encoder's format

**Why.** The Mac captures BGRA (`kCVPixelFormatType_32BGRA`), but H.264 needs YUV 4:2:0, so
every frame is converted before encoding.

**How.**
- Capture `420v` (video range) directly. The preview path takes YUV too.
- Try `queueDepth` 2 instead of 3.

**Check.** The Capture and Encode stages, and the Mac CPU/GPU, before and after.

(Not about speed, but related: capture runs at the display's size in points. On a Retina-style
virtual display, text is therefore captured at 1×. Capturing at pixel size would be sharper and
cost more bitrate. That is a quality option for later.)

### Step 5 — Packet pacing and loss repair

**Why.** A frame's packets leave in one burst, and on Wi-Fi a burst can overflow queues. A lost
packet ruins the frame until a keyframe arrives (at least one round trip, plus a big frame).

**How.**
- Spread each frame's packets over 1–2 ms.
- Add parity packets (XOR or Reed–Solomon) so the PC can rebuild one or two lost packets per
  frame.
- Ask for a keyframe only when the repair fails.

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
