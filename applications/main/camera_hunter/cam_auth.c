#include "cam_auth.h"
#include "../lan_attack/net_socket.h"

#include <furi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "CamAuth"

#define CAM_RESP_MAX 1024

// ============================================================================
//  MD5 (public domain, Brad Conte) — needed for HTTP/RTSP Digest auth.
// ============================================================================

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[4];
} MD5_CTX;

#define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define FF(a, b, c, d, m, s, t) \
    { (a) += MD5_F(b, c, d) + (m) + (t); (a) = (b) + ROTLEFT(a, s); }
#define GG(a, b, c, d, m, s, t) \
    { (a) += MD5_G(b, c, d) + (m) + (t); (a) = (b) + ROTLEFT(a, s); }
#define HH(a, b, c, d, m, s, t) \
    { (a) += MD5_H(b, c, d) + (m) + (t); (a) = (b) + ROTLEFT(a, s); }
#define II(a, b, c, d, m, s, t) \
    { (a) += MD5_I(b, c, d) + (m) + (t); (a) = (b) + ROTLEFT(a, s); }

static void md5_transform(MD5_CTX* ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, m[16], i, j;
    for(i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j]) + (data[j + 1] << 8) + (data[j + 2] << 16) + (data[j + 3] << 24);

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];

    FF(a, b, c, d, m[0], 7, 0xd76aa478);
    FF(d, a, b, c, m[1], 12, 0xe8c7b756);
    FF(c, d, a, b, m[2], 17, 0x242070db);
    FF(b, c, d, a, m[3], 22, 0xc1bdceee);
    FF(a, b, c, d, m[4], 7, 0xf57c0faf);
    FF(d, a, b, c, m[5], 12, 0x4787c62a);
    FF(c, d, a, b, m[6], 17, 0xa8304613);
    FF(b, c, d, a, m[7], 22, 0xfd469501);
    FF(a, b, c, d, m[8], 7, 0x698098d8);
    FF(d, a, b, c, m[9], 12, 0x8b44f7af);
    FF(c, d, a, b, m[10], 17, 0xffff5bb1);
    FF(b, c, d, a, m[11], 22, 0x895cd7be);
    FF(a, b, c, d, m[12], 7, 0x6b901122);
    FF(d, a, b, c, m[13], 12, 0xfd987193);
    FF(c, d, a, b, m[14], 17, 0xa679438e);
    FF(b, c, d, a, m[15], 22, 0x49b40821);

    GG(a, b, c, d, m[1], 5, 0xf61e2562);
    GG(d, a, b, c, m[6], 9, 0xc040b340);
    GG(c, d, a, b, m[11], 14, 0x265e5a51);
    GG(b, c, d, a, m[0], 20, 0xe9b6c7aa);
    GG(a, b, c, d, m[5], 5, 0xd62f105d);
    GG(d, a, b, c, m[10], 9, 0x02441453);
    GG(c, d, a, b, m[15], 14, 0xd8a1e681);
    GG(b, c, d, a, m[4], 20, 0xe7d3fbc8);
    GG(a, b, c, d, m[9], 5, 0x21e1cde6);
    GG(d, a, b, c, m[14], 9, 0xc33707d6);
    GG(c, d, a, b, m[3], 14, 0xf4d50d87);
    GG(b, c, d, a, m[8], 20, 0x455a14ed);
    GG(a, b, c, d, m[13], 5, 0xa9e3e905);
    GG(d, a, b, c, m[2], 9, 0xfcefa3f8);
    GG(c, d, a, b, m[7], 14, 0x676f02d9);
    GG(b, c, d, a, m[12], 20, 0x8d2a4c8a);

    HH(a, b, c, d, m[5], 4, 0xfffa3942);
    HH(d, a, b, c, m[8], 11, 0x8771f681);
    HH(c, d, a, b, m[11], 16, 0x6d9d6122);
    HH(b, c, d, a, m[14], 23, 0xfde5380c);
    HH(a, b, c, d, m[1], 4, 0xa4beea44);
    HH(d, a, b, c, m[4], 11, 0x4bdecfa9);
    HH(c, d, a, b, m[7], 16, 0xf6bb4b60);
    HH(b, c, d, a, m[10], 23, 0xbebfbc70);
    HH(a, b, c, d, m[13], 4, 0x289b7ec6);
    HH(d, a, b, c, m[0], 11, 0xeaa127fa);
    HH(c, d, a, b, m[3], 16, 0xd4ef3085);
    HH(b, c, d, a, m[6], 23, 0x04881d05);
    HH(a, b, c, d, m[9], 4, 0xd9d4d039);
    HH(d, a, b, c, m[12], 11, 0xe6db99e5);
    HH(c, d, a, b, m[15], 16, 0x1fa27cf8);
    HH(b, c, d, a, m[2], 23, 0xc4ac5665);

    II(a, b, c, d, m[0], 6, 0xf4292244);
    II(d, a, b, c, m[7], 10, 0x432aff97);
    II(c, d, a, b, m[14], 15, 0xab9423a7);
    II(b, c, d, a, m[5], 21, 0xfc93a039);
    II(a, b, c, d, m[12], 6, 0x655b59c3);
    II(d, a, b, c, m[3], 10, 0x8f0ccc92);
    II(c, d, a, b, m[10], 15, 0xffeff47d);
    II(b, c, d, a, m[1], 21, 0x85845dd1);
    II(a, b, c, d, m[8], 6, 0x6fa87e4f);
    II(d, a, b, c, m[15], 10, 0xfe2ce6e0);
    II(c, d, a, b, m[6], 15, 0xa3014314);
    II(b, c, d, a, m[13], 21, 0x4e0811a1);
    II(a, b, c, d, m[4], 6, 0xf7537e82);
    II(d, a, b, c, m[11], 10, 0xbd3af235);
    II(c, d, a, b, m[2], 15, 0x2ad7d2bb);
    II(b, c, d, a, m[9], 21, 0xeb86d391);

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md5_init(MD5_CTX* ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
}

