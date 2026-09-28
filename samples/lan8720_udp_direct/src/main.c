/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * lan8720_udp_direct — hand-built UDP upload at the AgRV2K MAC0.
 *
 * Why this exists
 *   samples/lan8720_iperf goes through Zephyr's socket + net_pkt + IP +
 *   UDP path and tops out around 46 Mbit/s on the 200 MHz dev
 *   bitstream, while the same board pushes 98 Mbit/s when the frames
 *   are handed straight to the driver (samples/lan8720_iperf's RAW
 *   mode). The difference is per-packet work in the upstream stack,
 *   not the MAC. This sample keeps the third-party verifiability of
 *   the iperf path -- the frames carry legal iperf2 UDP datagrams, so
 *   the PC still runs plain `iperf -s -u` -- while building every
 *   header by hand and sending through a packet socket.
 *
 * What it does
 *   1. Link up + iface up (same bridge as the other LAN8720 samples).
 *   2. Resolve the server's MAC with our own ARP request/reply: once
 *      the stack is out of the TX path there is no neighbour cache to
 *      borrow. (The packet socket must be created with a non-zero
 *      protocol -- proto 0 is TX-only in Zephyr; see main().)
 *   3. Build one 1502 B frame template:
 *        Ethernet (14) + IPv4 (20) + UDP (8) + iperf2 header (40)
 *        + 1420 B of 'z' payload
 *      with the IPv4 header checksum computed once (nothing in the IP
 *      header changes per packet) and the UDP checksum left at 0,
 *      which IPv4 permits (RFC 768) and Linux accepts. That choice is
 *      deliberate: computing it would mean scanning 1460 B per frame,
 *      ~20-25 us on this CPU, and the frame budget at line rate is
 *      only ~122 us.
 *   4. Loop for the configured duration, updating only the iperf2
 *      datagram id and timestamp per frame, and sendto() the frame.
 *   5. Report frames/s and Mbit/s, then idle.
 *
 * What it does NOT do
 *   - Receive anything except the ARP reply. The iperf2 server's final
 *     report is deliberately not read: it is not needed to measure
 *     throughput (the server prints its own numbers) and reading it
 *     would drag the RX path in for nothing.
 *   - Any kind of flow control, retransmission or fragmentation. It is
 *     a one-way dev board, not a transport.
 *
 * How to run
 *   PC:      iperf -s -u            (iperf2; the report is the truth)
 *   board:   flash + reset
 *   capture: tcpdump -i <if> -n udp port 5001     (optional cross-check;
 *            note tcpdump itself drops frames at this rate -- compare
 *            "received by filter" with "captured")
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

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lan8720_udp_direct, LOG_LEVEL_INF);

static const struct device *const eth0 = DEVICE_DT_GET(DT_NODELABEL(eth0));
static const struct device *const phy0 = DEVICE_DT_GET(DT_NODELABEL(phy0));

#define FRAME_SIZE   CONFIG_LAN8720_UDP_DIRECT_PACKET_SIZE
#define ETH_HDR_LEN  14
#define IP_HDR_LEN   20
#define UDP_HDR_LEN  8
#define IPERF_HDR_LEN 40
#define HDR_TOTAL    (ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + IPERF_HDR_LEN)
#define PAYLOAD_LEN  (FRAME_SIZE - HDR_TOTAL)
#define IP_TOTAL_LEN  (FRAME_SIZE - ETH_HDR_LEN)
#define UDP_DGRAM_LEN (IP_TOTAL_LEN - IP_HDR_LEN)
#define UDP_PAYLOAD_LEN (IPERF_HDR_LEN + PAYLOAD_LEN)

#define LINK_WAIT_TIMEOUT_MS 10000
#define POLL_MS              100
#define ARP_TIMEOUT_MS       3000
#define ARP_RETRY_MS         500

