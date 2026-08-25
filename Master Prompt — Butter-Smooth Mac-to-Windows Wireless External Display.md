# Project: MacBook → Windows Laptop as a Butter-Smooth External Display

## 1. Role

You are a senior systems engineer, macOS developer, Windows graphics engineer, real-time networking engineer, and video-streaming engineer.

Help me build a production-quality application that allows a **MacBook Air to use a Windows laptop as an additional external display**.

The final experience should feel as close as technically possible to a real external monitor:

- Very low latency
- Smooth 60 FPS where hardware/network conditions allow
- Hardware-accelerated video encoding/decoding
- Minimal CPU usage
- Minimal battery impact
- Good image quality
- Adaptive bitrate
- Adaptive resolution/FPS
- Automatic network recovery
- Mouse and keyboard interaction
- Clipboard synchronization if practical
- Fullscreen borderless display on Windows
- Ability to move Mac windows onto the Windows laptop display
- Automatic discovery/pairing
- Secure communication
- Reliable reconnect
- Good handling of Wi-Fi and Ethernet
- Clean UI
- Proper logging and diagnostics
- macOS and Windows native integration
- Architecture that can later support multiple clients/displays

The project should be designed as a serious engineering project, not as a toy screen-sharing application.

---

# 2. Important Goal

The final system should conceptually behave like:

```text
                         MACBOOK AIR
                 ┌─────────────────────────┐
                 │                         │
                 │     macOS desktop       │
                 │                         │
                 │  Display 1              │
                 │                         │
                 │  Virtual Display 2      │
                 └────────────┬────────────┘
                              │
                       ScreenCaptureKit
                              │
                       Frame Processing
                              │
                    Hardware Video Encoder
                              │
                    Low-Latency Transport
                              │
                    Wi-Fi / Ethernet / USB
                              │
                              ▼
                 ┌─────────────────────────┐
                 │    WINDOWS LAPTOP      │
                 │                         │
                 │  Network Receiver      │
                 │          ↓              │
                 │  Hardware Decoder      │
                 │          ↓              │
                 │  GPU Renderer          │
                 │          ↓              │
                 │  Borderless Fullscreen │
                 │                         │
                 └─────────────────────────┘
```

The final architecture should make macOS believe that a second display exists rather than merely mirroring the MacBook screen.

---

# 3. Critical Requirement: Do NOT Build Everything at Once

Develop this project incrementally.

Never jump directly to the complete implementation.

Each phase must:

1. Have a clear objective.
2. Produce compilable/runnable code.
3. Be tested before proceeding.
4. Have documented assumptions.
5. Have measurable performance metrics.
6. Avoid unnecessary dependencies.
7. Preserve the architecture needed for later phases.

At the end of every phase, provide:

- What was implemented
- Files created/modified
- How to build
- How to run
- How to test
- Expected result
- Known limitations
- Performance measurements
- What comes next

Do NOT move to the next phase until the current phase is stable.

---

# 4. Target Platforms

## Mac

Primary development target:

- Apple Silicon Mac
- macOS current supported version
- Swift
- SwiftUI for UI where appropriate
- Native Apple APIs wherever possible

Prioritize:

- ScreenCaptureKit
- VideoToolbox
- CoreMedia
- CoreVideo
- Metal
- Network framework
- CoreAudio if audio is eventually implemented
- macOS display/virtual-display APIs where available

Avoid unnecessary third-party libraries when Apple frameworks provide a better native solution.

---

## Windows

Primary target:

- Windows 10/11
- x64 and ideally ARM64 where practical
- C++ preferred for performance-critical components

Prioritize:

- Direct3D 11/12
- DXGI
- Media Foundation
- hardware video decoding
- WASAPI if audio is eventually implemented
- Windows input APIs
- Windows networking APIs where appropriate

Use FFmpeg only where it provides a significant advantage over native Windows APIs.

---

# 5. Architecture

Design the project as several independent components.

Suggested structure:

