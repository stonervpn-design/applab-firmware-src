#include "net_proto.h"

// ============ DHCP Implementation ============

// Write a 32-bit big-endian value
static inline void write_u32_be(uint8_t* buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

// Read a 32-bit big-endian value
static inline uint32_t read_u32_be(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

// Write a 16-bit big-endian value
static inline void write_u16_be(uint8_t* buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

// Read a 16-bit big-endian value
static inline uint16_t read_u16_be(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

// Append a DHCP option (TLV: type, length, value)
// Returns the offset after the option (or the same offset on error).
static size_t dhcp_append_option(uint8_t* options, size_t offset, size_t max_size,
                                  uint8_t type, const uint8_t* value, uint8_t len) {
    if (offset + 2 + len > max_size) {
        return offset; // not enough space
    }
    options[offset] = type;
    options[offset + 1] = len;
    if (len > 0 && value) {
        memcpy(&options[offset + 2], value, len);
    }
    return offset + 2 + len;
}

// Scan options section for a specific option type.
// Returns pointer to value (after type and length bytes), or NULL if not found.
static const uint8_t* dhcp_find_option(const uint8_t* options, size_t options_len,
                                        uint8_t option_type, uint8_t* out_len) {
    size_t i = 0;
    while (i < options_len) {
        uint8_t type = options[i];
        if (type == 255) break; // end of options
        if (type == 0) {         // padding
            i++;
            continue;
        }
        if (i + 1 >= options_len) break; // malformed
        uint8_t len = options[i + 1];
        if (i + 2 + len > options_len) break; // malformed
        if (type == option_type) {
            if (out_len) *out_len = len;
            return &options[i + 2];
        }
        i += 2 + len;
    }
    return NULL;
}

bool dhcp_parse(const uint8_t* frame, size_t len, DhcpParsed* out) {
    if (len < 236) {
        return false; // too short for DHCP header
    }

    // Clear output
    memset(out, 0, sizeof(*out));

    // Parse fixed header
    out->op = frame[0];
    if (out->op != 1 && out->op != 2) {
        return false; // invalid opcode
    }

    out->xid = read_u32_be(&frame[4]);
    memcpy(out->chaddr, &frame[28], 6);
    out->yiaddr = read_u32_be(&frame[16]);
    out->siaddr = read_u32_be(&frame[20]);
    out->giaddr = read_u32_be(&frame[24]);

    // Check for magic cookie (must be present for DHCP)
    if (len >= 240 && !(frame[236] == 0x63 && frame[237] == 0x82 &&
                        frame[238] == 0x53 && frame[239] == 0x63)) {
        return false;
    }

    // Default: no message type or requested IP
    out->msg_type = 0;
    out->requested_ip = 0;

    // Parse options if present
    if (len > 240) {
        const uint8_t* options = &frame[240];
        size_t options_len = len - 240;

        // Find message type (option 53)
        uint8_t opt_len = 0;
        const uint8_t* msg_type_opt = dhcp_find_option(options, options_len, 53, &opt_len);
        if (msg_type_opt && opt_len > 0) {
            out->msg_type = msg_type_opt[0];
        }

        // Find requested IP (option 50)
        opt_len = 0;
        const uint8_t* req_ip_opt = dhcp_find_option(options, options_len, 50, &opt_len);
        if (req_ip_opt && opt_len == 4) {
            out->requested_ip = read_u32_be(req_ip_opt);
        }
    }

    return true;
}

size_t dhcp_build_discover(uint8_t* frame_buf, size_t buf_max, const uint8_t client_mac[6]) {
    if (buf_max < 300) {
        return 0; // not enough space
    }

    // Initialize buffer
    memset(frame_buf, 0, 236);

    // DHCP header
    frame_buf[0] = 1;  // op = request
    frame_buf[1] = 1;  // htype = Ethernet
    frame_buf[2] = 6;  // hlen = 6 (MAC length)
    frame_buf[3] = 0;  // hops = 0

    // Random xid (in real code, would be random; for testing, use 0x12345678)
    write_u32_be(&frame_buf[4], 0x12345678);

    // secs = 0
    write_u16_be(&frame_buf[8], 0);

    // flags = 0x8000 (broadcast bit)
    write_u16_be(&frame_buf[10], 0x8000);

    // ciaddr, yiaddr, siaddr, giaddr all 0 (already cleared)

    // chaddr = client MAC
    memcpy(&frame_buf[28], client_mac, 6);

    // Magic cookie
    frame_buf[236] = 0x63;
    frame_buf[237] = 0x82;
    frame_buf[238] = 0x53;
    frame_buf[239] = 0x63;

    // Options section
    size_t opt_offset = 0;
    uint8_t options[60];
    memset(options, 0, sizeof(options));

    // Option 53: DHCP Message Type = 1 (DISCOVER)
    uint8_t msg_type = 1;
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 53, &msg_type, 1);

    // Option 55: Parameter Request List (request options 1, 3, 6)
    uint8_t param_list[3] = {1, 3, 6};
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 55, param_list, 3);

    // Option 255: End of Options
    if (opt_offset < sizeof(options)) {
        options[opt_offset] = 255;
        opt_offset++;
    }

    // Copy options to frame
    if (240 + opt_offset > buf_max) {
        return 0;
    }
    memcpy(&frame_buf[240], options, opt_offset);

    return 240 + opt_offset;
}

size_t dhcp_build_offer(uint8_t* frame_buf, size_t buf_max,
                        const uint8_t client_mac[6], uint32_t xid,
                        uint32_t offered_ip, uint32_t gateway_ip,
                        uint32_t dns_ip, uint16_t lease_sec) {
    if (buf_max < 300) {
        return 0;
    }

    // Initialize buffer
    memset(frame_buf, 0, 236);

    // DHCP header
    frame_buf[0] = 2;  // op = reply
    frame_buf[1] = 1;  // htype = Ethernet
    frame_buf[2] = 6;  // hlen = 6
    frame_buf[3] = 0;  // hops = 0

    write_u32_be(&frame_buf[4], xid);
    write_u16_be(&frame_buf[8], 0);      // secs = 0
    write_u16_be(&frame_buf[10], 0);     // flags = 0

    // yiaddr = offered IP
    write_u32_be(&frame_buf[16], offered_ip);

    // siaddr = server IP (could be gateway or 0, use offered_ip's network)
    // For simplicity, set to gateway
    write_u32_be(&frame_buf[20], gateway_ip);

    // giaddr = gateway/relay IP
    write_u32_be(&frame_buf[24], gateway_ip);

    // chaddr = client MAC
    memcpy(&frame_buf[28], client_mac, 6);

    // Magic cookie
    frame_buf[236] = 0x63;
    frame_buf[237] = 0x82;
    frame_buf[238] = 0x53;
    frame_buf[239] = 0x63;

    // Build options
    size_t opt_offset = 0;
    uint8_t options[60];
    memset(options, 0, sizeof(options));

    // Option 53: DHCP Message Type = 2 (OFFER)
    uint8_t msg_type = 2;
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 53, &msg_type, 1);

    // Option 1: Subnet Mask (assume /24 network)
    uint8_t subnet_mask[4] = {0xFF, 0xFF, 0xFF, 0x00};
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 1, subnet_mask, 4);

    // Option 3: Router/Gateway
    uint8_t gw_bytes[4];
    write_u32_be(gw_bytes, gateway_ip);
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 3, gw_bytes, 4);

    // Option 6: DNS Server
    uint8_t dns_bytes[4];
    write_u32_be(dns_bytes, dns_ip);
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 6, dns_bytes, 4);

    // Option 51: Lease Time (in seconds, big-endian)
    uint8_t lease_bytes[4];
    write_u32_be(lease_bytes, lease_sec);
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 51, lease_bytes, 4);

    // Option 255: End of Options
    if (opt_offset < sizeof(options)) {
        options[opt_offset] = 255;
        opt_offset++;
    }

    if (240 + opt_offset > buf_max) {
        return 0;
    }
    memcpy(&frame_buf[240], options, opt_offset);

    return 240 + opt_offset;
}

