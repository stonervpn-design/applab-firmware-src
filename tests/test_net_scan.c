#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../applications/main/lan_attack/net_scan.h"

// ============ Helpers ============

static uint16_t read_u16_be(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static uint32_t read_u32_be(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

// ============ ARP request byte-layout ============

static void test_arp_build_request(void) {
    printf("[TEST] ARP request byte layout\n");

    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t sender_ip = 0xC0A80102; // 192.168.1.2
    uint32_t target_ip = 0xC0A80101; // 192.168.1.1

    uint8_t buf[ARP_PACKET_LEN];
    memset(buf, 0xEE, sizeof(buf));
    size_t n = arp_build_request(buf, sizeof(buf), mac, sender_ip, target_ip);
    assert(n == ARP_PACKET_LEN);

    assert(read_u16_be(&buf[0]) == 0x0001);  // htype = Ethernet
    assert(read_u16_be(&buf[2]) == 0x0800);  // ptype = IPv4
    assert(buf[4] == 6);                     // hlen
    assert(buf[5] == 4);                     // plen
    assert(read_u16_be(&buf[6]) == 0x0001);  // oper = request

    assert(memcmp(&buf[8], mac, 6) == 0);          // sha
    assert(read_u32_be(&buf[14]) == sender_ip);    // spa
    // tha must be all-zero for a request
    for(int i = 0; i < 6; i++) assert(buf[18 + i] == 0);
    assert(read_u32_be(&buf[24]) == target_ip);    // tpa

    // buf-too-small must fail cleanly
    uint8_t small[10];
    assert(arp_build_request(small, sizeof(small), mac, sender_ip, target_ip) == 0);

    printf("  PASS: request opcode/sender/target fields correct\n");
}

// ============ ARP reply parse ============

static void test_arp_parse_reply(void) {
    printf("[TEST] ARP reply parse\n");

    // Craft a reply from 192.168.1.1 at aa:bb:cc:dd:ee:ff answering our request.
    uint8_t responder_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint32_t responder_ip = 0xC0A80101; // 192.168.1.1
    uint8_t our_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t our_ip = 0xC0A80102;

    uint8_t reply[ARP_PACKET_LEN];
    memset(reply, 0, sizeof(reply));
    reply[0] = 0x00; reply[1] = 0x01;          // htype
    reply[2] = 0x08; reply[3] = 0x00;          // ptype
    reply[4] = 6; reply[5] = 4;                // hlen/plen
    reply[6] = 0x00; reply[7] = 0x02;          // oper = reply
    memcpy(&reply[8], responder_mac, 6);       // sha
    reply[14] = 0xC0; reply[15] = 0xA8; reply[16] = 0x01; reply[17] = 0x01; // spa
    memcpy(&reply[18], our_mac, 6);            // tha
    reply[24] = 0xC0; reply[25] = 0xA8; reply[26] = 0x01; reply[27] = 0x02; // tpa

    uint8_t got_mac[6];
    uint32_t got_ip = 0;
    bool ok = arp_parse_reply(reply, sizeof(reply), got_mac, &got_ip);
    assert(ok);
    assert(memcmp(got_mac, responder_mac, 6) == 0);
    assert(got_ip == responder_ip);
    (void)our_ip;

    // A request (oper==1) must not parse as a reply.
    uint8_t req[ARP_PACKET_LEN];
    arp_build_request(req, sizeof(req), our_mac, our_ip, responder_ip);
    assert(arp_parse_reply(req, sizeof(req), got_mac, &got_ip) == false);

    // Too-short frame must fail.
    assert(arp_parse_reply(reply, 10, got_mac, &got_ip) == false);

    printf("  PASS: reply MAC=%02x:%02x:%02x:%02x:%02x:%02x IP=%08x extracted\n",
           got_mac[0], got_mac[1], got_mac[2], got_mac[3], got_mac[4], got_mac[5],
           got_ip);
}

// ============ TCP probe strings ============

static void test_tcp_probes(void) {
    printf("[TEST] TCP probe strings\n");

    uint8_t buf[64];

    size_t sn = tcp_build_ssh_probe(buf, sizeof(buf));
    assert(sn > 0 && sn < sizeof(buf));
    assert(memcmp(buf, "SSH-2.0-", 8) == 0);
    assert(buf[sn - 2] == '\r' && buf[sn - 1] == '\n');

    size_t hn = tcp_build_http_probe(buf, sizeof(buf));
    assert(hn > 0 && hn < sizeof(buf));
    assert(memcmp(buf, "GET / HTTP/1.0\r\n", 16) == 0);
    assert(buf[hn - 2] == '\r' && buf[hn - 1] == '\n');

    // Undersized buffers fail cleanly.
    uint8_t tiny[4];
    assert(tcp_build_ssh_probe(tiny, sizeof(tiny)) == 0);
    assert(tcp_build_http_probe(tiny, sizeof(tiny)) == 0);

    printf("  PASS: ssh(%zu) + http(%zu) probes correct\n", sn, hn);
}

int main(void) {
    test_arp_build_request();
    test_arp_parse_reply();
    test_tcp_probes();
    printf("PASS: all net_scan tests passed\n");
    return 0;
}
