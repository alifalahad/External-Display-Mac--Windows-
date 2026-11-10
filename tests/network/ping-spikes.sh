#!/bin/bash
# =============================================================================
# ping-spikes.sh — Delay spikes on this Mac's network (docs/NETWORK.md)
# =============================================================================
# Pings a host 5 times a second (default: the router) and reports the median,
# p95, p99 and maximum round trip, and when the spikes came. Spikes ~1 s apart
# in small groups are typical of AWDL (AirDrop, Handoff, Universal Control);
# a single spike about once a minute, of Wi-Fi scanning.
#
#   tests/network/ping-spikes.sh [host] [seconds]
#
# Run it before and after each change in docs/NETWORK.md. Pinging the PC's
# address instead of the router measures the whole Wi-Fi path the video takes.
# =============================================================================
set -euo pipefail
HOST=${1:-$(route -n get default 2>/dev/null | awk '/gateway/ {print $2}')}
DURATION=${2:-60}
[ -n "$HOST" ] || { echo "No default gateway: give a host to ping"; exit 1; }
COUNT=$((DURATION * 5))
LINK=$(system_profiler SPAirPortDataType 2>/dev/null | awk '/Current Network Information:/ {f = 1} f && /Channel:/ {sub(/^ +/, ""); print; exit}')
echo "Pinging $HOST 5 times a second for $DURATION s"
echo "Wi-Fi: ${LINK:-not connected}; AWDL: $(ifconfig awdl0 2>/dev/null | awk '/status:/ {print $2}')"

OUT=$(mktemp)
trap 'rm -f "$OUT" "$OUT.ms" "$OUT.spikes"' EXIT
ping -c "$COUNT" -i 0.2 "$HOST" > "$OUT" 2>&1 || true

# "64 bytes from 192.168.1.1: icmp_seq=12 ttl=64 time=2.927 ms" → "12 2.927"
awk '/icmp_seq=/ {
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^icmp_seq=/) { sub(/icmp_seq=/, "", $i); seq = $i }
        if ($i ~ /^time=/)     { sub(/time=/, "", $i); ms = $i }
    }
    print seq, ms
}' "$OUT" > "$OUT.ms"

REPLIES=$(wc -l < "$OUT.ms" | tr -d ' ')
[ "$REPLIES" -gt 0 ] || { echo "No replies from $HOST"; exit 1; }
sort -n -k2 "$OUT.ms" | awk -v n="$REPLIES" -v sent="$COUNT" '
    { ms[NR] = $2 }
    END {
        i50 = int(n * 0.50); if (i50 < 1) i50 = 1
        i95 = int(n * 0.95); if (i95 < 1) i95 = 1
        i99 = int(n * 0.99); if (i99 < 1) i99 = 1
        printf "Replies %d of %d (%.1f%% lost)\n", n, sent, 100 * (sent - n) / sent
        printf "Round trip: median %.1f ms, p95 %.1f ms, p99 %.1f ms, max %.1f ms\n", ms[i50], ms[i95], ms[i99], ms[n]
    }'

# Spikes: over 20 ms, when they came (seconds) and how far apart
awk '$2 > 20 { printf "%.1f\n", $1 * 0.2 }' "$OUT.ms" > "$OUT.spikes"
SPIKES=$(wc -l < "$OUT.spikes" | tr -d ' ')
echo "Over 20 ms: $SPIKES ($(awk -v s="$SPIKES" -v n="$REPLIES" 'BEGIN { printf "%.0f", 100 * s / n }')% of the replies)"
if [ "$SPIKES" -gt 0 ]; then
    echo "  at seconds: $(head -30 "$OUT.spikes" | tr '\n' ' ')"
    awk 'NR > 1 { printf "%.1f\n", $1 - prev } { prev = $1 }' "$OUT.spikes" | sort | uniq -c | sort -rn | head -3 |
        awk 'BEGIN { printf "  most common gaps:" } { printf " %s s (%d times)", $2, $1 } END { print "" }'
fi
