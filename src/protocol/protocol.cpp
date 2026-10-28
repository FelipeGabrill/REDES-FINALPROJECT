#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include <string>
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

int send_request(int fd, const struct sockaddr_in *server,
                 const char *request, char *reply, size_t reply_size)
{
    if (fd < 0 || server == nullptr || request == nullptr || reply == nullptr ||
        reply_size < 2) {
        fprintf(stderr, "Invalid UDP request parameters.\n");
        return -1;
    }

    const size_t request_length = strlen(request);
    const ssize_t sent = sendto(fd, request, request_length, 0,
                                reinterpret_cast<const struct sockaddr *>(server),
                                sizeof(*server));
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    if (static_cast<size_t>(sent) != request_length) {
        fprintf(stderr, "Incomplete UDP request sent to Directory Server.\n");
        return -1;
    }

    struct sockaddr_in from;
    socklen_t from_length = sizeof(from);
    const ssize_t received = recvfrom(fd, reply, reply_size - 1, 0,
                                     reinterpret_cast<struct sockaddr *>(&from),
                                     &from_length);
    if (received < 0) {
        perror("recvfrom");
        return -1;
    }
    if (from.sin_family != AF_INET || from.sin_addr.s_addr != server->sin_addr.s_addr ||
        from.sin_port != server->sin_port) {
        fprintf(stderr, "Ignored a UDP reply from an unexpected source.\n");
        return -1;
    }

    reply[received] = '\0';
    if (received == 0 || reply[received - 1] != '\n') {
        fprintf(stderr, "Directory Server UDP reply is missing its newline terminator.\n");
        return -1;
    }
    return 0;
}

int parse_reply(const char *reply, const char *expected_prefix,
                char *status, size_t status_size)
{
    if (reply == nullptr || expected_prefix == nullptr || status == nullptr ||
        status_size == 0) {
        fprintf(stderr, "Invalid reply parser parameters.\n");
        return -1;
    }

    std::istringstream input(reply);
    std::string prefix;
    std::string value;
    if (!(input >> prefix >> value) || prefix != expected_prefix) {
        fprintf(stderr, "Unexpected Directory Server reply: %s", reply);
        return -1;
    }
    if (value.size() >= status_size) {
        fprintf(stderr, "Directory Server status is too long.\n");
        return -1;
    }
    memcpy(status, value.c_str(), value.size() + 1);
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
