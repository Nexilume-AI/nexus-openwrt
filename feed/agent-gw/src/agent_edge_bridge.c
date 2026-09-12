#define _GNU_SOURCE

#include "agent_edge_proxy.h"
#include "agent_public_ipv6.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BRIDGE_BUFFER_SIZE 32768U
#define BRIDGE_DEFAULT_LISTEN "127.0.0.1:7791"
#define BRIDGE_DEFAULT_GATEWAY "127.0.0.1:7788"
#define BRIDGE_DEFAULT_ADAPTER "127.0.0.1:7790"
#define BRIDGE_DEFAULT_TOKEN "/var/run/agent-gw/public-ingress.token"
#define BRIDGE_IDLE_TIMEOUT_MS 300000

struct bridge_config {
    struct sockaddr_in listen;
    struct sockaddr_in6 direct_listen;
    bool direct_ipv6;
    bool lan_mode;
    bool lan_ipv6;
    bool lan_bootstrap;
    struct sockaddr_in gateway;
    struct sockaddr_in adapter;
    struct agent_public_ipv6_pool prefix;
    char token[129];
    uint16_t public_port;
    uint32_t max_children;
};

struct relay_buffer {
    uint8_t data[BRIDGE_BUFFER_SIZE];
    size_t offset;
    size_t length;
};

static volatile sig_atomic_t reap_requested;

static void child_signal(int signal_number)
{
    (void)signal_number;
    reap_requested = 1;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || text[0] < '0' || text[0] > '9') return false;
    parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_loopback_endpoint(
    const char *text,
    struct sockaddr_in *address
)
{
    const char *separator;
    char host[16];
    uint32_t port;
    size_t host_length;

    if (text == NULL || address == NULL) return false;
    separator = strrchr(text, ':');
    if (separator == NULL) return false;
    host_length = (size_t)(separator - text);
    if (host_length == 0U || host_length >= sizeof(host) ||
        !parse_u32(separator + 1U, &port) || port == 0U || port > 65535U) {
        return false;
    }
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &address->sin_addr) != 1 ||
        (ntohl(address->sin_addr.s_addr) >> 24U) != 127U) return false;
    return true;
}

static bool parse_ipv4_endpoint(
    const char *text,
    struct sockaddr_in *address
)
{
    const char *separator;
    char host[16];
    uint32_t port;
    size_t host_length;

    if (text == NULL || address == NULL) return false;
    separator = strrchr(text, ':');
    if (separator == NULL) return false;
    host_length = (size_t)(separator - text);
    if (host_length == 0U || host_length >= sizeof(host) ||
        !parse_u32(separator + 1U, &port) || port == 0U || port > 65535U) {
        return false;
    }
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, host, &address->sin_addr) == 1;
}

static bool parse_public_ipv6_endpoint(
    const char *text,
    struct sockaddr_in6 *address
)
{
    const char *closing;
    char host[46];
    size_t host_length;
    uint32_t port;

    if (text == NULL || address == NULL || text[0] != '[') return false;
    closing = strchr(text, ']');
    if (closing == NULL || closing[1] != ':' || closing[2] == '\0') return false;
    host_length = (size_t)(closing - text - 1U);
    if (host_length == 0U || host_length >= sizeof(host) ||
        !parse_u32(closing + 2U, &port) || port == 0U || port > 65535U) {
        return false;
    }
    memcpy(host, text + 1U, host_length);
    host[host_length] = '\0';
    memset(address, 0, sizeof(*address));
    address->sin6_family = AF_INET6;
    address->sin6_port = htons((uint16_t)port);
    return inet_pton(AF_INET6, host, &address->sin6_addr) == 1 &&
           IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr);
}

static bool read_token(const char *path, char token[129])
{
    FILE *file;
    size_t length;
    int extra;

    if (path == NULL || strncmp(path, "/var/run/agent-gw/", 18U) != 0 ||
        strstr(path, "..") != NULL) return false;
    file = fopen(path, "rb");
    if (file == NULL) return false;
    length = fread(token, 1U, 128U, file);
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    extra = fgetc(file);
    if (extra != EOF || fclose(file) != 0) return false;
    while (length > 0U &&
           (token[length - 1U] == '\r' || token[length - 1U] == '\n')) {
        length--;
    }
    if (length < 32U) return false;
    token[length] = '\0';
    return true;
}

static const uint8_t *find_bytes(
    const uint8_t *buffer,
    size_t length,
    const char *needle,
    size_t needle_length
)
{
    size_t index;

    if (needle_length > length) return NULL;
    for (index = 0U; index + needle_length <= length; index++) {
        if (memcmp(buffer + index, needle, needle_length) == 0) {
            return buffer + index;
        }
    }
    return NULL;
}

