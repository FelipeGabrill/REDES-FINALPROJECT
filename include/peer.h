#ifndef PEER_H
#define PEER_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class PeerServer {
public:
    PeerServer() = default;
    PeerServer(const PeerServer &) = delete;
    PeerServer &operator=(const PeerServer &) = delete;
    ~PeerServer();

    bool start(int port, std::string &error);
    void stop();

private:
    struct ClientWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void serve();

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::vector<ClientWorker> client_workers_;
};

int download_peer_file(const std::string &peer_ip, int peer_port,
                       const std::string &filename,
                       const std::string &destination,
                       unsigned long long *received_size,
                       std::string &error);

#endif
