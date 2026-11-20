#include "peer.h"

#include "protocol.h"
#include "network.h"

#include <arpa/inet.h>
#include <chrono>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <netinet/in.h>
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
constexpr int TCP_IDLE_TIMEOUT_SECONDS = 30;
constexpr int TCP_IDLE_TIMEOUT_MS = TCP_IDLE_TIMEOUT_SECONDS * 1000;

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

int write_all_with_idle_timeout(int fd, const char *data, size_t length,
                                int idle_timeout_ms)
{
    size_t offset = 0;
    PeerDeadline deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(idle_timeout_ms);
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
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(idle_timeout_ms);
    }
    return 0;
}

bool split_protocol_tokens(const std::string &line,
                           std::vector<std::string> &tokens)
{
    tokens.clear();
    if (line.empty())
        return false;

    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i < line.size()) {
            const unsigned char character = static_cast<unsigned char>(line[i]);
            if (character < ' ' || character > '~')
                return false;
        }
        if (i != line.size() && line[i] != ' ')
            continue;
        if (i == start)
            return false;
        tokens.emplace_back(line, start, i - start);
        start = i + 1;
    }
    return true;
}

bool parse_file_size(const std::string &text, unsigned long long &value)
{
    if (text.empty() || text.size() > 8)
        return false;
    value = 0;
    for (const unsigned char character : text) {
        if (character < '0' || character > '9')
            return false;
        value = value * 10 + static_cast<unsigned long long>(character - '0');
        if (value > static_cast<unsigned long long>(MAX_FILE_SIZE))
            return false;
    }
    return true;
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
        if (byte == '\r' || byte == '\0' ||
            static_cast<unsigned char>(byte) < static_cast<unsigned char>(' '))
            return false;
        line.push_back(byte);
    }
    return false;
}

void send_peer_status(int fd, const char *status)
{
    char response[32];
    const int length = snprintf(response, sizeof(response), "RGT %s\n", status);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(response) ||
        prevent_sigpipe(fd) < 0)
        return;

    size_t offset = 0;
    while (offset < static_cast<size_t>(length)) {
        const ssize_t sent = send(fd, response + offset,
                                  static_cast<size_t>(length) - offset,
                                  peer_send_flags());
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return;
        offset += static_cast<size_t>(sent);
    }
}

void serve_connection(int fd)
{
    if (set_tcp_io_timeouts(fd, TCP_IDLE_TIMEOUT_SECONDS) < 0)
        return;
    if (prevent_sigpipe(fd) < 0)
        return;
    const PeerDeadline request_deadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(TCP_CONTROL_DEADLINE_MS);

    std::string request;
    if (!read_line_until(fd, request, 128, request_deadline)) {
        send_peer_status(fd, "ERR");
        return;
    }

    std::vector<std::string> fields;
    if (!split_protocol_tokens(request, fields) || fields.size() != 2 ||
        fields[0] != "GET" || !valid_filename(fields[1].c_str())) {
        send_peer_status(fd, "ERR");
        return;
    }

    const std::string &filename = fields[1];
    const int file_fd = open(filename.c_str(), O_RDONLY | O_NOFOLLOW);
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
    const PeerDeadline header_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::milliseconds(TCP_CONTROL_DEADLINE_MS);
    if (header_length < 0 || static_cast<size_t>(header_length) >= sizeof(header) ||
        write_all_until(fd, header, static_cast<size_t>(header_length),
                        header_deadline) < 0) {
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
        if (received <= 0 ||
            write_all_with_idle_timeout(fd, buffer, static_cast<size_t>(received),
                                        TCP_IDLE_TIMEOUT_MS) < 0)
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
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (port < 1 || port > 65535) {
        error = "Peer TCP port must be between 1 and 65535.";
        return false;
    }
    if (running_.load()) {
        error = "Peer TCP server is already running.";
        return false;
    }
    if (server_thread_.joinable())
        server_thread_.join();

    running_.store(true);
    std::promise<std::string> startup_promise;
    std::future<std::string> startup_result = startup_promise.get_future();
    try {
        server_thread_ = std::thread(&PeerServer::serve, this, port,
                                     std::move(startup_promise));
    } catch (const std::exception &exception) {
        running_.store(false);
        error = std::string("Could not start peer server: ") + exception.what();
        return false;
    }

    error = startup_result.get();
    if (!error.empty()) {
        if (server_thread_.joinable())
            server_thread_.join();
        return false;
    }
    return true;
}

void PeerServer::stop()
{
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            running_.store(false);
            if (listen_fd_ >= 0)
                shutdown(listen_fd_, SHUT_RDWR);
            for (const int client_fd : active_client_fds_) {
                if (client_fd >= 0)
                    shutdown(client_fd, SHUT_RDWR);
            }
        }
        if (server_thread_.joinable())
            server_thread_.join();
    }
}

void PeerServer::remove_active_client(int fd)
{
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (int &client_fd : active_client_fds_) {
        if (client_fd == fd) {
            client_fd = -1;
            return;
        }
    }
}