size_t dhcp_build_ack(uint8_t* frame_buf, size_t buf_max,
                      const uint8_t client_mac[6], uint32_t xid,
                      uint32_t assigned_ip, uint32_t gateway_ip,
                      uint32_t dns_ip, uint16_t lease_sec) {
    if (buf_max < 300) {
        return 0;
    }

    // Initialize buffer
    memset(frame_buf, 0, 236);

    // DHCP header
    frame_buf[0] = 2;  // op = reply
    frame_buf[1] = 1;  // htype = Ethernet
    frame_buf[2] = 6;  // hlen = 6
    frame_buf[3] = 0;  // hops = 0

    write_u32_be(&frame_buf[4], xid);
    write_u16_be(&frame_buf[8], 0);
    write_u16_be(&frame_buf[10], 0);

    // yiaddr = assigned IP
    write_u32_be(&frame_buf[16], assigned_ip);

    // siaddr = server IP
    write_u32_be(&frame_buf[20], gateway_ip);

    // giaddr = gateway
    write_u32_be(&frame_buf[24], gateway_ip);

    // chaddr = client MAC
    memcpy(&frame_buf[28], client_mac, 6);

    // Magic cookie
    frame_buf[236] = 0x63;
    frame_buf[237] = 0x82;
    frame_buf[238] = 0x53;
    frame_buf[239] = 0x63;

    // Build options
    size_t opt_offset = 0;
    uint8_t options[60];
    memset(options, 0, sizeof(options));

    // Option 53: DHCP Message Type = 5 (ACK)
    uint8_t msg_type = 5;
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 53, &msg_type, 1);

    // Option 1: Subnet Mask
    uint8_t subnet_mask[4] = {0xFF, 0xFF, 0xFF, 0x00};
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 1, subnet_mask, 4);

    // Option 3: Router/Gateway
    uint8_t gw_bytes[4];
    write_u32_be(gw_bytes, gateway_ip);
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 3, gw_bytes, 4);

    // Option 6: DNS Server
    uint8_t dns_bytes[4];
    write_u32_be(dns_bytes, dns_ip);
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 6, dns_bytes, 4);

    // Option 51: Lease Time
    uint8_t lease_bytes[4];
    write_u32_be(lease_bytes, lease_sec);
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 51, lease_bytes, 4);

    // Option 255: End of Options
    if (opt_offset < sizeof(options)) {
        options[opt_offset] = 255;
        opt_offset++;
    }

    if (240 + opt_offset > buf_max) {
        return 0;
    }
    memcpy(&frame_buf[240], options, opt_offset);

    return 240 + opt_offset;
}

