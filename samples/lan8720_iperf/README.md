# lan8720_iperf

iperf2-compatible TCP **upload** client on top of the AgRV2K on-die MAC0
+ LAN8720 RMII PHY. Streams TCP data at a PC running `iperf -s` for a
fixed duration and reports the throughput over UART.

Upstream `zperf` does the wire format (`CONFIG_NET_ZPERF`, server and
shell disabled); this sample is the ~250 line glue:
link-up → iface-up → one `zperf_tcp_upload()` → bytes/packets/errors/
Mbit/s → PASS/FAIL.

There is deliberately **no ICMP pre-flight**: `net_icmp_init_ctx()`
dispatches on (family, type, code) only, so any echo reply satisfies
the handler -- on the dev board it reported "PING OK" for an address that
does not exist, and a source-address check did not make it trustworthy
either. The upload result is the only signal.

## What this verifies

- Sustained TX: `eth_agm_send()` + the descriptor rings actually push
  frames at line rate rather than one frame per round trip
  (`samples/lan8720_link` only proves a single ICMP echo).
- The Zephyr TCP stack on top of the AGM MAC, end to end.
- A throughput baseline to spot later regressions.

## What this does NOT cover

- **RX under load.** The upload is one-directional; the server only
  returns ACKs. Server-pushes-data needs its own sample.
- UDP / PPS / jitter (`zperf_udp_upload()` exists; add a variant here
  if that becomes interesting).
- TCP goodput under packet loss, or multiple concurrent streams.
- On-wire validation. `lan8720_link` plus `tcpdump` is the tool for
  that; this sample only reports what the stack and zperf saw.

## Compatibility: iperf2 only

This is an **iperf2** client. iperf3 speaks JSON-RPC over TCP and is
NOT compatible.

```sh
iperf -v        # must say "iperf version 2.x"
```

Debian / Ubuntu's `iperf` package is iperf2. Fedora / RHEL / Alpine /
macOS-Homebrew ship iperf3 as `iperf`; install the iperf2 package or
build it from sourceforge.

zperf does **not** send iperf2's 28-byte control header. A modern iperf2
server still reports `bytes / wallclock` correctly, but also logs a
cosmetic:

```
LAST PACKET NOT RECEIVED!!!
```

That line is expected and does not invalidate the throughput number.

## Two modes: TCP and UDP

`CONFIG_LAN8720_IPERF_PROTO_*` picks the protocol; the PC-side server has
to match:

| Mode | Kconfig | PC command | What it measures |
|---|---|---|---|
| TCP (default) | `LAN8720_IPERF_PROTO_TCP=y` | `iperf -s` | End-to-end TCP goodput. ACK-clocked, so the result is set by the round trip and the window, **not** by what the MAC can push. |
| UDP | `LAN8720_IPERF_PROTO_UDP=y` | `iperf -s -u` | Rate-paced, no ACK clocking. The server sends back received bytes / loss / jitter, which zperf decodes. Still runs the full socket/IP/UDP path, so its number is the *stack's*, not the MAC's. |
| RAW-L2 (dev board only) | `LAN8720_IPERF_PROTO_RAW=y` | `tcpdump -i <if> ether proto 0x88b5` | Hand-built Ethernet frames straight to the driver via `NET_AF_PACKET/NET_SOCK_RAW` -- no IP/UDP/TCP. **Not iperf**: no server, no report; the capture is the only yardstick. Use it to see what the MAC + driver can actually do. |

Measured with RAW-L2 on the 200 MHz dev bitstream: **98.34 Mbit/s**
(81 850 x 1502 B in 10.0 s; frame gap p50 125 us against a 121.8 us
line-rate floor), i.e. ~99.6% of what that frame size allows on a 100 Mb
PHY. That is the MAC ceiling; the iperf modes sit well below it because
of per-packet work in the upstream network stack. Note `tcpdump` itself
drops frames at this rate -- compare its "received by filter" count with
"captured" before believing a wire number.

`CONFIG_LAN8720_IPERF_UDP_RATE_KBPS` is the UDP pacing target in
zperf/iperf2 units (x1024 bit/s). The default 100000 = 102.4 Mbit/s is
deliberately above the 100 Mb PHY so the pacing never becomes the limit;
it must be non-zero (zperf divides by it).

Measured so far on the dev board (see `the development notes (not published here)`
§3.28.1/§3.28.8): on the current 200 MHz dev bitstream **TCP 13.6 Mbit/s,
UDP 45.1 Mbit/s**; the previous 100 MHz bitstream gave 6.7 / 22.4. Both are far below
line rate, and the UDP number is the honest "what the board can push"
figure — the per-frame cost is on the board, not on the wire.

Pushing those numbers toward line rate is a planned, **not started**
work item: the experiment ladder, the decision criteria and the
acceptance thresholds live in
`the development notes (not published here)`.

## Run

**Start the server first** — the board uploads a few seconds after it
boots, so an `iperf -s` that starts late misses the run:

```sh
# On the PC, on the same /24 as CONFIG_NET_CONFIG_MY_IPV4_ADDR
iperf -s -i 1          # TCP (default)
iperf -s -u -i 1       # UDP
```

Then, on the dev board host:

