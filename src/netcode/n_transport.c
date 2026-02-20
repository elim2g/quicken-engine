/*
 * QUICKEN Engine - Transport Layer
 *
 * Loopback (cross-wired ring buffers) and UDP socket wrapper.
 * Both share the same send/recv interface so all code above
 * this layer is transport-agnostic.
 */

#include "n_internal.h"

#ifdef QK_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#endif

// --- Loopback ---

void n_transport_open_loopback(n_transport_t *client, n_transport_t *server,
                               n_loopback_queue_t *q1, n_loopback_queue_t *q2) {
    // q1: client->server direction (client sends, server recvs)
    // q2: server->client direction (server sends, client recvs)
    memset(q1, 0, sizeof(*q1));
    memset(q2, 0, sizeof(*q2));

    memset(client, 0, sizeof(*client));
    client->type = N_TRANSPORT_LOOPBACK;
    client->socket_fd = -1;
    client->send_queue = q1;    // client writes to q1
    client->recv_queue = q2;    // client reads from q2

    memset(server, 0, sizeof(*server));
    server->type = N_TRANSPORT_LOOPBACK;
    server->socket_fd = -1;
    server->send_queue = q2;    // server writes to q2
    server->recv_queue = q1;    // server reads from q1
}

static i32 loopback_send(n_transport_t *t, const void *data, u32 len) {
    if (!t->send_queue) return -1;
    if (len > N_TRANSPORT_MTU) return -1;

    n_loopback_queue_t *queue = t->send_queue;
    u32 next_head = (queue->head + 1) % N_LOOPBACK_QUEUE_SIZE;
    if (next_head == queue->tail) {
        return -1; // queue full
    }

    queue->packets[queue->head].len = (u16)len;
    memcpy(queue->packets[queue->head].data, data, len);
    queue->head = next_head;
    return (i32)len;
}

static i32 loopback_recv(n_transport_t *t, void *data, u32 max_len) {
    if (!t->recv_queue) return -1;

    n_loopback_queue_t *queue = t->recv_queue;
    if (queue->tail == queue->head) {
        return 0; // nothing to read
    }

    u16 pkt_len = queue->packets[queue->tail].len;
    if (pkt_len > max_len) {
        // skip oversized packet
        queue->tail = (queue->tail + 1) % N_LOOPBACK_QUEUE_SIZE;
        return -1;
    }

    memcpy(data, queue->packets[queue->tail].data, pkt_len);
    queue->tail = (queue->tail + 1) % N_LOOPBACK_QUEUE_SIZE;
    return (i32)pkt_len;
}

// --- UDP ---

bool n_transport_open_udp(n_transport_t *t, u16 bind_port) {
    memset(t, 0, sizeof(*t));
    t->type = N_TRANSPORT_UDP;
    t->send_queue = NULL;
    t->recv_queue = NULL;

#ifdef QK_PLATFORM_WINDOWS
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    // non-blocking
    u_long one = 1;
    ioctlsocket(sock, FIONBIO, &one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return false;
    }

    t->socket_fd = (intptr_t)sock;
#else
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    // non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    t->socket_fd = sock;
#endif

    return true;
}

void n_transport_close(n_transport_t *t) {
    if (t->type == N_TRANSPORT_UDP && t->socket_fd >= 0) {
#ifdef QK_PLATFORM_WINDOWS
        closesocket((SOCKET)t->socket_fd);
#else
        close((int)t->socket_fd);
#endif
        t->socket_fd = -1;
    }
    t->send_queue = NULL;
    t->recv_queue = NULL;
}

static i32 udp_send(n_transport_t *t, const n_address_t *to,
                     const void *data, u32 len) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to->port);
    addr.sin_addr.s_addr = htonl(to->ip);

#ifdef QK_PLATFORM_WINDOWS
    int result = sendto((SOCKET)t->socket_fd, (const char *)data, (int)len, 0,
                        (struct sockaddr *)&addr, sizeof(addr));
    if (result == SOCKET_ERROR) return -1;
    return result;
#else
    ssize_t result = sendto((int)t->socket_fd, data, len, 0,
                            (struct sockaddr *)&addr, sizeof(addr));
    if (result < 0) return -1;
    return (i32)result;
#endif
}

static i32 udp_recv(n_transport_t *t, n_address_t *from,
                     void *data, u32 max_len) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

#ifdef QK_PLATFORM_WINDOWS
    int addr_len = sizeof(addr);
    int result = recvfrom((SOCKET)t->socket_fd, (char *)data, (int)max_len, 0,
                          (struct sockaddr *)&addr, &addr_len);
    if (result == SOCKET_ERROR) return 0;
#else
    socklen_t addr_len = sizeof(addr);
    ssize_t result = recvfrom((int)t->socket_fd, data, max_len, 0,
                              (struct sockaddr *)&addr, &addr_len);
    if (result < 0) return 0;
#endif

    if (from) {
        from->ip = ntohl(addr.sin_addr.s_addr);
        from->port = ntohs(addr.sin_port);
    }
    return (i32)result;
}

// --- Unified interface ---

i32 n_transport_send(n_transport_t *t, const n_address_t *to,
                     const void *data, u32 len) {
    if (len == 0 || len > N_TRANSPORT_MTU) return -1;

    if (t->type == N_TRANSPORT_LOOPBACK) {
        QK_UNUSED(to);
        return loopback_send(t, data, len);
    }
    return udp_send(t, to, data, len);
}

i32 n_transport_recv(n_transport_t *t, n_address_t *from,
                     void *data, u32 max_len) {
    if (t->type == N_TRANSPORT_LOOPBACK) {
        if (from) {
            from->ip = 0;
            from->port = 0;
        }
        return loopback_recv(t, data, max_len);
    }
    return udp_recv(t, from, data, max_len);
}