size_t dhcp_build_nak(uint8_t* frame_buf, size_t buf_max,
                      const uint8_t client_mac[6], uint32_t xid) {
    if (buf_max < 300) {
        return 0;
    }

    memset(frame_buf, 0, 236);

    frame_buf[0] = 2;  // op = reply
    frame_buf[1] = 1;  // htype = Ethernet
    frame_buf[2] = 6;  // hlen = 6
    frame_buf[3] = 0;  // hops = 0

    write_u32_be(&frame_buf[4], xid);
    write_u16_be(&frame_buf[8], 0);
    write_u16_be(&frame_buf[10], 0);

    memcpy(&frame_buf[28], client_mac, 6);

    // Magic cookie
    frame_buf[236] = 0x63;
    frame_buf[237] = 0x82;
    frame_buf[238] = 0x53;
    frame_buf[239] = 0x63;

    // Build options
    size_t opt_offset = 0;
    uint8_t options[60];
    memset(options, 0, sizeof(options));

    // Option 53: DHCP Message Type = 6 (NAK)
    uint8_t msg_type = 6;
    opt_offset = dhcp_append_option(options, opt_offset, sizeof(options), 53, &msg_type, 1);

    // Option 255: End of Options
    if (opt_offset < sizeof(options)) {
        options[opt_offset] = 255;
        opt_offset++;
    }

    if (240 + opt_offset > buf_max) {
        return 0;
    }
    memcpy(&frame_buf[240], options, opt_offset);

    return 240 + opt_offset;
}

