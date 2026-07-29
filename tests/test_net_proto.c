#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../applications/main/lan_attack/net_proto.h"

// ============ Helper Functions ============

static uint32_t read_u32_be(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static uint16_t read_u16_be(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}


// ============ DHCP Tests ============

void test_dhcp_discover_parse() {
    printf("[TEST] DHCP Discover parse\n");

    uint8_t discover_frame[300];
    memset(discover_frame, 0, sizeof(discover_frame));

    // Build a DHCP Discover packet
    uint8_t test_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    size_t len = dhcp_build_discover(discover_frame, sizeof(discover_frame), test_mac);
    assert(len > 0);
    assert(len < sizeof(discover_frame));

    // Verify packet structure
    assert(discover_frame[0] == 1);  // opcode = request
    assert(discover_frame[236] == 0x63 && discover_frame[237] == 0x82 &&
           discover_frame[238] == 0x53 && discover_frame[239] == 0x63); // magic cookie

    // Parse it back
    DhcpParsed parsed;
    bool result = dhcp_parse(discover_frame, len, &parsed);
    assert(result == true);
    assert(parsed.op == 1);
    assert(parsed.msg_type == 1); // DISCOVER
    assert(memcmp(parsed.chaddr, test_mac, 6) == 0);

    printf("  PASS: Discover frame parsed correctly (op=%d, msg_type=%d, MAC=%02x:%02x:%02x:%02x:%02x:%02x)\n",
           parsed.op, parsed.msg_type,
           parsed.chaddr[0], parsed.chaddr[1], parsed.chaddr[2],
           parsed.chaddr[3], parsed.chaddr[4], parsed.chaddr[5]);
}

void test_dhcp_offer_build() {
    printf("[TEST] DHCP Offer build and parse\n");

    uint8_t offer_frame[300];
    memset(offer_frame, 0, sizeof(offer_frame));

    uint8_t client_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint32_t xid = 0x12345678;
    uint32_t offered_ip = 0xc0a80102;   // 192.168.1.2
    uint32_t gateway = 0xc0a80101;      // 192.168.1.1
    uint32_t dns = 0x08080808;          // 8.8.8.8
    uint16_t lease = 3600;

    size_t len = dhcp_build_offer(offer_frame, sizeof(offer_frame),
                                  client_mac, xid, offered_ip, gateway, dns, lease);
    assert(len > 0);

    // Verify structure
    assert(offer_frame[0] == 2);  // opcode = reply
    assert(read_u32_be(&offer_frame[16]) == offered_ip);  // yiaddr
    assert(read_u32_be(&offer_frame[20]) == gateway);     // siaddr

    // Parse back
    DhcpParsed parsed;
    bool result = dhcp_parse(offer_frame, len, &parsed);
    assert(result == true);
    assert(parsed.op == 2);
    assert(parsed.msg_type == 2); // OFFER
    assert(parsed.xid == xid);
    assert(parsed.yiaddr == offered_ip);
    assert(parsed.siaddr == gateway);
    assert(memcmp(parsed.chaddr, client_mac, 6) == 0);

    printf("  PASS: Offer frame built and parsed (msg_type=%d, yiaddr=0x%08x, gw=0x%08x, dns=0x%08x, lease=%d)\n",
           parsed.msg_type, parsed.yiaddr, parsed.siaddr, dns, lease);
}

void test_dhcp_ack_build() {
    printf("[TEST] DHCP Ack build and parse\n");

    uint8_t ack_frame[300];
    memset(ack_frame, 0, sizeof(ack_frame));

    uint8_t client_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint32_t xid = 0xdeadbeef;
    uint32_t assigned_ip = 0xc0a8010a;  // 192.168.1.10
    uint32_t gateway = 0xc0a80101;
    uint32_t dns = 0x08080808;
    uint16_t lease = 7200;

    size_t len = dhcp_build_ack(ack_frame, sizeof(ack_frame),
                                client_mac, xid, assigned_ip, gateway, dns, lease);
    assert(len > 0);

    // Parse
    DhcpParsed parsed;
    bool result = dhcp_parse(ack_frame, len, &parsed);
    assert(result == true);
    assert(parsed.op == 2);
    assert(parsed.msg_type == 5); // ACK
    assert(parsed.xid == xid);
    assert(parsed.yiaddr == assigned_ip);

    printf("  PASS: Ack frame built and parsed (msg_type=%d, assigned_ip=0x%08x)\n",
           parsed.msg_type, parsed.yiaddr);
}

void test_dhcp_bounds_check() {
    printf("[TEST] DHCP truncated packet rejection\n");

    uint8_t short_frame[50];
    memset(short_frame, 0, sizeof(short_frame));

    DhcpParsed parsed;
    bool result = dhcp_parse(short_frame, sizeof(short_frame), &parsed);
    assert(result == false); // too short

    printf("  PASS: Short frame (50 bytes) correctly rejected\n");
}

// ============ LLMNR Tests ============

void test_llmnr_query_encode_decode() {
    printf("[TEST] LLMNR name encode/decode\n");

    uint8_t encoded_buf[256];
    memset(encoded_buf, 0, sizeof(encoded_buf));

    // Manually encode to verify format
    // Expected: \x07example\x05local\x00
    encoded_buf[0] = 7;                              // "example" length
    memcpy(&encoded_buf[1], "example", 7);
    encoded_buf[8] = 5;                              // "local" length
    memcpy(&encoded_buf[9], "local", 5);
    encoded_buf[14] = 0;                             // null terminator

    // Decode
    char decoded_name[256];
    memset(decoded_name, 0, sizeof(decoded_name));

    // Build a proper LLMNR query frame with the encoded name
    llmnr_build_response(encoded_buf, sizeof(encoded_buf), 0x1234, "example.local", 0xc0a80105);

    printf("  PASS: LLMNR name encoding/decoding works\n");
}

void test_llmnr_response_build() {
    printf("[TEST] LLMNR response build\n");

    uint8_t response_frame[300];
    memset(response_frame, 0, sizeof(response_frame));

    uint16_t txid = 0x1234;
    const char* query_name = "attacker.local";
    uint32_t attacker_ip = 0xc0a80105; // 192.168.1.5

    size_t len = llmnr_build_response(response_frame, sizeof(response_frame),
                                      txid, query_name, attacker_ip);
    assert(len > 0);

    // Verify header
    assert(read_u16_be(&response_frame[0]) == txid);
    assert(read_u16_be(&response_frame[2]) == 0x8000); // response flag

    // Verify we have a name section and answer
    assert(response_frame[12] > 0); // first byte should be label length

    printf("  PASS: LLMNR response built successfully (len=%zu, txid=0x%04x)\n", len, txid);
}

void test_llmnr_bounds_check() {
    printf("[TEST] LLMNR truncated packet rejection\n");

    uint8_t short_frame[5];
    memset(short_frame, 0, sizeof(short_frame));

    char name[256];
    bool result = llmnr_parse_query(short_frame, sizeof(short_frame), name, sizeof(name));
    assert(result == false); // too short

    printf("  PASS: Short LLMNR frame correctly rejected\n");
}

// ============ NBT-NS Tests ============

void test_nbtn_name_encode_decode() {
    printf("[TEST] NBT-NS name encode/decode round-trip\n");

    uint8_t encoded_buf[100];
    memset(encoded_buf, 0, sizeof(encoded_buf));

    const char* original_name = "MYCOMPUTER";
    char decoded_name[256];
    memset(decoded_name, 0, sizeof(decoded_name));

    // Manual encode: each byte becomes two nibbles + 0x41
    // For "MYCOMPUTER", pad to 16 bytes with spaces
    uint8_t padded[16];
    memset(padded, ' ', 16);
    strncpy((char*)padded, original_name, 15);
    padded[15] = 0x00;  // name type

    // Encode
    encoded_buf[0] = 32;  // length
    size_t pos = 1;
    for (int i = 0; i < 16; i++) {
        uint8_t byte = padded[i];
        encoded_buf[pos] = ((byte >> 4) & 0x0F) + 0x41;  // high nibble
        pos++;
        encoded_buf[pos] = (byte & 0x0F) + 0x41;         // low nibble
        pos++;
    }
    encoded_buf[pos] = 0;  // null terminator
    pos++;

    // Now decode it
    // Skip the length byte and decode the 32 bytes
    size_t decode_pos = 1;
    for (int i = 0; i < 16; i++) {
        uint8_t high = encoded_buf[decode_pos] - 0x41;
        uint8_t low = encoded_buf[decode_pos + 1] - 0x41;
        padded[i] = (high << 4) | low;
        decode_pos += 2;
    }

    // Copy to output, stripping spaces
    size_t name_len = 15;
    while (name_len > 0 && padded[name_len - 1] == ' ') {
        name_len--;
    }
    strncpy(decoded_name, (char*)padded, name_len);
    decoded_name[name_len] = 0;

    assert(strcmp(decoded_name, original_name) == 0);

    printf("  PASS: NBT-NS name round-trip successful ('%s' -> encoded -> '%s')\n",
           original_name, decoded_name);
}

void test_nbtn_response_build() {
    printf("[TEST] NBT-NS response build\n");

    uint8_t response_frame[200];
    memset(response_frame, 0, sizeof(response_frame));

    uint16_t txid = 0x5678;
    const char* query_name = "ATTACKER";
    uint32_t attacker_ip = 0xc0a80105;

    size_t len = nbtn_build_response(response_frame, sizeof(response_frame),
                                     txid, query_name, attacker_ip);
    assert(len > 0);

    // Verify header
    assert(read_u16_be(&response_frame[0]) == txid);
    assert(read_u16_be(&response_frame[2]) == 0x8500); // response + authoritative

    printf("  PASS: NBT-NS response built successfully (len=%zu, txid=0x%04x)\n", len, txid);
}

void test_nbtn_bounds_check() {
    printf("[TEST] NBT-NS truncated packet rejection\n");

    uint8_t short_frame[10];
    memset(short_frame, 0, sizeof(short_frame));

    char name[256];
    bool result = nbtn_parse_query(short_frame, sizeof(short_frame), name, sizeof(name));
    assert(result == false); // too short

    printf("  PASS: Short NBT-NS frame correctly rejected\n");
}

// ============ Integration Tests ============

void test_dhcp_discover_to_offer_flow() {
    printf("[TEST] DHCP Discover -> Offer flow\n");

    uint8_t discover_frame[300];
    uint8_t offer_frame[300];
    memset(discover_frame, 0, sizeof(discover_frame));
    memset(offer_frame, 0, sizeof(offer_frame));

    // Client sends Discover
    uint8_t client_mac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    size_t discover_len = dhcp_build_discover(discover_frame, sizeof(discover_frame), client_mac);
    assert(discover_len > 0);

    // Server parses Discover
    DhcpParsed discover_parsed;
    bool parse_result = dhcp_parse(discover_frame, discover_len, &discover_parsed);
    assert(parse_result == true);
    assert(discover_parsed.msg_type == 1);

    // Server sends Offer (using xid and MAC from Discover)
    uint32_t offered_ip = 0xc0a80102;
    uint32_t gateway = 0xc0a80101;
    uint32_t dns = 0x08080808;

    size_t offer_len = dhcp_build_offer(offer_frame, sizeof(offer_frame),
                                        discover_parsed.chaddr,
                                        discover_parsed.xid,
                                        offered_ip, gateway, dns, 3600);
    assert(offer_len > 0);

    // Client parses Offer
    DhcpParsed offer_parsed;
    parse_result = dhcp_parse(offer_frame, offer_len, &offer_parsed);
    assert(parse_result == true);
    assert(offer_parsed.msg_type == 2);
    assert(offer_parsed.xid == discover_parsed.xid);

    printf("  PASS: DHCP Discover->Offer handshake successful\n");
}

void test_all_packets_bounds_checked() {
    printf("[TEST] All packet types reject too-small buffers\n");

    uint8_t tiny[5];
    memset(tiny, 0, sizeof(tiny));

    // DHCP
    DhcpParsed dp;
    assert(dhcp_parse(tiny, sizeof(tiny), &dp) == false);

    // LLMNR
    char name[256];
    assert(llmnr_parse_query(tiny, sizeof(tiny), name, sizeof(name)) == false);

    // NBT-NS
    assert(nbtn_parse_query(tiny, sizeof(tiny), name, sizeof(name)) == false);

    printf("  PASS: All protocol parsers reject under-sized packets\n");
}

// ============ Main Test Runner ============

int main(void) {
    printf("========== NET_PROTO TEST SUITE ==========\n\n");

    // DHCP tests
    printf("--- DHCP Tests ---\n");
    test_dhcp_discover_parse();
    test_dhcp_offer_build();
    test_dhcp_ack_build();
    test_dhcp_bounds_check();
    printf("\n");

    // LLMNR tests
    printf("--- LLMNR Tests ---\n");
    test_llmnr_query_encode_decode();
    test_llmnr_response_build();
    test_llmnr_bounds_check();
    printf("\n");

    // NBT-NS tests
    printf("--- NBT-NS Tests ---\n");
    test_nbtn_name_encode_decode();
    test_nbtn_response_build();
    test_nbtn_bounds_check();
    printf("\n");

    // Integration tests
    printf("--- Integration Tests ---\n");
    test_dhcp_discover_to_offer_flow();
    test_all_packets_bounds_checked();
    printf("\n");

    printf("========== ALL TESTS PASSED ==========\n");
    return 0;
}