static void md5_update(MD5_CTX* ctx, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if(ctx->datalen == 64) {
            md5_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void md5_final(MD5_CTX* ctx, uint8_t hash[16]) {
    uint32_t i = ctx->datalen;
    if(ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while(i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while(i < 64) ctx->data[i++] = 0x00;
        md5_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    ctx->data[56] = (uint8_t)(ctx->bitlen);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[63] = (uint8_t)(ctx->bitlen >> 56);
    md5_transform(ctx, ctx->data);
    for(i = 0; i < 4; ++i) {
        hash[i] = (uint8_t)((ctx->state[0] >> (i * 8)) & 0xff);
        hash[i + 4] = (uint8_t)((ctx->state[1] >> (i * 8)) & 0xff);
        hash[i + 8] = (uint8_t)((ctx->state[2] >> (i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (i * 8)) & 0xff);
    }
}

// MD5 of a NUL-terminated string -> 32-char lowercase hex (out must hold 33).
static void md5_hex(const char* s, char out[33]) {
    MD5_CTX ctx;
    uint8_t dig[16];
    md5_init(&ctx);
    md5_update(&ctx, (const uint8_t*)s, strlen(s));
    md5_final(&ctx, dig);
    static const char* hx = "0123456789abcdef";
    for(int i = 0; i < 16; i++) {
        out[i * 2] = hx[dig[i] >> 4];
        out[i * 2 + 1] = hx[dig[i] & 0xf];
    }
    out[32] = '\0';
}

// ============================================================================
//  base64 (encode only) — for HTTP/RTSP Basic auth.
// ============================================================================

static void b64_encode(const char* in, char* out, size_t out_cap) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in);
    size_t o = 0;
    for(size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint8_t)in[i] << 16;
        if(i + 1 < len) n |= (uint8_t)in[i + 1] << 8;
        if(i + 2 < len) n |= (uint8_t)in[i + 2];
        char c0 = tbl[(n >> 18) & 63];
        char c1 = tbl[(n >> 12) & 63];
        char c2 = (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        char c3 = (i + 2 < len) ? tbl[n & 63] : '=';
        if(o + 4 >= out_cap) break;
        out[o++] = c0;
        out[o++] = c1;
        out[o++] = c2;
        out[o++] = c3;
    }
    if(o < out_cap) out[o] = '\0';
    else if(out_cap) out[out_cap - 1] = '\0';
}

// ============================================================================
//  Small parse helpers
// ============================================================================

// Case-insensitive search for needle in haystack; returns pointer or NULL.
static const char* ci_find(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if(!nl) return hay;
    for(const char* p = hay; *p; p++) {
        size_t i = 0;
        while(i < nl && p[i] &&
              ((p[i] | 0x20) == (needle[i] | 0x20))) {
            i++;
        }
        if(i == nl) return p;
    }
    return NULL;
}

// Extract a header value: find "\r\n<name>" then copy up to next CRLF.
static void hdr_value(const char* buf, const char* name, char* out, size_t cap) {
    out[0] = '\0';
    char pat[40];
    snprintf(pat, sizeof(pat), "\r\n%s", name);
    const char* p = ci_find(buf, pat);
    if(!p) return;
    p += strlen(pat);
    while(*p == ' ' || *p == ':') p++;
    size_t i = 0;
    while(*p && *p != '\r' && *p != '\n' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
}

// Extract key="value" (or key=value up to , or space) from a challenge string.
static void dq_param(const char* src, const char* key, char* out, size_t cap) {
    out[0] = '\0';
    if(!src) return;
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = ci_find(src, pat);
    if(!p) return;
    p += strlen(pat);
    size_t i = 0;
    if(*p == '"') {
        p++;
        while(*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    } else {
        while(*p && *p != ',' && *p != ' ' && *p != '\r' && *p != '\n' && i < cap - 1)
            out[i++] = *p++;
    }
    out[i] = '\0';
}

// Parse the numeric status code from "HTTP/1.x NNN" or "RTSP/1.0 NNN".
static int status_code(const char* buf) {
    const char* sp = strchr(buf, ' ');
    if(!sp) return -1;
    return (int)strtol(sp + 1, NULL, 10);
}

// Send full request, read the response head (up to CAM_RESP_MAX-1) into resp.
// Returns bytes read (0 on failure). resp is always NUL-terminated.
static size_t
    http_txn(uint32_t ip, uint16_t port, const char* req, uint32_t timeout, char* resp) {
    resp[0] = '\0';
    NetSocket* sock = net_socket_tcp_connect(ip, port, timeout);
    if(!sock) return 0;
    if(!net_socket_tcp_send(sock, (const uint8_t*)req, strlen(req))) {
        net_socket_close(sock);
        return 0;
    }
    size_t total = 0;
    while(total < CAM_RESP_MAX - 1) {
        size_t got = 0;
        if(!net_socket_tcp_recv(
               sock, (uint8_t*)resp + total, CAM_RESP_MAX - 1 - total, &got, timeout))
            break;
        if(got == 0) break; // peer closed
        total += got;
        resp[total] = '\0';
        if(ci_find(resp, "\r\n\r\n")) break; // got the header block
    }
    net_socket_close(sock);
    resp[total] = '\0';
    return total;
}

// Build the Digest Authorization header value into out.
static void build_digest(
    const char* user,
    const char* pass,
    const char* realm,
    const char* nonce,
    const char* qop,
    const char* opaque,
    const char* method,
    const char* uri,
    char* out,
    size_t cap) {
    char ha1[33], ha2[33], resp[33], buf[256];
    char cnonce[17];
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)furi_get_tick(),
             (unsigned long)(furi_get_tick() * 2654435761u));

    snprintf(buf, sizeof(buf), "%s:%s:%s", user, realm, pass);
    md5_hex(buf, ha1);
    snprintf(buf, sizeof(buf), "%s:%s", method, uri);
    md5_hex(buf, ha2);

    if(qop && qop[0]) {
        snprintf(buf, sizeof(buf), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
        md5_hex(buf, resp);
        int n = snprintf(
            out,
            cap,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "algorithm=MD5, qop=auth, nc=00000001, cnonce=\"%s\", response=\"%s\"",
            user, realm, nonce, uri, cnonce, resp);
        if(opaque && opaque[0] && n > 0 && (size_t)n < cap)
            snprintf(out + n, cap - n, ", opaque=\"%s\"", opaque);
    } else {
        snprintf(buf, sizeof(buf), "%s:%s:%s", ha1, nonce, ha2);
        md5_hex(buf, resp);
        int n = snprintf(
            out,
            cap,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "algorithm=MD5, response=\"%s\"",
            user, realm, nonce, uri, resp);
        if(opaque && opaque[0] && n > 0 && (size_t)n < cap)
            snprintf(out + n, cap - n, ", opaque=\"%s\"", opaque);
    }
}

// ============================================================================
//  HTTP
// ============================================================================

bool cam_http_probe(uint32_t ip_host, uint16_t port, uint32_t timeout_ms, CamHttpInfo* info) {
    memset(info, 0, sizeof(*info));
    info->auth = CamAuthUnknown;

    char ips[16];
    snprintf(
        ips, sizeof(ips), "%u.%u.%u.%u", (unsigned)((ip_host >> 24) & 0xFF),
        (unsigned)((ip_host >> 16) & 0xFF), (unsigned)((ip_host >> 8) & 0xFF),
        (unsigned)(ip_host & 0xFF));

    char req[256];
    snprintf(
        req,
        sizeof(req),
        "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\nAccept: */*\r\n"
        "Connection: close\r\n\r\n",
        ips);

    char resp[CAM_RESP_MAX];
    if(!http_txn(ip_host, port, req, timeout_ms, resp)) return false;

    info->responded = true;
    hdr_value(resp, "Server:", info->server, sizeof(info->server));

    int code = status_code(resp);
    if(code >= 200 && code < 400) {
        info->auth = CamAuthNone;
        return true;
    }

    char www[256];
    hdr_value(resp, "WWW-Authenticate:", www, sizeof(www));
    if(ci_find(www, "Digest")) {
        info->auth = CamAuthDigest;
        dq_param(www, "realm", info->realm, sizeof(info->realm));
        dq_param(www, "nonce", info->nonce, sizeof(info->nonce));
        dq_param(www, "qop", info->qop, sizeof(info->qop));
        dq_param(www, "opaque", info->opaque, sizeof(info->opaque));
    } else if(ci_find(www, "Basic")) {
        info->auth = CamAuthBasic;
        dq_param(www, "realm", info->realm, sizeof(info->realm));
    }
    return true;
}

bool cam_http_try_cred(
    uint32_t ip_host,
    uint16_t port,
    const char* user,
    const char* pass,
    uint32_t timeout_ms) {
    // Fetch a fresh challenge (also gives us a current Digest nonce).
    CamHttpInfo info;
    if(!cam_http_probe(ip_host, port, timeout_ms, &info)) return false;
    if(info.auth == CamAuthNone) return true; // open, no creds required

    char ips[16];
    snprintf(
        ips, sizeof(ips), "%u.%u.%u.%u", (unsigned)((ip_host >> 24) & 0xFF),
        (unsigned)((ip_host >> 16) & 0xFF), (unsigned)((ip_host >> 8) & 0xFF),
        (unsigned)(ip_host & 0xFF));

    char authhdr[512];
    if(info.auth == CamAuthBasic) {
        char up[96], b64[160];
        snprintf(up, sizeof(up), "%s:%s", user, pass);
        b64_encode(up, b64, sizeof(b64));
        snprintf(authhdr, sizeof(authhdr), "Basic %s", b64);
    } else if(info.auth == CamAuthDigest) {
        build_digest(
            user, pass, info.realm, info.nonce, info.qop, info.opaque, "GET", "/", authhdr,
            sizeof(authhdr));
    } else {
        return false;
    }

    char req[768];
    snprintf(
        req,
        sizeof(req),
        "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\nAccept: */*\r\n"
        "Authorization: %s\r\nConnection: close\r\n\r\n",
        ips, authhdr);

    char resp[CAM_RESP_MAX];
    if(!http_txn(ip_host, port, req, timeout_ms, resp)) return false;
    int code = status_code(resp);
    return (code >= 200 && code < 400); // 401/403 = wrong creds
}

// ============================================================================
//  RTSP
// ============================================================================

// One DESCRIBE round trip; returns the RTSP status code and (on 401) the
// challenge in the provided buffers.
static int rtsp_describe(
    uint32_t ip,
    uint16_t port,
    const char* url,
    const char* authhdr,
    uint32_t timeout,
    char* www_out,
    size_t www_cap) {
    if(www_out) www_out[0] = '\0';
    char req[768];
    if(authhdr && authhdr[0]) {
        snprintf(
            req,
            sizeof(req),
            "DESCRIBE %s RTSP/1.0\r\nCSeq: 2\r\nUser-Agent: cam\r\nAccept: application/sdp\r\n"
            "Authorization: %s\r\n\r\n",
            url, authhdr);
    } else {
        snprintf(
            req,
            sizeof(req),
            "DESCRIBE %s RTSP/1.0\r\nCSeq: 2\r\nUser-Agent: cam\r\nAccept: application/sdp\r\n\r\n",
            url);
    }
    char resp[CAM_RESP_MAX];
    if(!http_txn(ip, port, req, timeout, resp)) return -1;
    if(www_out) hdr_value(resp, "WWW-Authenticate:", www_out, www_cap);
    return status_code(resp);
}

bool cam_rtsp_probe(uint32_t ip_host, uint16_t port, uint32_t timeout_ms) {
    char url[48];
    snprintf(
        url, sizeof(url), "rtsp://%u.%u.%u.%u:%u/", (unsigned)((ip_host >> 24) & 0xFF),
        (unsigned)((ip_host >> 16) & 0xFF), (unsigned)((ip_host >> 8) & 0xFF),
        (unsigned)(ip_host & 0xFF), (unsigned)port);
    char req[256];
    snprintf(req, sizeof(req), "OPTIONS %s RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: cam\r\n\r\n", url);
    char resp[CAM_RESP_MAX];
    if(!http_txn(ip_host, port, req, timeout_ms, resp)) return false;
    return ci_find(resp, "RTSP/1.0") != NULL;
}

bool cam_rtsp_try_cred(
    uint32_t ip_host,
    uint16_t port,
    const char* user,
    const char* pass,
    uint32_t timeout_ms,
    char* out_url,
    size_t out_cap) {
    char url[48];
    snprintf(
        url, sizeof(url), "rtsp://%u.%u.%u.%u:%u/", (unsigned)((ip_host >> 24) & 0xFF),
        (unsigned)((ip_host >> 16) & 0xFF), (unsigned)((ip_host >> 8) & 0xFF),
        (unsigned)(ip_host & 0xFF), (unsigned)port);

    char www[256];
    int code = rtsp_describe(ip_host, port, url, NULL, timeout_ms, www, sizeof(www));
    if(code < 0) return false;

    if(code >= 200 && code < 300) {
        // Stream is open with no auth.
        snprintf(out_url, out_cap, "%s", url);
        return true;
    }
    if(code != 401) return false;

    char authhdr[512];
    if(ci_find(www, "Digest")) {
        char realm[48], nonce[64], qop[16], opaque[48];
        dq_param(www, "realm", realm, sizeof(realm));
        dq_param(www, "nonce", nonce, sizeof(nonce));
        dq_param(www, "qop", qop, sizeof(qop));
        dq_param(www, "opaque", opaque, sizeof(opaque));
        build_digest(
            user, pass, realm, nonce, qop, opaque, "DESCRIBE", url, authhdr, sizeof(authhdr));
    } else if(ci_find(www, "Basic")) {
        char up[96], b64[160];
        snprintf(up, sizeof(up), "%s:%s", user, pass);
        b64_encode(up, b64, sizeof(b64));
        snprintf(authhdr, sizeof(authhdr), "Basic %s", b64);
    } else {
        return false;
    }

    code = rtsp_describe(ip_host, port, url, authhdr, timeout_ms, NULL, 0);
    if(code >= 200 && code < 300) {
        snprintf(
            out_url,
            out_cap,
            "rtsp://%s:%s@%u.%u.%u.%u:%u/",
            user,
            pass,
            (unsigned)((ip_host >> 24) & 0xFF),
            (unsigned)((ip_host >> 16) & 0xFF),
            (unsigned)((ip_host >> 8) & 0xFF),
            (unsigned)(ip_host & 0xFF),
            (unsigned)port);
        return true;
    }
    return false;
}
