#include "commands.h"

#include "network.h"
#include "peer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

void report_resource_status(const char *operation, const char *status)
{
    if (strcmp(status, "OK") == 0)
        printf("%s successful.\n", operation);
    else if (strcmp(status, "NLG") == 0)
        printf("%s failed: user is not logged in.\n", operation);
    else if (strcmp(status, "UNR") == 0)
        printf("%s failed: user is not registered.\n", operation);
    else if (strcmp(status, "WRP") == 0)
        printf("%s failed: incorrect password.\n", operation);
    else if (strcmp(status, "NOK") == 0)
        printf("%s failed: resource not found or request rejected.\n", operation);
    else if (strcmp(status, "ERR") == 0)
        printf("%s failed: invalid request or parameters.\n", operation);
    else
        printf("%s failed: status %s.\n", operation, status);
}

int udp_resource_request(int fd, const struct sockaddr_in *server,
                         const char *request, const char *prefix,
                         const char *operation)
{
    char reply[MAX_MESSAGE];
    char status[16];
    if (send_request(fd, server, request, reply, sizeof(reply)) < 0)
        return -1;
    if (parse_reply(reply, prefix, status, sizeof(status)) < 0)
        return -1;
    report_resource_status(operation, status);
    return 0;
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

} 

int command_login(int fd, const struct sockaddr_in *server,
                  UserState *user, const char *uid,
                  const char *password, int peer_port)
{
    if (user == nullptr || !valid_uid(uid)) {
        printf("Invalid UID: exactly 6 decimal digits required.\n");
        return 0;
    }
    if (!valid_password(password)) {
        printf("Invalid password: exactly 8 ASCII letters or digits required.\n");
        return 0;
    }
    if (peer_port < 1 || peer_port > 65535) {
        printf("Invalid peer TCP port.\n");
        return 0;
    }

    char request[MAX_MESSAGE];
    char reply[MAX_MESSAGE];
    char status[16];
    snprintf(request, sizeof(request), "LIN %s %s %d\n", uid, password, peer_port);
    if (send_request(fd, server, request, reply, sizeof(reply)) < 0 ||
        parse_reply(reply, "RLI", status, sizeof(status)) < 0)
        return -1;

    print_login_result(status);
    if (strcmp(status, "OK") == 0 || strcmp(status, "REG") == 0) {
        snprintf(user->uid, sizeof(user->uid), "%s", uid);
        snprintf(user->password, sizeof(user->password), "%s", password);
        user->logged_in = 1;
    }
    return 0;
}

int command_logout(int fd, const struct sockaddr_in *server, UserState *user)
{
    if (user == nullptr || !user->logged_in) {
        printf("User is not logged in.\n");
        return 0;
    }

    char request[MAX_MESSAGE];
    char reply[MAX_MESSAGE];
    char status[16];
    snprintf(request, sizeof(request), "LOU %s %s\n", user->uid, user->password);
    if (send_request(fd, server, request, reply, sizeof(reply)) < 0 ||
        parse_reply(reply, "RLO", status, sizeof(status)) < 0)
        return -1;

    print_logout_result(status);
    if (strcmp(status, "OK") == 0)
        user->logged_in = 0;
    return 0;
}

int command_unregister(int fd, const struct sockaddr_in *server, UserState *user)
{
    if (user == nullptr || !user->logged_in) {
        printf("User is not logged in.\n");
        return 0;
    }

    char request[MAX_MESSAGE];
    char reply[MAX_MESSAGE];
    char status[16];
    snprintf(request, sizeof(request), "UNR %s %s\n", user->uid, user->password);
    if (send_request(fd, server, request, reply, sizeof(reply)) < 0 ||
        parse_reply(reply, "RUR", status, sizeof(status)) < 0)
        return -1;

    print_unregister_result(status);
    if (strcmp(status, "OK") == 0)
        user->logged_in = 0;
    return 0;
}

int command_publish(int fd, const struct sockaddr_in *server, UserState *user,
                    const char *filename, const char *label)
{
    if (user == nullptr || !user->logged_in) {
        printf("User is not logged in.\n");
        return 0;
    }
    if (!valid_filename(filename)) {
        printf("Invalid filename: use up to 24 ASCII characters with a 3-character extension.\n");
        return 0;
    }
    if (!valid_label(label)) {
        printf("Invalid label: use 1 to 20 ASCII letters, digits, '-' or '_'.\n");
        return 0;
    }

    struct stat info;
    if (stat(filename, &info) < 0 || !S_ISREG(info.st_mode)) {
        printf("File does not exist in the current directory: %s\n", filename);
        return 0;
    }
    if (info.st_size < 0 || info.st_size > MAX_FILE_SIZE) {
        printf("File size must be between 0 and 10,000,000 bytes.\n");
        return 0;
    }

    char request[MAX_MESSAGE];
    snprintf(request, sizeof(request), "PUB %s %s %s %lld %s\n",
             user->uid, user->password, filename,
             static_cast<long long>(info.st_size), label);
    return udp_resource_request(fd, server, request, "RPB", "Publish");
}

