#include "network.h"

#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using TcpDeadline = std::chrono::steady_clock::time_point;

int remaining_ms(const TcpDeadline &deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        errno = ETIMEDOUT;
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(remaining.count());
}

int wait_for_socket(int fd, short events, const TcpDeadline &deadline)
{
    while (true) {
        const int timeout_ms = remaining_ms(deadline);
        if (timeout_ms == 0)
            return -1;

        struct pollfd descriptor = {fd, events, 0};
        const int ready = poll(&descriptor, 1, timeout_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return -1;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (descriptor.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        return 0;
    }
}

int send_flags(void)
{
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return flags;
}

int prevent_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
    return 0;
#endif
}

int write_all_until(int fd, const char *data, size_t length,
                    const TcpDeadline &deadline)
{
    size_t offset = 0;
    while (offset < length) {
        if (wait_for_socket(fd, POLLOUT, deadline) < 0)
            return -1;
        const ssize_t written = send(fd, data + offset, length - offset,
                                     send_flags());
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written <= 0)
            return -1;
        offset += static_cast<size_t>(written);
    }
    return 0;
}

} // namespace

int create_udp_socket(void)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket UDP");
        return -1;
    }

    // Avoid leaving the interactive client stuck forever if the DS is down.
    const struct timeval timeout = {10, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(fd);
        return -1;
    }
    return fd;
}

int make_server_address(struct sockaddr_in *address, const char *ds_ip, int ds_port)
{
    if (address == nullptr || ds_ip == nullptr || ds_port < 1 || ds_port > 65535) {
        fprintf(stderr, "Invalid Directory Server address or port.\n");
        return -1;
    }

    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(static_cast<uint16_t>(ds_port));
    if (inet_pton(AF_INET, ds_ip, &address->sin_addr) != 1) {
        fprintf(stderr, "Invalid Directory Server IPv4 address: %s\n", ds_ip);
        return -1;
    }
    return 0;
}

int connect_tcp_with_timeout(int fd, const struct sockaddr_in *address,
                             int timeout_ms)
{
    if (fd < 0 || address == nullptr || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }

    const int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
        return -1;

    int result = -1;
    int saved_error = 0;
    const int connected = connect(fd,
                                  reinterpret_cast<const struct sockaddr *>(address),
                                  sizeof(*address));
    if (connected == 0) {
        result = 0;
    } else if (errno != EINPROGRESS) {
        saved_error = errno;
    } else {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                saved_error = ETIMEDOUT;
                break;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            const int wait_ms = static_cast<int>(remaining.count()) + 1;
            struct pollfd descriptor = {fd, POLLOUT, 0};
            const int ready = poll(&descriptor, 1, wait_ms);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0) {
                saved_error = errno;
                break;
            }
            if (ready == 0) {
                saved_error = ETIMEDOUT;
                break;
            }

            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                           &error_length) < 0) {
                saved_error = errno;
            } else if (socket_error != 0) {
                saved_error = socket_error;
            } else {
                result = 0;
            }
            break;
        }
    }

    if (fcntl(fd, F_SETFL, original_flags) < 0 && result == 0) {
        result = -1;
        saved_error = errno;
    }
    if (result < 0)
        errno = saved_error != 0 ? saved_error : EIO;
    return result;
}

int set_tcp_io_timeouts(int fd, int timeout_seconds)
{
    if (fd < 0 || timeout_seconds <= 0) {
        errno = EINVAL;
        return -1;
    }

    const struct timeval timeout = {timeout_seconds, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        return -1;
    return 0;
}

int tcp_exchange(const struct sockaddr_in *server, int port,
                 const char *request, char *reply, size_t reply_size)
{
    if (server == nullptr || request == nullptr || reply == nullptr ||
        reply_size < 2 || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid TCP exchange parameters.\n");
        return -1;
    }

    constexpr int TCP_CONTROL_DEADLINE_MS = 30000;
    const TcpDeadline deadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(TCP_CONTROL_DEADLINE_MS);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket TCP");
        return -1;
    }

    struct sockaddr_in target = *server;
    target.sin_port = htons(static_cast<uint16_t>(port));
    int connect_timeout_ms = remaining_ms(deadline);
    if (connect_timeout_ms > 10000)
        connect_timeout_ms = 10000;
    if (prevent_sigpipe(fd) < 0 ||
        connect_tcp_with_timeout(fd, &target, connect_timeout_ms) < 0) {
        perror("connect to Directory Server");
        close(fd);
        return -1;
    }

    const int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        perror("set non-blocking mode for Directory Server TCP socket");
        close(fd);
        return -1;
    }

    const size_t request_length = strlen(request);
    if (write_all_until(fd, request, request_length, deadline) < 0) {
        perror("write to Directory Server");
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);

    size_t used = 0;
    bool terminated = false;
    while (used < reply_size - 1) {
        if (wait_for_socket(fd, POLLIN, deadline) < 0) {
            perror("read from Directory Server");
            close(fd);
            return -1;
        }
        const ssize_t received = recv(fd, reply + used, reply_size - used - 1,
                                      MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received < 0) {
            perror("read from Directory Server");
            close(fd);
            return -1;
        }
        if (received == 0)
            break;
        if (memchr(reply + used, '\0', static_cast<size_t>(received)) != nullptr) {
            fprintf(stderr, "Directory Server sent a non-text TCP reply.\n");
            close(fd);
            return -1;
        }
        used += static_cast<size_t>(received);
        if (memchr(reply, '\n', used) != nullptr) {
            terminated = true;
            break;
        }
    }

    close(fd);
    reply[used] = '\0';
    if (used == 0) {
        fprintf(stderr, "Empty reply from Directory Server.\n");
        return -1;
    }
    if (!terminated) {
        fprintf(stderr, "Incomplete or oversized TCP reply from Directory Server.\n");
        return -1;
    }
    if (reply[used - 1] != '\n' || memchr(reply, '\n', used) != reply + used - 1) {
        fprintf(stderr, "Directory Server sent data outside the TCP reply line.\n");
        return -1;
    }
    return 0;
}