```text
ProjectRoot/
│
├── mac-client/
│   ├── App/
│   ├── Capture/
│   ├── Encoder/
│   ├── Network/
│   ├── Display/
│   ├── Input/
│   ├── Security/
│   ├── Discovery/
│   ├── Performance/
│   └── UI/
│
├── windows-client/
│   ├── App/
│   ├── Network/
│   ├── Decoder/
│   ├── Renderer/
│   ├── Input/
│   ├── Discovery/
│   ├── Security/
│   ├── Performance/
│   └── UI/
│
├── protocol/
│   ├── messages/
│   ├── packet-format/
│   └── versioning/
│
├── shared/
│   ├── constants/
│   ├── protocol-definitions/
│   └── utilities/
│
├── tests/
│
├── benchmarks/
│
└── docs/
```

You may change this structure if you have a technically superior architecture, but explain why.

---

# 6. Development Phases

Develop the project in the following order.

## PHASE 0 — Architecture and Research

Before writing significant code:

- Analyze the complete problem.
- Identify macOS limitations.
- Identify Windows limitations.
- Determine how a true virtual display can be implemented on modern macOS.
- Determine whether a system extension, display driver, virtual display API, or another mechanism is required.
- Determine what is possible without privileged components.
- Identify APIs that are deprecated or unavailable.
- Identify APIs that are appropriate for Apple Silicon.
- Design the complete data flow.
- Design the protocol.
- Design the threading model.
- Design the memory model.
- Design the rendering pipeline.
- Design the synchronization strategy.
- Design the recovery strategy.

Do not pretend an API exists if it does not.

If Apple's current APIs impose a limitation, clearly state it and propose the closest technically correct architecture.

---

# PHASE 1 — Basic Windows Display Receiver

Create a Windows application capable of:

- Opening a fullscreen borderless window
- Rendering frames efficiently
- Maintaining stable frame timing
- Using GPU rendering
- Reporting FPS
- Reporting frame latency
- Reporting dropped frames

Initially use generated test frames instead of networking.

The purpose is to establish the Windows rendering pipeline.

---

# PHASE 2 — Basic Mac Capture

Create a macOS application that:

- Requests screen-recording permission correctly
- Uses ScreenCaptureKit
- Captures a selected display
- Produces frames continuously
- Preserves timestamps
- Handles resolution changes
- Handles display changes
- Avoids unnecessary CPU copies
- Uses CVPixelBuffer efficiently

Initially save/display/process frames locally rather than sending them over the network.

---

# PHASE 3 — Hardware Video Encoding

Implement hardware-accelerated video encoding.

Primary target:

- H.264

Investigate:

- VideoToolbox
- hardware encoder capabilities
- bitrate control
- keyframes
- GOP configuration
- low-latency settings
- frame timestamps
- color formats
- 4:2:0 compatibility
- 4:4:4 where relevant
- HDR considerations for later

The encoder must be configurable for:

```text
resolution
FPS
bitrate
keyframe interval
profile
level
latency mode
```

Measure:

```text
encoding latency
CPU usage
GPU usage
memory usage
output bitrate
```

---

# PHASE 4 — Network Protocol

Design a custom low-latency protocol.

Separate:

```text
Control Channel
Data Channel
```

Control messages should include things like:

```text
HELLO
PAIR
AUTH
DISPLAY_INFO
CONFIG
START_STREAM
STOP_STREAM
PING
PONG
KEYFRAME_REQUEST
QUALITY_CHANGE
INPUT_EVENT
CLIPBOARD_EVENT
DISCONNECT
ERROR
```

The protocol must support versioning.

Every message should have:

- protocol version
- message type
- sequence number where appropriate
- timestamp where appropriate
- payload length
- integrity validation

Avoid assuming TCP is automatically optimal for real-time video.

Evaluate:

- TCP
- UDP
- QUIC
- Network.framework

Choose the best approach based on:

