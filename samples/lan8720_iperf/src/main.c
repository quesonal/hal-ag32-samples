/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * lan8720_iperf — iperf2-compatible TCP throughput client for the
 * AgRV2K on-die MAC0 + LAN8720 RMII PHY.
 *
 * Streams TCP data at an iperf2 server (`iperf -s` on the PC) for a
 * fixed duration and reports the throughput over UART. The upload path
 * is upstream zperf (CONFIG_NET_ZPERF, server and shell disabled) so
 * the wire format is the tested one this sample does not have to
 * implement.
 *
 * Sequence:
 *   1. Wait for the PHY link (the MAC follows it by itself: DT
 *      phy-handle -> the MAC driver subscribes to the PHY's link
 *      callback; same as samples/lan8720_link).
 *   2. Wait for the iface to go operational.
 *   3. One zperf TCP upload: LAN8720_IPERF_DURATION_MS at
 *      LAN8720_IPERF_PACKET_SIZE per segment.
 *   4. Print bytes / packets / errors / Mbit/s and a PASS/FAIL verdict
 *      against LAN8720_IPERF_MIN_MBITPS, then idle so the UART log can
 *      still be captured.
 *
 * There is deliberately no ICMP pre-flight before the upload.
 * net_icmp_init_ctx() dispatches a context on (family, type, code) only,
 * so *any* echo reply reaching the board satisfies the handler: on the
 * dev board (2026-09-15) it reported "PING OK" for 192.168.15.222, an
 * address that does not exist. Adding a source-address check did not
 * make it trustworthy either (it still reported a reply from a host the
 * wire showed the board never reached), so the upload result is the
 * only signal this sample reports.
 *
 * What this verifies:
 *   - eth_agm_send() really puts frames on the wire (descriptor rings +
 *     DMA), sustained -- lan8720_link only proves a single ICMP round
 *     trip.
 *   - The Zephyr net stack (TCP) underneath the AGM MAC.
 *   - A throughput baseline for spotting later regressions.
 *
 * What this does NOT verify:
 *   - The RX data path. The upload is one-directional; the server only
 *     sends ACKs back. RX under load (server pushes data) needs a
 *     different test.
 *   - UDP / PPS / jitter. zperf_udp_upload() exists; add a UDP variant
 *     here if that becomes interesting.
 *
 * iperf2 only. iperf3 speaks JSON-RPC over TCP and is NOT compatible.
 * zperf does not send iperf2's 28-byte control header, so a modern
 * iperf2 server still reports bytes/wallclock but also logs a cosmetic
 * "LAST PACKET NOT RECEIVED!!!" -- see README.md.
 *
 * Board IP:    CONFIG_NET_CONFIG_MY_IPV4_ADDR   (prj.conf, 192.168.15.1)
 * Server IP:   CONFIG_LAN8720_IPERF_SERVER_IP   (192.168.15.100)
 * Both must sit in the same /24.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/mii.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/zperf.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lan8720_iperf, LOG_LEVEL_INF);

/* zperf clamps an oversized packet_size to CONFIG_NET_ZPERF_MAX_PACKET_SIZE
 * and only logs a runtime warning; catch the mismatch at build time
 * instead, because it silently changes what "packet_size" means. */
BUILD_ASSERT(CONFIG_LAN8720_IPERF_PACKET_SIZE <= CONFIG_NET_ZPERF_MAX_PACKET_SIZE,
	     "LAN8720_IPERF_PACKET_SIZE must be <= CONFIG_NET_ZPERF_MAX_PACKET_SIZE");

/* zperf adds its own header on top of the payload we ask for, so the
 * segment must still fit the link MTU. For UDP the payload goes out as
 * one datagram (zperf's 40 B header is already inside packet_size):
 * +8 UDP +20 IP. IPv4 fragmentation is off in this sample, so an
 * oversized datagram is dropped at send time rather than split. */
#if defined(CONFIG_LAN8720_IPERF_PROTO_RAW)
/* RAW: packet_size is the whole frame, header included. */
BUILD_ASSERT(CONFIG_LAN8720_IPERF_RAW_PACKET_SIZE <= CONFIG_NET_ZPERF_MAX_PACKET_SIZE,
	     "LAN8720_IPERF_RAW_PACKET_SIZE must be <= CONFIG_NET_ZPERF_MAX_PACKET_SIZE");