/* ETH_P_IP / ETH_P_ARP come from zephyr/net/ethernet.h. */
BUILD_ASSERT(FRAME_SIZE >= HDR_TOTAL + 1, "frame too small for the headers");
BUILD_ASSERT(FRAME_SIZE <= NET_ETH_MTU + ETH_HDR_LEN,
	     "frame must fit NET_ETH_MTU plus the Ethernet header");
BUILD_ASSERT(CONFIG_LAN8720_UDP_DIRECT_SERVER_PORT > 0, "port must be set");

/* The iperf2 UDP datagram header zperf speaks (zperf_internal.h). We
 * reproduce it byte for byte so an off-the-shelf `iperf -s -u` accepts
 * the datagrams. id2 is left as payload filler, exactly like zperf
 * does -- iperf 2.2.1 accepts that (measured). */
struct iperf2_udp_hdr {
	uint32_t id;      /* big endian, 1-based */
	uint32_t tv_sec;  /* elapsed seconds */
	uint32_t tv_usec; /* elapsed microseconds */
	uint32_t id2;
	int32_t flags;          /* 0 => the fields below are informational */
	int32_t num_of_threads; /* 1 */
	int32_t port;
	int32_t buffer_len;
	int32_t bandwidth;
	int32_t num_of_bytes;
} __packed;

static uint8_t frame[FRAME_SIZE] __aligned(4);
static struct iperf2_udp_hdr *const pdu = (struct iperf2_udp_hdr *)(void *)
	(frame + ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN);

static int sock = -1;
static struct net_sockaddr_ll sock_addr;

static void on_link_state_change(const struct device *phy_dev,
				 struct phy_link_state *state, void *user_data)
{
	ARG_UNUSED(phy_dev);
	ARG_UNUSED(user_data);

	/* The MAC follows this PHY by itself: the eth0 node has
	 * phy-handle = <&phy0> and the MAC driver subscribes with
	 * phy_link_callback_set(), so the link state reaches the net stack
	 * without application glue. A PHY has exactly one callback slot, and
	 * it belongs to the MAC -- this function only reports what it sees. */
}

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