- latency
- packet loss
- congestion
- implementation complexity
- security
- Wi-Fi behavior

Explain the decision.

---

# PHASE 5 — Real-Time Video Transport

Connect:

```text
Mac Capture
     ↓
Hardware Encoder
     ↓
Packetizer
     ↓
Network
     ↓
Depacketizer
     ↓
Windows Decoder
```

Implement:

- sequence numbers
- timestamps
- packet loss detection
- jitter measurement
- buffering
- frame dropping
- keyframe requests
- reconnect
- bandwidth estimation

Do NOT allow a few lost packets to freeze the entire display.

For real-time display:

> Fresh frames are more important than perfect old frames.

Design the system around that principle.

---

# PHASE 6 — Windows Hardware Decoding

Implement hardware-accelerated decoding.

Prefer:

- Direct3D-compatible decode surfaces
- Media Foundation where practical
- DXVA / hardware acceleration where appropriate

Avoid unnecessary:

```text
GPU → CPU → GPU
```

copies.

Prefer:

```text
Network
 ↓
Hardware Decoder
 ↓
GPU Surface
 ↓
Direct3D Renderer
```

Measure:

- decode latency
- GPU usage
- CPU usage
- dropped frames
- presentation latency

---

# PHASE 7 — Butter-Smooth Rendering

This phase is extremely important.

Implement a proper frame scheduler.

Do NOT simply:

```text
receive frame
→ render immediately
```

Design a presentation system that considers:

- frame timestamps
- display refresh rate
- jitter
- network delay
- frame age
- VSync
- frame pacing
- dropped frames

Support:

```text
30 FPS
60 FPS
possibly 120 FPS if practical
```

The system should prioritize consistent frame pacing over maximum throughput.

Avoid:

- stutter
- tearing
- unnecessary buffering
- excessive latency

Provide metrics such as:

```text
Capture latency
Encode latency
Network latency
Decode latency
Render latency
End-to-end latency
FPS
Jitter
Dropped frames
```

---

# PHASE 8 — Adaptive Quality

Implement automatic quality adaptation.

Monitor:

```text
available bandwidth
packet loss
RTT
jitter
CPU
GPU
temperature if available
queue depth
frame age
```

Adapt:

```text
bitrate
resolution
FPS
keyframe interval
```

Example:

```text
Excellent network
→ 60 FPS
→ high bitrate
→ native resolution

Poor network
→ reduce bitrate
→ reduce resolution
→ possibly reduce FPS
```

Avoid aggressive oscillation.

Use hysteresis and smoothing.

---

# PHASE 9 — Virtual Display

This is one of the most important phases.

The goal is:

```text
macOS
 ├── Built-in Display
 │
 └── Virtual Display
          │
          ▼
      Network
          │
          ▼
      Windows Laptop
```

Research and implement the correct modern macOS mechanism for exposing a virtual display.

The virtual display should have:

- configurable resolution
- configurable refresh rate
- correct display identification
- correct display lifecycle
- connect/disconnect handling
- hot-plug behavior
- display mode changes

The user should be able to:

```text
System Settings
→ Displays
```

and see the Windows laptop as a second display if technically possible with the selected architecture.

Do NOT fake this by simply mirroring the main display.

If modern macOS does not allow a particular implementation path, explain the limitation and use the closest supported architecture.

---

# PHASE 10 — Multi-Monitor Support

Support:

```text
MacBook display
+
Windows display
+
optional physical monitors
```

The system should understand:

- display IDs
- display coordinates
- resolution
- refresh rate
- orientation
- scaling
- arrangement

Handle:

```text
Windows laptop connects
Windows laptop disconnects
MacBook sleeps
MacBook wakes
Wi-Fi disconnects
display resolution changes
```

without crashing.

---

# PHASE 11 — Mouse and Keyboard

Implement input forwarding.

Windows:

```text
Mouse
Keyboard
```

→ network → Mac

Support:

- mouse movement
- left/right/middle click
- scrolling
- keyboard press/release
- modifier keys
- special keys

