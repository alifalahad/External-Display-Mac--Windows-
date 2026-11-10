# Troubleshooting

## Where the logs are

| | Location | More detail |
|---|---|---|
| Windows | `receiver.log` next to `ExternalDisplayReceiver.exe` | run with `--verbose` (or `--log=trace`); `--console` shows it live |
| Mac | `~/Library/Logs/ExternalDisplay/sender.log` (menu 🔍 → Show Log) | menu 🔍 → Diagnostics logging; live: `log stream --predicate 'subsystem == "com.externaldisplay.sender"' --level debug` |
| Windows crash | `receiver-crash.txt` / `.dmp` next to the exe | open the `.dmp` in Visual Studio or WinDbg with the `.pdb` next to it |

The logs never contain keys, verification codes, keystrokes or clipboard text, so they are safe
to share.

## Windows receiver

**The window flashes and closes / "nothing opens."**
Since version 2 the reason is shown in a message box and written to `receiver.log`. Look for
`[Renderer] ... failed` lines. The receiver tries three renderer modes (full → compatible →
software); if all three fail, the message lists each reason.
- Update the graphics driver.
- On hybrid laptops try `--gpu=high` or `--gpu=low`.
- `--renderer=software` always works (slowly) and shows whether the GPU path is the problem.
- If the build came from a plain "Developer Command Prompt", it is 32-bit: rebuild from
  "x64 Native Tools Command Prompt".

(Before version 2 such errors were invisible: the message box closed immediately and the exit
code was 0.)

**"Looking for your Mac…" for a long time.**
- On the Mac, External Display must be running with **Start Capture** on.
- Both machines must be on the same network (no guest Wi-Fi isolation).
- Windows Firewall must allow the receiver on *private* networks, including mDNS (UDP 5353)
  and UDP 9877.
- Or skip discovery: `ExternalDisplayReceiver.exe <Mac IP>`.

**"That code does not match the Mac's."** A digit was mistyped, or someone is between the two
machines. A new code appears on the Mac automatically; type the new one. If it keeps failing on
a trusted network, check whether another PC is also trying to connect.

**"Pairing was rejected on the Mac."** Someone clicked Reject. Restart the receiver to try again.

**"The Mac app and this receiver are different versions."** Update both from the same commit.
Protocol 2 does not talk to protocol 1.

**Connected, but the screen stays on "Waiting for the picture".** Click **Start Capture** on
the Mac. If capture is running and the Mac's screen is static, nothing is sent until something
changes; move a window.

**Status card says "Graphics: compatible / software mode".** The GPU refused the full mode
(details in `receiver.log`). Everything works, with more latency (compatible) or much more CPU
(software).

**High latency or stutter.** Press F2 and see which stage dominates
([PERFORMANCE.md](PERFORMANCE.md)):
- **Decode (CPU)** in the FPS line means software decoding: check the `[Decoder]` lines in the
  log.
- **Network** spikes point to Wi-Fi. Regular hitches about once a second usually come from AWDL
  on the Mac (AirDrop, Handoff, Universal Control). [NETWORK.md](NETWORK.md) shows how to measure
  it (`tests/network/ping-spikes.sh`) and what to turn off. Also: 5 GHz, near the access point,
  or Ethernet on one side.
- **Display n/a** means the driver gives no frame statistics. Total is then measured to
  Present().

**The mouse lands in the wrong place.** Fixed in version 2: clicks are mapped through the
letterboxed picture. If it still happens, send `receiver.log` with `--verbose`.

**Colours look washed out (black is grey, white is dull).** That was a bug: the Mac sent video
range (16–235) and the PC showed it as full range. Fixed in smoothness step 4; update both apps.
The F2 overlay names the colours the stream uses (`full-range BT.709`).

**A line tears through the picture while scrolling.** "Present at once" is on (F5 or
`--present=immediate`): frames are shown without waiting for the refresh. Press F5 to go back
to presenting on the refresh (the default).

**Text on the status card or overlay is tiny or huge.** The receiver follows Windows' display
scale (Settings → System → Display → Scale). `receiver.log` shows what it found
(`DPI awareness: ..., screen at 150 %`). If it says `not DPI-aware`, Windows refused. Send the
log.

**The picture freezes for a moment, then continues.** A frame went missing, and the PC keeps
the last good picture until the Mac's keyframe arrives, usually within a few hundredths of a
second ([SMOOTHNESS.md](SMOOTHNESS.md), step 2). A single lost packet is normally rebuilt from
parity without any freeze (step 5). Freezes mean several packets of one frame were lost.
- The F2 `Loss` line shows how many packets were lost and rebuilt. If loss is high, see step 8
  of SMOOTHNESS.md (Wi-Fi setup).
