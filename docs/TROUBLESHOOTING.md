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
- **Network** spikes point to Wi-Fi: use 5 GHz, stay near the access point, or use Ethernet;
  lower the quality preset.
- **Display n/a** means the driver gives no frame statistics. Total is then measured to
  Present().

**The mouse lands in the wrong place.** Fixed in version 2: clicks are mapped through the
letterboxed picture. If it still happens, send `receiver.log` with `--verbose`.

**The picture freezes for a moment, then continues.** After a lost packet the PC keeps the
last good picture until the Mac's keyframe arrives, usually within a few hundredths of a second
([SMOOTHNESS.md](SMOOTHNESS.md), step 2). If it happens often, the Wi-Fi is losing packets:
watch the `Loss` and `Keyframe` lines in the F2 overlay, and see step 8 of SMOOTHNESS.md.

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
