#include "net_scan.h"

// ---- little helpers ---------------------------------------------------------

static void write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// ---- ARP --------------------------------------------------------------------

size_t arp_build_request(
    uint8_t* buf,
    size_t buf_max,
    const uint8_t sender_mac[6],
    uint32_t sender_ip,
    uint32_t target_ip) {
    if(!buf || !sender_mac || buf_max < ARP_PACKET_LEN) return 0;

    write_u16_be(&buf[0], 0x0001);  // htype: Ethernet
    write_u16_be(&buf[2], 0x0800);  // ptype: IPv4
    buf[4] = 6;                     // hlen
    buf[5] = 4;                     // plen
    write_u16_be(&buf[6], 0x0001);  // oper: request

    memcpy(&buf[8], sender_mac, 6); // sha
    write_u32_be(&buf[14], sender_ip); // spa

    memset(&buf[18], 0, 6);         // tha: unknown -> zero
    write_u32_be(&buf[24], target_ip); // tpa

    return ARP_PACKET_LEN;
}

bool arp_parse_reply(const uint8_t* frame, size_t len, uint8_t out_mac[6], uint32_t* out_ip) {
    if(!frame || len < ARP_PACKET_LEN) return false;

    if(read_u16_be(&frame[0]) != 0x0001) return false; // htype Ethernet
    if(read_u16_be(&frame[2]) != 0x0800) return false; // ptype IPv4
    if(frame[4] != 6 || frame[5] != 4) return false;   // hlen/plen
    if(read_u16_be(&frame[6]) != 0x0002) return false; // oper reply

    if(out_mac) memcpy(out_mac, &frame[8], 6);          // sha
    if(out_ip) *out_ip = read_u32_be(&frame[14]);       // spa
    return true;
}

// ---- TCP probes -------------------------------------------------------------

size_t tcp_build_ssh_probe(uint8_t* buf, size_t buf_max) {
    static const char probe[] = "SSH-2.0-FZScan\r\n";
    size_t n = sizeof(probe) - 1; // exclude terminator
    if(!buf || buf_max < n) return 0;
    memcpy(buf, probe, n);
    return n;
}

size_t tcp_build_http_probe(uint8_t* buf, size_t buf_max) {
    static const char probe[] = "GET / HTTP/1.0\r\n\r\n";
    size_t n = sizeof(probe) - 1;
    if(!buf || buf_max < n) return 0;
    memcpy(buf, probe, n);
    return n;
}
