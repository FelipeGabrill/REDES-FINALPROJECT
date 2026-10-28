#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

int write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        const ssize_t written = write(fd, data + offset, length - offset);
        if (written < 0 && errno == EINTR)
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

int tcp_exchange(const struct sockaddr_in *server, int port,
                 const char *request, char *reply, size_t reply_size)
{
    if (server == nullptr || request == nullptr || reply == nullptr ||
        reply_size < 2 || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid TCP exchange parameters.\n");
        return -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket TCP");
        return -1;
    }

    struct sockaddr_in target = *server;
    target.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<const struct sockaddr *>(&target),
                sizeof(target)) < 0) {
        perror("connect to Directory Server");
        close(fd);
        return -1;
    }

    const size_t request_length = strlen(request);
    if (write_all(fd, request, request_length) < 0) {
        perror("write to Directory Server");
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);

    size_t used = 0;
    bool terminated = false;
    while (used + 1 < reply_size) {
        const ssize_t received = read(fd, reply + used, reply_size - used - 1);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0) {
            perror("read from Directory Server");
            close(fd);
            return -1;
        }
        if (received == 0)
            break;
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
    return 0;
}
