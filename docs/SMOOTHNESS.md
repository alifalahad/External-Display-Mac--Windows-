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
| 5 | Packet pacing and loss repair | **done here, to test on the PCs** | a lost packet is rebuilt on the PC (at 1 % loss 98.7 % of frames arrive, was 74 %); packets paced over ~1 ms |
| 6 | Bitrate that follows the delay | **done here, to test on the PCs** | the bitrate goes down as soon as a queue builds, before packets are lost (8 Mbps test link: below it within 1 s, 99.5 % of frames arrive, was 17–20 %) |
| 7 | 120 Hz | **done here, to test on the PCs** | 120 FPS to a 120 Hz screen (High preset, where the encoder keeps up: on an M2 up to 1920×1200); halves both refresh waits |
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

**Why.**
- A frame's packets left in one burst, and on Wi-Fi a burst can overflow queues.
- One lost packet ruined its whole frame. The PC also asked for a keyframe at once for every
  lost packet: a large frame, one round trip later, and a short freeze (step 2).

**How (implemented).**
- **Parity packets** (Mac, `FrameParity.swift`):
  - The Mac adds about one parity packet per 10 fragments (at least one per frame; ≈10 % more
    data). Parity *j* of *P* is the XOR of fragments *j*, *j+P*, *j+2P*…
  - The PC rebuilds any group that lost exactly one fragment (`FrameAssembler`). Because the
    groups interleave, a burst of up to *P* consecutive lost packets is also rebuilt; a
    keyframe of 112 fragments survives a burst of 12.
- **Keyframes only when repair fails:** with parity active, a gap in the packet numbers no
  longer triggers a keyframe request. The keyframe gate (step 2) asks only when a frame is
  really missing.
- **Pacing** (Mac, `PacketPacer`): a frame's datagrams leave in bursts of 8, at most 250 µs
  apart, and a whole frame within about 1 ms. Frames of 8 packets or fewer leave at once. The
  Mac measures the real spread (about 1.05 ms for large frames, up to about 2 ms when the Mac is
  busy) and reports it in SENDER_STATS; the F2 overlay shows `paced 1.0 ms`.
- **Negotiated per connection** (`RECEIVER_CAPS` bits 1 and 2): `--fec=off` and `--pacing=off`
  on the PC switch them off, to compare on your own Wi-Fi.
- **`--simulate-loss=PERCENT[:BURST]`** on the PC drops incoming packets on purpose, to test
  repair on a good network. For example `--simulate-loss=1` or `--simulate-loss=2:4` (2 % in
  bursts of 4).
- **F2 overlay:** `Loss 1.03% packets, 329 rebuilt (300 frames saved)`. The benchmark files get
  a `packets_rebuilt` column and a `Repair` summary line, and the summary records whether
  parity, pacing and simulated loss were on.
- Later, if needed: Reed–Solomon instead of XOR (several losses per group), or VideoToolbox
  long-term reference frames (`kVTCompressionPropertyKey_EnableLTR`, macOS 12+), which refer
  back to an acknowledged frame instead of sending a keyframe.

**Check.**
- Loopback, 2026-11-03, real Mac sender and VideoToolbox encoder against the real Windows
  receiver networking (64- and 32-bit), 720p60, 1 % of the packets dropped on purpose:

  | | With parity and pacing | Without (as before) |
  |---|---|---|
  | Frames that arrived | **98.7 %** | 73.5–74.2 % |
  | Lost packets rebuilt | ~330 of ~340 | 0 |
  | Keyframes in 20 s | 22 | 117 |

  Without loss: about 3 parity packets per frame, nothing lost, and the Mac's pacing spread is
  about 1.05 ms.
- Automated, all passing:
  - Mac: 43 XCTests. New `LossRepairTests` check the parity bytes against vectors computed
    independently in Python, rebuilding every single lost fragment, the parity header layout and
    the pacing schedule.
  - Windows: 462 checks on x64 and x86. New `NetworkTests` (65) cover the same parity vectors
    and assembly in any order with duplicates. They also cover repair: every single loss, bursts
    up to the group count, the short last fragment, parity before data, and one-fragment frames.
    Two losses in one group must count as lost. Hostile headers are refused, late frames
    ignored, numbering restarts handled, and the loss simulator's rate and bursts checked.

