#include "user.h"

#include "commands.h"
#include "network.h"
#include "peer.h"

#include <arpa/inet.h>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char *DEFAULT_DS_IP = "193.136.138.142";
constexpr int DEFAULT_DS_PORT = 59000;

void usage(const char *program)
{
    std::cerr << "Usage: " << program << " -m peerport [-n DSIP] [-p DSport]\n";
}

void print_help()
{
    std::cout << "Commands:\n"
              << "  login UID password\n"
              << "  publish filename label\n"
              << "  remove filename\n"
              << "  list\n"
              << "  versions filename\n"
              << "  where filename\n"
              << "  get filename\n"
              << "  logout | unregister | help | exit\n"
              << "Files are read from and saved to the current directory.\n";
}

void run_user_interface(int udp_fd, const struct sockaddr_in &server,
                        UserState &user, int peer_port)
{
    std::string line;
    print_help();

    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            if (user.logged_in) {
                std::cout << "Input closed; attempting to log out the active user.\n";
                command_logout(udp_fd, &server, &user);
                if (user.logged_in)
                    std::cerr << "Logout could not be confirmed; the Directory Server may still consider this user active.\n";
            }
            return;
        }
        if (line.size() > MAX_LINE - 1) {
            std::cout << "Command is too long (maximum " << MAX_LINE - 1
                      << " characters).\n";
            continue;
        }

        std::istringstream input(line);
        std::vector<std::string> fields;
        std::string field;
        while (input >> field)
            fields.push_back(field);
        if (fields.empty())
            continue;

        const std::string &command = fields[0];
        if (command == "help") {
            if (fields.size() != 1)
                std::cout << "Usage: help\n";
            else
                print_help();
        } else if (command == "login") {
            if (fields.size() != 3) {
                std::cout << "Usage: login UID password\n";
            } else if (user.logged_in) {
                std::cout << "A user is already logged in. Logout first.\n";
            } else {
                command_login(udp_fd, &server, &user,
                              fields[1].c_str(), fields[2].c_str(), peer_port);
            }
        } else if (command == "logout") {
            if (fields.size() != 1)
                std::cout << "Usage: logout\n";
            else
                command_logout(udp_fd, &server, &user);
        } else if (command == "unregister") {
            if (fields.size() != 1)
                std::cout << "Usage: unregister\n";
            else
                command_unregister(udp_fd, &server, &user);
        } else if (command == "publish") {
            if (fields.size() != 3)
                std::cout << "Usage: publish filename label\n";
            else
                command_publish(udp_fd, &server, &user,
                                fields[1].c_str(), fields[2].c_str());
        } else if (command == "remove") {
            if (fields.size() != 2)
                std::cout << "Usage: remove filename\n";
            else
                command_remove(udp_fd, &server, &user, fields[1].c_str());
        } else if (command == "list") {
            if (fields.size() != 1)
                std::cout << "Usage: list\n";
            else
                command_list(udp_fd, &server);
        } else if (command == "versions") {
            if (fields.size() != 2)
                std::cout << "Usage: versions filename\n";
            else
                command_versions(&server, ntohs(server.sin_port), fields[1].c_str());
        } else if (command == "where") {
            if (fields.size() != 2)
                std::cout << "Usage: where filename\n";
            else
                command_where(&server, ntohs(server.sin_port), fields[1].c_str());
        } else if (command == "get") {
            if (fields.size() != 2)
                std::cout << "Usage: get filename\n";
            else
                command_get(udp_fd, &server, ntohs(server.sin_port),
                            fields[1].c_str());
        } else if (command == "exit") {
            if (fields.size() != 1) {
                std::cout << "Usage: exit\n";
            } else if (user.logged_in) {
                std::cout << "Please execute logout before exit.\n";
            } else {
                return;
            }
        } else {
            std::cout << "Unknown command. Type 'help' to see the available commands.\n";
        }
    }
}

} 

int main(int argc, char *argv[])
{
    const char *ds_ip = DEFAULT_DS_IP;
    int ds_port = DEFAULT_DS_PORT;
    int peer_port = -1;
    int option;

    while ((option = getopt(argc, argv, "m:n:p:")) != -1) {
        switch (option) {
        case 'm':
            if (!valid_port(optarg, &peer_port)) {
                std::cerr << "Invalid peer port: " << optarg << '\n';
                return 1;
            }
            break;
        case 'n':
            ds_ip = optarg;
            break;
        case 'p':
            if (!valid_port(optarg, &ds_port)) {
                std::cerr << "Invalid Directory Server port: " << optarg << '\n';
                return 1;
            }
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind != argc) {
        usage(argv[0]);
        return 1;
    }
    if (peer_port == -1) {
        std::cerr << "Error: -m peerport is mandatory.\n";
        usage(argv[0]);
        return 1;
    }

    struct sockaddr_in server;
    if (make_server_address(&server, ds_ip, ds_port) < 0)
        return 1;

    const int udp_fd = create_udp_socket();
    if (udp_fd < 0)
        return 1;

    std::signal(SIGPIPE, SIG_IGN);
    PeerServer peer_server;
    std::string peer_error;
    if (!peer_server.start(peer_port, peer_error)) {
        std::cerr << "Peer server startup failed: " << peer_error << '\n';
        close(udp_fd);
        return 1;
    }

    std::cout << "NetBoX User (C++17)\n"
              << "Directory Server: " << ds_ip << ':' << ds_port
              << " (UDP and TCP)\n"
              << "Peer TCP server listening on port " << peer_port << "\n";

    UserState user = {{0}, {0}, 0};
    run_user_interface(udp_fd, server, user, peer_port);
    close(udp_fd);
    return 0;
}
