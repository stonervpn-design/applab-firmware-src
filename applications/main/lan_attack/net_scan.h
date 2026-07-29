#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

/**
 * net_scan — PURE ARP + TCP-probe primitives for the LAN recon feature.
 *
 * This module contains no firmware/lwIP/furi dependencies so it can be built
 * and unit-tested on the host. All IPv4 addresses are passed and returned in
 * host byte order; the on-wire fields are written/read big-endian (network
 * order) by these helpers.
 */

// ============ ARP (RFC 826, IPv4-over-Ethernet) ============

// Size of a IPv4/Ethernet ARP payload (htype..tpa), no Ethernet header.
#define ARP_PACKET_LEN 28

/**
 * Build an ARP "who-has target_ip? tell sender_ip" request into buf.
 * sender_mac is the 6-byte hardware address of the querying station.
 * Target hardware address is left zero (unknown). Returns ARP_PACKET_LEN on
 * success, 0 on error (NULL args or buf too small).
 */
size_t arp_build_request(
    uint8_t* buf,
    size_t buf_max,
    const uint8_t sender_mac[6],
    uint32_t sender_ip,
    uint32_t target_ip);

/**
 * Parse an ARP reply (oper == 2). On success writes the responder's hardware
 * address into out_mac (6 bytes) and its IPv4 address into *out_ip (host
 * order), then returns true. Returns false if the frame is too short, not an
 * IPv4/Ethernet ARP, or not a reply.
 */
bool arp_parse_reply(const uint8_t* frame, size_t len, uint8_t out_mac[6], uint32_t* out_ip);

// ============ TCP service probes ============

/**
 * Write a minimal SSH client identification line ("SSH-2.0-...\r\n"). Sending
 * it nudges an SSH server to emit its banner. Returns bytes written, 0 on error.
 */
size_t tcp_build_ssh_probe(uint8_t* buf, size_t buf_max);

/**
 * Write a minimal HTTP/1.0 GET request ("GET / HTTP/1.0\r\n\r\n") used to
 * trigger an HTTP response/server header. Returns bytes written, 0 on error.
 */
size_t tcp_build_http_probe(uint8_t* buf, size_t buf_max);