#### Checklist for step 5 (on each laptop)
1. F2: the `Mac` line shows `paced ~1 ms`, and `Loss` shows packets rebuilt when Wi-Fi loses some.
2. Walk away from the router: fewer freezes than before, and the `rebuilt` count goes up.
3. On a good network, test the repair yourself: `--simulate-loss=1`, then
   `--simulate-loss=1 --fec=off`. Compare how often it freezes and the `Keyframe` count.
4. `--bench=60`, then `--bench=60 --pacing=off`: compare Network p95/p99 and the drops.

### Step 6 — Bitrate that follows the delay

**Why.**
- The old adaptive quality (loss/RTT tiers) waited for loss above 3 %, RTT above 50 ms, drops or
  a growing queue on the PC. It needed two bad 1-second reports per step, and a step was at most
  5 Mbps. By then the picture already stuttered.
- Wi-Fi shows rising delay before it loses packets: its queues fill first.

**How (implemented).**
- **The PC measures the queuing delay** (`DelayMonitor`):
  - For each frame's first packet: its arrival time (PC clock) minus the Mac's send time in the
    header. The clocks differ by a constant, so the minimum of the last 10 s is subtracted. What
    remains is the time spent waiting in queues on the way.
  - Every 100 ms it sends a new message, RATE_FEEDBACK, with:
    - the median of the last 100 ms (a single spike does not count);
    - what arrived in the last 500 ms, and the loss;
    - a verdict: *overuse* (above 15 ms and not falling), *normal* (below 5 ms), or *hold*
      (in between, or a queue still draining after a cut);
    - the **link's capacity**: what arrived in the last 100 ms, but only if every frame in that
      time waited at least 5 ms. The queue never emptied, so the link was full, and what it
      carried is what it can carry.
  - It counts packets, not complete frames: on an overloaded link most frames lose a packet, and
    the signal must not go blind exactly then.