static bool receive_until(
    int fd,
    uint8_t *buffer,
    size_t capacity,
    size_t *length,
    const char *needle,
    size_t needle_length
)
{
    while (find_bytes(buffer, *length, needle, needle_length) == NULL) {
        ssize_t received;

        if (*length == capacity) return false;
        received = recv(fd, buffer + *length, capacity - *length, 0);
        if (received <= 0) return false;
        *length += (size_t)received;
    }
    return true;
}

static bool send_all(int fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0U;

    while (sent < length) {
        ssize_t result = send(fd, buffer + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        sent += (size_t)result;
    }
    return true;
}

static int connect_backend(const struct sockaddr_in *address)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) return -1;
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void compact_buffer(struct relay_buffer *buffer)
{
    if (buffer->offset > 0U && buffer->length > 0U) {
        memmove(buffer->data, buffer->data + buffer->offset, buffer->length);
    }
    if (buffer->length == 0U || buffer->offset > 0U) buffer->offset = 0U;
}

static bool relay_connection(int client, int backend)
{
    struct relay_buffer client_to_backend = {0};
    struct relay_buffer backend_to_client = {0};
    bool client_read = true;
    bool backend_read = true;
    bool client_shutdown = false;
    bool backend_shutdown = false;

    while (client_read || backend_read || client_to_backend.length > 0U ||
           backend_to_client.length > 0U) {
        struct pollfd descriptors[2];
        int result;

        compact_buffer(&client_to_backend);
        compact_buffer(&backend_to_client);
        descriptors[0].fd = client;
        descriptors[0].events = 0;
        descriptors[0].revents = 0;
        descriptors[1].fd = backend;
        descriptors[1].events = 0;
        descriptors[1].revents = 0;
        if (client_read && client_to_backend.length < BRIDGE_BUFFER_SIZE) {
            descriptors[0].events |= POLLIN;
        }
        if (backend_to_client.length > 0U) descriptors[0].events |= POLLOUT;
        if (backend_read && backend_to_client.length < BRIDGE_BUFFER_SIZE) {
            descriptors[1].events |= POLLIN;
        }
        if (client_to_backend.length > 0U) descriptors[1].events |= POLLOUT;
        result = poll(descriptors, 2U, BRIDGE_IDLE_TIMEOUT_MS);
        if (result <= 0) return false;
        if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) return false;
        if ((descriptors[0].revents & POLLIN) != 0) {
            ssize_t count = recv(client,
                client_to_backend.data + client_to_backend.length,
                BRIDGE_BUFFER_SIZE - client_to_backend.length, 0);
            if (count > 0) client_to_backend.length += (size_t)count;
            else client_read = false;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            ssize_t count = recv(backend,
                backend_to_client.data + backend_to_client.length,
                BRIDGE_BUFFER_SIZE - backend_to_client.length, 0);
            if (count > 0) backend_to_client.length += (size_t)count;
            else backend_read = false;
        }
        if ((descriptors[1].revents & POLLOUT) != 0 &&
            client_to_backend.length > 0U) {
            ssize_t count = send(backend,
                client_to_backend.data + client_to_backend.offset,
                client_to_backend.length, MSG_NOSIGNAL);
            if (count <= 0) return false;
            client_to_backend.offset += (size_t)count;
            client_to_backend.length -= (size_t)count;
        }
        if ((descriptors[0].revents & POLLOUT) != 0 &&
            backend_to_client.length > 0U) {
            ssize_t count = send(client,
                backend_to_client.data + backend_to_client.offset,
                backend_to_client.length, MSG_NOSIGNAL);
            if (count <= 0) return false;
            backend_to_client.offset += (size_t)count;
            backend_to_client.length -= (size_t)count;
        }
        if (!client_read && client_to_backend.length == 0U &&
            !backend_shutdown) {
            (void)shutdown(backend, SHUT_WR);
            backend_shutdown = true;
        }
        if (!backend_read && backend_to_client.length == 0U &&
            !client_shutdown) {
            (void)shutdown(client, SHUT_WR);
            client_shutdown = true;
        }
        if ((descriptors[0].revents & POLLHUP) != 0) client_read = false;
        if ((descriptors[1].revents & POLLHUP) != 0) backend_read = false;
    }
    return true;
}