// ============ LLMNR Implementation ============

// Encode a DNS-style name label sequence
// Input: "example.local" -> output encoded as: \x07example\x05local\x00
static size_t llmnr_encode_name(const char* name, uint8_t* buf, size_t max_size) {
    if (!name || max_size < 2) return 0;

    size_t pos = 0;
    size_t label_start = 0;

    while (name[label_start] && pos < max_size) {
        // Find next dot or end
        size_t label_end = label_start;
        while (name[label_end] && name[label_end] != '.') {
            label_end++;
        }

        size_t label_len = label_end - label_start;
        if (label_len == 0 || label_len > 63) {
            return 0; // invalid label
        }

        if (pos + 1 + label_len > max_size) {
            return 0; // buffer overflow
        }

        // Write length byte
        buf[pos] = (uint8_t)label_len;
        pos++;

        // Write label
        memcpy(&buf[pos], &name[label_start], label_len);
        pos += label_len;

        // Move to next label
        if (name[label_end] == '.') {
            label_start = label_end + 1;
        } else {
            label_start = label_end;
            break;
        }
    }

    // Write root label (null terminator)
    if (pos < max_size) {
        buf[pos] = 0;
        pos++;
    }

    return pos;
}

// Decode a DNS-style name label sequence to a string
static size_t llmnr_decode_name(const uint8_t* buf, size_t buf_len, char* name_out, size_t max_name_len) {
    if (!buf || !name_out || max_name_len < 1) return 0;

    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < buf_len && out_pos < max_name_len) {
        uint8_t len = buf[in_pos];
        in_pos++;

        if (len == 0) {
            // End of name
            break;
        }

        if (len > 63 || in_pos + len > buf_len) {
            // Invalid label
            return 0;
        }

        // Add dot if not first label
        if (out_pos > 0 && out_pos < max_name_len) {
            name_out[out_pos] = '.';
            out_pos++;
        }

        // Copy label
        if (out_pos + len > max_name_len) {
            return 0; // buffer overflow
        }

        memcpy(&name_out[out_pos], &buf[in_pos], len);
        out_pos += len;
        in_pos += len;
    }

    // Null terminate
    if (out_pos < max_name_len) {
        name_out[out_pos] = 0;
    }

    return out_pos;
}

bool llmnr_parse_query(const uint8_t* frame, size_t len, char* query_name, size_t max_name_len) {
    if (len < 12) {
        return false; // LLMNR header is 12 bytes minimum
    }

    // LLMNR header:
    // 0-1: Transaction ID
    // 2-3: Flags
    // 4-5: Questions
    // 6-7: Answer RRs
    // 8-9: Authority RRs
    // 10-11: Additional RRs

    uint16_t questions = read_u16_be(&frame[4]);
    if (questions != 1) {
        return false; // expect exactly one question
    }

    // Question section starts at byte 12
    // Format: encoded name, then 2-byte type, 2-byte class

    if (len < 12 + 6) {
        return false; // need at least space for name (1+) + type (2) + class (2)
    }

    size_t name_end = 12;
    // Find end of name (null label)
    while (name_end < len && frame[name_end] != 0) {
        uint8_t label_len = frame[name_end];
        if (label_len > 63) {
            return false; // invalid
        }
        name_end += 1 + label_len;
        if (name_end > len) {
            return false;
        }
    }

    if (name_end >= len) {
        return false;
    }

    // name_end points to the 0 byte; skip it
    name_end++;

    // Decode the name
    if (llmnr_decode_name(&frame[12], name_end - 12, query_name, max_name_len) == 0) {
        return false;
    }

    return true;
}

