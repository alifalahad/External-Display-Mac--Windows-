# Architecture

## Overview

```
MAC (Swift)                                             WINDOWS (C++)
───────────────────────────────────────────────         ───────────────────────────────────────────────
VirtualDisplayManager   CGVirtualDisplay (2nd monitor)
ScreenCaptureEngine     ScreenCaptureKit, BGRA IOSurface
        │ CVPixelBuffer (stays on the GPU)
VideoEncoder            VideoToolbox H.264, low-latency RC
        │ AVCC → Annex B (H264Converter) + timing prefix
StreamSender            packetize · AES-GCM · UDP ────────▶ StreamReceiver   UDP thread: decrypt, replay check,
                        control · handshake · TCP ◀──────▶                   reassemble; TCP thread: handshake
        ▲                                                       │ H.264 access units (bounded queue)
InputInjector     ◀── input events ───────────────────── InputCapture     window messages → normalized 0…1
ClipboardSync     ◀── clipboard text ─────────────────▶  ClipboardSync    clipboard listener window
PairingManager    keys at rest (0600 file)               PairingManager   keys at rest (DPAPI)
AdaptiveQuality   bitrate within the QualityPreset ◀──── quality reports (loss, RTT, latency)
                                                         H264Decoder      Media Foundation, D3D11 (DXVA)
                                                                │ NV12 texture (GPU → GPU copy)
                                                         D3D11Renderer    NV12→RGB shader, letterbox,
                                                                          waitable swap chain (1 frame)
                                                         ReceiverApp      phases, status card, metrics
```

## Components

**Mac** (`mac-sender/ExternalDisplay/`)

| Folder | Responsibility |
|---|---|
| `Capture/` | `ScreenCaptureEngine`: capture, encoder wiring, preview, quality presets, recovery, diagnostics tick |
| `Display/` | `VirtualDisplayManager`: creates and removes the virtual monitor |
| `Encoder/` | `VideoEncoder` (VideoToolbox), `H264Converter`, `AdaptiveQuality`, `QualityPreset`, `EncoderConfig` |
| `Network/` | `Protocol` (wire format), `StreamSender` (listener, handshake, encryption, video packets) |
| `Security/` | `SecureChannel` (key agreement, AES-GCM), `PairingManager` (pairings, approval) |
| `Input/`, `Clipboard/` | Injecting Windows input with CGEvent; pasteboard sync |
| `Performance/` | Clock, rolling statistics, process CPU and memory |
| `App/`, `UI/` | Logging; SwiftUI views |

**Windows** (`windows-receiver/src/`)

| Folder | Responsibility |
|---|---|
| `App/` | `ReceiverApp` (main loop, phases), `MacFinder`, `Options`, `StatusText`, `Window`, `Log`, `CrashHandler` |
| `Network/` | `Protocol.h`, `StreamReceiver` (TCP/UDP, handshake, decryption, reassembly, clock sync) |
| `Security/` | `Crypto` (CNG wrappers, DPAPI), `SecureChannel` (key schedule, session ciphers), `ReplayWindow`, `PairingManager` |
| `Decoder/` | `H264Decoder` (Media Foundation; D3D11 hardware or software) |
| `Renderer/` | `D3D11Renderer` (swap chain, NV12 shader, status card, overlay, frame statistics) |
| `Input/`, `Clipboard/`, `Discovery/`, `UI/` | Input capture and mapping, clipboard, mDNS browsing, code entry dialog |
| `Performance/` | Pipeline metrics, clock sync, benchmark recorder, system monitor |

## Threads and queues

Every queue is bounded. When a stage falls behind, the **oldest** work is dropped: a fresh frame
matters more than a complete history.

| Where | Thread / queue | Work | Bound / drop rule |
|---|---|---|---|
| Mac | ScreenCaptureKit sample queue | frame arrives, submit to VideoToolbox | SCK `queueDepth` 3 |
| Mac | VideoToolbox callback | AVCC → Annex B | encoder's own (real-time mode) |
| Mac | `networkQueue` (serial) | packetize, encrypt, send; control messages; handshake | sends immediately, no app queue |
| Mac | main | SwiftUI, 1 Hz diagnostics, clipboard poll | — |
| Windows | UDP thread | receive, authenticate, de-duplicate, reassemble | incomplete frames dropped 10 frames later |
| Windows | network thread | TCP control, handshake, reconnection | — |
| Windows | UI thread | messages, decode, render, metrics | input queue 4 frames (trimmed to the newest keyframe); newest decoded picture wins; 1 frame in the swap chain |
| Windows | SystemMonitor thread | CPU / RAM / GPU counters, 1 Hz | — |

