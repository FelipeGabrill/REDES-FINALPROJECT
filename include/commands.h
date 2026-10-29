#ifndef COMMANDS_H
#define COMMANDS_H

#include "user.h"

#include <string>

struct PeerEndpoint {
    std::string uid;
    std::string ip;
    int port;
};

int command_login(int fd, const struct sockaddr_in *server,
                  UserState *user, const char *uid,
                  const char *password, int peer_port);
int command_logout(int fd, const struct sockaddr_in *server, UserState *user);
int command_unregister(int fd, const struct sockaddr_in *server, UserState *user);
int command_publish(int fd, const struct sockaddr_in *server, UserState *user,
                    const char *filename, const char *label);
int command_remove(int fd, const struct sockaddr_in *server, UserState *user,
                   const char *filename);
int command_list(int fd, const struct sockaddr_in *server);
int command_versions(const struct sockaddr_in *server, int ds_port,
                     const char *filename);
int command_where(const struct sockaddr_in *server, int ds_port,
                  const char *filename);
int command_get(int udp_fd, const struct sockaddr_in *server, int ds_port,
                const char *filename);

#endif
