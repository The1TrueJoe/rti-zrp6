/* w5100.h — the Ethernet controller.
 *
 * The W5100 runs the whole TCP/IP stack in hardware, so this is socket
 * bookkeeping rather than a protocol implementation: the 8032 never sees a
 * retransmit, a window or a checksum.
 */
#ifndef ZRP6_W5100_H
#define ZRP6_W5100_H
#include <stdint.h>

void w5100_init(const uint8_t *mac, const uint8_t *ip,
                const uint8_t *netmask, const uint8_t *gateway);

/* How often the external bus dropped a bit on a write into a W5100 buffer and
 * had to be written again, and how often three attempts still failed. This
 * board does that about once every two thousand bytes — see w5100.c. Read them
 * over the recovery channel to know whether the bus has got worse. */
extern __xdata uint16_t w5100_rewrites;
extern __xdata uint16_t w5100_badbytes;
extern __xdata uint16_t w5100_ptr_retry;
extern __xdata uint16_t w5100_ptr_bad;

uint8_t sock_status(uint8_t s);
void sock_close(uint8_t s);

/* TCP */
void tcp_listen(uint8_t s, uint16_t port);
/* Outbound TCP, for joining a house broker (mqtt.c). Not built into the image
 * — every socket is a listener — so compiled only with -DZRP6_BRIDGE. */
uint8_t tcp_connect(uint8_t s, const uint8_t *ip, uint16_t port);
uint16_t tcp_recv(uint8_t s, uint8_t *buf, uint16_t max);
uint8_t tcp_send(uint8_t s, const uint8_t *buf, uint16_t len);
void tcp_disconnect(uint8_t s);      /* graceful: FIN */
void tcp_abort(uint8_t s);           /* CLOSE, now */

/* Stream access. The W5100 has 2 KB of receive buffer per socket in its own
 * RAM, so a partly-arrived packet can simply be left there: peek at it, and
 * only tcp_drop() it once the whole thing is present. That is 2 KB of
 * reassembly per connection that the 8032's 8 KB does not have to find. */
uint16_t tcp_avail(uint8_t s);
uint8_t tcp_peek(uint8_t s, uint16_t off);
/* A long read that must be exact (a firmware image): take the ring pointer
 * once, read at it with every byte proofed against the D4 bus fault, and
 * release up to the new pointer. See w5100.c. */
uint16_t tcp_rx_ptr(uint8_t s);
#ifdef TX_DEBUG
void txd_log(uint8_t ev, uint8_t s);
extern __xdata uint8_t txd[48][8];
extern __xdata uint8_t txd_i, txd_left, txd_hits;
#endif
void tcp_read_sure(uint8_t s, uint16_t rd, uint8_t *buf, uint16_t len);
void tcp_rx_release(uint8_t s, uint16_t rd);
void tcp_read(uint8_t s, uint16_t off, uint8_t *buf, uint16_t len);
void tcp_drop(uint8_t s, uint16_t n);

/* Streaming transmit, for bodies far larger than any buffer this board can
 * spare: stage into the W5100's TX ring a piece at a time, then commit once.
 * tcp_stage_code() copies straight out of Flash, so serving the UI costs no
 * XDATA at all. */
uint16_t tcp_txfree(uint8_t s);
void tcp_stage(uint8_t s, uint16_t off, const uint8_t *buf, uint16_t len);
void tcp_stage_code(uint8_t s, uint16_t off, __code const uint8_t *src, uint16_t len);
void tcp_commit(uint8_t s, uint16_t len);

/* UDP, for DHCP (net.c). Data is staged with tcp_stage() and sent with
 * tcp_commit(), read with tcp_avail()/tcp_rx_ptr()/tcp_read_sure() and freed
 * with tcp_rx_release(), exactly as for TCP; each datagram arrives behind an
 * 8-byte header (sender IP, port, length). */
void udp_open(uint8_t s, uint16_t port);
void udp_to(uint8_t s, const uint8_t *ip, uint16_t port);

/* A UDP socket that sends to a multicast group (SDDP, net.c). The W5100
 * cannot ARP a group address, so the group's MAC, address and port are set
 * before OPEN with the MULTI bit, which also joins the group while it is
 * open. `grp` is 6 bytes of MAC then 4 of IP. */
void udp_open_group(uint8_t s, const uint8_t *grp, uint16_t port);

/* The box's own address, without resetting the chip. */
void w5100_addr(const uint8_t *ip, const uint8_t *netmask, const uint8_t *gateway);

/* 1 if the chip still holds its IP — 0 means it was reset and needs
 * w5100_init() again. */
uint8_t w5100_ip_ok(const uint8_t *ip);

#endif
