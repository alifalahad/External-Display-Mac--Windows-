# External Display — use a Windows laptop as a Mac display

A MacBook streams a **virtual second display** to a Windows laptop over Wi-Fi or Ethernet. The
Windows screen behaves like an extra monitor: move windows onto it, click and type on it, and
copy and paste between the two machines.

```
 MacBook                                         Windows laptop
 ┌──────────────────────────┐                    ┌──────────────────────────┐
 │ virtual display (macOS)  │                    │ full-screen receiver      │
 │ ScreenCaptureKit capture │   encrypted LAN    │ hardware H.264 decode     │
 │ VideoToolbox H.264 (HW)  │ ─── video (UDP) ──▶│ Direct3D 11, 1 frame queue│
 │ input / clipboard inject │ ◀── input, ──────  │ mouse, keyboard, clipboard│
 └──────────────────────────┘   clipboard (TCP)  └──────────────────────────┘
```

## Features

- A real virtual display: it appears in System Settings → Displays and can be arranged like a
  monitor.
- Hardware encoding (VideoToolbox, low-latency mode) and hardware decoding (Media Foundation
  through Direct3D 11). Frames stay on the GPU, with a software fallback.
- Event-driven rendering with a single queued frame: nothing is drawn when nothing changed.
- A bitrate that follows the network: it drops as soon as packets start to queue on the way,
  before any are lost, and rises again when the queue is gone. The quality preset (Battery
  Saver / Balanced / High) sets how high it goes.
- Lost packets are rebuilt on the PC from parity packets, so a single lost packet causes no
  freeze.
- 120 FPS to a PC screen at 120 Hz or more (High preset, where the Mac's encoder keeps up at the
  display's size).
- Mouse, keyboard and clipboard (text) forwarding. Windows draws the Mac's pointer itself, so
  the mouse feels local: no video round trip ([docs/SMOOTHNESS.md](docs/SMOOTHNESS.md)).
- Automatic discovery (Bonjour/mDNS), automatic reconnection and recovery after sleep.
- **Security:** pairing with a verification code, and every message encrypted
  (ECDH P-256, AES-256-GCM). Details in [docs/SECURITY.md](docs/SECURITY.md).
- Diagnostics overlay (F2) with real measurements: latency per stage, FPS, jitter, losses,
  CPU/GPU. A benchmark mode writes CSV files.
- Structured logs on both sides.

## Quick start

**Mac** (macOS 13+, Apple silicon recommended):
1. Open `mac-sender/Package.swift` in Xcode and run.
2. Allow **Screen Recording** when asked. Also allow **Accessibility**, so the PC can control
   the mouse and keyboard.
3. Click **Create Virtual Display**, then **Start Capture**.

**Windows** (10/11, x64):
```bat
cd windows-receiver
cmake -B build
cmake --build build
.\build\ExternalDisplayReceiver.exe
```
The receiver finds the Mac on its own. If several Macs answer, press the number of the one to use.

**The first time, pair the two machines:**
1. The Mac shows a 6-digit code.
2. Type it on Windows.
3. Click **Pair** on the Mac.

From then on they connect without asking.

Controls on Windows: **Esc** quit · **F11** full screen / window · **F2** diagnostics ·
**F3** mouse & keyboard to the Mac · **F4** pointer drawn on the PC / in the video ·
**F5** present at once / on the refresh.

## Documentation

| | |
|---|---|
| [docs/BUILD.md](docs/BUILD.md) | Prerequisites, building, running, command-line options |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, data flow, threads, design decisions |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Wire format, messages, handshake, encryption |
| [docs/SECURITY.md](docs/SECURITY.md) | Threat model, pairing, encryption, limitations |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | What is measured, benchmarks, results |
| [docs/SMOOTHNESS.md](docs/SMOOTHNESS.md) | Roadmap to lower latency: the steps, their status, how each is checked |
| [docs/NETWORK.md](docs/NETWORK.md) | Wi-Fi and network setup for the smoothest picture (AWDL on the Mac, power saving on the PC) |
| [docs/TESTING.md](docs/TESTING.md) | Unit, integration and stress tests |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Symptoms, causes, fixes, where the logs are |

## Status and limitations

- One receiver at a time. The architecture keeps a per-connection session, so several receivers
  are possible later.
- Text clipboard only; no images or files yet.
- The virtual display uses `CGVirtualDisplay`, a private macOS API: there is no public API for
  virtual displays (see [ARCHITECTURE.md](docs/ARCHITECTURE.md#virtual-display)).
- No audio.
- Performance targets: measured results are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
  Numbers that have not been measured are not claimed.