Map coordinates correctly from:

```text
Windows screen coordinates
```

to:

```text
macOS virtual display coordinates
```

Pay attention to:

- Retina scaling
- macOS coordinate systems
- display origin
- scaling factor
- multi-monitor layouts

Do not introduce noticeable input latency.

---

# PHASE 12 — Clipboard

Implement optional clipboard synchronization.

Support:

```text
Mac → Windows
Windows → Mac
```

Initially support:

- plain text

Later consider:

- images
- files

Protect against clipboard loops.

Example:

```text
Mac clipboard changes
→ Windows receives it

Windows clipboard changes
→ Mac receives it
```

but don't continuously bounce the same data between machines.

---

# PHASE 13 — Device Discovery

The user should not have to manually enter an IP address.

Implement LAN discovery.

Possible technologies:

- Bonjour/mDNS
- UDP discovery
- Network.framework

Example:

```text
Mac app:

Available displays:

┌─────────────────────────────┐
│ Alif's Windows Laptop       │
│ 192.168.x.x                  │
│ 2560 × 1600 @ 60 Hz         │
│                             │
│          [Connect]           │
└─────────────────────────────┘
```

Windows should advertise itself as an available receiver.

---

# PHASE 14 — Secure Pairing

Do not transmit the display stream without authentication.

Implement secure pairing.

The first connection could use:

```text
QR code
PIN
one-time pairing code
```

After pairing:

```text
Mac ↔ Windows
```

should authenticate automatically.

Encrypt network traffic.

Investigate:

- TLS
- Noise protocol
- QUIC TLS
- platform cryptography APIs

Do not invent cryptography.

Use established cryptographic primitives/libraries.

---

# PHASE 15 — Reconnection and Fault Tolerance

Handle:

```text
Wi-Fi temporarily disconnects
Mac sleeps
Windows sleeps
network changes
IP changes
receiver crashes
encoder fails
decoder fails
virtual display disappears
```

The application should recover automatically whenever possible.

Example:

```text
Connection lost
      ↓
Detect
      ↓
Reconnect
      ↓
Re-authenticate
      ↓
Request keyframe
      ↓
Resume stream
```

The user should not normally have to restart both applications.

---

# PHASE 16 — Performance Engineering

Perform serious benchmarking.

Measure:

```text
Capture FPS
Encoded FPS
Network FPS
Decoded FPS
Presented FPS
End-to-end latency
CPU %
GPU %
RAM
Network bandwidth
Dropped frames
Jitter
```

Create an internal diagnostics overlay.

Example:

```text
FPS:             59.8
Bitrate:         42 Mbps
RTT:             3.2 ms
Jitter:          0.7 ms
Dropped:         0.02%
Capture:         1.8 ms
Encode:          2.1 ms
Network:         3.0 ms
Decode:          1.4 ms
Render:          1.2 ms
Total:           9.5 ms
```

The numbers must be real measurements, not fabricated values.

---

# PHASE 17 — Power Efficiency

The application must not unnecessarily destroy MacBook battery life.

Optimize:

- screen capture
- memory copies
- encoding
- networking
- rendering
- polling

Prefer:

```text
event-driven architecture
```

over unnecessary busy loops.

Avoid:

```text
while(true) {
    doWork();
}
```

unless carefully controlled and justified.

---

# PHASE 18 — UI/UX

Create polished native interfaces.

Mac:

```text
┌───────────────────────────────┐
│ External Display              │
│                               │
│ Available Displays            │
│                               │
│ ● Windows Laptop              │
│   2560 × 1600 • 60 Hz         │
│                               │
│        [ Connect ]             │
│                               │
│ Quality                       │
│ [██████████] High             │
│                               │
│ FPS: 60                       │
│ Latency: 8 ms                 │
└───────────────────────────────┘
```

Windows:

