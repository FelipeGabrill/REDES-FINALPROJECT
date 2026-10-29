#include "commands.h"

#include "network.h"
#include "peer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
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

bool status_is_allowed(const char *status,
                       std::initializer_list<const char *> allowed_statuses)
{
    for (const char *allowed : allowed_statuses) {
        if (strcmp(status, allowed) == 0)
            return true;
    }
    return false;
}

void clear_user_session(UserState *user)
{
    if (user == nullptr)
        return;
    memset(user->uid, 0, sizeof(user->uid));
    memset(user->password, 0, sizeof(user->password));
    user->logged_in = 0;
}

int udp_resource_request(int fd, const struct sockaddr_in *server,
                         const char *request, const char *prefix,
                         const char *operation,
                         std::initializer_list<const char *> allowed_statuses)
{
    char reply[MAX_MESSAGE];
    char status[16];
    if (send_request(fd, server, request, reply, sizeof(reply)) < 0)
        return -1;
    if (parse_reply(reply, prefix, status, sizeof(status)) < 0)
        return -1;
    if (!status_is_allowed(status, allowed_statuses)) {
        fprintf(stderr, "Unexpected %s status from Directory Server: %s.\n",
                prefix, status);
        return -1;
    }
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

bool split_protocol_line(const char *line, std::vector<std::string> &tokens)
{
    tokens.clear();
    if (line == nullptr)
        return false;

    const std::string message(line);
    if (message.size() < 2 || message.back() != '\n' ||
        message.find('\n') != message.size() - 1)
        return false;

    const size_t content_length = message.size() - 1;
    size_t start = 0;
    for (size_t i = 0; i <= content_length; ++i) {
        if (i < content_length &&
            static_cast<unsigned char>(message[i]) < static_cast<unsigned char>(' ') &&
            message[i] != '\n')
            return false;
        if (i != content_length && message[i] != ' ')
            continue;
        if (i == start)
            return false;
        tokens.emplace_back(message, start, i - start);
        start = i + 1;
    }
    return true;
}

struct Version {
    std::string uid;
    std::string size;
    std::string label;
    std::string publication_time;
    std::string availability;
};

enum class VersionsResult { Ok, Empty, Error };

VersionsResult fetch_versions(const struct sockaddr_in *server, int ds_port,
                              const char *filename,
                              std::vector<Version> &versions)
{
    char request[64];
    char reply[MAX_MESSAGE];
    snprintf(request, sizeof(request), "VRS %s\n", filename);
    if (tcp_exchange(server, ds_port, request, reply, sizeof(reply)) < 0)
        return VersionsResult::Error;

    std::vector<std::string> fields;
    if (!split_protocol_line(reply, fields) || fields.size() < 2 ||
        fields[0] != "RVR") {
        fprintf(stderr, "Invalid versions reply from Directory Server.\n");
        return VersionsResult::Error;
    }
    const std::string &status = fields[1];
    if (status == "NOK") {
        if (fields.size() != 2) {
            fprintf(stderr, "Unexpected fields in empty versions reply.\n");
            return VersionsResult::Error;
        }
        return VersionsResult::Empty;
    }
    if (status != "OK") {
        fprintf(stderr, "Versions request failed: status %s.\n", status.c_str());
        return VersionsResult::Error;
    }

    if (fields.size() == 2) {
        fprintf(stderr,
                "Inconsistent versions reply: RVR OK contains no versions.\n");
        return VersionsResult::Error;
    }
    if ((fields.size() - 2) % 5 != 0) {
        fprintf(stderr, "Incomplete version entry from Directory Server.\n");
        return VersionsResult::Error;
    }

    std::set<std::string> unique_uids;
    for (size_t i = 2; i < fields.size(); i += 5) {
        Version version{fields[i], fields[i + 1], fields[i + 2], fields[i + 3],
                        fields[i + 4]};
        unsigned long long size = 0;
        if (!valid_uid(version.uid.c_str()) || !parse_file_size(version.size, size) ||
            !valid_label(version.label.c_str()) ||
            (version.availability != "AVL" && version.availability != "NAV") ||
            !unique_uids.insert(version.uid).second) {
            fprintf(stderr, "Invalid version entry from Directory Server.\n");
            return VersionsResult::Error;
        }
        versions.push_back(version);
    }
    return VersionsResult::Ok;
}

int query_location(int fd, const struct sockaddr_in *server,
                   const std::string &filename, const Version &version,
                   std::vector<PeerEndpoint> &locations)
{
    char request[64];
    char reply[MAX_MESSAGE];
    snprintf(request, sizeof(request), "WHR %s %s\n",
             filename.c_str(), version.uid.c_str());
    if (send_request(fd, server, request, reply, sizeof(reply)) < 0)
        return -1;

    std::istringstream input(reply);
    std::string prefix;
    std::string status;
    if (!(input >> prefix >> status) || prefix != "RWH") {
        fprintf(stderr, "Invalid location reply from Directory Server.\n");
        return -1;
    }
    if (status == "NOK") {
        std::string extra;
        if (input >> extra) {
            fprintf(stderr, "Unexpected fields in unavailable location reply.\n");
            return -1;
        }
        return 0;
    }
    if (status != "OK") {
        fprintf(stderr, "Location request failed: status %s.\n", status.c_str());
        return -1;
    }

    std::vector<PeerEndpoint> found;
    std::string ip;
    std::string port_text;
    while (input >> ip) {
        int port = 0;
        struct in_addr address;
        if (!(input >> port_text) || inet_pton(AF_INET, ip.c_str(), &address) != 1 ||
            !valid_port(port_text.c_str(), &port)) {
            fprintf(stderr, "Invalid peer endpoint in Directory Server reply.\n");
            return -1;
        }
        found.push_back({version.uid, ip, port});
    }
    if (found.empty()) {
        fprintf(stderr, "Directory Server returned RWH OK without a peer endpoint.\n");
        return -1;
    }
    locations.insert(locations.end(), found.begin(), found.end());
    return static_cast<int>(found.size());
}

int find_locations(int fd, const struct sockaddr_in *server,
                   const std::string &filename,
                   const std::vector<Version> &versions,
                   std::vector<PeerEndpoint> &locations)
{
    std::set<std::string> unique_endpoints;
    for (const Version &version : versions) {
        if (version.availability != "AVL")
            continue;
        std::vector<PeerEndpoint> found;
        const int result = query_location(fd, server, filename, version, found);
        if (result < 0)
            return -1;
        for (const PeerEndpoint &endpoint : found) {
            const std::string key = endpoint.uid + " " + endpoint.ip + " " +
                                    std::to_string(endpoint.port);
            if (unique_endpoints.insert(key).second)
                locations.push_back(endpoint);
        }
    }
    return static_cast<int>(locations.size());
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
    if (!status_is_allowed(status, {"OK", "NOK", "REG", "ERR"})) {
        fprintf(stderr, "Unexpected RLI status from Directory Server: %s.\n", status);
        return -1;
    }

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
    if (!status_is_allowed(status, {"OK", "NLG", "UNR", "WRP", "ERR"})) {
        fprintf(stderr, "Unexpected RLO status from Directory Server: %s.\n", status);
        return -1;
    }

    print_logout_result(status);
    if (strcmp(status, "OK") == 0 || strcmp(status, "NLG") == 0 ||
        strcmp(status, "UNR") == 0)
        clear_user_session(user);
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
    if (!status_is_allowed(status, {"OK", "NOK", "UNR", "WRP", "ERR"})) {
        fprintf(stderr, "Unexpected RUR status from Directory Server: %s.\n", status);
        return -1;
    }

    print_unregister_result(status);
    if (strcmp(status, "OK") == 0 || strcmp(status, "NOK") == 0 ||
        strcmp(status, "UNR") == 0)
        clear_user_session(user);
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
    return udp_resource_request(fd, server, request, "RPB", "Publish",
                                {"OK", "NLG", "UNR", "WRP", "NOK", "ERR"});
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
    return udp_resource_request(fd, server, request, "RRM", "Remove",
                                {"OK", "NLG", "UNR", "WRP", "NOK", "ERR"});
}

int command_list(int fd, const struct sockaddr_in *server)
{
    char reply[MAX_MESSAGE];
    if (send_request(fd, server, "LST\n", reply, sizeof(reply)) < 0)
        return -1;

    std::vector<std::string> fields;
    if (!split_protocol_line(reply, fields) || fields.size() < 2 ||
        fields[0] != "RLS") {
        fprintf(stderr, "Invalid resource list reply from Directory Server.\n");
        return -1;
    }
    const std::string &status = fields[1];
    if (status != "OK" && status != "NOK") {
        fprintf(stderr, "Unexpected RLS status from Directory Server: %s.\n",
                status.c_str());
        return -1;
    }
    if (status == "NOK") {
        if (fields.size() != 2) {
            fprintf(stderr, "Unexpected fields in empty resource list reply.\n");
            return -1;
        }
        printf("No resources are currently known.\n");
        return 0;
    }

    std::vector<std::string> names;
    std::set<std::string> unique_names;
    if (fields.size() == 2 || fields.size() > 52) {
        fprintf(stderr, "Directory Server returned an invalid filename count.\n");
        return -1;
    }
    for (size_t i = 2; i < fields.size(); ++i) {
        const std::string &name = fields[i];
        if (!valid_filename(name.c_str())) {
            fprintf(stderr, "Directory Server returned an invalid filename.\n");
            return -1;
        }
        if (!unique_names.insert(name).second) {
            fprintf(stderr, "Directory Server returned a duplicate filename.\n");
            return -1;
        }
        names.push_back(name);
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

    std::vector<Version> versions;
    const VersionsResult result = fetch_versions(server, ds_port, filename, versions);
    if (result == VersionsResult::Error)
        return -1;
    if (result == VersionsResult::Empty) {
        printf("No available versions for %s.\n", filename);
        return 0;
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
    if (!valid_filename(filename)) {
        printf("Invalid filename.\n");
        return 0;
    }

    std::vector<Version> versions;
    const VersionsResult result = fetch_versions(server, ds_port, filename, versions);
    if (result == VersionsResult::Error)
        return -1;
    if (result == VersionsResult::Empty || versions.empty()) {
        printf("No available peers for %s.\n", filename);
        return 0;
    }

    const int fd = create_udp_socket();
    if (fd < 0)
        return -1;
    std::vector<PeerEndpoint> locations;
    const int lookup_result = find_locations(fd, server, filename, versions, locations);
    close(fd);
    if (lookup_result < 0)
        return -1;

    if (locations.empty()) {
        printf("No peer is currently online and publishing %s.\n", filename);
        return 0;
    }
    printf("Available peers for %s:\n", filename);
    for (const PeerEndpoint &endpoint : locations)
        printf("  UID %s | %s:%d\n", endpoint.uid.c_str(), endpoint.ip.c_str(),
               endpoint.port);
    return 0;
}

int command_get(int udp_fd, const struct sockaddr_in *server, int ds_port,
                const char *filename)
{
    if (!valid_filename(filename)) {
        printf("Invalid filename.\n");
        return 0;
    }

    struct stat destination_info;
    if (lstat(filename, &destination_info) == 0) {
        printf("Download failed: destination already exists: %s\n", filename);
        return 0;
    }
    if (errno != ENOENT) {
        printf("Download failed: cannot inspect destination %s: %s\n",
               filename, strerror(errno));
        return -1;
    }

    std::vector<Version> versions;
    const VersionsResult result = fetch_versions(server, ds_port, filename, versions);
    if (result == VersionsResult::Error)
        return -1;
    if (result == VersionsResult::Empty || versions.empty()) {
        printf("No available versions for %s.\n", filename);
        return 0;
    }

    std::vector<PeerEndpoint> locations;
    if (find_locations(udp_fd, server, filename, versions, locations) < 0)
        return -1;
    if (locations.empty()) {
        printf("No peer is currently online and publishing %s.\n", filename);
        return 0;
    }

    std::string last_error;
    for (const PeerEndpoint &endpoint : locations) {
        unsigned long long received_size = 0;
        if (download_peer_file(endpoint.ip, endpoint.port, filename, filename,
                               &received_size, last_error) == 0) {
            printf("Downloaded %s (%llu bytes) from UID %s at %s:%d.\n",
                   filename, received_size, endpoint.uid.c_str(),
                   endpoint.ip.c_str(), endpoint.port);
            return 0;
        }
        if (lstat(filename, &destination_info) == 0) {
            printf("Download failed: destination already exists: %s\n", filename);
            return 0;
        }
    }

    printf("Download failed from all available peers: %s\n", last_error.c_str());
    return -1;
}
