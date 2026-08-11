#pragma once

// cam_auth — IP-camera fingerprint + default-credential testing over HTTP and
// RTSP. Self-contained: vendors its own MD5 + base64 so it needs nothing beyond
// net_socket (lwIP TCP). All calls are blocking and honor a millisecond timeout;
// every function returns false / a null result on error and never asserts.
//
// Intended for authorized security testing of cameras you own or are permitted
// to assess.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CamAuthNone = 0, // service answered 2xx with no credentials
    CamAuthBasic, // HTTP/RTSP Basic
    CamAuthDigest, // HTTP/RTSP Digest (MD5)
    CamAuthUnknown, // responded but scheme not understood
} CamAuthType;

typedef struct {
    bool responded; // host spoke HTTP at all
    CamAuthType auth; // auth scheme required on "/"
    char server[40]; // Server: header (vendor hint), "" if absent
    char realm[48]; // WWW-Authenticate realm, "" if absent
    char nonce[64]; // Digest nonce from the probe challenge (may be reused)
    char qop[16]; // Digest qop token ("auth" / ""), "" if absent
    char opaque[48]; // Digest opaque, "" if absent
} CamHttpInfo;

// GET / on ip_host:port. Fills info with the status/auth scheme and vendor hints.
// Returns true if the host answered as HTTP (info->responded set too).
bool cam_http_probe(uint32_t ip_host, uint16_t port, uint32_t timeout_ms, CamHttpInfo* info);

// Try one credential over HTTP using the scheme learned in probe. For Digest a
// fresh challenge is fetched so the nonce is always current. Returns true when
// the server accepts the credential (2xx and not a repeat 401).
bool cam_http_try_cred(
    uint32_t ip_host,
    uint16_t port,
    const char* user,
    const char* pass,
    uint32_t timeout_ms);

// RTSP DESCRIBE on ip_host:port with user/pass, handling a no-auth / Basic /
// Digest challenge internally. On success (RTSP 200) writes a playable URL
// (rtsp://user:pass@ip:port/) into out_url and returns true.
bool cam_rtsp_try_cred(
    uint32_t ip_host,
    uint16_t port,
    const char* user,
    const char* pass,
    uint32_t timeout_ms,
    char* out_url,
    size_t out_cap);

// True if the host speaks RTSP at all (OPTIONS answered), regardless of auth.
bool cam_rtsp_probe(uint32_t ip_host, uint16_t port, uint32_t timeout_ms);
