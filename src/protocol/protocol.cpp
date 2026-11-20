#include "protocol.h"

#include <errno.h>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <sys/select.h>
#include <sys/socket.h>

int valid_port(const char *text, int *port)
{
    if (text == nullptr || port == nullptr || *text == '\0')
        return 0;
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(text);
         *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9')
            return 0;
    }

    errno = 0;
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535)
        return 0;
    *port = static_cast<int>(value);
    return 1;
}

int valid_uid(const char *uid)
{
    if (uid == nullptr || strlen(uid) != UID_LEN)
        return 0;
    for (int i = 0; i < UID_LEN; ++i) {
        if (uid[i] < '0' || uid[i] > '9')
            return 0;
    }
    return 1;
}

static int ascii_alnum(unsigned char character)
{
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
}

int valid_password(const char *password)
{
    if (password == nullptr || strlen(password) != PASSWORD_LEN)
        return 0;
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(password);
         *cursor != '\0'; ++cursor) {
        if (!ascii_alnum(*cursor))
            return 0;
    }
    return 1;
}

int valid_filename(const char *filename)
{
    if (filename == nullptr)
        return 0;
    const size_t length = strlen(filename);
    if (length < 5 || length > MAX_FILENAME)
        return 0;

    const size_t dot = length - 4;
    if (filename[dot] != '.')
        return 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char character = static_cast<unsigned char>(filename[i]);
        if (i == dot)
            continue;
        if (i > dot) {
            if (!ascii_alnum(character))
                return 0;
        } else if (!ascii_alnum(character) && character != '-' && character != '_') {
            return 0;
        }
    }
    return 1;
}

int valid_label(const char *label)
{
    if (label == nullptr)
        return 0;
    const size_t length = strlen(label);
    if (length < 1 || length > MAX_LABEL)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char character = static_cast<unsigned char>(label[i]);
        if (!ascii_alnum(character) && character != '-' && character != '_')
            return 0;
    }
    return 1;
}

double Calculate_Random(void)
{
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return distribution(generator);
}

int send_request(int fd, const struct sockaddr_in *server,
                 const char *request, char *reply, size_t reply_size)
{
    if (fd < 0 || server == nullptr || request == nullptr || reply == nullptr ||
        server->sin_family != AF_INET || server->sin_port == 0 ||
        fd >= FD_SETSIZE || reply_size < 2) {
        fprintf(stderr, "Invalid UDP request parameters.\n");
        return -1;
    }
    reply[0] = '\0';

    const size_t request_length = strlen(request);
    if (request_length == 0) {
        fprintf(stderr, "UDP request cannot be empty.\n");
        return -1;
    }
    ssize_t sent;
    do {
        sent = sendto(fd, request, request_length, 0,
                      reinterpret_cast<const struct sockaddr *>(server),
                      sizeof(*server));
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    if (static_cast<size_t>(sent) != request_length) {
        fprintf(stderr, "Incomplete UDP request sent to Directory Server.\n");
        return -1;
    }

    constexpr int UDP_REPLY_TIMEOUT_MS = 10000;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(UDP_REPLY_TIMEOUT_MS);
    // This holds any complete IPv4 UDP payload, so recvfrom cannot silently
    // truncate a larger datagram before we compare it with reply_size.
    constexpr size_t IPV4_UDP_DATAGRAM_SIZE = 65536;
    char datagram[IPV4_UDP_DATAGRAM_SIZE];

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            fprintf(stderr, "Timed out waiting for a UDP reply from Directory Server.\n");
            return -1;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const int timeout_ms = static_cast<int>(remaining.count()) + 1;
        struct timeval timeout = {
            timeout_ms / 1000,
            (timeout_ms % 1000) * 1000
        };
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        const int ready = select(fd + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            perror("select for Directory Server UDP reply");
            return -1;
        }
        if (ready == 0)
            continue;
        if (!FD_ISSET(fd, &readable))
            continue;

        struct sockaddr_in from = {};
        socklen_t from_size = sizeof(from);
        const ssize_t received = recvfrom(
            fd, datagram, sizeof(datagram), 0,
            reinterpret_cast<struct sockaddr *>(&from), &from_size);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received < 0) {
            perror("recvfrom");
            return -1;
        }
        if (from_size < sizeof(from) || from.sin_family != AF_INET ||
            from.sin_addr.s_addr != server->sin_addr.s_addr ||
            from.sin_port != server->sin_port)
            continue;
        const size_t received_size = static_cast<size_t>(received);
        if (received_size >= reply_size) {
            fprintf(stderr, "Directory Server UDP reply is too large.\n");
            return -1;
        }

        const char *newline = static_cast<const char *>(
            memchr(datagram, '\n', received_size));
        if (received_size == 0 || datagram[received_size - 1] != '\n' ||
            memchr(datagram, '\0', received_size) != nullptr ||
            newline != datagram + received_size - 1) {
            fprintf(stderr, "Directory Server UDP reply is not a single text line.\n");
            return -1;
        }
        memcpy(reply, datagram, received_size);
        reply[received_size] = '\0';
        return 0;
    }
}

