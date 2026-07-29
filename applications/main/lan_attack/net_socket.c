#include "net_socket.h"

#include <furi.h>
#include <string.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#define TAG "NetSocket"

/**
 * Opaque socket handle. The raw lwIP fd is kept private here so callers never
 * touch descriptors directly. fd == -1 means "closed / invalid".
 */
struct NetSocket {
    int fd;
};

// ---- internal helpers -------------------------------------------------------

static NetSocket* net_socket_wrap(int fd) {
    NetSocket* sock = malloc(sizeof(NetSocket));
    if(!sock) {
        FURI_LOG_W(TAG, "alloc failed for fd %d, closing", fd);
        lwip_close(fd);
        return NULL;
    }
    sock->fd = fd;
    return sock;
}

/**
 * Wait until the fd is readable, or timeout_ms elapses. timeout_ms == 0 blocks
 * forever. Returns 1 if readable, 0 on timeout, -1 on error.
 */
static int net_socket_wait_readable(int fd, uint32_t timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv;
    struct timeval* tvp = NULL;
    if(timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000U;
        tv.tv_usec = (timeout_ms % 1000U) * 1000U;
        tvp = &tv;
    }

    int rc = lwip_select(fd + 1, &rfds, NULL, NULL, tvp);
    if(rc < 0) {
        FURI_LOG_W(TAG, "select failed on fd %d (errno %d)", fd, errno);
        return -1;
    }
    return rc > 0 ? 1 : 0;
}

// ---- UDP --------------------------------------------------------------------

