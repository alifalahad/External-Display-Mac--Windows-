# Protocol (version 2)

Definitions: `mac-sender/ExternalDisplay/Network/Protocol.swift` and
`windows-receiver/src/Network/Protocol.h`. Both must change together; the tests check the layout
on each side (`ProtocolTests.swift`, `PipelineMetricsTests.cpp`, `SecureChannelTests.*`).

Integers are **little-endian**, except the two security fields marked *big-endian* below.

## Channels

| Channel | Port | Direction | Carries |
|---|---|---|---|
| TCP | 9876 (Mac listens) | both | handshake, control, input, clipboard, statistics |
| UDP | 9877 (PC listens) | Mac → PC | video |
| mDNS | 5353 | — | the Mac advertises `_externaldisplay._tcp` |

## Header (28 bytes, every message)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | magic | `"EXDP"` (0x50445845) |
| 4 | 1 | version | **2**. A mismatch is refused: `PAIR_REJECT(version)` |
| 5 | 1 | type | see below |
| 6 | 2 | flags | bit 0 keyframe · bit 1 timing prefix · bit 2 **encrypted** |
| 8 | 4 | sequence | TCP: per-sender message counter. UDP: per-packet counter (gaps = loss) |
| 12 | 8 | timestamp | PING: sender clock. PONG: Mac clock (µs). Video: µs since stream start |
| 20 | 4 | payload length | bytes after the header (including counter and tag when encrypted) |
| 24 | 4 | CRC-32 | plaintext messages: over header (CRC = 0) + payload. Encrypted: 0 (the tag protects them) |

## Messages

| Type | Name | Direction | Payload | Encrypted |
|---|---|---|---|---|
| 0x01 | HELLO | PC → Mac | 162 bytes (below) | no |
| 0x80 | KEY_EXCHANGE | Mac → PC | 73 bytes | no |
| 0x84 | KEY_REVEAL | PC → Mac | 65 bytes: PC's ephemeral public key | no |
| 0x81 | PAIR_CONFIRM | PC → Mac | 32 bytes: HMAC(confirm-win, TH) | no |
| 0x82 | PAIR_ACCEPT | Mac → PC | 40 bytes: newlyPaired u8, 7 reserved, HMAC(confirm-mac, TH) | no |
| 0x83 | PAIR_REJECT | Mac → PC | 4 bytes: reason u8 (0 generic, 1 user rejected, 2 unknown device, 3 version, 4 bad handshake) | no |
| 0x02 | WELCOME | Mac → PC | — | yes |
| 0x10 | START_STREAM | Mac → PC | width, height, fps, bitrate, codec (u32 each) | yes |
| 0x11 | STOP_STREAM | Mac → PC | — | yes |
| 0x20 / 0x21 | PING / PONG | both | PONG: the PING's timestamp (u64), so the PC can sync clocks | after auth |
| 0x30 | KEYFRAME_REQ | PC → Mac | — (the Mac makes keyframes only when asked, plus one a minute) | yes |
| 0x50 | QUALITY_REPORT | PC → Mac | loss ×100, RTT ms, frames dropped, queue depth, latency µs (u32 each) | yes |
| 0x51 | SENDER_STATS | Mac → PC | capture FPS ×100, encode FPS ×100, CPU % ×100, RAM MB, encoder kbps, target kbps, flags, reserved | yes |
| 0x60 | INPUT_EVENT | PC → Mac | 24 bytes: type, button, key code, modifiers, x, y, scroll x, scroll y (x, y are 0…1 across the video) | yes |
| 0x70 | CLIPBOARD | both | UTF-8 text (≤ 1 MB) | yes |
| 0x90 | RECEIVER_CAPS | PC → Mac | 8 bytes: flags u32 (bit 0: the PC draws the pointer), reserved u32. Sent after PAIR_ACCEPT and whenever it changes (F4) | yes |
| 0x91 | CURSOR_SHAPE | Mac → PC | pointer image, when it changes (below) | yes |
| 0x92 | CURSOR_POSITION | Mac → PC | 12 bytes: x, y (f32, 0…1 across the video), flags u32 (bit 0: visible) | yes |
| 0x40 | VIDEO_FRAME | Mac → PC (UDP) | one encrypted datagram: the whole frame | yes |
| 0x41 | VIDEO_FRAGMENT | Mac → PC (UDP) | encrypted: fragment header (frame no. u32, index u16, total u16) + data | yes |
| 0xFF | DISCONNECT | both | — | after auth |

Before authentication the Mac accepts only HELLO, KEY_REVEAL, PAIR_CONFIRM, PING, PONG and
DISCONNECT, and the PC only the handshake messages, PING, PONG and DISCONNECT. After
authentication anything unencrypted closes the connection. The one exception is a PONG that was
already in flight.