size_t llmnr_build_response(uint8_t* frame_buf, size_t buf_max,
                            uint16_t transaction_id,
                            const char* query_name,
                            uint32_t response_ip) {
    if (buf_max < 50) {
        return 0; // too small
    }

    // LLMNR header (12 bytes)
    write_u16_be(&frame_buf[0], transaction_id);
    write_u16_be(&frame_buf[2], 0x8000); // flags: response, no error
    write_u16_be(&frame_buf[4], 1);      // 1 question
    write_u16_be(&frame_buf[6], 1);      // 1 answer RR
    write_u16_be(&frame_buf[8], 0);      // 0 authority RRs
    write_u16_be(&frame_buf[10], 0);     // 0 additional RRs

    // Encode the question name
    size_t encoded_len = llmnr_encode_name(query_name, &frame_buf[12], buf_max - 12);
    if (encoded_len == 0) {
        return 0;
    }

    size_t pos = 12 + encoded_len;

    // Question type (A record = 1) and class (IN = 1)
    if (pos + 4 > buf_max) return 0;
    write_u16_be(&frame_buf[pos], 1);     // type A
    write_u16_be(&frame_buf[pos + 2], 1); // class IN
    pos += 4;

    // Answer RR
    // Name (pointer or full name) - we'll use the same encoded name
    encoded_len = llmnr_encode_name(query_name, &frame_buf[pos], buf_max - pos);
    if (encoded_len == 0) {
        return 0;
    }
    pos += encoded_len;

    // Type (A = 1), Class (IN = 1), TTL (4 bytes), RDLength (2 bytes), RDATA
    if (pos + 10 > buf_max) return 0;

    write_u16_be(&frame_buf[pos], 1);     // type A
    pos += 2;
    write_u16_be(&frame_buf[pos], 1);     // class IN
    pos += 2;
    write_u32_be(&frame_buf[pos], 120);   // TTL = 120 seconds
    pos += 4;
    write_u16_be(&frame_buf[pos], 4);     // RDLength = 4 (IPv4 address)
    pos += 2;

    // RDATA: IPv4 address
    if (pos + 4 > buf_max) return 0;
    write_u32_be(&frame_buf[pos], response_ip);
    pos += 4;

    return pos;
}

// ============ NBT-NS Implementation ============

// Encode a NetBIOS name (half-ASCII encoding)
// Each byte is encoded as two ASCII bytes (high nibble + 0x41, low nibble + 0x41)
static size_t nbtn_encode_name(const char* name, uint8_t* buf, size_t max_size) {
    if (!name || max_size < 34) return 0; // 16 bytes * 2 = 32, plus length + null

    // For NetBIOS, pad name to 16 bytes with spaces
    uint8_t nbname[16];
    memset(nbname, ' ', 16);

    // Copy name (max 15 bytes, as 16th is name type)
    size_t i = 0;
    while (i < 15 && name[i]) {
        nbname[i] = (uint8_t)name[i];
        i++;
    }
    // nbname[15] is the name type byte (we'll use 0x00 for workstation)
    nbname[15] = 0x00;

    // Encode with length prefix
    size_t pos = 0;
    buf[pos] = 32; // 16 bytes * 2 = 32
    pos++;

    // Encode each byte
    for (int j = 0; j < 16; j++) {
        uint8_t byte = nbname[j];
        buf[pos] = ((byte >> 4) & 0x0F) + 0x41; // high nibble
        pos++;
        buf[pos] = (byte & 0x0F) + 0x41;        // low nibble
        pos++;
    }

    // Root label
    buf[pos] = 0;
    pos++;

    return pos;
}