NetSocket* net_socket_udp_bind(uint16_t port, bool broadcast) {
    int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(fd < 0) {
        FURI_LOG_W(TAG, "udp socket() failed (errno %d)", errno);
        return NULL;
    }

    int on = 1;
    if(lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        FURI_LOG_W(TAG, "udp SO_REUSEADDR failed (errno %d)", errno);
    }
    if(broadcast) {
        if(lwip_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
            FURI_LOG_W(TAG, "udp SO_BROADCAST failed (errno %d)", errno);
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(lwip_bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        FURI_LOG_W(TAG, "udp bind(port %u) failed (errno %d)", (unsigned)port, errno);
        lwip_close(fd);
        return NULL;
    }

    return net_socket_wrap(fd);
}

bool net_socket_udp_send(
    NetSocket* sock,
    const struct sockaddr_in* dest,
    const uint8_t* data,
    size_t len) {
    if(!sock || sock->fd < 0 || !dest || (!data && len > 0)) {
        FURI_LOG_W(TAG, "udp_send bad args");
        return false;
    }

    ssize_t sent = lwip_sendto(
        sock->fd, data, len, 0, (const struct sockaddr*)dest, sizeof(*dest));
    if(sent < 0 || (size_t)sent != len) {
        FURI_LOG_W(TAG, "udp sendto failed (sent %d, errno %d)", (int)sent, errno);
        return false;
    }
    return true;
}

bool net_socket_udp_recv(
    NetSocket* sock,
    struct sockaddr_in* src,
    uint8_t* data,
    size_t max_len,
    size_t* out_len,
    uint32_t timeout_ms) {
    if(out_len) *out_len = 0;
    if(!sock || sock->fd < 0 || !data || max_len == 0) {
        FURI_LOG_W(TAG, "udp_recv bad args");
        return false;
    }

    int ready = net_socket_wait_readable(sock->fd, timeout_ms);
    if(ready <= 0) {
        return false; // timeout or select error (already logged)
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t got =
        lwip_recvfrom(sock->fd, data, max_len, 0, (struct sockaddr*)&from, &from_len);
    if(got < 0) {
        FURI_LOG_W(TAG, "udp recvfrom failed (errno %d)", errno);
        return false;
    }

    if(src) memcpy(src, &from, sizeof(from));
    if(out_len) *out_len = (size_t)got;
    return true;
}

// ---- TCP --------------------------------------------------------------------

NetSocket* net_socket_tcp_bind(uint16_t port) {
    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(fd < 0) {
        FURI_LOG_W(TAG, "tcp socket() failed (errno %d)", errno);
        return NULL;
    }

    int on = 1;
    if(lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        FURI_LOG_W(TAG, "tcp SO_REUSEADDR failed (errno %d)", errno);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(lwip_bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        FURI_LOG_W(TAG, "tcp bind(port %u) failed (errno %d)", (unsigned)port, errno);
        lwip_close(fd);
        return NULL;
    }

    if(lwip_listen(fd, 1) < 0) {
        FURI_LOG_W(TAG, "tcp listen(port %u) failed (errno %d)", (unsigned)port, errno);
        lwip_close(fd);
        return NULL;
    }

    return net_socket_wrap(fd);
}

/**
 * Wait until the fd is writable, or timeout_ms elapses. Used to detect
 * non-blocking connect() completion. Returns 1 if writable, 0 on timeout,
 * -1 on error.
 */
static int net_socket_wait_writable(int fd, uint32_t timeout_ms) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);

    struct timeval tv;
    struct timeval* tvp = NULL;
    if(timeout_ms > 0) {
        tv.tv_sec = timeout_ms / 1000U;
        tv.tv_usec = (timeout_ms % 1000U) * 1000U;
        tvp = &tv;
    }

    int rc = lwip_select(fd + 1, NULL, &wfds, NULL, tvp);
    if(rc < 0) {
        FURI_LOG_W(TAG, "connect select failed on fd %d (errno %d)", fd, errno);
        return -1;
    }
    return rc > 0 ? 1 : 0;
}

NetSocket* net_socket_tcp_connect(uint32_t dest_ip, uint16_t port, uint32_t timeout_ms) {
    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(fd < 0) {
        FURI_LOG_W(TAG, "connect socket() failed (errno %d)", errno);
        return NULL;
    }

    // Non-blocking connect so we can bound the wait with select().
    int nonblock = 1;
    lwip_ioctl(fd, FIONBIO, &nonblock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(dest_ip);
    addr.sin_port = htons(port);

    int rc = lwip_connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if(rc < 0 && errno != EINPROGRESS) {
        // Immediate refusal/error.
        lwip_close(fd);
        return NULL;
    }

    if(rc < 0) {
        // In progress: wait for writability then check SO_ERROR.
        int ready = net_socket_wait_writable(fd, timeout_ms);
        if(ready <= 0) {
            lwip_close(fd); // timeout or select error
            return NULL;
        }
        int so_err = 0;
        socklen_t elen = sizeof(so_err);
        if(lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &elen) < 0 || so_err != 0) {
            lwip_close(fd);
            return NULL;
        }
    }

    // Restore blocking mode for straightforward send/recv afterwards.
    int blocking = 0;
    lwip_ioctl(fd, FIONBIO, &blocking);
    return net_socket_wrap(fd);
}

NetSocket* net_socket_tcp_accept(
    NetSocket* listen_sock,
    struct sockaddr_in* client_addr,
    uint32_t timeout_ms) {
    if(!listen_sock || listen_sock->fd < 0) {
        FURI_LOG_W(TAG, "tcp_accept bad args");
        return NULL;
    }

    int ready = net_socket_wait_readable(listen_sock->fd, timeout_ms);
    if(ready <= 0) {
        return NULL; // timeout or select error (already logged)
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int client_fd = lwip_accept(listen_sock->fd, (struct sockaddr*)&from, &from_len);
    if(client_fd < 0) {
        FURI_LOG_W(TAG, "tcp accept failed (errno %d)", errno);
        return NULL;
    }

    if(client_addr) memcpy(client_addr, &from, sizeof(from));
    return net_socket_wrap(client_fd);
}

bool net_socket_tcp_send(NetSocket* sock, const uint8_t* data, size_t len) {
    if(!sock || sock->fd < 0 || (!data && len > 0)) {
        FURI_LOG_W(TAG, "tcp_send bad args");
        return false;
    }

    size_t total = 0;
    while(total < len) {
        ssize_t sent = lwip_send(sock->fd, data + total, len - total, 0);
        if(sent <= 0) {
            FURI_LOG_W(TAG, "tcp send failed (rc %d, errno %d)", (int)sent, errno);
            return false;
        }
        total += (size_t)sent;
    }
    return true;
}

bool net_socket_tcp_recv(
    NetSocket* sock,
    uint8_t* data,
    size_t max_len,
    size_t* out_len,
    uint32_t timeout_ms) {
    if(out_len) *out_len = 0;
    if(!sock || sock->fd < 0 || !data || max_len == 0) {
        FURI_LOG_W(TAG, "tcp_recv bad args");
        return false;
    }

    int ready = net_socket_wait_readable(sock->fd, timeout_ms);
    if(ready <= 0) {
        return false; // timeout or select error (already logged)
    }

    ssize_t got = lwip_recv(sock->fd, data, max_len, 0);
    if(got < 0) {
        FURI_LOG_W(TAG, "tcp recv failed (errno %d)", errno);
        return false;
    }

    // got == 0 means the peer performed an orderly shutdown; report success
    // with out_len == 0 so callers can distinguish close from error.
    if(out_len) *out_len = (size_t)got;
    return true;
}

// ---- Lifecycle --------------------------------------------------------------

void net_socket_close(NetSocket* sock) {
    if(!sock) return;
    if(sock->fd >= 0) {
        lwip_close(sock->fd);
        sock->fd = -1;
    }
    free(sock);
}

bool net_socket_is_open(NetSocket* sock) {
    return sock && sock->fd >= 0;
}
