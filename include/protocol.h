#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <netinet/in.h>
#include <stddef.h>

#define UID_LEN 6
#define PASSWORD_LEN 8
#define MAX_LINE 512
#define MAX_MESSAGE 8192
#define MAX_FILENAME 24
#define MAX_LABEL 20
#define MAX_FILE_SIZE 10000000

int valid_port(const char *s, int *port);
int valid_uid(const char *uid);
int valid_password(const char *password);
int valid_filename(const char *filename);
int valid_label(const char *label);

int send_request(int fd, const struct sockaddr_in *server,
                 const char *request, char *reply, size_t reply_size);
int parse_reply(const char *reply, const char *expected_prefix,
                char *status, size_t status_size);

void print_login_result(const char *status);
void print_logout_result(const char *status);
void print_unregister_result(const char *status);

#endif
