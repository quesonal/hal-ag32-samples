/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * lan8720_link — bring up the AgRV2K MAC0 + LAN8720 PHY, report
 * link state, and run an ICMP echo (ping) round-trip once the
 * net_if is up.
 *
 * What this exercises (relative to the link-only baseline that
 * shipped before 2026-09-15):
 *   1..6 same as before: soc.c opens the AHB gate, pinctrl_apply_state
 *      sets AFSEL on the MAC pins, mdio_agm_init runs the reset
 *      handshake, the LAN8720 PHY driver probes / resets / autonegs
 *      (the MAC follows the PHY's link itself -- DT phy-handle -- so the
 *      net_if carrier comes on without application glue),
 *      and ethernet_init wires the iface.
 *   7. wait_for_iface_up() polls net_if_is_up() until the net stack
 *      finishes its post-carrier-on bringup (operational state).
 *   8. board_ping() runs an ICMP echo round-trip via Zephyr's
 *      net_icmp_ctx API (which builds a proper type=8/code=0/checksum
 *      header for us, so we don't need to hand-roll ICMP). The
 *      handler fires from the RX path inside net_recv_data(), so
 *      this probes the full eth_agm RX → net stack → net_icmp chain.
 *
 * What this does NOT exercise (see samples/lan8720_iperf):
 *   - TCP / UDP throughput; no zperf dependency here.
 *   - group-filtered multicast: eth_agm_start() sets
 *     MAC_CTRL_MULTICAST_EN, i.e. every multicast / broadcast frame
 *     is accepted (the IP has no wired-up hash table). See
 *     the port record (ethernet) §3.28.
 *
 * Logging notes
 *   - Do NOT raise CONFIG_LOG_DEFAULT_LEVEL (4 = DEBUG in Zephyr, not
 *     INFO) nor turn on CONFIG_NET_LOG / CONFIG_NET_ARP_LOG_LEVEL_DBG
 *     while measuring round-trip time: the net-stack debug output is
 *     ~425 ms per packet at 115200 with LOG_MODE_IMMEDIATE and
 *     dominates the measurement (see the port record (ethernet)
 *     §3.28.6).
 *   - The PASS/FAIL line goes through LOG_INF/LOG_ERR at level 4 (the
 *     prj.conf default), i.e. it is visible without lowering the
 *     global log level.
 *
 * IP addressing
 *   - Board IP: CONFIG_NET_CONFIG_MY_IPV4_ADDR (default 192.168.1.10)
 *   - Target IP: CONFIG_LAN8720_LINK_PING_TARGET_IP (default 192.168.1.1)
 *   - Both must be in the same /24 for the link to be useful; override
 *     at build time with -DCONFIG_NET_CONFIG_MY_IPV4_ADDR=... or
 *     -DCONFIG_LAN8720_LINK_PING_TARGET_IP=...
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/mii.h>
#include <zephyr/drivers/mdio.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/icmp.h>
#include <zephyr/net/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lan8720_link, LOG_LEVEL_INF);

static const struct device *const eth0 = DEVICE_DT_GET(DT_NODELABEL(eth0));
static const struct device *const phy0 = DEVICE_DT_GET(DT_NODELABEL(phy0));
static const struct device *const mdio0 = DEVICE_DT_GET(DT_NODELABEL(mdio0));

#define LINK_WAIT_TIMEOUT_MS 10000
#define IFACE_WAIT_TIMEOUT_MS 10000
#define IFACE_POLL_MS         100

static void on_link_state_change(const struct device *phy_dev,
				 struct phy_link_state *state,
				 void *user_data)
{
	ARG_UNUSED(user_data);

	if (state->is_up) {
		LOG_INF("LAN8720 link UP: %s Mb %s duplex",
			PHY_LINK_IS_SPEED_100M(state->speed) ? "100" : "10",
			PHY_LINK_IS_FULL_DUPLEX(state->speed) ? "full" : "half");
	} else {
		LOG_INF("LAN8720 link DOWN");
	}

	/* The MAC follows this PHY by itself: the eth0 node has
	 * phy-handle = <&phy0> and the MAC driver subscribes with
	 * phy_link_callback_set(), so the link state reaches the net stack
	 * without application glue. A PHY has exactly one callback slot, and
	 * it belongs to the MAC -- this function only reports what it sees. */
}

/* Poll the PHY until link is up or timeout. The PHY driver's
 * monitor work reports the transition through the callback; this
 * is just a bounded wait so the sample doesn't race past
 * wait_for_iface_up() before the carrier has actually been
 * declared. */
static int wait_for_phy_link_up(int timeout_ms)
{
	int waited = 0;

	while (waited < timeout_ms) {
		struct phy_link_state st = { 0 };

		if (phy_get_link_state(phy0, &st) == 0 && st.is_up) {
			on_link_state_change(phy0, &st, NULL);
			return 0;
		}
		k_msleep(IFACE_POLL_MS);
		waited += IFACE_POLL_MS;
	}
	return -ETIMEDOUT;
}

/* Wait for the Zephyr net stack to mark the default iface as
 * operational. net_if_carrier_on() (called from eth_agm_init via
 * ethernet_init) flips the carrier; net_if_is_up() only flips after
 * update_operational_state() runs, which is on the net_mgmt
 * workqueue. Without this wait, board_ping() would fire before the
 * iface is actually usable and the kernel would drop the ICMP at
 * net_if_try_queue_tx(). */
static int wait_for_iface_up(int timeout_ms)
{
	struct net_if *iface = net_if_get_default();
	int waited = 0;

	while (waited < timeout_ms) {
		if (iface != NULL && net_if_is_up(iface)) {
			return 0;
		}
		k_msleep(IFACE_POLL_MS);
		waited += IFACE_POLL_MS;
	}
	return -ETIMEDOUT;
}

/* --- ICMP echo round-trip ----------------------------------------------
 *
 * Use Zephyr's net_icmp API rather than SOCK_RAW + IPPROTO_ICMP:
 *   - The kernel builds the ICMP header (type=8/code=0/checksum)
 *     for us, so we don't have to hand-roll it;
 *   - The reply handler is invoked from the net stack after
 *     net_recv_data() returns, which means a successful PASS
 *     proves the full RX chain (eth_agm ISR -> net_pkt_write ->
 *     net_recv_data -> net_icmp input -> handler).
 *
 * The handler is called in ISR-ish context (net_rx workqueue);
 * we just set a flag and let the main thread observe it.
 */

static struct net_icmp_ctx icmp_ctx;
static volatile bool icmp_reply_seen;
static struct k_sem icmp_done;

static enum net_verdict icmp_echo_handler(struct net_icmp_ctx *ctx,
					  struct net_pkt *pkt,
					  struct net_icmp_ip_hdr *ip_hdr,
					  struct net_icmp_hdr *icmp_hdr,
					  void *user_data)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(pkt);
	ARG_UNUSED(ip_hdr);
	ARG_UNUSED(icmp_hdr);
	ARG_UNUSED(user_data);

	icmp_reply_seen = true;
	k_sem_give(&icmp_done);
	return NET_OK;
}