int parse_reply(const char *reply, const char *expected_prefix,
                char *status, size_t status_size)
{
    if (reply == nullptr || expected_prefix == nullptr || status == nullptr ||
        status_size == 0) {
        fprintf(stderr, "Invalid reply parser parameters.\n");
        return -1;
    }

    const char *separator = strchr(reply, ' ');
    const char *newline = strchr(reply, '\n');
    if (separator == nullptr || newline == nullptr || newline[1] != '\0' ||
        separator == reply || separator >= newline || strchr(separator + 1, ' ') != nullptr ||
        static_cast<size_t>(separator - reply) != strlen(expected_prefix) ||
        strncmp(reply, expected_prefix, strlen(expected_prefix)) != 0 ||
        separator + 1 == newline) {
        fprintf(stderr, "Unexpected Directory Server reply: %s", reply);
        return -1;
    }
    const size_t value_size = static_cast<size_t>(newline - separator - 1);
    if (value_size >= status_size) {
        fprintf(stderr, "Directory Server status is too long.\n");
        return -1;
    }
    for (const char *cursor = separator + 1; cursor < newline; ++cursor) {
        if (*cursor < 'A' || *cursor > 'Z') {
            fprintf(stderr, "Directory Server returned an invalid status token.\n");
            return -1;
        }
    }
    memcpy(status, separator + 1, value_size);
    status[value_size] = '\0';
    return 0;
}

void print_login_result(const char *status)
{
    if (strcmp(status, "OK") == 0)
        printf("Login successful.\n");
    else if (strcmp(status, "NOK") == 0)
        printf("Login failed: incorrect password.\n");
    else if (strcmp(status, "REG") == 0)
        printf("New user registered and logged in successfully.\n");
    else if (strcmp(status, "ERR") == 0)
        printf("Login failed: invalid request or parameters.\n");
    else
        printf("Login failed: status %s.\n", status);
}

void print_logout_result(const char *status)
{
    if (strcmp(status, "OK") == 0)
        printf("Logout successful.\n");
    else if (strcmp(status, "NLG") == 0)
        printf("Logout failed: user is not logged in.\n");
    else if (strcmp(status, "UNR") == 0)
        printf("Logout failed: user is not registered.\n");
    else if (strcmp(status, "WRP") == 0)
        printf("Logout failed: incorrect password.\n");
    else if (strcmp(status, "ERR") == 0)
        printf("Logout failed: invalid request or parameters.\n");
    else
        printf("Logout failed: status %s.\n", status);
}

void print_unregister_result(const char *status)
{
    if (strcmp(status, "OK") == 0)
        printf("Unregister successful.\n");
    else if (strcmp(status, "NOK") == 0)
        printf("Unregister failed: user is not logged in.\n");
    else if (strcmp(status, "UNR") == 0)
        printf("Unregister failed: user is not registered.\n");
    else if (strcmp(status, "WRP") == 0)
        printf("Unregister failed: incorrect password.\n");
    else if (strcmp(status, "ERR") == 0)
        printf("Unregister failed: invalid request or parameters.\n");
    else
        printf("Unregister failed: status %s.\n", status);
}
