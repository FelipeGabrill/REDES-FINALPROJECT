#ifndef PEER_H
#define PEER_H

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
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
        std::atomic<bool> done{false};
    };

    void serve(int port, std::promise<std::string> startup_result);
    void remove_active_client(int fd);

    std::mutex lifecycle_mutex_;
    std::mutex workers_mutex_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::vector<std::shared_ptr<ClientWorker>> client_workers_;
    std::vector<int> active_client_fds_;
};

int download_peer_file(const std::string &peer_ip, int peer_port,
                       const std::string &filename,
                       const std::string &destination,
                       unsigned long long *received_size,
                       std::string &error);

#endif
