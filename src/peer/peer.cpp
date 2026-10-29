#include "peer.h"

#include "protocol.h"
#include "network.h"

#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr size_t MAX_CLIENT_WORKERS = 16;
constexpr int TCP_CONTROL_DEADLINE_MS = 30000;

using PeerDeadline = std::chrono::steady_clock::time_point;

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

int remaining_ms(const PeerDeadline &deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        errno = ETIMEDOUT;
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(remaining.count());
}

int wait_for_socket(int fd, short events, const PeerDeadline &deadline)
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

int peer_send_flags(void)
{
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return flags;
}

int write_all_until(int fd, const char *data, size_t length,
                    const PeerDeadline &deadline)
{
    size_t offset = 0;
    while (offset < length) {
        if (wait_for_socket(fd, POLLOUT, deadline) < 0)
            return -1;
        const ssize_t written = send(fd, data + offset, length - offset,
                                     peer_send_flags());
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written <= 0)
            return -1;
        offset += static_cast<size_t>(written);
    }
    return 0;
}

bool read_line_until(int fd, std::string &line, size_t max_length,
                     const PeerDeadline &deadline)
{
    line.clear();
    while (line.size() < max_length) {
        char byte = '\0';
        if (wait_for_socket(fd, POLLIN, deadline) < 0)
            return false;
        const ssize_t received = recv(fd, &byte, 1, MSG_DONTWAIT);
        if (received < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (received <= 0)
            return false;
        if (byte == '\n')
            return true;
        if (byte != '\r')
            line.push_back(byte);
    }
    return false;
}

void send_peer_status(int fd, const char *status)
{
    char response[32];
    const int length = snprintf(response, sizeof(response), "RGT %s\n", status);
    if (length > 0 && static_cast<size_t>(length) < sizeof(response))
        write_all(fd, response, static_cast<size_t>(length));
}

void serve_connection(int fd)
{
    if (set_tcp_io_timeouts(fd, 30) < 0)
        return;
    if (prevent_sigpipe(fd) < 0)
        return;
    const PeerDeadline deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(TCP_CONTROL_DEADLINE_MS);

    std::string request;
    if (!read_line_until(fd, request, 128, deadline)) {
        send_peer_status(fd, "ERR");
        return;
    }

    std::istringstream input(request);
    std::string operation;
    std::string filename;
    std::string extra;
    if (!(input >> operation >> filename) || (input >> extra) ||
        operation != "GET" || !valid_filename(filename.c_str())) {
        send_peer_status(fd, "ERR");
        return;
    }

    const int file_fd = open(filename.c_str(), O_RDONLY);
    if (file_fd < 0) {
        send_peer_status(fd, "NOK");
        return;
    }

    struct stat info;
    if (fstat(file_fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > MAX_FILE_SIZE) {
        close(file_fd);
        send_peer_status(fd, "NOK");
        return;
    }

    char header[64];
    const int header_length = snprintf(header, sizeof(header), "RGT OK %lld\n",
                                       static_cast<long long>(info.st_size));
    if (header_length < 0 || static_cast<size_t>(header_length) >= sizeof(header) ||
        write_all_until(fd, header, static_cast<size_t>(header_length), deadline) < 0) {
        close(file_fd);
        return;
    }

    char buffer[16384];
    off_t remaining = info.st_size;
    while (remaining > 0) {
        const size_t wanted = remaining < static_cast<off_t>(sizeof(buffer))
                                  ? static_cast<size_t>(remaining)
                                  : sizeof(buffer);
        const ssize_t received = read(file_fd, buffer, wanted);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0 || write_all(fd, buffer, static_cast<size_t>(received)) < 0)
            break;
        remaining -= received;
    }
    close(file_fd);
}

bool connect_ipv4(const std::string &ip, int port, int &fd, std::string &error)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        error = "Invalid peer IPv4 address.";
        return false;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("Could not create TCP socket: ") + strerror(errno);
        return false;
    }
    if (prevent_sigpipe(fd) < 0) {
        error = std::string("Could not configure peer TCP socket: ") + strerror(errno);
        close(fd);
        fd = -1;
        return false;
    }
    if (connect_tcp_with_timeout(fd, &address, 10000) < 0) {
        error = std::string("Could not connect to peer: ") + strerror(errno);
        close(fd);
        fd = -1;
        return false;
    }
    return true;
}

}

PeerServer::~PeerServer()
{
    stop();
}