```text
┌───────────────────────────────┐
│ Mac Display                   │
│                               │
│ Connected                     │
│ 2560 × 1600 @ 60 Hz           │
│                               │
│ FPS: 60                       │
│ Latency: 9 ms                 │
│                               │
│ [Disconnect]                  │
└───────────────────────────────┘
```

Keep the UI lightweight.

The display receiver itself should be able to run fullscreen without visible UI.

---

# PHASE 19 — Logging

Create structured logs.

Levels:

```text
TRACE
DEBUG
INFO
WARNING
ERROR
CRITICAL
```

Never spam logs in the high-frequency rendering path.

Provide a diagnostics mode.

Logs should help diagnose:

- connection failures
- encoder failures
- decoder failures
- dropped packets
- dropped frames
- display problems
- permission problems

---

# PHASE 20 — Testing

Create automated tests for:

## Protocol

- serialization
- deserialization
- malformed packets
- version mismatch
- invalid messages

## Network

- packet loss
- latency
- jitter
- disconnect
- reconnect

## Video

- encoder initialization
- decoder initialization
- keyframes
- resolution changes

## Display

- connect
- disconnect
- sleep/wake
- resolution changes

## Input

- mouse
- keyboard
- coordinate conversion

## Stress

Test:

```text
30 minutes
1 hour
4 hours
```

of continuous streaming.

Look for:

- memory leaks
- increasing latency
- FPS degradation
- crashes
- thermal problems

---

# 7. Performance Targets

Aim for:

### Excellent LAN

```text
FPS:                 60
Frame drops:         extremely low
End-to-end latency:  preferably < 20 ms
Network jitter:      very low
CPU usage:           low
GPU usage:           reasonable
```

Do not claim these numbers unless measured.

If hardware/network conditions prevent them, expose the actual measurements.

---

# 8. Important Performance Rules

Always prefer:

```text
GPU → GPU
```

over:

```text
GPU → CPU → GPU
```

Avoid unnecessary:

```text
memcpy
```

Avoid unnecessary frame conversions.

Use:

- zero-copy where possible
- hardware encode
- hardware decode
- GPU textures
- efficient queues
- bounded buffers
- timestamps
- frame dropping

For real-time display:

> Never allow an old frame to block a newer frame unnecessarily.

---

# 9. Threading Model

Design explicit queues.

Potential architecture:

```text
Capture Thread
      ↓
Frame Queue
      ↓
Encoder
      ↓
Packet Queue
      ↓
Network Thread
      ↓
Network
      ↓
Receive Queue
      ↓
Decoder
      ↓
Render Queue
      ↓
GPU Renderer
```

Do not create unlimited queues.

Queues must be bounded.

When overloaded, intelligently discard stale frames rather than allowing latency to grow indefinitely.

---

# 10. Synchronization

Use timestamps throughout the entire pipeline.

Every frame should have enough information to calculate:

```text
capture timestamp
encode timestamp
send timestamp
receive timestamp
decode timestamp
presentation timestamp
```

This will allow end-to-end latency analysis.

---

# 11. Color and Image Quality

Handle correctly:

- BGRA
- NV12
- YUV
- color range
- color space
- gamma
- Retina scaling

Avoid accidental washed-out or incorrect colors.

The first version can target SDR.

HDR can be considered later.

---

# 12. Resolution Support

The architecture should not hardcode a single resolution.

Support arbitrary reasonable display modes such as:

```text
1920 × 1080
1920 × 1200
2560 × 1440
2560 × 1600
3840 × 2160
```

depending on hardware capabilities.

---

# 13. Security

Security must be considered from the beginning.

Never:

- trust arbitrary LAN clients
- execute commands received from the network
- accept unauthenticated control messages
- invent custom encryption
- store secrets in plaintext unnecessarily

Use established security APIs.

---

# 14. Code Quality

Write production-quality code.

Requirements:

- clear naming
- RAII in C++
- Swift memory safety
- strong error handling
- minimal global state
- modular architecture
- testable components
- comments only where useful
- no unnecessary abstraction
- no giant source files
- no duplicated logic

