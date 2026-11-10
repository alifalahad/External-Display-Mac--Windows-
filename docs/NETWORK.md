# Network setup

The network is the part of the latency that the code controls least. Steps 2, 5 and 6 of
[SMOOTHNESS.md](SMOOTHNESS.md) handle lost packets and a congested link. Regular delay spikes
from the machines' own Wi-Fi features still show as regular hitches. This guide removes them.
Measure before and after each change.

## Measure first

**On the Mac:** `tests/network/ping-spikes.sh` pings the router 5 times a second for 60 s. It
prints the median, p95, p99 and maximum round trip, and when the spikes came. Give it the PC's
address to measure the whole path the video takes: `tests/network/ping-spikes.sh 192.168.1.23`.

This Mac on 2026-11-10 (5 GHz channel 161, 80 MHz, signal −46 dBm, AWDL active):

```
Round trip: median 2.9 ms, p95 68.0 ms, p99 89.8 ms, max 91.0 ms
Over 20 ms: 42 (14% of the replies)
  at seconds: 1.4 2.4 3.4 6.0 7.0 9.6 10.6 11.6 13.2 14.2 15.2 …
```

The link itself is excellent (median 3 ms), but one ping in seven waits 20–91 ms. A second,
shorter run had one in five, and the longest took 189 ms. The spikes come in groups, 1.0 s apart:
that is AWDL (section 1). The aim is a p95 close to the median, under about 10 ms.

**On the PC:** `ExternalDisplayReceiver.exe --bench=60`, with something moving on the Mac's
virtual display (scroll a long page or play a video). The summary gives Network avg, p95, p99 and
max, the dropped frames, the packet loss, and `Net wait` (seconds in overuse). Don't judge the PC
by occasional pings: the Wi-Fi adapter dozes between them and adds delay that a running stream
never sees.

Keep a note of each change:

| Change | Mac ping p95 | PC Network p95 | PC Network max |
|---|---|---|---|
| none (before) | | | |
| AWDL off | | | |
| … | | | |

## 1. The Mac: AWDL (the biggest one)

AWDL (Apple Wireless Direct Link) is the direct Mac-to-device Wi-Fi behind AirDrop, Handoff,
Universal Control, Sidecar and AirPlay to the Mac. While it is active, the Wi-Fi radio regularly
leaves the router's channel for a few tens of milliseconds. Every packet sent then waits.

While streaming, turn off what uses it (System Settings; names as in macOS 13–15, newer versions
may word them differently):
- **General → AirDrop & Handoff:** AirDrop *No One*, *Allow Handoff* off, *AirPlay Receiver* off.
  AirDrop can also be set to *No One* in Control Center.
- **Displays → Advanced:** the option that lets the pointer and keyboard move to a nearby Mac or
  iPad (Universal Control) off.
- Then run `ifconfig awdl0 | grep status` (*inactive* is the goal) and the ping script again.

Faster, until the next restart: `sudo ifconfig awdl0 down`. AirDrop, Handoff, Universal Control
and Sidecar stop working until `sudo ifconfig awdl0 up` or a restart. macOS may also bring it
back when one of them is used.

## 2. The Wi-Fi network

- **5 GHz or 6 GHz**, not 2.4 GHz. Channel width 80 MHz; 160 MHz only where few other networks are
  around. (This Mac: channel 161, 80 MHz: good.)
- **A channel without radar detection:** 36–48 or 149–165 in most countries. On the others
  (52–144, "DFS"), the router must leave the channel when it detects radar, and every device
  pauses until it has moved.
- **WPA2 or WPA3 (AES).** The old TKIP limits Wi-Fi to 54 Mbps.
- **The same network and, with a mesh system, the same node.** Not a guest network: it keeps
  devices apart, so the PC cannot reach the Mac. The router's *AP isolation* or *client isolation*
  must be off.
- **Ethernet on one side.** With both on Wi-Fi, each packet crosses the air twice (Mac → router →
  PC). A cable on either side halves that, and on the PC it is usually easiest. This is the
  biggest single improvement after AWDL.
- **A direct cable** between the Mac and the PC (Ethernet adapters on both) should work too, but
  it is not tested yet. Both get 169.254.x.x addresses. If the receiver does not find the Mac,
  start it with the Mac's address.

## 3. The PC

- **Wi-Fi power saving off:** Control Panel → Power Options → Change plan settings → Change
  advanced power settings → Wireless Adapter Settings → Power Saving Mode → *Maximum Performance*,
  on battery and plugged in. Many newer laptops hide this entry; the same from a terminal run as
  administrator:
  ```bat
  powercfg /setacvalueindex SCHEME_CURRENT 19cbb8fa-5279-450e-9fac-8a3d5fedd0c1 12bbebe6-58d6-4636-95bb-3217ef867c1a 0
  powercfg /setdcvalueindex SCHEME_CURRENT 19cbb8fa-5279-450e-9fac-8a3d5fedd0c1 12bbebe6-58d6-4636-95bb-3217ef867c1a 0
  powercfg /setactive SCHEME_CURRENT
  ```
- **The adapter's own settings:** Device Manager → Network adapters → your Wi-Fi adapter →
  Properties → Advanced. The names differ by maker; change only the ones you find:
  - *Roaming Aggressiveness*: *Lowest*. It looks for other access points less often.
  - *Preferred Band*: *Prefer 5GHz* (or 6 GHz).
  - *MIMO Power Save Mode*: *No SMPS*.
  - *Packet Coalescing* (Intel): *Disabled*. It holds back received packets to save power.
- **Windows' own scans:** Windows looks for networks from time to time, and whenever the network
  list is open. Keep that list closed while streaming. Location services (Settings → Privacy &
  security → Location) can also trigger scans; turn them off if you don't need them.
- **Only if the benchmark still shows a spike about once a minute:** stop Windows from scanning
  while you stream. The connection stays up, but Windows will not reconnect to another network
  or list networks until you turn scanning back on. The adapter's name comes from
  `netsh wlan show interfaces`:
  ```bat
  netsh wlan set autoconfig enabled=no interface="Wi-Fi"
  netsh wlan set autoconfig enabled=yes interface="Wi-Fi"
  ```
- **The network profile should be *Private*** (Settings → Network & internet → Wi-Fi → your
  network), and the receiver allowed on private networks in Windows Firewall.

## 4. Check

Run the ping script on the Mac and `--bench=60` on the PC again, with the same content as before.
Expected:
- the Mac's ping p95 close to the median, without the groups of spikes 1 s apart;
- on the PC, Network p95 close to the average and no regular spikes in `bench-*-frames.csv`;
- in the summary, `Net wait` in overuse for a few seconds at most.