int command_remove(int fd, const struct sockaddr_in *server, UserState *user,
                   const char *filename)
{
    if (user == nullptr || !user->logged_in) {
        printf("User is not logged in.\n");
        return 0;
    }
    if (!valid_filename(filename)) {
        printf("Invalid filename.\n");
        return 0;
    }

    char request[MAX_MESSAGE];
    snprintf(request, sizeof(request), "REM %s %s %s\n",
             user->uid, user->password, filename);
    return udp_resource_request(fd, server, request, "RRM", "Remove");
}

int command_list(int fd, const struct sockaddr_in *server)
{
    char reply[MAX_MESSAGE];
    if (send_request(fd, server, "LST\n", reply, sizeof(reply)) < 0)
        return -1;

    std::istringstream input(reply);
    std::string prefix;
    std::string status;
    if (!(input >> prefix >> status) || prefix != "RLS") {
        fprintf(stderr, "Invalid resource list reply from Directory Server.\n");
        return -1;
    }
    if (status == "NOK") {
        std::string extra;
        if (input >> extra) {
            fprintf(stderr, "Unexpected fields in empty resource list reply.\n");
            return -1;
        }
        printf("No resources are currently known.\n");
        return 0;
    }
    if (status != "OK") {
        printf("List failed: status %s.\n", status.c_str());
        return 0;
    }

    std::vector<std::string> names;
    std::set<std::string> unique_names;
    std::string name;
    while (input >> name) {
        if (!valid_filename(name.c_str())) {
            fprintf(stderr, "Directory Server returned an invalid filename.\n");
            return -1;
        }
        if (names.size() == 50) {
            fprintf(stderr, "Directory Server returned more than 50 filenames.\n");
            return -1;
        }
        if (unique_names.insert(name).second)
            names.push_back(name);
    }
    if (names.empty()) {
        printf("No resources are currently known.\n");
        return 0;
    }

    printf("Known resources (%zu):\n", names.size());
    for (const std::string &entry : names)
        printf("  %s\n", entry.c_str());
    return 0;
}

int command_versions(const struct sockaddr_in *server, int ds_port,
                     const char *filename)
{
    if (!valid_filename(filename)) {
        printf("Invalid filename.\n");
        return 0;
    }

    char request[64];
    char reply[MAX_MESSAGE];
    snprintf(request, sizeof(request), "VRS %s\n", filename);
    if (tcp_exchange(server, ds_port, request, reply, sizeof(reply)) < 0)
        return -1;

    std::istringstream input(reply);
    std::string prefix;
    std::string status;
    if (!(input >> prefix >> status) || prefix != "RVR") {
        fprintf(stderr, "Invalid versions reply from Directory Server.\n");
        return -1;
    }
    if (status == "NOK") {
        std::string extra;
        if (input >> extra) {
            fprintf(stderr, "Unexpected fields in empty versions reply.\n");
            return -1;
        }
        printf("No available versions for %s.\n", filename);
        return 0;
    }
    if (status != "OK") {
        printf("Versions request failed: status %s.\n", status.c_str());
        return 0;
    }

    struct Version {
        std::string uid;
        std::string size;
        std::string label;
        std::string publication_time;
        std::string availability;
    };
    std::vector<Version> versions;
    Version version;
    while (input >> version.uid) {
        if (!(input >> version.size >> version.label >> version.publication_time >>
              version.availability)) {
            fprintf(stderr, "Incomplete version entry from Directory Server.\n");
            return -1;
        }
        unsigned long long size = 0;
        if (!valid_uid(version.uid.c_str()) || !parse_file_size(version.size, size) ||
            !valid_label(version.label.c_str()) ||
            (version.availability != "AVL" && version.availability != "NAV")) {
            fprintf(stderr, "Invalid version entry from Directory Server.\n");
            return -1;
        }
        versions.push_back(version);
    }

    printf("Versions of %s:\n", filename);
    if (versions.empty()) {
        printf("  No peer versions were returned.\n");
        return 0;
    }
    for (const Version &entry : versions) {
        printf("  UID %s | %s bytes | label %s | published %s | %s\n",
               entry.uid.c_str(), entry.size.c_str(), entry.label.c_str(),
               entry.publication_time.c_str(),
               entry.availability == "AVL" ? "online" : "offline");
    }
    return 0;
}

int command_where(const struct sockaddr_in *server, int ds_port,
                  const char *filename)
{
    const int result = command_versions(server, ds_port, filename);
    if (result == 0) {
        printf("Phase II location lookup is incomplete: the specified RVR fields do not include a peer IP address or TCP port.\n");
    }
    return result;
}

int command_get(const char *filename, const char *peer_ip, const char *peer_port_text,
                const char *destination)
{
    int peer_port = 0;
    if (!valid_filename(filename)) {
        printf("Invalid remote filename.\n");
        return 0;
    }
    if (!valid_port(peer_port_text, &peer_port)) {
        printf("Invalid peer port: expected a decimal integer from 1 to 65535.\n");
        return 0;
    }

    const std::string output_name = destination == nullptr ? filename : destination;
    std::string error;
    if (download_peer_file(peer_ip, peer_port, filename, output_name, error) < 0) {
        printf("Download failed: %s\n", error.c_str());
        return -1;
    }
    printf("Downloaded %s to %s successfully.\n", filename, output_name.c_str());
    return 0;
}
