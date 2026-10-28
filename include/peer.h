#ifndef PEER_H
#define PEER_H

#include <atomic>
#include <string>
#include <thread>

class PeerServer {
public:
    PeerServer() = default;
    PeerServer(const PeerServer &) = delete;
    PeerServer &operator=(const PeerServer &) = delete;
    ~PeerServer();

    bool start(int port, std::string &error);
    void stop();

private:
    void serve();

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

int download_peer_file(const std::string &peer_ip, int peer_port,
                       const std::string &filename,
                       const std::string &destination,
                       std::string &error);

#endif
