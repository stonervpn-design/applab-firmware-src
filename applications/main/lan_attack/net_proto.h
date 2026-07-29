#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

// ============ DHCP Packet Builders/Parsers (RFC 2131) ============

typedef struct {
    uint8_t op;            // 1=request, 2=reply
    uint32_t xid;          // transaction ID
    uint8_t chaddr[6];     // client hardware address (MAC)
    uint32_t yiaddr;       // your (offered) IP address
    uint32_t siaddr;       // server IP address
    uint32_t giaddr;       // gateway/relay IP address
    uint8_t msg_type;      // DHCP message type (option 53: 1=discover, 2=offer, 3=request, 5=ack, 6=nak)
    uint32_t requested_ip; // requested IP (from option 50, if any)
} DhcpParsed;

// Parse a DHCP packet. Returns true if valid, false if malformed/too short.
// Extracts: op, xid, chaddr, msg_type, yiaddr, siaddr, giaddr, requested_ip
bool dhcp_parse(const uint8_t* frame, size_t len, DhcpParsed* out);

// Build a DHCP Discover packet (client broadcasts "who-has-DHCP-server?")
// Returns the size written to frame_buf, 0 on error.
size_t dhcp_build_discover(uint8_t* frame_buf, size_t buf_max, const uint8_t client_mac[6]);

// Build a DHCP Offer packet (server replies "here's an IP for you")
// lease_sec is lease time in seconds (e.g., 3600)
// Returns the size written to frame_buf, 0 on error.
size_t dhcp_build_offer(uint8_t* frame_buf, size_t buf_max,
                        const uint8_t client_mac[6], uint32_t xid,
                        uint32_t offered_ip, uint32_t gateway_ip,
                        uint32_t dns_ip, uint16_t lease_sec);

// Build a DHCP Ack packet (server confirms "assignment confirmed")
// lease_sec is lease time in seconds (e.g., 3600)
// Returns the size written to frame_buf, 0 on error.
size_t dhcp_build_ack(uint8_t* frame_buf, size_t buf_max,
                      const uint8_t client_mac[6], uint32_t xid,
                      uint32_t assigned_ip, uint32_t gateway_ip,
                      uint32_t dns_ip, uint16_t lease_sec);

// Build a DHCP Nak packet (server rejects "no, can't give you that IP")
// Returns the size written to frame_buf, 0 on error.
size_t dhcp_build_nak(uint8_t* frame_buf, size_t buf_max,
                      const uint8_t client_mac[6], uint32_t xid);

// ============ LLMNR Query/Response Builders/Parsers (RFC 4795) ============

// Parse an LLMNR query packet. Extracts the queried name.
// Returns true if valid, false if malformed.
// query_name will be null-terminated UTF-8 string (max 255 bytes).
bool llmnr_parse_query(const uint8_t* frame, size_t len, char* query_name, size_t max_name_len);

// Build an LLMNR response pointing a name at an attacker IP.
// Returns the size written to frame_buf, 0 on error.
size_t llmnr_build_response(uint8_t* frame_buf, size_t buf_max,
                            uint16_t transaction_id,
                            const char* query_name,
                            uint32_t response_ip);

// ============ NBT-NS Name Query/Response Builders/Parsers (RFC 1002) ============

// Parse an NBT-NS name query packet. Extracts the queried NetBIOS name.
// Returns true if valid, false if malformed.
// query_name will be null-terminated string of the decoded name (max 255 bytes).
bool nbtn_parse_query(const uint8_t* frame, size_t len, char* query_name, size_t max_name_len);

// Build an NBT-NS response pointing a NetBIOS name at an attacker IP.
// Returns the size written to frame_buf, 0 on error.
size_t nbtn_build_response(uint8_t* frame_buf, size_t buf_max,
                           uint16_t transaction_id,
                           const char* query_name,
                           uint32_t response_ip);
