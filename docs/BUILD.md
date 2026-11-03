# Build and run

## Prerequisites

| | Mac sender | Windows receiver |
|---|---|---|
| OS | macOS 13 Ventura or later (Apple silicon recommended; tested on M2) | Windows 10 (1803+) or 11, x64 (x86 builds and runs too) |
| Tools | Xcode 15+ (Swift 5.9 package) | Visual Studio 2022+ (or Build Tools) with "Desktop development with C++"; CMake 3.20+ |
| Libraries | system only: ScreenCaptureKit, VideoToolbox, CryptoKit, Network | system only: Direct3D 11, DXGI, Direct2D, DirectWrite, Media Foundation, Winsock, CNG, DPAPI, PDH |
| Permissions | Screen Recording; Accessibility (for mouse and keyboard from the PC) | Firewall: allow the receiver on private networks (UDP 9877, mDNS 5353) |

No third-party dependencies on either side.

## Mac

**Xcode:** open `mac-sender/Package.swift`, choose the **ExternalDisplay** scheme and press ⌘R.
For measurements use Release (Product → Scheme → Edit Scheme → Run → Build Configuration).

**Command line:**
```bash
cd mac-sender
xcodebuild -scheme ExternalDisplay -configuration Release -destination 'platform=macOS' build
```
(`swift build` works too on a standard machine. If the compiler complains about headers in
`/usr/local/include`, another toolchain's headers are in the way: use `xcodebuild`.)

**First run:**
1. Allow Screen Recording when asked, then click ↺.
2. Add the app under System Settings → Privacy & Security → **Accessibility**, then restart it.
   Without this, clicks and keys from the PC are ignored.
3. Create the virtual display, arrange it in System Settings → Displays, then click
   **Start Capture**.

## Windows

Use **"x64 Native Tools Command Prompt for VS"**. The plain "Developer Command Prompt" builds
**32-bit** binaries. Those work, but a 64-bit build is what is tested and benchmarked, and its
crash reports include stack traces.

```bat
cd windows-receiver
cmake -B build
cmake --build build
.\build\ExternalDisplayReceiver.exe
```

- With the Visual Studio generator the exe is in `build\Release\` or `build\Debug\`.
- If a build fails after pulling, delete the `build` folder and run both commands again.

### Command line

```
ExternalDisplayReceiver.exe [mac-ip] [options]
  (no IP)                         find the Mac on the network (remembers the last one)
  --gpu=high|low|default          GPU on hybrid laptops (also used for decoding)
  --renderer=full|compatible|software
                                  start lower in the renderer fallback chain
  --bench=SECONDS                 benchmark from the first frame, write bench-*.csv/txt, exit
  --log=trace|debug|info|warning|error   detail in receiver.log (default info)
  --verbose                       same as --log=debug
  --console                       also show the log in a console window
  --test-pattern                  no network: animated test pattern (GPU check)
  --cursor=local|video            pointer drawn on this PC (default) or in the video (F4 switches)
```

Files next to the exe:

| File | Content |
|---|---|
| `receiver.log` | the log (rotated at 5 MB) |
| `pairing.dat` | the pairing key, DPAPI-encrypted |
| `device_id.dat` | this receiver's ID |
| `receiver-settings.txt` | the last Mac used |
| `receiver-crash.txt/.dmp` | written after a crash |

## Tests

See [TESTING.md](TESTING.md).

```bash
# Mac (36 tests)
cd mac-sender && xcodebuild test -scheme ExternalDisplay -destination 'platform=macOS'
```
```bat
:: Windows (4 suites, 334 checks)
cmake -B build -DEXDP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Updating from protocol v1

Both apps must be the same version. The first start of version 2 deletes the old pairings
(see [SECURITY.md](SECURITY.md)). Pair once again: the code is shown on the Mac, typed on the PC,
and confirmed with **Pair** on the Mac.