BUILD_ASSERT(CONFIG_LAN8720_IPERF_RAW_PACKET_SIZE <= NET_ETH_MTU + 14,
	     "RAW frame must fit NET_ETH_MTU plus the Ethernet header");
#elif defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
BUILD_ASSERT(CONFIG_LAN8720_IPERF_PACKET_SIZE + 28 <= NET_ETH_MTU,
	     "UDP packet_size + 8 (UDP) + 20 (IP) must fit NET_ETH_MTU");
#else
BUILD_ASSERT(CONFIG_LAN8720_IPERF_PACKET_SIZE + 40 <= NET_ETH_MTU,
	     "TCP packet_size + 20 (TCP) + 20 (IP) must fit NET_ETH_MTU");
#endif

static const struct device *const eth0 = DEVICE_DT_GET(DT_NODELABEL(eth0));
static const struct device *const phy0 = DEVICE_DT_GET(DT_NODELABEL(phy0));

#define LINK_WAIT_TIMEOUT_MS 10000
#define POLL_MS              100

static void on_link_state_change(const struct device *phy_dev,
				 struct phy_link_state *state,
				 void *user_data)
{
	ARG_UNUSED(phy_dev);
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

/* Wait for the PHY to report link up, replaying the callback on the way
 * (the PHY driver's monitor work owns the transition; this just makes
 * sure we do not race past it). */
static int wait_for_phy_link_up(int timeout_ms)
{
	int waited = 0;

	while (waited < timeout_ms) {
		struct phy_link_state state = { 0 };

		if (phy_get_link_state(phy0, &state) == 0 && state.is_up) {
			on_link_state_change(phy0, &state, NULL);
			return 0;
		}
		k_msleep(POLL_MS);
		waited += POLL_MS;
	}
	return -ETIMEDOUT;
}

/* Wait for the net stack to finish its post-carrier bringup. The carrier
 * goes on when the MAC driver hears the PHY's link callback;
 * net_if_is_up() only flips once update_operational_state() has run on
 * the net_mgmt workqueue, and a connect() before that is dropped. */
static int wait_for_iface_up(int timeout_ms)
{
	struct net_if *iface = net_if_get_default();
	int waited = 0;

	while (waited < timeout_ms) {
		if (iface != NULL && net_if_is_up(iface)) {
			return 0;
		}
		k_msleep(POLL_MS);
		waited += POLL_MS;
	}
	return -ETIMEDOUT;
}

int main(void)
{
	struct zperf_upload_params param = { 0 };
	struct zperf_results result = { 0 };
	struct net_sockaddr_in server = { 0 };
	struct phy_link_state initial = { 0 };
	uint64_t elapsed_us;
	uint64_t mbps_x100;
	int ret;

	LOG_INF("lan8720_iperf: iperf2 %s client, board %s -> server %s:%d",
#if defined(CONFIG_LAN8720_IPERF_PROTO_RAW)
		"RAW-L2",
#elif defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
		"UDP",
#else
		"TCP",
#endif
		CONFIG_NET_CONFIG_MY_IPV4_ADDR,
		CONFIG_LAN8720_IPERF_SERVER_IP,
		CONFIG_LAN8720_IPERF_SERVER_PORT);
	LOG_INF("duration=%d ms  packet_size=%d B  min=%d Mbit/s",
		CONFIG_LAN8720_IPERF_DURATION_MS,
		CONFIG_LAN8720_IPERF_PACKET_SIZE,
		CONFIG_LAN8720_IPERF_MIN_MBITPS);
#if defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
	LOG_INF("UDP target rate: %d kbit/s (x1024 bit/s)",
		CONFIG_LAN8720_IPERF_UDP_RATE_KBPS);
#endif

	if (!device_is_ready(eth0) || !device_is_ready(phy0)) {
		LOG_ERR("eth0/phy0 not ready -- check the bitstream pin list");
		return 0;
	}

	if (phy_get_link_state(phy0, &initial) == 0) {
		on_link_state_change(phy0, &initial, NULL);
	}

	if (wait_for_phy_link_up(LINK_WAIT_TIMEOUT_MS) < 0) {
		LOG_ERR("PHY link not up after %d ms, aborting",
			LINK_WAIT_TIMEOUT_MS);
		return 0;
	}
	if (wait_for_iface_up(LINK_WAIT_TIMEOUT_MS) < 0) {
		LOG_ERR("net iface not up after %d ms, aborting",
			LINK_WAIT_TIMEOUT_MS);
		return 0;
	}
	LOG_INF("link + iface up");

	server.sin_family = NET_AF_INET;
	server.sin_port = net_htons(CONFIG_LAN8720_IPERF_SERVER_PORT);
	ret = net_addr_pton(NET_AF_INET, CONFIG_LAN8720_IPERF_SERVER_IP,
			    &server.sin_addr);
	if (ret < 0) {
		LOG_ERR("net_addr_pton(%s) failed: %d",
			CONFIG_LAN8720_IPERF_SERVER_IP, ret);
		goto idle;
	}
	memcpy(&param.peer_addr_storage, &server, sizeof(server));

	param.duration_ms = CONFIG_LAN8720_IPERF_DURATION_MS;
#if defined(CONFIG_LAN8720_IPERF_PROTO_RAW)
	param.packet_size = CONFIG_LAN8720_IPERF_RAW_PACKET_SIZE;
	param.rate_kbps = CONFIG_LAN8720_IPERF_RAW_RATE_KBPS;
#else
	param.packet_size = CONFIG_LAN8720_IPERF_PACKET_SIZE;
#endif
#if defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
	/* zperf paces the UDP loop by rate_kbps and divides by it, so this
	 * must be non-zero. The default is above the 100 Mb PHY, which
	 * makes the pacing a no-op and the run a max-throughput probe. */
	param.rate_kbps = CONFIG_LAN8720_IPERF_UDP_RATE_KBPS;
#else
	param.rate_kbps = 0U; /* TCP ignores it; 0 = no rate limit */
	/* Without TCP_NODELAY the stack waits up to 200 ms for an ACK
	 * before flushing each segment, which caps throughput far below
	 * the link rate. */
	param.options.tcp_nodelay = 1;
#endif

	LOG_INF("upload -> %s:%d for %d ms ...",
		CONFIG_LAN8720_IPERF_SERVER_IP,
		CONFIG_LAN8720_IPERF_SERVER_PORT,
		param.duration_ms);

#if defined(CONFIG_LAN8720_IPERF_PROTO_RAW)
	/* Hand-built Ethernet frame: broadcast destination (the host NIC
	 * accepts it at L2 and the kernel then drops the unknown
	 * ethertype), the board's own MAC as source. */
	uint8_t raw_hdr[14] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct net_linkaddr *link_addr = net_if_get_link_addr(net_if_get_default());
	struct zperf_raw_upload_params raw = { 0 };

	memcpy(&raw_hdr[6], link_addr->addr, 6);
	raw_hdr[12] = (CONFIG_LAN8720_IPERF_RAW_ETHERTYPE >> 8) & 0xff;
	raw_hdr[13] = CONFIG_LAN8720_IPERF_RAW_ETHERTYPE & 0xff;

	raw.duration_ms = CONFIG_LAN8720_IPERF_DURATION_MS;
	raw.rate_kbps = CONFIG_LAN8720_IPERF_RAW_RATE_KBPS;
	raw.packet_size = CONFIG_LAN8720_IPERF_RAW_PACKET_SIZE;
	raw.hdr = raw_hdr;
	raw.hdr_len = sizeof(raw_hdr);
	raw.if_index = net_if_get_by_iface(net_if_get_default());

	LOG_INF("RAW frame %u B (ethertype 0x%04x) -- capture with "
		"`tcpdump -i <if> ether proto 0x%04x`",
		raw.packet_size, CONFIG_LAN8720_IPERF_RAW_ETHERTYPE,
		CONFIG_LAN8720_IPERF_RAW_ETHERTYPE);

	ret = zperf_raw_upload(&raw, &result);
#elif defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
	ret = zperf_udp_upload(&param, &result);
#else
	ret = zperf_tcp_upload(&param, &result);
#endif
	if (ret < 0) {
		LOG_ERR("zperf upload failed: %d (errno %d)", ret, errno);
		LOG_ERR("is iperf2 running on %s:%d?  (%s)",
			CONFIG_LAN8720_IPERF_SERVER_IP,
			CONFIG_LAN8720_IPERF_SERVER_PORT,
#if defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
			"`iperf -s -u`");
#else
			"`iperf -s`");
#endif
		LOG_INF("Result: FAIL (upload error)");
		goto idle;
	}

	/* zperf reports elapsed time in two fields and the two protocols
	 * fill different ones:
	 *   TCP: only client_time_in_us (its own send loop) -- time_in_us
	 *        is receiver-reported and stays 0, which is what produced a
	 *        bogus "0.00 Mbit/s -> FAIL" before (dev board 2026-09-15).
	 *   UDP: time_in_us is the *server's* measured duration and
	 *        total_len is the bytes the server actually received, i.e.
	 *        the honest "what arrived on the wire" number. Prefer it.
	 *
	 * Mbit/s = bytes * 8 / us (the 1e6 cancels). Scale by 100 first so
	 * the print shows hundredths without floating point. */
#if defined(CONFIG_LAN8720_IPERF_PROTO_RAW)
	/* RAW has no peer report; the client loop time is all there is. */
	elapsed_us = result.client_time_in_us;
#elif defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
	elapsed_us = (result.time_in_us != 0U) ? result.time_in_us
					       : result.client_time_in_us;
#else
	elapsed_us = (result.client_time_in_us != 0U) ? result.client_time_in_us
						      : result.time_in_us;
#endif

	if (elapsed_us == 0U) {
		mbps_x100 = 0U;
	} else {
		mbps_x100 = (result.total_len * 8ULL * 100ULL) /
			    elapsed_us;
	}

	LOG_INF("client sent %u packets, %llu bytes, %u send errors",
		result.nb_packets_sent,
		(unsigned long long)result.nb_packets_sent *
			(unsigned long long)result.packet_size,
		result.nb_packets_errors);
#if defined(CONFIG_LAN8720_IPERF_PROTO_RAW)
	LOG_INF("no peer report (RAW, TX only) over %llu us",
		(unsigned long long)elapsed_us);
	LOG_WRN("RAW mode: the only honest yardstick is the capture on the "
		"PC (frames x packet_size / wallclock)");
#elif defined(CONFIG_LAN8720_IPERF_PROTO_UDP)
	LOG_INF("server received %u packets, %u lost, %u out of order, "
		"jitter %u us, interval %llu us",
		result.nb_packets_rcvd, result.nb_packets_lost,
		result.nb_packets_outorder, result.jitter_in_us,
		(unsigned long long)elapsed_us);
#else
	LOG_INF("client handed %llu bytes to the socket over %llu us",
		(unsigned long long)result.total_len,
		(unsigned long long)elapsed_us);
#endif
	LOG_INF("Throughput: %llu.%02llu Mbit/s",
		(unsigned long long)(mbps_x100 / 100ULL),
		(unsigned long long)(mbps_x100 % 100ULL));

	if (result.nb_packets_errors > 0U) {
		LOG_WRN("%u send errors -- zperf counts net_buf exhaustion "
			"here; raise CONFIG_NET_BUF_TX_COUNT if the rate is low",
			result.nb_packets_errors);
	}

	if (CONFIG_LAN8720_IPERF_MIN_MBITPS > 0 &&
	    (mbps_x100 / 100ULL) < (uint64_t)CONFIG_LAN8720_IPERF_MIN_MBITPS) {
		LOG_WRN("Result: FAIL (below %d Mbit/s)",
			CONFIG_LAN8720_IPERF_MIN_MBITPS);
	} else {
		LOG_INF("Result: PASS");
	}

idle:
	/* Idle instead of returning, so the UART stays alive for capture
	 * and so a late server-side log can still be correlated. */
	while (1) {
		k_sleep(K_SECONDS(60));
	}
	return 0;
}