The Windows UI thread sleeps in `MsgWaitForMultipleObjectsEx`. It wakes for:
- a frame arriving (an event set by the network thread);
- window input;
- a timer (overlay updates at 4 Hz, statistics at 1 Hz).

It renders only when the picture, the overlay or the status card changed.

## Video path and latency

1. **Capture:** ScreenCaptureKit delivers only changed frames, as IOSurface-backed buffers.
   `displayTime` is when the frame was on screen.
2. **Encode:** VideoToolbox H.264 in hardware, *low-latency rate control*, Constrained High, no
   B-frames. Keyframes only on request, plus one a minute as a safety net
   ([SMOOTHNESS.md](SMOOTHNESS.md), step 2).
3. **Send:** Annex B plus a 40-byte timing prefix. The frame is cut into fragments of up to
   1,340 bytes, and each datagram is encrypted with AES-256-GCM.
4. **Receive:** authenticate, check for replays, reassemble, then queue for decoding.
5. **Decode:** the Media Foundation H.264 decoder on the renderer's D3D11 device (DXVA).
   Pictures stay in GPU memory and are copied GPU→GPU into the texture the shader samples.
   In front of it, the keyframe gate holds frames that follow a missing one until a keyframe
   arrives.
6. **Present:** wait for the swap chain's frame slot (maximum frame latency 1), decode whatever
   arrived meanwhile, draw the newest picture, then present with VSync.

Every frame carries timestamps through all of these steps, and the receiver syncs its clock with
the Mac's. That is how the overlay and the benchmark measure each stage
([PERFORMANCE.md](PERFORMANCE.md)).

## Design decisions

**TCP + UDP with our own framing (rather than WebRTC, QUIC or one TCP stream).**
- Video goes over UDP: a lost packet costs a short freeze until the requested keyframe (about
  one round trip), while TCP would stall every following frame (head-of-line blocking).
- Control messages go over TCP: they must arrive and stay in order.
- QUIC (Network.framework on the Mac, MsQuic on Windows) would add a dependency on Windows and
  a second congestion controller on top of our adaptive bitrate.
- WebRTC is far larger than the problem.
- Recovery on UDP:
  - Lost packets show up as gaps in the per-packet sequence, and the network thread asks for a
    keyframe at once (at most every 0.5 s).
  - The keyframe gate in front of the decoder checks the Mac's frame numbers. That covers frames
    lost entirely, incomplete ones, and ones trimmed from the decode queue. After a gap it holds
    the picture (at most 300 ms) and asks every 250 ms until a keyframe arrives.

**The Mac listens; Windows discovers and connects.**
- The Mac app advertises `_externaldisplay._tcp` with Bonjour; the receiver browses with its own
  minimal mDNS client (no Bonjour SDK needed on Windows).
- The master prompt sketched the reverse (the Mac discovering Windows). This direction means the
  PC never runs a server and needs no inbound firewall rule for discovery. The Mac already has
  to approve every new receiver anyway.

**Security from standard primitives.**
- ECDH P-256 + HKDF + AES-256-GCM, with a hash commitment and a verification code, modelled on
  ZRTP and Bluetooth numeric comparison.
- TLS/DTLS was considered: Windows SChannel has no PSK mode, DTLS needs certificate handling on
  both sides, and the pairing UX would still need a code. Noise needs a third-party library on
  both platforms.
- Reasoning and limitations: [SECURITY.md](SECURITY.md).

**Hardware first, software fallbacks.**
- Every hardware path has a fallback that keeps the display working, and the fallback is logged
  and shown:
  - hardware encoder, then the normal encoder;
  - hardware decoder, then software decode;
  - full renderer, then compatible, then WARP.

**Event-driven, power-aware.**
- The Mac preview is off while streaming, and App Nap is blocked during capture.
- The Windows loop sleeps until there is work; a static screen costs almost nothing.
- Battery Saver halves the frame rate.

## Virtual display

macOS has no public API to create a display. `CGVirtualDisplay`, a private CoreGraphics class,
is reached at runtime with `NSClassFromString`. The display appears
in System Settings → Displays and can be arranged like any monitor.

Risk: a macOS update can change or remove the class. `VirtualDisplayManager` checks for every
class and selector before using it, and reports a clear error instead of crashing. Capturing a
physical display (mirroring) still works without it.

The public alternative is a DriverKit display driver. That needs a system extension, entitlements
and notarization, which is out of scope for this project.
