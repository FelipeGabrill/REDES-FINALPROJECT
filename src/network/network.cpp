#include "network.h"

#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

constexpr int TCP_CONTROL_TIMEOUT_MS = 30000;
constexpr int TCP_CONNECT_TIMEOUT_MS = 10000;
// The protocol has no version-count limit, so cap memory use defensively.
constexpr size_t MAX_TCP_REPLY_BYTES = 1024 * 1024;

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

int wait_for_socket(int fd, bool wait_for_read, const TcpDeadline &deadline)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fd >= FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    while (true) {
        const int timeout_ms = remaining_ms(deadline);
        if (timeout_ms == 0)
            return -1;

        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(fd, &descriptors);

        struct timeval timeout = {
            timeout_ms / 1000,
            (timeout_ms % 1000) * 1000
        };
        const int ready = select(fd + 1,
                                 wait_for_read ? &descriptors : nullptr,
                                 wait_for_read ? nullptr : &descriptors,
                                 nullptr, &timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return -1;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (FD_ISSET(fd, &descriptors))
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
        if (wait_for_socket(fd, false, deadline) < 0)
            return -1;
        const ssize_t written = send(fd, data + offset, length - offset,
                                     send_flags());
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written < 0)
            return -1;
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }
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
    if (fd < 0 || address == nullptr || address->sin_family != AF_INET ||
        timeout_ms <= 0) {
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
    } else {
        const int connect_error = errno;
        if (connect_error == EINPROGRESS || connect_error == EALREADY ||
            connect_error == EINTR) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
            if (wait_for_socket(fd, false, deadline) < 0) {
                saved_error = errno;
            } else {
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
            }
        } else {
            saved_error = connect_error;
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
                 const char *request, std::string &reply)
{
    reply.clear();
    if (server == nullptr || server->sin_family != AF_INET || request == nullptr ||
        port < 1 || port > 65535) {
        fprintf(stderr, "Invalid TCP exchange parameters.\n");
        return -1;
    }

    const size_t request_length = strlen(request);
    const char *request_newline = static_cast<const char *>(
        memchr(request, '\n', request_length));
    if (request_length == 0 ||
        request_newline != request + request_length - 1) {
        fprintf(stderr, "TCP request must be a single newline-terminated message.\n");
        return -1;
    }
    for (size_t i = 0; i + 1 < request_length; ++i) {
        const unsigned char character = static_cast<unsigned char>(request[i]);
        if (character < ' ' || character > '~') {
            fprintf(stderr, "TCP request contains an invalid character.\n");
            return -1;
        }
    }

    const TcpDeadline deadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(TCP_CONTROL_TIMEOUT_MS);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket TCP");
        return -1;
    }

    struct sockaddr_in target = *server;
    target.sin_port = htons(static_cast<uint16_t>(port));
    int connect_timeout_ms = remaining_ms(deadline);
    if (connect_timeout_ms > TCP_CONNECT_TIMEOUT_MS)
        connect_timeout_ms = TCP_CONNECT_TIMEOUT_MS;
    if (connect_timeout_ms <= 0) {
        errno = ETIMEDOUT;
        perror("connect to Directory Server");
        close(fd);
        return -1;
    }
    if (prevent_sigpipe(fd) < 0) {
        perror("configure Directory Server TCP socket");
        close(fd);
        return -1;
    }
    if (connect_tcp_with_timeout(fd, &target, connect_timeout_ms) < 0) {
        perror("connect to Directory Server");
        close(fd);
        return -1;
    }

    if (write_all_until(fd, request, request_length, deadline) < 0) {
        perror("write to Directory Server");
        close(fd);
        return -1;
    }
    if (shutdown(fd, SHUT_WR) < 0) {
        perror("shutdown Directory Server TCP socket");
        close(fd);
        return -1;
    }

    bool terminated = false;
    char buffer[4096];
    while (!terminated) {
        if (wait_for_socket(fd, true, deadline) < 0) {
            perror("read from Directory Server");
            close(fd);
            reply.clear();
            return -1;
        }
        const ssize_t received = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received < 0) {
            perror("read from Directory Server");
            close(fd);
            reply.clear();
            return -1;
        }
        if (received == 0)
            break;

        const size_t received_size = static_cast<size_t>(received);
        if (received_size > MAX_TCP_REPLY_BYTES - reply.size()) {
            fprintf(stderr, "Directory Server TCP reply exceeds the 1 MiB limit.\n");
            close(fd);
            reply.clear();
            return -1;
        }
        const char *newline = static_cast<const char *>(
            memchr(buffer, '\n', received_size));
        if (newline != nullptr && newline != buffer + received - 1) {
            fprintf(stderr, "Directory Server sent data outside the TCP reply line.\n");
            close(fd);
            reply.clear();
            return -1;
        }
        for (size_t i = 0; i < received_size; ++i) {
            const unsigned char character = static_cast<unsigned char>(buffer[i]);
            if (character != '\n' && (character < ' ' || character > '~')) {
                fprintf(stderr, "Directory Server sent a non-text TCP reply.\n");
                close(fd);
                reply.clear();
                return -1;
            }
        }
        try {
            reply.append(buffer, received_size);
        } catch (const std::exception &) {
            fprintf(stderr, "Could not store Directory Server TCP reply.\n");
            close(fd);
            reply.clear();
            return -1;
        }
        terminated = newline != nullptr;
    }

    close(fd);
    if (reply.empty()) {
        fprintf(stderr, "Empty reply from Directory Server.\n");
        return -1;
    }
    if (!terminated) {
        fprintf(stderr, "Incomplete TCP reply from Directory Server.\n");
        reply.clear();
        return -1;
    }
    if (reply.size() < 2 || reply.back() != '\n' ||
        reply.find('\n') != reply.size() - 1) {
        fprintf(stderr, "Directory Server sent data outside the TCP reply line.\n");
        reply.clear();
        return -1;
    }
    return 0;
}