```sh
source <your-venv>/bin/activate
cd $HOME/zephyrproject

# The sample's board overlay already sets the 100 MHz dev-bitstream
# clock, so no EXTRA_DTC_OVERLAY_FILE is needed here.
west build -d /tmp/b_lan8720_iperf -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/lan8720_iperf

bash $HAL_AGM_HOME/tools/flash_fw.sh /tmp/b_lan8720_iperf/zephyr/zephyr.bin
bash $HAL_AGM_HOME/tools/test_uart_capture.sh -n -t 30
```

`flash_fw.sh` keeps the FPGA bitstream at `0x800e7000` and leaves the CPU
running, so `test_uart_capture.sh -n` (reset + capture) is what produces
the boot log + result. Defaults: board `192.168.15.1/24`, server
`192.168.15.100:5001`, 10 s, 1460 B segments.

Override at build time:

```sh
west build ... -- \
  -DCONFIG_NET_CONFIG_MY_IPV4_ADDR='"192.168.15.1"' \
  -DCONFIG_LAN8720_IPERF_SERVER_IP='"192.168.15.200"' \
  -DCONFIG_LAN8720_IPERF_SERVER_PORT=5001 \
  -DCONFIG_LAN8720_IPERF_DURATION_MS=30000 \
  -DCONFIG_LAN8720_IPERF_PACKET_SIZE=1460 \
  -DCONFIG_LAN8720_IPERF_MIN_MBITPS=5 \
  -DCONFIG_LAN8720_IPERF_PROTO_UDP=y \
  -DCONFIG_LAN8720_IPERF_UDP_RATE_KBPS=1000000
```

## Expected output

```
[inf] lan8720_iperf: lan8720_iperf: iperf2 TCP client, board 192.168.15.1 -> server 192.168.15.100:5001
[inf] lan8720_iperf: duration=10000 ms  packet_size=1460 B  min=1 Mbit/s
[inf] lan8720_iperf: LAN8720 link UP: 100 Mb full duplex
[inf] eth_agm: link UP
[inf] lan8720_iperf: link + iface up
[inf] lan8720_iperf: PING 192.168.15.100 OK
[inf] lan8720_iperf: TCP upload -> 192.168.15.100:5001 for 10000 ms ...
[inf] lan8720_iperf: sent 8254 packets, 12050840 bytes in 10000123 us, 0 send errors
[inf] lan8720_iperf: Throughput: 9.64 Mbit/s
[inf] lan8720_iperf: Result: PASS
```

Cross-check the number against the server's own report — they measure
the same transfer from opposite ends and should agree within a few
percent (the client counts bytes handed to the socket, the server
counts bytes that arrived).

## Reading the result

| Symptom | Meaning / next step |
|---|---|
| `Result: FAIL (no reachability)` | ICMP pre-flight failed. Cable, ARP, subnet or PC firewall — fix this first, a TCP run cannot work. |
| `zperf_tcp_upload failed` + "is `iperf -s` running?" | The PC answered ICMP but nothing is listening on the port (or it is iperf3). |
| `send errors` > 0 | zperf hit `-ENOMEM`; it counts net_buf exhaustion here. Raise `CONFIG_NET_BUF_TX_COUNT` (and/or `CONFIG_NET_PKT_TX_COUNT`). |
| Throughput plateaus well below ~90 Mbit/s with 0 errors | The CPU is the bottleneck, not the wire (100 MHz, no cache, XIP flash). Try `CONFIG_NET_BUF_DATA_SIZE=1536` (already set), a longer `DURATION_MS`, then look at `eth_agm_send()`'s per-frame busy-wait. |
| RTT/ping suddenly ~0.4 s | Check `CONFIG_LOG_DEFAULT_LEVEL`: 4 means DEBUG, not INFO, and the console then costs hundreds of ms per packet. See `the development notes (not published here)` |

## Tuning knobs

- `CONFIG_NET_BUF_DATA_SIZE=1536` — one TCP segment per net_buf instead
  of a dozen 128 B fragments. The single biggest software lever here.
- `CONFIG_NET_BUF_TX_COUNT` / `CONFIG_NET_PKT_TX_COUNT` — how much can be
  in flight (~1.4 KB of send window per TX buffer).
- `CONFIG_LAN8720_IPERF_PACKET_SIZE` — must stay
  `<= CONFIG_NET_ZPERF_MAX_PACKET_SIZE`; `main.c` has a `BUILD_ASSERT`
  so a bad pair fails the build rather than being silently clamped.
- `CONFIG_SPEED_OPTIMIZATIONS=y` — already on; the CPU is the bottleneck.

RAM budget is ~84 KB of the 128 KB on-chip RAM (`west build -t
ram_report`), dominated by the two net_buf data pools and the driver's
own 4+4 x 1536 B descriptor buffers.

## Bitstream dependency

Same dev bitstream as `samples/lan8720_link`:
`<your canonical bitstream>`
(VE: SYSCLK 200 / BUSCLK 100 / HSECLK 8; MAC pins on PIN_43..47 +
57..59, no `PHY_RSTB` and no `PHY_INTB`). That matches the agrv2k_407
board defaults, so the overlay in this directory leaves the clocks
alone. For the older 100 MHz bitstream build with
`-DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay`.

## Status

Build-verified (`west twister -T samples -p agrv2k_407 --build-only`).
Bench numbers are **not** recorded yet — the first real run should land
its output in `the development notes (not published here)` before anyone
quotes a figure.