static int board_ping(uint32_t timeout_ms)
{
	struct sockaddr_in dst;
	struct net_icmp_ping_params params = {
		.identifier = 0x1234,
		.sequence = 1,
		.tc_tos = 0,
		.priority = 0,
		.data = NULL,
		.data_size = 0,
	};
	int ret;

	LOG_INF("PING %s: registering ICMP echo handler",
		CONFIG_LAN8720_LINK_PING_TARGET_IP);

	k_sem_init(&icmp_done, 0, 1);
	icmp_reply_seen = false;

	ret = net_icmp_init_ctx(&icmp_ctx, NET_AF_INET,
				NET_ICMPV4_ECHO_REPLY, 0,
				icmp_echo_handler);
	if (ret < 0) {
		LOG_ERR("net_icmp_init_ctx failed: %d", ret);
		return ret;
	}

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	ret = net_addr_pton(AF_INET, CONFIG_LAN8720_LINK_PING_TARGET_IP,
			    &dst.sin_addr);
	if (ret < 0) {
		LOG_ERR("net_addr_pton(%s) failed: %d",
			CONFIG_LAN8720_LINK_PING_TARGET_IP, ret);
		(void)net_icmp_cleanup_ctx(&icmp_ctx);
		return ret;
	}

	LOG_INF("PING %s: sending echo request", CONFIG_LAN8720_LINK_PING_TARGET_IP);

	ret = net_icmp_send_echo_request(&icmp_ctx, NULL,
					(struct net_sockaddr *)&dst,
					&params, NULL);
	if (ret < 0) {
		LOG_ERR("net_icmp_send_echo_request failed: %d", ret);
		(void)net_icmp_cleanup_ctx(&icmp_ctx);
		return ret;
	}

	/* Wait for the handler to fire; bound so a stuck RX path
	 * doesn't hang the sample forever. */
	ret = k_sem_take(&icmp_done, K_MSEC(timeout_ms));
	if (ret == -ETIMEDOUT) {
		LOG_ERR("PING %s: timed out after %u ms",
			CONFIG_LAN8720_LINK_PING_TARGET_IP, timeout_ms);
		(void)net_icmp_cleanup_ctx(&icmp_ctx);
		return ret;
	}

	LOG_INF("PING %s: reply received",
		CONFIG_LAN8720_LINK_PING_TARGET_IP);

	(void)net_icmp_cleanup_ctx(&icmp_ctx);
	return 0;
}