static uint16_t ipv4_checksum(const uint8_t *hdr, size_t len)
{
	uint32_t sum = 0U;

	for (size_t i = 0U; i < len; i += 2U) {
		sum += ((uint16_t)hdr[i] << 8) | hdr[i + 1U];
	}
	while ((sum >> 16) != 0U) {
		sum = (sum & 0xffffU) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static void build_frame_template(const uint8_t *dst_mac, const uint8_t *src_mac)
{
	struct net_if *iface = net_if_get_default();
	struct net_in_addr my_addr;
	struct net_in_addr server_addr;
	uint8_t *ip = frame + ETH_HDR_LEN;
	uint8_t *udp = ip + IP_HDR_LEN;

	(void)net_addr_pton(NET_AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &my_addr);
	(void)net_addr_pton(NET_AF_INET, CONFIG_LAN8720_UDP_DIRECT_SERVER_IP,
			    &server_addr);
	ARG_UNUSED(iface);

	/* Ethernet */
	memcpy(&frame[0], dst_mac, 6);
	memcpy(&frame[6], src_mac, 6);
	frame[12] = ETH_P_IP >> 8;
	frame[13] = ETH_P_IP & 0xff;

	/* IPv4: fixed 20 B header, nothing changes per packet. */
	memset(ip, 0, IP_HDR_LEN);
	ip[0] = 0x45;                     /* v4, IHL = 5 */
	ip[2] = (uint8_t)(IP_TOTAL_LEN >> 8);
	ip[3] = (uint8_t)(IP_TOTAL_LEN & 0xff);
	ip[8] = 64;                       /* TTL */
	ip[9] = 17;                       /* UDP */
	memcpy(&ip[12], my_addr.s4_addr, 4);
	memcpy(&ip[16], server_addr.s4_addr, 4);
	{
		/* Network byte order: write it out byte by byte, a native
		 * 16-bit store would land byte-swapped on RISC-V. */
		uint16_t csum = ipv4_checksum(ip, IP_HDR_LEN);

		ip[10] = (uint8_t)(csum >> 8);
		ip[11] = (uint8_t)(csum & 0xff);
	}

	/* UDP: checksum 0 -- "not computed", which IPv4 allows (RFC 768)
	 * and Linux accepts. It is what keeps the per-frame CPU cost down;
	 * see the file header. */
	udp[0] = 0x1f; udp[1] = 0x90;     /* source port 8080 (unused by the server) */
	udp[2] = (uint8_t)(CONFIG_LAN8720_UDP_DIRECT_SERVER_PORT >> 8);
	udp[3] = (uint8_t)(CONFIG_LAN8720_UDP_DIRECT_SERVER_PORT & 0xff);
	udp[4] = (uint8_t)(UDP_DGRAM_LEN >> 8);
	udp[5] = (uint8_t)(UDP_DGRAM_LEN & 0xff);
	udp[6] = 0; udp[7] = 0;

	/* Payload: 'z', like zperf. This also fills id2 (see above). */
	memset(frame + HDR_TOTAL, 'z', PAYLOAD_LEN);

	pdu->flags = 0;
	pdu->num_of_threads = net_htonl(1);
	pdu->port = net_htonl(CONFIG_LAN8720_UDP_DIRECT_SERVER_PORT);
	pdu->buffer_len = net_htonl(PAYLOAD_LEN);
	pdu->bandwidth = net_htonl(0);   /* no pacing; 0 = "as fast as it goes" */
	pdu->num_of_bytes = net_htonl((int32_t)UDP_PAYLOAD_LEN);
}

/* Our own ARP: once the stack is bypassed there is no neighbour cache. */
static int arp_resolve(uint8_t *mac_out)
{
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	struct net_linkaddr *link = net_if_get_link_addr(net_if_get_default());
	struct net_in_addr my_addr, server_addr;
	uint8_t req[42];
	uint8_t rx[128];
	int64_t deadline;

	(void)net_addr_pton(NET_AF_INET, CONFIG_NET_CONFIG_MY_IPV4_ADDR, &my_addr);
	(void)net_addr_pton(NET_AF_INET, CONFIG_LAN8720_UDP_DIRECT_SERVER_IP,
			    &server_addr);

	memset(req, 0, sizeof(req));
	memcpy(&req[0], bcast, 6);
	memcpy(&req[6], link->addr, 6);
	req[12] = ETH_P_ARP >> 8;
	req[13] = ETH_P_ARP & 0xff;
	req[14] = 0; req[15] = 1;          /* htype = Ethernet */
	req[16] = ETH_P_IP >> 8; req[17] = ETH_P_IP & 0xff;
	req[18] = 6; req[19] = 4;          /* hlen, plen */
	req[20] = 0; req[21] = 1;          /* opcode = request */
	memcpy(&req[22], link->addr, 6);
	memcpy(&req[28], my_addr.s4_addr, 4);
	memcpy(&req[38], server_addr.s4_addr, 4);

	deadline = k_uptime_get() + ARP_TIMEOUT_MS;

	while (k_uptime_get() < deadline) {
		struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };
		int sent = zsock_sendto(sock, req, sizeof(req), 0,
					(struct net_sockaddr *)&sock_addr,
					sizeof(sock_addr));
		int polls;

		if (sent < 0) {
			LOG_ERR("ARP request send failed: %d (errno %d)", sent, errno);
			return -EIO;
		}
		LOG_INF("ARP request sent (%d B), waiting for the reply", sent);

		/* zsock_poll rather than a blocking recv: whether a packet
		 * socket honours SO_RCVTIMEO is not something to bet a hang
		 * on (it hung here the first time). Drain a few frames per
		 * wakeup -- this socket sees every ethertype now. */
		for (polls = 0; polls < ARP_RETRY_MS / 20; polls++) {
			/* Drain a handful of frames per wakeup: this socket is
			 * bound to ETH_P_ALL, so it sees every ethertype. */
			int budget = 8;

			if (zsock_poll(&pfd, 1, 20) <= 0) {
				continue;
			}

			while (budget-- > 0) {
				int n = zsock_recv(sock, rx, sizeof(rx), 0);

				if (n < 42) {
					break;
				}
				if (rx[12] != (ETH_P_ARP >> 8) ||
				    rx[13] != (ETH_P_ARP & 0xff)) {
					continue;
				}
				if (rx[21] != 2 || /* opcode = reply */
				    memcmp(&rx[28], server_addr.s4_addr, 4) != 0) {
					continue;
				}

				memcpy(mac_out, &rx[22], 6);
				LOG_INF("ARP: %s is at %02x:%02x:%02x:%02x:%02x:%02x",
					CONFIG_LAN8720_UDP_DIRECT_SERVER_IP,
					mac_out[0], mac_out[1], mac_out[2],
					mac_out[3], mac_out[4], mac_out[5]);
				return 0;
			}
		}
	}

	LOG_ERR("no ARP reply from %s in %d ms -- is the server on this L2?",
		CONFIG_LAN8720_UDP_DIRECT_SERVER_IP, ARP_TIMEOUT_MS);
	return -ETIMEDOUT;
}

int main(void)
{
	struct net_if *iface;
	struct net_linkaddr *link;
	uint8_t server_mac[6];
	struct timeval rcvtimeo = { .tv_sec = 0, .tv_usec = 50000 };
	uint64_t sent = 0;
	int64_t start, end;
	int ret;

	LOG_INF("lan8720_udp_direct: hand-built UDP%u -> %s:%d, frame %d B "
		"(payload %d B)", IPERF_HDR_LEN + 8U,
		CONFIG_LAN8720_UDP_DIRECT_SERVER_IP,
		CONFIG_LAN8720_UDP_DIRECT_SERVER_PORT, FRAME_SIZE, PAYLOAD_LEN);

	if (!device_is_ready(eth0) || !device_is_ready(phy0)) {
		LOG_ERR("eth0/phy0 not ready -- check the bitstream pin list");
		return 0;
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

	iface = net_if_get_default();
	link = net_if_get_link_addr(iface);

	/* Protocol must NOT be 0: Zephyr's connection lookup answers
	 * "Local proto 0 doesn't forward any packets" (subsys/net/ip/
	 * connection.c), so a proto-0 socket is TX-only -- which is why
	 * zperf's RAW mode cannot see an ARP reply. ETH_P_ALL receives
	 * every ethertype; the ARP parser filters. */
	sock = zsock_socket(NET_AF_PACKET, NET_SOCK_RAW, net_htons(ETH_P_ALL));
	if (sock < 0) {
		LOG_ERR("packet socket failed: %d (errno %d)", sock, errno);
		goto idle;
	}

	memset(&sock_addr, 0, sizeof(sock_addr));
	sock_addr.sll_family = NET_AF_PACKET;
	sock_addr.sll_protocol = net_htons(ETH_P_ALL);
	sock_addr.sll_ifindex = net_if_get_by_iface(iface);
	ret = zsock_bind(sock, (struct net_sockaddr *)&sock_addr, sizeof(sock_addr));
	if (ret < 0) {
		LOG_ERR("packet socket bind failed: %d (errno %d)", ret, errno);
		goto idle;
	}
	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo,
			       sizeof(rcvtimeo));

	if (arp_resolve(server_mac) < 0) {
		/* No fallback on purpose: sending to a broadcast MAC works
		 * but is not what the frame says it is. If the ARP reply is
		 * not visible there is something to fix first. */
		LOG_INF("Result: FAIL (no ARP reply)");
		goto idle;
	}

	build_frame_template(server_mac, link->addr);

	LOG_INF("sending for %d ms ...", CONFIG_LAN8720_UDP_DIRECT_DURATION_MS);
	start = k_uptime_get();
	end = start + CONFIG_LAN8720_UDP_DIRECT_DURATION_MS;

	while (k_uptime_get() < end) {
		int64_t elapsed = k_uptime_get() - start;

		/* The only per-frame work: sequence number + timestamp. */
		pdu->id = net_htonl((uint32_t)sent + 1U);
		pdu->tv_sec = net_htonl((uint32_t)(elapsed / 1000));
		pdu->tv_usec = net_htonl((uint32_t)((elapsed % 1000) * 1000));

		if (zsock_sendto(sock, frame, sizeof(frame), 0,
				 (struct net_sockaddr *)&sock_addr,
				 sizeof(sock_addr)) < 0) {
			LOG_ERR("send failed at frame %llu: %d (errno %d)",
				(unsigned long long)sent, errno, errno);
			break;
		}
		sent++;
	}

	/* iperf2 end-of-test datagram: id is negative. Without it the
	 * server never closes the interval and reports the average over
	 * however long it kept waiting (measured: 20.4 Mbit/s reported for
	 * a 98.3 Mbit/s run). Send it twice -- a single UDP datagram is
	 * easy to lose and iperf2 also has a retransmit path for it. */
	{
		int64_t elapsed = k_uptime_get() - start;

		pdu->id = net_htonl((uint32_t)(-(int32_t)((uint32_t)sent + 1U)));
		pdu->tv_sec = net_htonl((uint32_t)(elapsed / 1000));
		pdu->tv_usec = net_htonl((uint32_t)((elapsed % 1000) * 1000));
		for (int i = 0; i < 2; i++) {
			(void)zsock_sendto(sock, frame, sizeof(frame), 0,
					   (struct net_sockaddr *)&sock_addr,
					   sizeof(sock_addr));
			k_msleep(10);
		}
	}

	{
		uint64_t us = (uint64_t)(k_uptime_get() - start) * 1000ULL;
		uint64_t bits = sent * (unsigned long long)FRAME_SIZE * 8ULL;
		uint64_t mbps_x100 = us ? (bits * 100ULL) / us : 0ULL;
		uint64_t payload_mbps_x100 =
			us ? (sent * (unsigned long long)PAYLOAD_LEN * 8ULL * 100ULL) / us
			   : 0ULL;

		LOG_INF("sent %llu frames in %llu.%03llu s (%llu frames/s)",
			(unsigned long long)sent, (unsigned long long)(us / 1000000ULL),
			(unsigned long long)((us % 1000000ULL) / 1000ULL),
			(unsigned long long)(us ? (sent * 1000000ULL) / us : 0ULL));
		LOG_INF("Throughput: %llu.%02llu Mbit/s on the wire "
			"(%llu.%02llu Mbit/s of UDP payload)",
			(unsigned long long)(mbps_x100 / 100ULL),
			(unsigned long long)(mbps_x100 % 100ULL),
			(unsigned long long)(payload_mbps_x100 / 100ULL),
			(unsigned long long)(payload_mbps_x100 % 100ULL));
		LOG_INF("cross-check: the iperf2 server's own report");
	}

	if (CONFIG_LAN8720_UDP_DIRECT_MIN_MBITPS > 0) {
		/* reuse the same math for the verdict */
		uint64_t us = (uint64_t)(k_uptime_get() - start) * 1000ULL;
		uint64_t mbps = us ? (sent * (unsigned long long)FRAME_SIZE * 8ULL) / us
				   : 0ULL;

		if (mbps < (uint64_t)CONFIG_LAN8720_UDP_DIRECT_MIN_MBITPS) {
			LOG_WRN("Result: FAIL (below %d Mbit/s)",
				CONFIG_LAN8720_UDP_DIRECT_MIN_MBITPS);
		} else {
			LOG_INF("Result: PASS");
		}
	}

idle:
	while (1) {
		k_sleep(K_SECONDS(60));
	}
	return 0;
}