// Decode a NetBIOS name (half-ASCII decoding)
static size_t nbtn_decode_name(const uint8_t* buf, size_t buf_len, char* name_out, size_t max_name_len) {
    if (!buf || !name_out || buf_len < 33) return 0;

    if (buf[0] != 32) return 0; // expect 32 bytes encoded = 16 bytes raw

    // Decode pairs
    uint8_t nbname[16];
    size_t in_pos = 1;

    for (int i = 0; i < 16; i++) {
        if (in_pos + 1 >= buf_len) return 0;

        uint8_t high = buf[in_pos] - 0x41;
        uint8_t low = buf[in_pos + 1] - 0x41;

        if (high > 15 || low > 15) return 0; // invalid encoding

        nbname[i] = (high << 4) | low;
        in_pos += 2;
    }

    // The 16th byte is name type; first 15 are the name
    // Copy to output, stripping trailing spaces
    size_t len = 15;
    while (len > 0 && nbname[len - 1] == ' ') {
        len--;
    }

    if (len >= max_name_len) {
        len = max_name_len - 1;
    }

    memcpy(name_out, nbname, len);
    name_out[len] = 0;

    return len;
}

bool nbtn_parse_query(const uint8_t* frame, size_t len, char* query_name, size_t max_name_len) {
    if (len < 12) {
        return false; // NBT-NS header is 12 bytes
    }

    // Header:
    // 0-1: Transaction ID
    // 2-3: Flags
    // 4-5: Questions
    // 6-7: Answer RRs
    // 8-9: Authority RRs
    // 10-11: Additional RRs

    uint16_t questions = read_u16_be(&frame[4]);
    if (questions != 1) {
        return false;
    }

    // Question section starts at byte 12
    if (len < 12 + 34) {
        return false; // need at least 34 bytes for NBT name
    }

    // Decode the name
    if (nbtn_decode_name(&frame[12], len - 12, query_name, max_name_len) == 0) {
        return false;
    }

    return true;
}

size_t nbtn_build_response(uint8_t* frame_buf, size_t buf_max,
                           uint16_t transaction_id,
                           const char* query_name,
                           uint32_t response_ip) {
    if (buf_max < 100) {
        return 0;
    }

    // NBT-NS header
    write_u16_be(&frame_buf[0], transaction_id);
    write_u16_be(&frame_buf[2], 0x8500); // flags: response, no error, authoritative
    write_u16_be(&frame_buf[4], 1);      // 1 question
    write_u16_be(&frame_buf[6], 1);      // 1 answer RR
    write_u16_be(&frame_buf[8], 0);      // 0 authority RRs
    write_u16_be(&frame_buf[10], 0);     // 0 additional RRs

    // Encode the name in question
    size_t encoded_len = nbtn_encode_name(query_name, &frame_buf[12], buf_max - 12);
    if (encoded_len == 0) {
        return 0;
    }

    size_t pos = 12 + encoded_len;

    // Question type and class
    if (pos + 4 > buf_max) return 0;
    write_u16_be(&frame_buf[pos], 0x0020); // type NB (NetBIOS name)
    write_u16_be(&frame_buf[pos + 2], 0x0001); // class IN
    pos += 4;

    // Answer RR
    // Name (use pointer or full name)
    encoded_len = nbtn_encode_name(query_name, &frame_buf[pos], buf_max - pos);
    if (encoded_len == 0) {
        return 0;
    }
    pos += encoded_len;

    // Type, Class, TTL, RDLength
    if (pos + 10 > buf_max) return 0;
    write_u16_be(&frame_buf[pos], 0x0020); // type NB
    pos += 2;
    write_u16_be(&frame_buf[pos], 0x0001); // class IN
    pos += 2;
    write_u32_be(&frame_buf[pos], 120);    // TTL
    pos += 4;
    write_u16_be(&frame_buf[pos], 6);      // RDLength: 2 (flags) + 4 (address)
    pos += 2;

    // RDATA: NB address (flags + IPv4)
    if (pos + 6 > buf_max) return 0;
    write_u16_be(&frame_buf[pos], 0x0000); // flags: unique, active
    pos += 2;
    write_u32_be(&frame_buf[pos], response_ip);
    pos += 4;

    return pos;
}