- **The Mac follows it** (`DelayRateController`):
  - Overuse on a full link: 85 % of the link's capacity (its video share: parity and headers
    travel too), at most every 0.3 s, so the queue can drain.
  - Overuse whose queue emptied again within 100 ms (a keyframe's burst): no cut, the link had
    room. Heavy loss (above 10 %) still lowers the bitrate, at most once a second.
  - Normal: +8 % per second; +25 % per second while far below (under 70 % of) the capacity last
    found, so a cut that went too deep recovers in seconds. It rises only while the encoder uses
    the bitrate it has: a static screen cannot test the link.
  - Under congestion it may go down to a quarter of the preset's minimum: a picture that keeps up
    is smoother than a sharp one that lags. The preset sets the maximum.
  - The encoder takes a new bitrate before its next frame.
- **Compatible both ways:** an older Mac ignores RATE_FEEDBACK. With an older PC no RATE_FEEDBACK
  arrives, and the Mac keeps the loss/RTT tiers (`BitrateGovernor`). `--adapt=tiers` on the PC
  brings the tiers back, to compare.
- **`--simulate-link=KBPS[:QUEUEMS]`** on the PC puts a narrow link with a queue (200 ms by
  default) in front of the receiver, to test on a good network. Packets wait in it until they
  leave the link, and a full queue drops them, as a router would. This replaces Network Link
  Conditioner from the plan: that needs Xcode's Additional Tools and throttles all of the Mac's
  network traffic.
- **F2 overlay:** `Net wait 2.1 ms queued on the way (normal)`, and the `Mac` line shows the
  encoder's bitrate and target (`enc 5.9/6.8 Mbps`). The Mac's log shows every cut:
  `Bitrate down to 5980 kbps (overuse, queue 21 ms, …, link full at 7960 kbps)`. The benchmark's
  seconds file gets `queuing_delay_ms` and `rate_state` columns, and its summary a `Net wait`
  line (average, maximum, seconds in overuse).

**Check.**
- Loopback, 2026-11-03: the real Mac sender and VideoToolbox encoder against the real Windows
  receiver networking (64- and 32-bit), 720p60, 20 s per run. The Mac starts at 20 Mbps
  (Balanced), with an 8 Mbps link and a 200 ms queue in front of the receiver
  (`--simulate-link=8000`):

  | | Follows the delay (now) | Loss/RTT tiers (before) |
  |---|---|---|
  | First cut | after 0.1 s: 20 → 6.8 Mbps | after 4 s: 20 → 15 Mbps |
  | Below the 8 Mbps link | within 1 s | after 12.4–12.7 s |
  | Bitrate after that | 5.5–6.5 Mbps | 8 Mbps (the tiers cannot go below the preset's minimum) |
  | Queue on the link, last 10 s, average | 2.4–3.2 ms | 32–77 ms |
  | Frames that arrived | **99.4–99.5 %** | 16.6–19.5 % |
  | Packets lost | 50–67, all in the first 2 s | 10,600–10,900 |

- What the numbers mean:
  - The first cut comes about 0.1 s after the stream starts, straight to 6.8 Mbps (85 % of
    the 8 Mbps the full link carried). After that the bitrate moves between about 5.5 and
    6.5 Mbps: it rises slowly until a small queue appears, then cuts.
  - The first second still has a long queue. When the first report arrives, about 170 ms of
    data are already queued: the PC sees only the delay of packets that have arrived. After the
    cut, the queue drains at 15 % of the link's rate. WebRTC behaves the same way. This only
    matters when the start bitrate is far above the link.
  - The comparison is not quite fair to the old tiers: the simulated link delays only the
    video, not the TCP round trip, so the tiers see the loss but not the RTT rise. Even with
    perfect signals they need at least 6 s from 20 to 8 Mbps (two 1-second reports per step of
    at most 5 Mbps).
  - A stall on the PC looks like a queue on the way. This happens when the receive thread does
    not read for tens of milliseconds. In one 64-bit run on the busy test Mac, 3 of 9 cuts read
    the link 15–30 % too low, and the bitrate ended at 3.9 Mbps. The receive thread now runs at
    high priority to make such stalls rare. Whether that helps can only be seen on the PCs.
  - A keyframe that keeps the link busy for more than 100 ms counts as a full link, and the Mac
    cuts once (the 4.8 Mbps dip after the frame the test drops on purpose).
- Automated, all passing:
  - Mac: 54 XCTests. New `RateControlTests` (11) cover the cut to 85 % of what a full link
    carried, only its video share, no cut for a queue that emptied again, the floor, slow and fast
    rises only while the encoder uses its bitrate, heavy loss, limits and the switch from the
    tiers. The RATE_FEEDBACK layout is shared with Windows.
  - Windows: 509 checks on x64 and x86. New in `NetworkTests` (now 106): the delay monitor and
    the link simulator. The delay monitor checks cover the queuing delay whatever the clock offset,
    the verdicts, a draining queue, a keyframe's burst, the capacity only while the link was full,
    spikes, a lasting route change, the rate and the loss. The link simulator checks cover its
    rate, queue, tail drop, and packets held in order.
  - The loopback's `narrow` run must go below the link within 3 s, keep the queue under 40 ms on
    average, and deliver at least 90 % of the frames.

#### Checklist for step 6 (on each laptop)
1. F2: `Net wait` stays at a few milliseconds (`normal`) on good Wi-Fi.
2. Walk away from the router, or start a large download on the same network. `Net wait` says
   `overuse` briefly, the target in the `Mac` line goes down within a second, and the picture
   gets softer instead of stuttering.
3. On a good network: `--simulate-link=8000`, then `--simulate-link=8000 --adapt=tiers`.
   Compare how long the picture lags and freezes at the start, and `Net wait`.
4. `--bench=60` on your Wi-Fi: keep the summary's `Net wait` line (time in overuse) and compare
   with `--adapt=tiers`.

### Step 7 — 120 Hz

**Why.** Every wait for a refresh is half a frame on average: about 8 ms at 60 Hz and 4 ms at
120 Hz. There are two such waits: on the Mac before capture, and on the PC before the frame
shows. Motion is also twice as smooth.

**How (implemented).**
- **The PC says how fast its screen is:** RECEIVER_CAPS now carries the refresh rate of the
  monitor the receiver's window is on (it used 4 spare bytes; older Macs ignore them). The
  receiver checks it again when the window moves to another monitor or a display mode changes,
  and tells the Mac at once. `--max-fps=60` on the PC asks for 60 at most, to compare.
- **The Mac streams 120 FPS only when every part can** (`FrameRate`):
  - the **High** preset (Balanced stays at 60, Battery Saver at 30);
  - a PC screen at 119 Hz or more (144 Hz gets 120; a 48 Hz power-saving panel still gets 60);
  - a captured display that refreshes at 120 Hz. The virtual display now has a 120 Hz and a 60 Hz
    mode, and High switches it to 120 Hz;
  - an **encoder that keeps up** at this size (`EncoderLoad`). Every encode call and every output
    is timed. If it falls behind for two seconds in a row, the stream drops to 60 FPS at this
    size and the log says why.
- **No restart:** a new frame rate only changes the capture interval and the encoder, so the PC
  keeps its decoder. That happens when the PC connects or moves, when the preset changes, or when
  the encoder drops back. START_STREAM therefore still says the rate the stream started with (60):
  the PC's caps arrive a moment after it.
- **The Mac window says why:** next to the preset, for example `now 120 FPS`,
  `now 60 FPS — the PC's screen runs at 60 Hz`, or `now 60 FPS — the encoder cannot keep up with
  120 at this size`.
- **F2 overlay:** `shown 119.8 on a 120 Hz screen`; the FPS line shows what the Mac captures and
  encodes. The benchmark summary records the screen's refresh rate.

**Check.**
- **The encoder on an M2** (2026-11-05): the app's VideoEncoder settings, synthetic content
  scrolling like a web page, 6 s per row:

  | Size | Encode time per frame | At 120 FPS |
  |---|---|---|
  | 1920×1080 | 8.1 ms (the call returns at once; VideoToolbox overlaps frames) | keeps up: 119.7 FPS |
  | 1920×1200 | 7.0 ms (the call blocks until the frame is done) | keeps up: 118.8 FPS |
  | 2560×1440 | 9.8–10.0 ms (blocks) | 92–95 FPS: too slow, drops to 60 |
  | 2560×1600 | 10.6–10.8 ms (blocks) | 85–87 FPS: too slow, drops to 60 |

  The time per frame is the same at 60 FPS. So 120 FPS does not make a frame slower to encode,
  but at 1920×1080 the encoder is busy almost all the time.
- **Loopback** (the real Mac sender and VideoToolbox against the real Windows receiver
  networking, 720p, 20 s): told of a 120 Hz screen, the Mac switched as soon as the caps
  arrived, 2 ms after the stream started. 2,399 frames arrived, captured 8.30 ms apart, with none
  lost. Every run that says 60 Hz stays at 16.67 ms.
- **Not measured here:** the Capture and Display stages on a real 120 Hz screen. This Mac
  cannot run ScreenCaptureKit from the test tools (no Screen Recording permission), and the
  virtual display's 120 Hz mode can only be seen in the app. Its log says
  `Created: 1920×1080 @ 120 Hz` and `Frame rate 60 → 120 FPS`.
- Automated, all passing:
  - Mac: 63 XCTests. New `FrameRateTests` (9) cover the choice for every preset, PC screen,
    display and encoder, the window's text, the two-second rule for a slow encoder (with the M2's
    measured times), late output, a still screen, the RECEIVER_CAPS bytes shared with Windows,
    and an encoder switched to 120 FPS without a new session.
  - Windows: 518 checks on x64 and x86. They cover `--max-fps` and what the Mac is told, the
    overlay text, the RECEIVER_CAPS bytes, and the refresh rate Windows reports.
  - The loopback's `fps120` run must deliver ~120 FPS, 7–9.5 ms apart. The others must stay
    at ~60.

#### Checklist for step 7 (on a laptop with a 120 Hz or faster screen)
1. Windows: Settings → System → Display → Advanced display → refresh rate 120 Hz or more.
   `receiver.log` says `Screen refresh rate: 120 Hz`.
2. Mac: choose **High**, create the virtual display at 1920×1080 or 1920×1200, and start. The window
   says `now 120 FPS`. The Mac's log says `Created: … @ 120 Hz` (or `Refresh rate 60 → 120 Hz`
   for a display made before you chose High) and `Frame rate 60 → 120 FPS`.
3. F2 on the PC: capture and encode near 120, `shown … on a 120 Hz screen`. Scroll a long page:
   motion is smoother than with Balanced.
4. `--bench=60`, then `--bench=60 --max-fps=60`: compare Capture, Display and Total.
5. At 2560×1440 the Mac should say `the encoder cannot keep up with 120 at this size` after a
   few seconds of motion (on an M2). Tell me what yours says.

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
