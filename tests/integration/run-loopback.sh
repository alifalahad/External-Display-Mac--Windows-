#!/bin/bash
# =============================================================================
# run-loopback.sh — End-to-end test: real Mac sender ↔ real Windows receiver
# =============================================================================
# On a Mac with Xcode, mingw-w64 and Wine (brew install mingw-w64 wine-stable):
#   tests/integration/run-loopback.sh [seconds] [x86_64|i686]
#
# Builds copies of both apps' networking/crypto code on test ports 19876/19877
# (a running External Display app on 9876 is left alone), then:
#   pair       new pairing — code shown on the "Mac", typed on "Windows", approved
#   paired     reconnect with the stored key, no code
#   impostor   wrong key for a known device ID → refused → pairs again
#   wrongcode  wrong code typed → refused, new code → pairs
#   rejected   the Mac user rejects → no session, no video, no retries
# Streaming runs send encrypted synthetic 720p60 video and check frames,
# timing, H.264 start codes, clock sync and statistics, and the pointer drawn
# by the receiver (RECEIVER_CAPS → CURSOR_SHAPE / CURSOR_POSITION; switched
# off and on again in the paired run, never asked for in the wrongcode run).
# Nothing is advertised on the network (Bonjour stripped, localhost only).
# =============================================================================
set -euo pipefail
SECONDS_PER_RUN=${1:-20}
ARCH=${2:-x86_64}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/exdp-loopback.XXXXXX")"
MAC="$ROOT/mac-sender/ExternalDisplay"
WIN="$ROOT/windows-receiver/src"
echo "Work directory: $WORK"

if lsof -nP -iTCP:19876 -sTCP:LISTEN >/dev/null 2>&1; then
    echo "Port 19876 is in use — stop the other test first"; exit 1
fi

# ── Mac side (Swift) ──
mkdir -p "$WORK/mac" "$WORK/home" "$WORK/pairings"
cp "$MAC"/Network/Protocol.swift "$MAC"/Security/{PairingManager,SecureChannel}.swift "$MAC"/Input/PointerMonitor.swift \
   "$MAC"/Encoder/{VideoEncoder,EncoderConfig,H264Converter}.swift \
   "$MAC"/Performance/{HostClock,RollingWindow,ProcessMetrics}.swift "$MAC"/App/Log.swift "$WORK/mac/"
sed -i '' -e 's/tcpPort: UInt16 = 9876/tcpPort: UInt16 = 19876/' -e 's/udpPort: UInt16 = 9877/udpPort: UInt16 = 19877/' "$WORK/mac/Protocol.swift"
sed -e 's/            listener.service = NWListener.Service(/            _ = NWListener.Service(/' \
    -e 's/params.acceptLocalOnly = false/params.acceptLocalOnly = true/' "$MAC/Network/StreamSender.swift" > "$WORK/mac/StreamSender.swift"
cp "$ROOT/tests/integration/MacSenderHarness.swift" "$WORK/mac/main.swift"   # top-level code needs main.swift
SDK="$(xcrun --sdk macosx --show-sdk-path)"
xcrun swiftc -sdk "$SDK" -swift-version 5 -O -o "$WORK/harness" "$WORK"/mac/*.swift

# ── Windows side (C++, MinGW) ──
cp -R "$WIN" "$WORK/src"
sed -i '' -e 's/TCP_PORT         = 9876/TCP_PORT         = 19876/' -e 's/UDP_PORT         = 9877/UDP_PORT         = 19877/' "$WORK/src/Network/Protocol.h"
"$ARCH-w64-mingw32-g++" -std=c++17 -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 \
    -Wno-unknown-pragmas -I"$WORK/src" "$ROOT/tests/integration/ReceiverClient.cpp" \
    "$WORK"/src/Network/StreamReceiver.cpp "$WORK"/src/Security/{PairingManager,Crypto,SecureChannel}.cpp "$WORK"/src/Input/Pointer.cpp \
    "$WORK"/src/Performance/{ClockSync,PipelineMetrics}.cpp "$WORK"/src/App/Log.cpp \
    -o "$WORK/client.exe" -static -lws2_32 -lbcrypt -lcrypt32

export WINEPREFIX="${WINEPREFIX:-$WORK/wineprefix}" WINEDEBUG=-all
CODEFILE_WIN="Z:$(echo "$WORK/code.txt" | tr / '\\')"
result=0
for mode in pair paired impostor wrongcode rejected; do
    echo; echo "=== Run: $mode ==="
    rm -f "$WORK/code.txt"
    duration=$SECONDS_PER_RUN
    [ "$mode" = wrongcode ] || [ "$mode" = rejected ] && rm -f "$WORK/pairing.dat" "$WORK/device_id.dat"
    [ "$mode" = rejected ] && duration=8
    [ "$mode" = rejected ] && export EXDP_REJECT=1 || unset EXDP_REJECT
    # Started directly (not in a subshell) so that kill reaches the harness itself
    CFFIXED_USER_HOME="$WORK/home" HOME="$WORK/home" \
        "$WORK/harness" $((duration + 25)) "$WORK/code.txt" "$WORK/pairings" > "$WORK/harness-$mode.log" 2>&1 &
    HARNESS=$!
    for _ in $(seq 1 50); do   # wait until it listens
        lsof -nP -iTCP:19876 -sTCP:LISTEN >/dev/null 2>&1 && break
        sleep 0.2
    done
    (cd "$WORK" && wine client.exe 127.0.0.1 "$duration" "$CODEFILE_WIN" "$mode" 2>/dev/null | grep -E "^\[Client\]|FAIL") || result=1
    kill "$HARNESS" 2>/dev/null || true
    wait "$HARNESS" 2>/dev/null || true
    while lsof -nP -iTCP:19876 -sTCP:LISTEN >/dev/null 2>&1; do sleep 0.2; done
    grep -E "Pairing request|approving|rejecting|session encrypted|failed authentication|rejected" \
        "$WORK/home/Library/Logs/ExternalDisplay/sender.log" 2>/dev/null | tail -n +1 | sed 's/^.*\] /  mac: /' | tail -4 || true
    : > "$WORK/home/Library/Logs/ExternalDisplay/sender.log" 2>/dev/null || true
done
wineserver -k 2>/dev/null || true
echo; [ $result -eq 0 ] && echo "LOOPBACK PASSED" || echo "LOOPBACK FAILED (logs in $WORK)"
exit $result