bool PeerServer::start(int port, std::string &error)
{
    if (port < 1 || port > 65535) {
        error = "Peer TCP port must be between 1 and 65535.";
        return false;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error = std::string("Could not create peer TCP socket: ") + strerror(errno);
        return false;
    }

    int reuse_address = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) < 0) {
        error = std::string("Could not configure peer socket: ") + strerror(errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listen_fd_, reinterpret_cast<const struct sockaddr *>(&address),
             sizeof(address)) < 0) {
        error = std::string("Could not bind peer TCP port ") + std::to_string(port) +
                ": " + strerror(errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (listen(listen_fd_, 16) < 0) {
        error = std::string("Could not listen on peer TCP port: ") + strerror(errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    try {
        client_workers_.reserve(MAX_CLIENT_WORKERS);
        worker_ = std::thread(&PeerServer::serve, this);
    } catch (const std::exception &exception) {
        running_.store(false);
        close(listen_fd_);
        listen_fd_ = -1;
        error = std::string("Could not start peer server: ") + exception.what();
        return false;
    }
    return true;
}

void PeerServer::stop()
{
    running_.store(false);
    if (listen_fd_ >= 0)
        shutdown(listen_fd_, SHUT_RDWR);
    if (worker_.joinable())
        worker_.join();
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void PeerServer::serve()
{
    while (running_.load()) {
        for (auto it = client_workers_.begin(); it != client_workers_.end();) {
            if (it->done->load()) {
                if (it->thread.joinable())
                    it->thread.join();
                it = client_workers_.erase(it);
            } else {
                ++it;
            }
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listen_fd_, &readable);
        struct timeval timeout = {1, 0};
        const int ready = select(listen_fd_ + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            if (!running_.load())
                break;
            continue;
        }
        if (ready == 0 || !FD_ISSET(listen_fd_, &readable))
            continue;

        const int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (!running_.load())
                break;
            continue;
        }
        if (client_workers_.size() >= MAX_CLIENT_WORKERS) {
            send_peer_status(client_fd, "NOK");
            close(client_fd);
            continue;
        }

        std::shared_ptr<std::atomic<bool>> done;
        try {
            done = std::make_shared<std::atomic<bool>>(false);
        } catch (const std::exception &) {
            send_peer_status(client_fd, "NOK");
            close(client_fd);
            continue;
        }

        ClientWorker client;
        client.done = done;
        try {
            client.thread = std::thread([client_fd, done]() {
                serve_connection(client_fd);
                close(client_fd);
                done->store(true);
            });
        } catch (const std::exception &) {
            send_peer_status(client_fd, "NOK");
            close(client_fd);
            continue;
        }
        client_workers_.push_back(std::move(client));
    }

    for (ClientWorker &client : client_workers_) {
        if (client.thread.joinable())
            client.thread.join();
    }
    client_workers_.clear();
}

int download_peer_file(const std::string &peer_ip, int peer_port,
                       const std::string &filename,
                       const std::string &destination,
                       unsigned long long *received_size,
                       std::string &error)
{
    if (!valid_filename(filename.c_str())) {
        error = "Invalid remote filename.";
        return -1;
    }
    if (!valid_filename(destination.c_str())) {
        error = "Invalid destination filename.";
        return -1;
    }
    if (peer_port < 1 || peer_port > 65535) {
        error = "Peer TCP port must be between 1 and 65535.";
        return -1;
    }

    int socket_fd = -1;
    if (!connect_ipv4(peer_ip, peer_port, socket_fd, error))
        return -1;

    if (set_tcp_io_timeouts(socket_fd, 30) < 0) {
        error = std::string("Could not configure peer socket timeout: ") + strerror(errno);
        close(socket_fd);
        return -1;
    }

    const std::string request = "GET " + filename + "\n";
    const PeerDeadline deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(TCP_CONTROL_DEADLINE_MS);
    if (write_all_until(socket_fd, request.data(), request.size(), deadline) < 0) {
        error = std::string("Could not send download request: ") + strerror(errno);
        close(socket_fd);
        return -1;
    }
    shutdown(socket_fd, SHUT_WR);

    std::string response;
    if (!read_line_until(socket_fd, response, 128, deadline)) {
        error = "Peer sent an incomplete response.";
        close(socket_fd);
        return -1;
    }

    std::istringstream header(response);
    std::string prefix;
    std::string status;
    long long file_size = -1;
    std::string extra;
    if (!(header >> prefix >> status) || prefix != "RGT") {
        error = "Peer sent an invalid file header.";
        close(socket_fd);
        return -1;
    }
    if (status != "OK") {
        error = status == "NOK" ? "Peer does not have that file." :
                status == "ERR" ? "Peer rejected the request." :
                "Peer sent an invalid transfer status.";
        if (header >> extra)
            error = "Peer sent an invalid error response.";
        close(socket_fd);
        return -1;
    }
    if (!(header >> file_size) || (header >> extra) || file_size < 0 ||
        file_size > MAX_FILE_SIZE) {
        error = "Peer sent an invalid file size.";
        close(socket_fd);
        return -1;
    }

    const int output_fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output_fd < 0) {
        error = std::string("Cannot create destination file ") + destination +
                ": " + strerror(errno);
        close(socket_fd);
        return -1;
    }

    char buffer[16384];
    long long remaining = file_size;
    while (remaining > 0) {
        const size_t wanted = remaining < static_cast<long long>(sizeof(buffer))
                                  ? static_cast<size_t>(remaining)
                                  : sizeof(buffer);
        const ssize_t received = read(socket_fd, buffer, wanted);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0) {
            error = "Peer disconnected before the complete file arrived.";
            close(socket_fd);
            close(output_fd);
            unlink(destination.c_str());
            return -1;
        }
        if (write_all(output_fd, buffer, static_cast<size_t>(received)) < 0) {
            error = std::string("Could not write downloaded file: ") + strerror(errno);
            close(socket_fd);
            close(output_fd);
            unlink(destination.c_str());
            return -1;
        }
        remaining -= received;
    }

    close(socket_fd);
    if (close(output_fd) < 0) {
        error = std::string("Could not finish downloaded file: ") + strerror(errno);
        unlink(destination.c_str());
        return -1;
    }
    if (received_size != nullptr)
        *received_size = static_cast<unsigned long long>(file_size);
    return 0;
}