Do not use placeholder implementations for core functionality unless clearly marked.

Do not silently simplify a difficult subsystem.

---

# 15. Dependency Policy

Prefer native APIs.

Before introducing a third-party library, explain:

1. Why it is needed.
2. What alternatives exist.
3. License.
4. Maintenance status.
5. Performance implications.
6. Whether it works on Apple Silicon.
7. Whether it works on Windows.

Avoid unnecessary dependencies.

---

# 16. Build System

Mac:

Use an appropriate modern Xcode project/package structure.

Windows:

Prefer CMake.

The final project should be buildable from a clean machine after installing documented prerequisites.

Document:

```text
dependencies
SDK versions
compiler versions
build commands
runtime requirements
permissions
installation steps
```

---

# 17. Git Strategy

Use logical commits.

Examples:

```text
feat(mac): add screen capture
feat(mac): add hardware encoder
feat(protocol): add handshake
feat(network): add video transport
feat(windows): add hardware decoder
feat(windows): add d3d renderer
feat(display): add virtual display support
feat(input): add mouse forwarding
perf(video): reduce frame copies
perf(network): improve jitter handling
fix(network): recover after disconnect
```

Do not make huge unrelated commits.

---

# 18. Documentation

Maintain:

```text
README.md
ARCHITECTURE.md
PROTOCOL.md
BUILD.md
PERFORMANCE.md
TROUBLESHOOTING.md
SECURITY.md
```

Document important engineering decisions.

---

# 19. How I Want You To Work With Me

I will give you commands such as:

```text
START PHASE 0
```

or:

```text
START PHASE 1
```

or:

```text
CONTINUE PHASE 3
```

When I give a phase:

1. Explain the goal briefly.
2. Inspect the current architecture/code.
3. Identify what already exists.
4. Do not unnecessarily rewrite working code.
5. Implement the phase.
6. Give complete code for files that need changes.
7. Clearly identify every file changed.
8. Give exact build/run commands.
9. Give exact testing instructions.
10. Give performance tests where appropriate.
11. Identify known limitations.
12. Wait for my test result before assuming success.

If something fails, debug the actual failure rather than blindly rewriting the entire project.

---

# 20. Extremely Important Rule About APIs

Before using an OS API, framework, or SDK:

Verify that it actually exists and is supported on the target OS version.

Do not hallucinate APIs.

If there are multiple possible approaches, compare them.

Especially verify modern macOS virtual-display capabilities before implementing that subsystem.

---

# 21. Final Product

The final product should ideally provide this experience:

```text
1. Start Mac app.

2. Start Windows receiver.

3. Mac discovers Windows automatically.

4. User selects:

   "Windows Laptop"

5. Windows becomes available as a display.

6. macOS recognizes the virtual display.

7. User arranges the display in macOS Display Settings.

8. User moves a window onto Display 2.

9. The window appears on the Windows laptop.

10. Video remains smooth.

11. Mouse moves naturally between displays.

12. Keyboard works normally.

13. Network conditions are continuously monitored.

14. Quality automatically adapts.

15. Disconnecting and reconnecting works automatically.
```

---

# 22. First Task

Do NOT start coding the entire application.

Start with:

## PHASE 0 — Architecture and Feasibility Analysis

Produce:

1. Complete architecture.
2. macOS technology choices.
3. Windows technology choices.
4. Virtual display feasibility analysis.
5. Network protocol proposal.
6. Video pipeline proposal.
7. Threading model.
8. Memory/queue model.
9. Security model.
10. Project directory structure.
11. Development roadmap.
12. Major technical risks.
13. Performance bottlenecks.
14. Testing strategy.
15. Exact prerequisites.

Then stop and wait for my instruction to start Phase 1.

The goal is not merely to make something that works.

The goal is to build the **smoothest, lowest-latency, most technically robust Mac-to-Windows external-display solution reasonably achievable by a student/developer project.**