int main(void)
{
	struct phy_link_state initial;
	int ret;

	/* Diagnostic: scan MDIO 0..7 for the PHY ID and log it. The
	 * dtsi defaults to reg = <1> (off-the-shelf LAN8720 strap
	 * PHYAD=00b), but a board with a different PHYAD wiring can
	 * override the overlay -- this scan makes the actual address
	 * visible in the boot log so link-state anomalies are
	 * attributable. */
	if (device_is_ready(mdio0)) {
		LOG_INF("MDIO probe: scanning addr 0..7 for PHYID1R/PHYID2R");
		for (uint8_t addr = 0; addr < 8; addr++) {
			uint16_t id1 = 0xFFFF, id2 = 0xFFFF;
			(void)mdio_read(mdio0, addr, MII_PHYID1R, &id1);
			(void)mdio_read(mdio0, addr, MII_PHYID2R, &id2);
			LOG_INF("  PHY @%u: ID1=0x%04x ID2=0x%04x",
				addr, id1, id2);
		}
	} else {
		LOG_ERR("mdio0 (%s) not ready -- a clock missing?",
			mdio0->name);
	}

	if (!device_is_ready(eth0)) {
		LOG_ERR("eth0 (%s) not ready", eth0->name);
		return 0;
	}
	if (!device_is_ready(phy0)) {
		LOG_ERR("phy0 (%s) not ready", phy0->name);
		return 0;
	}

	LOG_INF("LAN8720 link sample starting on %s / phy %s",
		eth0->name, phy0->name);

	/* Read the current PHY state once so the boot line prints
	 * "link DOWN" before the link comes up. The MAC follows this PHY
	 * itself (the PHY's callback slot belongs to it), so the sample only
	 * reports what it sees; wait_for_phy_link_up() logs the up event. */
	if (phy_get_link_state(phy0, &initial) == 0) {
		on_link_state_change(phy0, &initial, NULL);
	}

	ret = wait_for_phy_link_up(LINK_WAIT_TIMEOUT_MS);
	if (ret < 0) {
		LOG_ERR("PHY link not up after %d ms, aborting",
			LINK_WAIT_TIMEOUT_MS);
		return 0;
	}

	ret = wait_for_iface_up(IFACE_WAIT_TIMEOUT_MS);
	if (ret < 0) {
		LOG_ERR("Net iface not up after %d ms, aborting",
			IFACE_WAIT_TIMEOUT_MS);
		return 0;
	}
	LOG_INF("Link is up; iface is up");

	/* Reachability probe. A successful reply means ARP resolved
	 * AND the ICMP echo came back through eth_agm_rx_drain. A
	 * failure modes to attribute, in priority order:
	 *   - ARP timeout       -> board never sent an ARP request, or
	 *                          the request never reached the PC.
	 *   - sendto/enqueue    -> ICMP enqueue failed.
	 *   - no reply          -> RX path broken (driver bug, MAC
	 *                          filter, etc.); see net stats below.
	 */
	ret = board_ping(CONFIG_LAN8720_LINK_PING_TIMEOUT_MS);
	if (ret < 0) {
		LOG_ERR("PING %s failed (ret=%d) -- board-to-PC unreachable",
			CONFIG_LAN8720_LINK_PING_TARGET_IP, ret);
	} else {
		LOG_INF("PING %s OK -- board-to-PC reachability confirmed",
			CONFIG_LAN8720_LINK_PING_TARGET_IP);
	}

	/* Hang so UART logs can be captured after the test ends.
	 * Power-cycle the board to run the test again. */
	while (1) {
		k_sleep(K_SECONDS(60));
	}
	return 0;
}