- To check that the repair works at all, use `--simulate-loss=1` on a good network: the
  `rebuilt` count rises and the picture stays smooth.
- If `paced` is missing from the overlay's `Mac` line, the Mac app is older than step 5.

**The picture gets softer when the Wi-Fi is busy.** That is intended
([SMOOTHNESS.md](SMOOTHNESS.md), step 6). When packets start to wait in a queue on the way,
before any are lost, the Mac lowers the bitrate within a few tenths of a second. It raises it
again once the queue is gone. A softer picture that keeps up is smoother than a sharp one that
lags.
- F2: the `Net wait` line shows how long packets waited on the way and the verdict: `normal`,
  `hold`, or `overuse: Mac lowers the bitrate`. The `Mac` line shows the encoder's bitrate and
  its target (`enc 5.9/6.8 Mbps`).
- The Mac's log (menu 🔍 → Show Log) shows every cut and, once a second, the rises:
  `Bitrate down to 5980 kbps (overuse, queue 21 ms, loss 0 %, received 7900 kbps, link full at
  7960 kbps)`. `link full at` is what the network carried while it was full.
- `Net wait` says overuse again and again while the target is already low: the network is too
  slow or very busy. See step 8 of SMOOTHNESS.md.
- `--adapt=tiers` brings back the older behaviour (lower only after loss or a long RTT), to
  compare.
- To see it work on a good network: `--simulate-link=8000` (an 8 Mbps link with a 200 ms queue).
  The target goes below 8 Mbps within about 2 s, and `Net wait` stays at a few milliseconds.
- If the target never changes while `Net wait` says overuse, the Mac app is older than step 6:
  it ignores the PC's reports and keeps the tiers.

**No 120 FPS on a 120 Hz screen.** The Mac's window says why next to the preset
([SMOOTHNESS.md](SMOOTHNESS.md), step 7):
- `High streams 120 to this PC's 120 Hz screen`: choose the High preset.
- `the PC's screen runs at 60 Hz`: Windows runs the screen at 60. Change it in Settings → System
  → Display → Advanced display. `receiver.log` says `Screen refresh rate: … Hz`. Was the receiver
  started with `--max-fps=60`?
- `the PC has not said its screen's refresh rate`: the receiver is older than step 7. Update it.
- `the captured display refreshes at 60 Hz`: the virtual display has no 120 Hz mode. The Mac's
  log lists the modes it offers (`No 120 Hz mode (this display offers …)`). Create the virtual
  display again with High selected.
- `the encoder cannot keep up with 120 at this size`: this Mac encodes a frame of this size in
  more than 8.3 ms. On an M2 that happens at 2560×1440 and larger. Use 1920×1080 or 1920×1200
  for 120 FPS. The log line says how long a frame took.
- `receiver.log` shows `START_STREAM … @ 60 FPS` even when it then runs at 120. The PC's refresh
  rate arrives a moment after the stream starts, and the frame rate changes without a new
  START_STREAM. F2's FPS line shows the real rate.

**Two pointers, no pointer, or the pointer looks wrong.** Since step 1 of
[SMOOTHNESS.md](SMOOTHNESS.md), Windows draws the Mac's pointer. With F3 off you see Windows'
own arrow *and* the Mac's pointer: that is intended. Press **F4** (or start with
`--cursor=video`) to put the pointer back into the video. `receiver.log` says which mode the
Mac was told (`Telling the Mac: pointer drawn here / in the video`).

## Mac sender

**Mouse and keyboard from the PC do nothing.** Accessibility permission is missing (logged as a
warning at start). System Settings → Privacy & Security → Accessibility: add the app, then
**restart it**. A rebuilt development copy can lose the permission even though it is still
listed (each build is signed differently): remove it from the list with −, add it again, and
restart it.

**"Permission denied" / no displays.** Screen Recording permission: System Settings → Privacy &
Security → Screen Recording, then click ↺.

**No pairing request appears.**
- The receiver may already be paired: check the "Paired PCs" menu.
- After *Forget*, the receiver has to pair again on its next connection.

**The virtual display does not appear.** It uses a private macOS API; see
[ARCHITECTURE.md](ARCHITECTURE.md#virtual-display). Look for `[Display]` errors in
`sender.log`. Capturing a physical display still works.

**Battery drains fast.**
- Use the **Battery Saver** preset (30 FPS, ≤ 12 Mbps).
- Keep "Preview while streaming" off.
- A static screen costs almost nothing on either side.
