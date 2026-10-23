# Security

## What is protected, and from whom

**Threat model:** an attacker on the same network (café, office, home Wi-Fi with guests). They
can read, inject, modify, replay and drop traffic, and can run their own copy of either app.

| Goal | How |
|---|---|
| Nobody else sees the screen | Every video packet is AES-256-GCM encrypted with per-session keys |
| Nobody else injects mouse, keyboard or clipboard | Control messages are encrypted and authenticated; before authentication the Mac ignores everything except the handshake |
| Only paired receivers connect | New receivers need the Mac user's approval and the verification code; paired receivers prove they hold the pairing key |
| A receiver connects only to the right Mac | The Mac proves the same keys back (PAIR_ACCEPT carries an HMAC) |
| No man in the middle during pairing | Hash commitment + 6-digit code, compared by the user (typed on the PC) |
| Recorded traffic stays private later | Fresh ECDH keys per connection (forward secrecy); the long-term pairing key alone does not decrypt old sessions |
| No replayed or forged video | GCM tag over header and counter; a replay window refuses repeated counters |
| Keys at rest | Windows: DPAPI (current user). Mac: owner-only file (see limitations) |
| Logs leak nothing | No keys, verification codes, keystrokes or clipboard text are ever logged |

## Construction

Only standard primitives, from the platforms' own libraries: **CryptoKit** on the Mac and
**CNG (bcrypt)** on Windows.

| Purpose | Primitive |
|---|---|
| Key agreement | ECDH on P-256, a fresh key pair per connection |
| Commitment, transcript | SHA-256 |
| Key derivation | HKDF-SHA256 (RFC 5869) |
| Key confirmation | HMAC-SHA256 |
| Encryption | AES-256-GCM, 96-bit counter nonces |
| Randomness | `SecRandomCopyBytes` / `BCryptGenRandom` (via CryptoKit / CNG) |

The full message flow is in [PROTOCOL.md](PROTOCOL.md#handshake).

**Why the hash commitment matters.** HELLO commits to the PC's key before the Mac reveals its own,
and the Mac only reveals after the commitment. Without it, a man in the middle could try key after
key until both sides showed the same 6-digit code (about a million tries, well under a second).
With it, the attacker has to fix their keys before seeing the other side's, so they fool the code
with probability 10⁻⁶. This is the approach of ZRTP (RFC 6189) and Bluetooth's numeric
comparison.

**Why both the code and an approval click.** The code proves there is no one in the middle
between the user's PC and the user's Mac. But anyone who connects can compute their own session's
code, so the code alone cannot prove that the connecting device is the user's PC. The Mac
therefore also needs the user to click **Pair**. That button only unlocks after the PC has
confirmed the code.

**Paired reconnects.** The stored pairing key is mixed into the key derivation. A device without
the key cannot produce the confirmation HMAC, which gives mutual authentication with fresh keys
every session.

**Version 1 was not safe.** It sent the PIN, and then the long-term key itself, over the network in
plaintext, and nothing was encrypted. Version 2 deletes all version 1 pairings on first start, so
every receiver pairs again once.

## Verification

- **Cross-platform vectors.** The Mac generates the ECDH secret, transcript hash, code, every
  derived key, the confirmation MACs and an AES-GCM ciphertext. The Windows tests reproduce them
  byte for byte (`SecureChannelTests.cpp`), and the Mac tests check the same values
  (`SecureChannelTests.swift`).
- **Standards.** HKDF (RFC 5869) and HMAC (RFC 4231) are checked against the published test
  vectors.
- **Startup self-test.** Windows checks its ECDH byte order against a known answer at startup.
  CNG returns raw secrets in an unusual byte order, and the check catches that instead of
  assuming it. The receiver refuses to run if the self-test fails.
- **End-to-end attack scenarios.** Pairing, paired reconnect, a receiver with a wrong key, a wrong
  code typed, and the Mac user rejecting are all exercised against the real code by
  `tests/integration/run-loopback.sh`.

## Limitations (known, accepted for now)

1. **Our own composition of standard primitives.** It follows established designs but has not
   been formally verified or audited. TLS and DTLS were rejected:
   - Windows SChannel has no PSK mode.
   - Certificate pinning on both sides adds a lot of code.
   - Pairing would still need a code.

   Noise would need third-party libraries on both platforms. A future version could move to a
   PAKE (CPace/SPAKE2) once both platforms offer one natively.
2. **The 6-digit code** gives a 1-in-a-million chance against a man in the middle per pairing
   attempt. That is the same trade-off as Bluetooth. Repeated failed pairings are visible to the
   user (a new code appears each time).
3. **Mac key storage is a file with 0600 permissions, not the Keychain.** Development builds are
   ad-hoc signed and change signature on every build, and Keychain items then prompt for access
   after each rebuild. A signed release build should move the keys to the Keychain:
   `PairingManager` is the only place that touches storage.
4. **Metadata is visible:** message types, sizes and timing (headers are authenticated, not
   encrypted). The content is not.
5. **Denial of service** by a LAN attacker is not prevented. They can drop packets or flood the
   ports; they cannot read or inject content.
6. **Clipboard:** text sent to the other machine is visible there, by design. Turn off clipboard
   use for sensitive content.

## Reporting

Please report security problems privately to the repository owner rather than in a public issue.
