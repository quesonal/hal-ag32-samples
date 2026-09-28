# lan8720_link

Bring up the AgRV2K on-die MAC0 + a [Microchip LAN8720](https://www.microchip.com/en-us/product/LAN8720A)
10/100 RMII Ethernet PHY, report link state, and run a single
ICMP echo (ping) round-trip once the net_if is up.

## What this verifies

- soc.c opens the MAC0 AHB gate (bit 3) from the DT-defined
  `agm,ahb-clkenable-bit` (`dts/riscv/agm/agrv2k.dtsi`).
- `pinctrl_apply_state()` sets AFSEL on the MAC pins (eth0_default
  state), so the IP owns them.
- The MAC0 hardware MDIO controller (`drivers/ethernet/mdio/mdio_agm.c`)
  talks to the LAN8720 over the MDC/MDIO pins.
- The LAN8720 PHY driver (`drivers/ethernet/phy/phy_agm_lan8720.c`)
  probes ID (0x0007C0Fx), runs reset, kicks off autoneg.
- The PHY framework's link-state callback fires on transitions; the
  sample bridges it into `eth_agm_set_link_state()` so the MAC
  starts/stops on link transitions.
- The Zephyr Ethernet framework registers the net_if, link-local
  address, and stats hooks.
- Once carrier is on, the net stack passes an ICMP echo through
  `eth_agm_send()` and the echo reply comes back through
  `eth_agm_rx_drain()` → `net_recv_data()` → `net_icmp` handler.
  A `PASS` confirms both TX and RX end-to-end at the IP layer
  (ARP + ICMP round-trip).

## What this does NOT cover

- TCP / UDP throughput. Use `samples/lan8720_iperf` for that.
- Group-filtered multicast. `eth_agm_start()` sets
  `MAC_CTRL_MULTICAST_EN`, which accepts **every** multicast /
  broadcast frame (including `FF:FF:FF:FF:FF:FF`) because the IP's
  hash table (`HTMSB`/`HTLSB`) is not wired up. Broadcast ARP works
  thanks to this; mDNS / DHCPv6 multicast also lands in the RX ring
  instead of being filtered. See `the development notes (not published here)`
  §3.28.2 and the known-gap list in §3.28.
- TX descriptor / DMA pressure (single ICMP request, no flood).
- Latency under load. **Do not** raise `CONFIG_LOG_DEFAULT_LEVEL`
  (4 = DEBUG in Zephyr, not INFO) or re-enable `CONFIG_NET_LOG` /
  `CONFIG_NET_ARP_LOG_LEVEL_DBG` while measuring round-trip time:
  with `LOG_MODE_IMMEDIATE` at 115200 the net-stack debug output is
  ~425 ms per packet and dominates the result (see
  `the development notes (not published here)`)

## Build

```sh
source <your-venv>/bin/activate
cd $HOME/zephyrproject
west build -d /tmp/b_lan8720_link -b agrv2k_407 --pristine=auto \
    modules/hal_ag32/samples/lan8720_link
```

Default IP plan (override at build time):

- Board: `CONFIG_NET_CONFIG_MY_IPV4_ADDR="192.168.15.1"`
- Target: `CONFIG_LAN8720_LINK_PING_TARGET_IP="192.168.15.100"`

The two must be on the same /24. Build-time overrides:

```sh
west build ... -DCONFIG_NET_CONFIG_MY_IPV4_ADDR='"192.168.15.1"' \
              -DCONFIG_LAN8720_LINK_PING_TARGET_IP='"192.168.15.100"'
```

## Run

Burn the firmware (bitstream at `0x800e7000` survives):

```sh
source <your-venv>/bin/activate
bash $HAL_AGM_HOME/tools/flash_fw.sh /tmp/b_lan8720_link/zephyr/zephyr.bin
```

Then capture the boot log + ICMP round-trip line:

```sh
bash $HAL_AGM_HOME/tools/test_uart_capture.sh -n -t 20
```

If the dev bitstream on the board is the 100 MHz one
(`example_board.bin`), build with:

```sh
west build ... -DEXTRA_DTC_OVERLAY_FILE=/tmp/agm100.overlay
```

## Expected output

MDIO scan + link-up (same as the link-only baseline):

```
[00:00:01.275,000] lan8720_link: MDIO probe: scanning addr 0..7 for PHYID1R/PHYID2R
[00:00:01.284,000] lan8720_link:   PHY @0: ID1=0xffff ID2=0xffff
[00:00:01.291,000] lan8720_link:   PHY @1: ID1=0x0007 ID2=0xc0f1
[00:00:01.339,000] lan8720_link: LAN8720 link sample starting on ethernet@41040000 / phy ethernet-phy@1
[00:00:01.350,000] lan8720_link: LAN8720 link DOWN
[00:00:01.698,000] phy_agm_lan8720: PHY 1: link UP, 100 Mb full duplex (autoneg)
[00:00:01.700,000] lan8720_link: LAN8720 link UP: 100 Mb full duplex
[00:00:01.7xx,000] lan8720_link: Link is up; iface is up
[00:00:01.7xx,000] lan8720_link: PING 192.168.15.100: registering ICMP echo handler
[00:00:01.7xx,000] lan8720_link: PING 192.168.15.100: sending echo request
[00:00:01.7xx,000] lan8720_link: PING 192.168.15.100: reply received
[00:00:01.7xx,000] lan8720_link: PING 192.168.15.100 OK -- board-to-PC reachability confirmed
```

If the ICMP reply line never prints, the failure is in the TX or
RX path. Capture from PC side with `tcpdump -i eth0 -nn -e 'arp or
icmp'` to tell which:

- **No ARP request from the board**: TX path didn't deliver the
  frame. Check `eth_agm` `STAT.TOO_SMALL`/`TX_AHBERR` via
  `probe_state.sh`; verify `MAC_CTRL.TX_EN` is set after link-up.
- **ARP request on the wire, no ARP reply from PC**: PC firewall
  or PC's ARP table already has a stale entry. Try
  `arp -d 192.168.15.1` on PC.
- **ARP reply on the wire, no ICMP echo reply**: RX path didn't
  deliver the frame. Check `eth_agm` `STAT.RX_ERR`/`RX_AHBERR`/
  `TOO_SMALL`. If the reply *is* delivered but the round trip is
  ~0.8 s, look at console logging first (§3.28.6), not at the MAC.

## Bitstream dependency

This sample expects the dev bitstream at
`the 100 MHz development bitstream`
(100 MHz SYSCLK / 100 MHz HCLK, MAC pins on PIN_43..47 + 57..59
per the post-route netlist; no `PHY_RSTB`, no `PHY_INTB` — the
sample uses `MII_BMCR` soft reset and BMSR polling only).
The `<build_dir>/zephyr/board.bin` shipped with the SDK does NOT route
the MAC pins and will hang in pinctrl init.
The overlay `samples/lan8720_link/boards/agrv2k_407.overlay`
matches the dev bitstream: pins 43..47 + 57..59, `clk0`/`cpu0`
to 100 MHz, `sys::flash-max-frequency` to 50 MHz.
