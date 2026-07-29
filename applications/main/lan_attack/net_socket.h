#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <lwip/sockets.h>

/**
 * net_socket — UDP/TCP lifecycle wrappers over lwIP (firmware-only)
 *
 * Thin, task-safe wrappers around lwip/sockets.h. The raw file descriptor is
 * hidden behind an opaque NetSocket handle. Every allocating call returns NULL
 * on error and every action call returns false on error, logging at WARN via
 * FURI_LOG_W. Timeouts are honored via select()/SO_RCVTIMEO and expressed in
 * milliseconds (0 = block indefinitely).
 */

typedef struct NetSocket NetSocket; // opaque; hides the raw fd

// ============ UDP ============

/**
 * Create a UDP socket bound to the given local port (INADDR_ANY).
 * Sets SO_REUSEADDR. If broadcast is true, enables SO_BROADCAST so the socket
 * may send to 255.255.255.255. Pass port 0 to bind an ephemeral local port.
 * Returns an open NetSocket* on success, NULL on error.
 */
NetSocket* net_socket_udp_bind(uint16_t port, bool broadcast);

/**
 * Send a UDP datagram to dest. Returns true if the whole payload was accepted
 * by the stack, false on error.
 */
bool net_socket_udp_send(
    NetSocket* sock,
    const struct sockaddr_in* dest,
    const uint8_t* data,
    size_t len);

/**
 * Receive a single UDP datagram, waiting up to timeout_ms (0 = block forever).
 * On success writes up to max_len bytes into data, stores the count in out_len,
 * and (if src is non-NULL) the sender address in src. Returns false on error or
 * timeout (out_len set to 0 when non-NULL).
 */
bool net_socket_udp_recv(
    NetSocket* sock,
    struct sockaddr_in* src,
    uint8_t* data,
    size_t max_len,
    size_t* out_len,
    uint32_t timeout_ms);

// ============ TCP ============

/**
 * Create a listening TCP socket bound to the given local port (INADDR_ANY).
 * Sets SO_REUSEADDR and calls listen(). Returns an open NetSocket* on success,
 * NULL on error.
 */
NetSocket* net_socket_tcp_bind(uint16_t port);

/**
 * Connect a TCP socket to dest_ip (host byte order) : port, waiting up to
 * timeout_ms for the handshake to complete (0 = block indefinitely). Uses a
 * non-blocking connect + select() so a filtered/dead host times out instead of
 * hanging. On success returns a connected NetSocket* (restored to blocking
 * mode); returns NULL on refusal, timeout or error.
 */
NetSocket* net_socket_tcp_connect(uint32_t dest_ip, uint16_t port, uint32_t timeout_ms);

/**
 * Accept one incoming connection on a listening socket, waiting up to
 * timeout_ms (0 = block forever). On success returns a new open NetSocket* for
 * the accepted connection and (if client_addr is non-NULL) fills the peer
 * address. Returns NULL on error or timeout.
 */
NetSocket* net_socket_tcp_accept(
    NetSocket* listen_sock,
    struct sockaddr_in* client_addr,
    uint32_t timeout_ms);

/**
 * Send all bytes over a connected TCP socket (loops until len bytes are sent).
 * Returns true on success, false on error.
 */
bool net_socket_tcp_send(NetSocket* sock, const uint8_t* data, size_t len);

/**
 * Receive from a connected TCP socket, waiting up to timeout_ms (0 = block
 * forever). On success writes up to max_len bytes into data and stores the
 * count in out_len. A count of 0 with a true return means the peer closed the
 * connection. Returns false on error or timeout.
 */
bool net_socket_tcp_recv(
    NetSocket* sock,
    uint8_t* data,
    size_t max_len,
    size_t* out_len,
    uint32_t timeout_ms);

// ============ Lifecycle ============

/** Close the underlying fd (if open) and free the handle. NULL-safe. */
void net_socket_close(NetSocket* sock);

/** Returns true if sock is non-NULL and holds a valid open fd. */
bool net_socket_is_open(NetSocket* sock);