static bool handle_connection(int client, const struct bridge_config *config)
{
    uint8_t input[BRIDGE_BUFFER_SIZE];
    char rewritten[AGENT_EDGE_HTTP_HEADER_MAX + 512U];
    size_t input_length = 0U;
    size_t proxy_length;
    size_t header_length;
    size_t rewritten_length = 0U;
    const uint8_t *end;
    struct agent_edge_proxy_info info;
    enum agent_edge_backend selected;
    const struct sockaddr_in *backend_address;
    const struct timeval header_timeout = { .tv_sec = 10, .tv_usec = 0 };
    int backend;
    char lan_source[INET6_ADDRSTRLEN] = {0};

    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &header_timeout,
                   sizeof(header_timeout)) != 0) return false;
    if (config->lan_mode) {
        struct sockaddr_storage source;
        socklen_t source_length = sizeof(source);
        const void *address = NULL;

        memset(&source, 0, sizeof(source));
        if (getpeername(client, (struct sockaddr *)&source, &source_length) != 0) {
            return false;
        }
        if (source.ss_family == AF_INET) {
            address = &((const struct sockaddr_in *)&source)->sin_addr;
        } else if (source.ss_family == AF_INET6) {
            address = &((const struct sockaddr_in6 *)&source)->sin6_addr;
        } else {
            return false;
        }
        if (inet_ntop(source.ss_family, address, lan_source,
                      sizeof(lan_source)) == NULL) return false;
    } else if (config->direct_ipv6) {
        struct sockaddr_in6 source;
        struct sockaddr_in6 destination;
        socklen_t source_length = sizeof(source);
        socklen_t destination_length = sizeof(destination);

        memset(&source, 0, sizeof(source));
        memset(&destination, 0, sizeof(destination));
        if (getpeername(client, (struct sockaddr *)&source, &source_length) != 0 ||
            getsockname(client, (struct sockaddr *)&destination,
                        &destination_length) != 0 ||
            source.sin6_family != AF_INET6 || destination.sin6_family != AF_INET6 ||
            inet_ntop(AF_INET6, &source.sin6_addr, info.source_ipv6,
                      sizeof(info.source_ipv6)) == NULL ||
            inet_ntop(AF_INET6, &destination.sin6_addr, info.destination_ipv6,
                      sizeof(info.destination_ipv6)) == NULL) return false;
        info.source_port = ntohs(source.sin6_port);
        info.destination_port = ntohs(destination.sin6_port);
        if (info.destination_port != config->public_port ||
            !agent_public_ipv6_contains(&config->prefix,
                                        info.destination_ipv6)) return false;
    } else {
        if (!receive_until(client, input, sizeof(input), &input_length,
                           "\r\n", 2U)) return false;
        end = find_bytes(input, input_length, "\r\n", 2U);
        proxy_length = (size_t)(end - input) + 2U;
        if (proxy_length > AGENT_EDGE_PROXY_LINE_MAX ||
            !agent_edge_proxy_parse_v1((const char *)input, proxy_length, &info) ||
            info.destination_port != config->public_port ||
            !agent_public_ipv6_contains(&config->prefix,
                                        info.destination_ipv6)) return false;
        memmove(input, input + proxy_length, input_length - proxy_length);
        input_length -= proxy_length;
    }
    if (!receive_until(client, input, sizeof(input), &input_length,
                       "\r\n\r\n", 4U)) return false;
    end = find_bytes(input, input_length, "\r\n\r\n", 4U);
    header_length = (size_t)(end - input) + 4U;
    if (header_length > AGENT_EDGE_HTTP_HEADER_MAX) return false;
    selected = config->lan_mode
        ? agent_edge_proxy_rewrite_lan_http(
            (const char *)input, header_length,
            config->lan_bootstrap ? lan_source : NULL,
            config->lan_bootstrap ? config->token : NULL, rewritten,
            sizeof(rewritten), &rewritten_length)
        : agent_edge_proxy_rewrite_http(
            (const char *)input, header_length, info.destination_ipv6,
            config->token, rewritten, sizeof(rewritten), &rewritten_length);
    if (selected == AGENT_EDGE_BACKEND_INVALID) return false;
    backend_address = selected == AGENT_EDGE_BACKEND_GATEWAY
        ? &config->gateway : &config->adapter;
    backend = connect_backend(backend_address);
    if (backend < 0) return false;
    if (!send_all(backend, (const uint8_t *)rewritten, rewritten_length) ||
        !send_all(backend, input + header_length,
                  input_length - header_length)) {
        close(backend);
        return false;
    }
    (void)relay_connection(client, backend);
    close(backend);
    return true;
}

