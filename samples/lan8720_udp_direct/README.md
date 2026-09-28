# lan8720_udp_direct

Hand-built UDP upload at the AgRV2K on-die MAC0 + LAN8720 RMII PHY, still
verifiable with a stock `iperf -s -u`.

**Measured: 98.15 Mbit/s on the wire (92.79 Mbit/s of UDP payload),
0 datagrams lost of 81,849.** The server's own report agrees:

```
[  1] 0.00-10.00 sec   114 MBytes  95.6 Mbits/sec   0.214 ms  0/81850 (0%)
```

## Why this exists

`samples/lan8720_iperf` runs Zephyr's socket + net_pkt + IP + UDP path and
tops out at **46.4 Mbit/s** on the 200 MHz dev bitstream. Handing the
same frames straight to the driver (`lan8720_iperf`'s dev board-only RAW
mode) reaches **98.34 Mbit/s**. So the missing factor of two is
per-packet work in the upstream network stack, not the MAC.

This sample keeps the third-party verification of the iperf path while
removing that per-packet work: every header is written by the sample and
the frame goes out through a packet socket, but the payload is a legal
iperf2 UDP datagram, so the PC side is unmodified.

| Path | Mbit/s (wire) | Mbit/s (UDP payload) |
|---|---|---|
| `lan8720_iperf`, UDP mode (full stack) | — | 46.4 |
| `lan8720_iperf`, RAW mode (no iperf) | 98.34 | — |
| **this sample (hand-built UDP + iperf2 server)** | **98.15** | **92.79** |

## What it does

1. Link up + iface up (same bridge as the other LAN8720 samples).
2. Resolves the server's MAC with an ARP request it builds itself, and
   parses the reply off the same packet socket.
3. Builds one 1502 B frame template once:

   ```
   Ethernet 14 + IPv4 20 + UDP 8 + iperf2 40 + 1420 B of 'z' payload
   ```

   The IPv4 header checksum is computed once (nothing in the IP header
   changes per frame). **The UDP checksum is left at 0**, which IPv4
   permits (RFC 768) and Linux accepts: computing it would mean scanning
   1460 B per frame out of a ~122 µs line-rate budget.
4. Loops for `CONFIG_LAN8720_UDP_DIRECT_DURATION_MS`, touching only the
   datagram id and timestamp per frame.
5. Sends the iperf2 end datagram (`id` negative) so the server closes the
   interval instead of averaging over however long it waits.

## Run

Start the server first (the board sends a few seconds after reset):

```sh
iperf -s -u -i 2          # iperf2; NOT iperf3
```

```sh
source <your-venv>/bin/activate
cd $HOME/zephyrproject
west build -d /tmp/b_udp_direct -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/lan8720_udp_direct
bash $HAL_AGM_HOME/tools/flash_fw.sh /tmp/b_udp_direct/zephyr/zephyr.bin
bash $HAL_AGM_HOME/tools/test_uart_capture.sh -n -t 30
```

Optional wire cross-check (see the caveat below about tcpdump's own
drops):

```sh
tcpdump -i <if> -n -e 'udp port 5001'
```

## Expected output

```
[inf] lan8720_udp_direct: lan8720_udp_direct: hand-built UDP48 -> 192.168.15.100:5001, frame 1502 B (payload 1420 B)
[inf] lan8720_udp_direct: link + iface up
[inf] lan8720_udp_direct: ARP: 192.168.15.100 is at 00:23:57:1c:01:41
[inf] lan8720_udp_direct: sending for 10000 ms ...
[inf] lan8720_udp_direct: sent 81849 frames in 10.020 s (8168 frames/s)
[inf] lan8720_udp_direct: Throughput: 98.15 Mbit/s on the wire (92.79 Mbit/s of UDP payload)
[inf] lan8720_udp_direct: Result: PASS
```

## Known limitations

- **ARP does not complete yet.** The request goes out and the host
  answers it (visible on the wire), but Zephyr's packet socket does not
  deliver the reply back to this sample, so after its 1.5 s ARP timeout it
  logs a warning and sends to the broadcast destination MAC instead. The IPv4 destination stays
  the server's unicast address and Linux accepts that, so the benchmark
  is unaffected — but the ARP path is a known gap, not a design choice.
- **No transport semantics.** No retransmission, no flow control, no
  fragmentation, no checksums beyond the IPv4 header. It is a one-way
  dev board, not a stack.
- **Receive is unused** except for the (currently non-functional) ARP
  reply: the server's final report is deliberately not read, because
  throughput does not depend on it and reading it would drag the RX path
  in for nothing.
- **Frame size is capped at 1502 B** (1500 B IP packet). That is what
  fixes the payload at 1460 B and the ceiling at ~98.7 Mbit/s of frame
  bytes for a 100 Mb PHY; there is no headroom to win by sending more.
- `tcpdump` **drops frames at this rate.** It saw 0 drops with `-c
  300000` on this run, but an earlier RAW run had it drop 12% — always
  compare "received by filter" with "captured" before believing a
  wire-derived number. The iperf server's count is the primary evidence.

## Bitstream / clocks

Same dev bitstream as the other LAN8720 samples:
`<your canonical bitstream>`
(VE: SYSCLK 200 / BUSCLK 100 / HSECLK 8), which matches the agrv2k_407
board defaults. See `the development notes (not published here)`.