### HELLO (162 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 64 | receiver name, UTF-8, zero-padded |
| 64 | 2 | UDP port |
| 66 | 64 | device ID (random UUID, identifies a paired receiver) |
| 130 | 32 | commitment = SHA-256(PC's ephemeral public key) |

### KEY_EXCHANGE (73 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | mode: 0 = new pairing, 1 = paired |
| 1 | 7 | reserved |
| 8 | 65 | Mac's ephemeral P-256 public key, X9.63 uncompressed (`04 | X | Y`) |

### Pointer drawn by the PC (CURSOR_SHAPE, CURSOR_POSITION)

Only after the PC set bit 0 in RECEIVER_CAPS. The Mac then captures without the pointer
([SMOOTHNESS.md](SMOOTHNESS.md), step 1). CURSOR_SHAPE is a 24-byte header plus the bitmap:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | shape ID (FNV-1a of the rest: the same image has the same ID) |
| 8 | 2 | width, bitmap pixels (1–256) |
| 10 | 2 | height (1–256) |
| 12 | 2 | hot spot x, from the left |
| 14 | 2 | hot spot y, from the top |
| 16 | 4 | scale (f32): stream pixels per bitmap pixel (the bitmap is drawn at 2×) |
| 20 | 4 | reserved |
| 24 | w × h × 4 | BGRA, premultiplied alpha, top row first |

CURSOR_POSITION is sent when the pointer moves on the Mac. Moves the PC's own mouse caused are
not sent back. Not visible means the pointer is on another Mac display (x = y = 0).

## Handshake

```
PC                                                       Mac
HELLO (name, port, device ID, SHA-256(eW))  ───────────▶
                                            ◀─────────── KEY_EXCHANGE (mode, eM)
KEY_REVEAL (eW)                             ───────────▶  check SHA-256(eW) = commitment
        both: Z  = ECDH(e, e')                           (32-byte x-coordinate, big-endian)
              TH = SHA-256("EXDP/2 handshake" | HELLO | KEY_EXCHANGE | KEY_REVEAL)
              K(label) = HKDF-SHA256(IKM = Z [| pairing key], salt = TH, info = "EXDP/2 " + label)

 new pairing: user types the 6-digit code from K("sas") into the PC; PC checks it
PAIR_CONFIRM (HMAC(K("confirm win"), TH))   ───────────▶  verify; new pairing: user clicks Pair
                                            ◀─────────── PAIR_ACCEPT (HMAC(K("confirm mac"), TH))
        everything after PAIR_ACCEPT is encrypted                  then WELCOME, START_STREAM, ...
```

- **Code:** the first 4 bytes of `K("sas")` as a big-endian u32, mod 1,000,000, written as
  6 digits.
- **Pairing key:** `K("pairing key")`. Both sides store it after a new pairing; it is never sent.
- **Session keys:** `K("control m2w")`, `K("control w2m")`, `K("video m2w")`.

## Encryption (AES-256-GCM)

Nonce = 4 zero bytes | counter (u64, **big-endian**). Every key has its own counter, and a counter
value is never reused with the same key.

| Channel | Format | Associated data | Counter |
|---|---|---|---|
| TCP | header \| ciphertext \| tag (16) | the 28-byte header (CRC 0) | implicit: messages sent in this direction so far |
| UDP | header \| counter (8) \| ciphertext \| tag (16) | header \| counter | explicit, never reset during a session |

- **Replay protection (UDP):** the receiver accepts each counter once, within a 64-packet window
  (RFC 4303 style). The window advances only after the tag has verified.
- **Datagram size:** 28 + 8 + 16 + 8 + 1,340 = 1,400 bytes at most, under the usual 1,500-byte
  MTU with room for IP/UDP headers and VPNs.

## Video payload

The reassembled frame is:
```
FrameTimingPrefix (40 bytes, when flag bit 1) | H.264 Annex B access unit
```
Prefix fields (u32 frame number, u32 flags, then u64 µs on the Mac clock):

| Field | Meaning |
|---|---|
| frame no. | frame number |
| flags | bit 0: the capture time is real |
| capture | when the frame was on the Mac's screen |
| encode start | handed to VideoToolbox |
| encode end | encoder output |
| send | packetization started |

Keyframes start with SPS and PPS. The SPS's VUI states the colours: full-range BT.709 from the
Mac app (since smoothness step 4). The receiver reads range and matrix from it, so a
video-range stream also shows correctly. They come at the start of a stream, after a KEYFRAME_REQ, and
once a minute. Frame numbers are consecutive, so the receiver notices a missing frame and asks for
a keyframe.

## Versioning

- The version byte is checked on every message.
- Version 2 is not compatible with version 1 (the encryption and the handshake changed).
- The receiver shows "the Mac app and this receiver are different versions — update both"
  instead of retrying quickly.
- Layout changes need a version bump and matching tests on both sides.
- New message types do not: both sides ignore types they don't know, and a feature that needs
  new messages is only used once the other side asked for it (RECEIVER_CAPS).
