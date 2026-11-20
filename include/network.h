#ifndef NETWORK_H
#define NETWORK_H

#include <netinet/in.h>
#include <string>

int create_udp_socket(void);
int make_server_address(struct sockaddr_in *address,
                        const char *ds_ip, int ds_port);
int connect_tcp_with_timeout(int fd, const struct sockaddr_in *address,
                             int timeout_ms);
int set_tcp_io_timeouts(int fd, int timeout_seconds);
int tcp_exchange(const struct sockaddr_in *server, int port,
                 const char *request, std::string &reply);

#endif