void PeerServer::serve(int port, std::promise<std::string> startup_result)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd < 0) {
        running_.store(false);
        startup_result.set_value(
            std::string("Could not create peer TCP socket: ") + strerror(errno));
        return;
    }

    auto fail_start = [&](const std::string &message) {
        close(listener_fd);
        running_.store(false);
        startup_result.set_value(message);
    };

    int reuse_address = 1;
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) < 0) {
        fail_start(std::string("Could not configure peer socket: ") + strerror(errno));
        return;
    }

    const int listener_flags = fcntl(listener_fd, F_GETFL, 0);
    if (listener_flags < 0 ||
        fcntl(listener_fd, F_SETFL, listener_flags | O_NONBLOCK) < 0) {
        fail_start(std::string("Could not configure peer socket: ") + strerror(errno));
        return;
    }
    if (listener_fd >= FD_SETSIZE) {
        fail_start("Peer TCP socket exceeds the select descriptor limit.");
        return;
    }

    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listener_fd, reinterpret_cast<const struct sockaddr *>(&address),
             sizeof(address)) < 0) {
        fail_start(std::string("Could not bind peer TCP port ") + std::to_string(port) +
                   ": " + strerror(errno));
        return;
    }
    if (listen(listener_fd, static_cast<int>(MAX_CLIENT_WORKERS)) < 0) {
        fail_start(std::string("Could not listen on peer TCP port: ") + strerror(errno));
        return;
    }

    try {
        client_workers_.clear();
        client_workers_.reserve(MAX_CLIENT_WORKERS);
        active_client_fds_.assign(MAX_CLIENT_WORKERS, -1);
    } catch (const std::exception &exception) {
        fail_start(std::string("Could not prepare peer workers: ") + exception.what());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        listen_fd_ = listener_fd;
    }
    startup_result.set_value("");

    while (running_.load()) {
        for (auto it = client_workers_.begin(); it != client_workers_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                if ((*it)->thread.joinable())
                    (*it)->thread.join();
                it = client_workers_.erase(it);
            } else {
                ++it;
            }
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener_fd, &readable);
        struct timeval timeout = {1, 0};
        const int ready = select(listener_fd + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            if (running_.load())
                perror("select on peer server socket");
            break;
        }
        if (ready == 0 || !FD_ISSET(listener_fd, &readable))
            continue;

        const int client_fd = accept(listener_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (running_.load())
                perror("accept on peer server socket");
            break;
        }

        bool reject_busy = false;
        bool reject_request = false;
        bool close_without_reply = false;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            if (!running_.load()) {
                close_without_reply = true;
            } else {
                for (auto it = client_workers_.begin(); it != client_workers_.end();) {
                    if ((*it)->done.load(std::memory_order_acquire)) {
                        if ((*it)->thread.joinable())
                            (*it)->thread.join();
                        it = client_workers_.erase(it);
                    } else {
                        ++it;
                    }
                }

                int client_slot = -1;
                for (size_t i = 0; i < active_client_fds_.size(); ++i) {
                    if (active_client_fds_[i] < 0) {
                        client_slot = static_cast<int>(i);
                        break;
                    }
                }
                if (client_workers_.size() >= MAX_CLIENT_WORKERS || client_slot < 0) {
                    reject_busy = true;
                } else {
                    active_client_fds_[static_cast<size_t>(client_slot)] = client_fd;
                    try {
                        auto client = std::make_shared<ClientWorker>();
                        client_workers_.push_back(client);
                        try {
                            client->thread = std::thread([this, client_fd, client]() {
                                try {
                                    serve_connection(client_fd);
                                } catch (...) {
                                    send_peer_status(client_fd, "ERR");
                                }
                                remove_active_client(client_fd);
                                close(client_fd);
                                client->done.store(true, std::memory_order_release);
                            });
                        } catch (...) {
                            client_workers_.pop_back();
                            active_client_fds_[static_cast<size_t>(client_slot)] = -1;
                            reject_request = true;
                        }
                    } catch (...) {
                        active_client_fds_[static_cast<size_t>(client_slot)] = -1;
                        reject_request = true;
                    }
                }
            }
        }
        if (close_without_reply || reject_busy || reject_request) {
            if (!close_without_reply)
                send_peer_status(client_fd, reject_busy ? "NOK" : "ERR");
            close(client_fd);
        }
    }

    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        running_.store(false);
        for (const int client_fd : active_client_fds_) {
            if (client_fd >= 0)
                shutdown(client_fd, SHUT_RDWR);
        }
        if (listen_fd_ == listener_fd) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    for (const auto &client : client_workers_) {
        if (client->thread.joinable())
            client->thread.join();
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

    if (set_tcp_io_timeouts(socket_fd, TCP_IDLE_TIMEOUT_SECONDS) < 0) {
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

    std::vector<std::string> fields;
    if (!split_protocol_tokens(response, fields) || fields.size() < 2 ||
        fields[0] != "RGT") {
        error = "Peer sent an invalid file header.";
        close(socket_fd);
        return -1;
    }
    if (fields[1] != "OK") {
        error = fields[1] == "NOK" ? "Peer does not have that file." :
                fields[1] == "ERR" ? "Peer rejected the request." :
                "Peer sent an invalid transfer status.";
        if (fields.size() != 2)
            error = "Peer sent an invalid error response.";
        close(socket_fd);
        return -1;
    }
    unsigned long long file_size = 0;
    if (fields.size() != 3 || !parse_file_size(fields[2], file_size)) {
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
    unsigned long long remaining = file_size;
    while (remaining > 0) {
        const size_t wanted = remaining < static_cast<unsigned long long>(sizeof(buffer))
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
        *received_size = file_size;
    return 0;
}
