#include "peer.h"

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

bool read_line(int fd, std::string &line, size_t max_length)
{
    line.clear();
    while (line.size() < max_length) {
        char byte = '\0';
        const ssize_t received = read(fd, &byte, 1);
        if (received < 0 && errno == EINTR)
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

void send_peer_error(int fd)
{
    static const char response[] = "ERR\n";
    write_all(fd, response, sizeof(response) - 1);
}

void serve_connection(int fd)
{
    const struct timeval timeout = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string request;
    if (!read_line(fd, request, 128)) {
        send_peer_error(fd);
        return;
    }

    std::istringstream input(request);
    std::string operation;
    std::string filename;
    std::string extra;
    if (!(input >> operation >> filename) || (input >> extra) ||
        operation != "GET" || !valid_filename(filename.c_str())) {
        send_peer_error(fd);
        return;
    }

    const int file_fd = open(filename.c_str(), O_RDONLY);
    if (file_fd < 0) {
        send_peer_error(fd);
        return;
    }

    struct stat info;
    if (fstat(file_fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > MAX_FILE_SIZE) {
        close(file_fd);
        send_peer_error(fd);
        return;
    }

    char header[64];
    const int header_length = snprintf(header, sizeof(header), "OK %lld\n",
                                       static_cast<long long>(info.st_size));
    if (header_length < 0 || static_cast<size_t>(header_length) >= sizeof(header) ||
        write_all(fd, header, static_cast<size_t>(header_length)) < 0) {
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
    if (connect(fd, reinterpret_cast<const struct sockaddr *>(&address),
                sizeof(address)) < 0) {
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
        serve_connection(client_fd);
        close(client_fd);
    }
}

int download_peer_file(const std::string &peer_ip, int peer_port,
                       const std::string &filename,
                       const std::string &destination,
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

    const int output_fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output_fd < 0) {
        error = std::string("Cannot create destination file ") + destination +
                ": " + strerror(errno);
        return -1;
    }

    int socket_fd = -1;
    if (!connect_ipv4(peer_ip, peer_port, socket_fd, error)) {
        close(output_fd);
        unlink(destination.c_str());
        return -1;
    }

    const std::string request = "GET " + filename + "\n";
    if (write_all(socket_fd, request.data(), request.size()) < 0) {
        error = std::string("Could not send download request: ") + strerror(errno);
        close(socket_fd);
        close(output_fd);
        unlink(destination.c_str());
        return -1;
    }
    shutdown(socket_fd, SHUT_WR);

    std::string response;
    if (!read_line(socket_fd, response, 128)) {
        error = "Peer sent an incomplete response.";
        close(socket_fd);
        close(output_fd);
        unlink(destination.c_str());
        return -1;
    }

    std::istringstream header(response);
    std::string status;
    long long file_size = -1;
    std::string extra;
    if (!(header >> status) || status != "OK" || !(header >> file_size) ||
        (header >> extra) || file_size < 0 || file_size > MAX_FILE_SIZE) {
        error = status == "ERR" ? "Peer does not have that file." :
                "Peer sent an invalid file header.";
        close(socket_fd);
        close(output_fd);
        unlink(destination.c_str());
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
    return 0;
}