static bool read_options(
    int argc,
    char **argv,
    struct bridge_config *config
)
{
    char listen[64] = BRIDGE_DEFAULT_LISTEN;
    char gateway[64] = BRIDGE_DEFAULT_GATEWAY;
    char adapter[64] = BRIDGE_DEFAULT_ADAPTER;
    char token_path[256] = BRIDGE_DEFAULT_TOKEN;
    char prefix[64] = {0};
    char direct_listen[64] = {0};
    char lan_listen[64] = {0};
    uint32_t value;
    int option;

    config->public_port = 7443U;
    config->max_children = 32U;
    while ((option = getopt(argc, argv, "a:r:l:g:d:p:t:T:P:c:h")) != -1) {
        switch (option) {
        case 'a':
            if (snprintf(listen, sizeof(listen), "%s", optarg) >=
                (int)sizeof(listen)) return false;
            break;
        case 'r':
            if (snprintf(direct_listen, sizeof(direct_listen), "%s", optarg) >=
                (int)sizeof(direct_listen)) return false;
            config->direct_ipv6 = true;
            break;
        case 'l':
            if (snprintf(lan_listen, sizeof(lan_listen), "%s", optarg) >=
                (int)sizeof(lan_listen)) return false;
            config->lan_mode = true;
            break;
        case 'g':
            if (snprintf(gateway, sizeof(gateway), "%s", optarg) >=
                (int)sizeof(gateway)) return false;
            break;
        case 'd':
            if (snprintf(adapter, sizeof(adapter), "%s", optarg) >=
                (int)sizeof(adapter)) return false;
            break;
        case 'p':
            if (snprintf(prefix, sizeof(prefix), "%s", optarg) >=
                (int)sizeof(prefix)) return false;
            break;
        case 't':
            if (snprintf(token_path, sizeof(token_path), "%s", optarg) >=
                (int)sizeof(token_path)) return false;
            break;
        case 'T':
            if (snprintf(token_path, sizeof(token_path), "%s", optarg) >=
                (int)sizeof(token_path)) return false;
            config->lan_bootstrap = true;
            break;
        case 'P':
            if (!parse_u32(optarg, &value) || value == 0U || value > 65535U)
                return false;
            config->public_port = (uint16_t)value;
            break;
        case 'c':
            if (!parse_u32(optarg, &value) || value == 0U || value > 128U)
                return false;
            config->max_children = value;
            break;
        default:
            return false;
        }
    }
    if (optind != argc || (config->lan_mode && config->direct_ipv6) ||
        !parse_loopback_endpoint(gateway, &config->gateway) ||
        !parse_loopback_endpoint(adapter, &config->adapter)) return false;
    if (config->lan_mode) {
        if (config->lan_bootstrap && !read_token(token_path, config->token)) {
            return false;
        }
        if (lan_listen[0] == '[') {
            config->lan_ipv6 = true;
            return parse_public_ipv6_endpoint(lan_listen,
                                              &config->direct_listen);
        }
        return parse_ipv4_endpoint(lan_listen, &config->listen);
    }
    return prefix[0] != '\0' &&
           (config->direct_ipv6
                ? parse_public_ipv6_endpoint(direct_listen,
                                             &config->direct_listen)
                : parse_loopback_endpoint(listen, &config->listen)) &&
           agent_public_ipv6_pool_init(&config->prefix, prefix, 1U) &&
           read_token(token_path, config->token);
}

int main(int argc, char **argv)
{
    struct bridge_config config;
    struct sigaction action;
    uint32_t children = 0U;
    int listener;
    int reuse = 1;
    bool ipv6_listener;

    memset(&config, 0, sizeof(config));
    if (!read_options(argc, argv, &config)) {
        fprintf(stderr, "invalid agent-edge-bridge configuration\n");
        return EXIT_FAILURE;
    }
    ipv6_listener = config.direct_ipv6 ||
        (config.lan_mode && config.lan_ipv6);
    listener = socket(ipv6_listener ? AF_INET6 : AF_INET,
                      SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) != 0 ||
        bind(listener,
             ipv6_listener
                ? (struct sockaddr *)&config.direct_listen
                : (struct sockaddr *)&config.listen,
             ipv6_listener
                ? sizeof(config.direct_listen) : sizeof(config.listen)) != 0 ||
        listen(listener, 32) != 0) {
        if (listener >= 0) close(listener);
        if (!config.lan_mode) agent_public_ipv6_pool_free(&config.prefix);
        return EXIT_FAILURE;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = child_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGCHLD, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int client;
        pid_t child;

        if (reap_requested) {
            while (waitpid(-1, NULL, WNOHANG) > 0) {
                if (children > 0U) children--;
            }
            reap_requested = 0;
        }
        client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (children >= config.max_children) {
            close(client);
            continue;
        }
        child = fork();
        if (child < 0) {
            close(client);
            continue;
        }
        if (child == 0) {
            bool ok;

            close(listener);
            ok = handle_connection(client, &config);
            close(client);
            _exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        children++;
        close(client);
    }
    close(listener);
    if (!config.lan_mode) agent_public_ipv6_pool_free(&config.prefix);
    return EXIT_FAILURE;
}
