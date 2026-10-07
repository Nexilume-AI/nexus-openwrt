#include "agent_relay_bootstrap.h"

#include "peer_table.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libubox/uloop.h>
#include <limits.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define RELAY_HTTP_RESPONSE_MAX \
    (AGENT_RELAY_DIRECTORY_BODY_MAX + 2048U)
#define RELAY_TICK_MS 100U

enum relay_bootstrap_phase {
    RELAY_BOOTSTRAP_DISABLED = 0,
    RELAY_BOOTSTRAP_IDLE,
    RELAY_BOOTSTRAP_TCP,
    RELAY_BOOTSTRAP_TLS,
    RELAY_BOOTSTRAP_WRITE,
    RELAY_BOOTSTRAP_READ
};

struct agent_relay_bootstrap {
    struct agent_relay_bootstrap_config config;
    uint64_t (*now_ms)(void);
    struct agent_relay_directory_endpoint
        endpoints[AGENT_RELAY_BOOTSTRAP_MAX_DIRECTORIES];
    size_t endpoint_count;
    struct agent_mesh_profile mesh_profile;
    size_t endpoint_index;
    size_t endpoint_attempts;
    char resolved_directory_ipv4[AGENT_RELAY_IPV4_LEN];
    enum relay_bootstrap_phase phase;
    struct uloop_timeout timer;
    struct uloop_fd watcher;
    uint64_t deadline_ms;
    uint64_t next_query_ms;
    char request[AGENT_RELAY_DIRECTORY_REQUEST_MAX];
    size_t request_length;
    size_t request_offset;
    char response[RELAY_HTTP_RESPONSE_MAX + 1U];
    size_t response_used;
    mbedtls_x509_crt ca_chain;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config tls_config;
    mbedtls_ssl_context tls;
    bool tls_global_initialized;
    bool tls_initialized;
    bool assignment_active;
    struct agent_relay_assignment assignment;
    uint64_t queries;
    uint64_t assignments;
    uint64_t renewals;
    uint64_t failures;
    uint64_t expirations;
    uint64_t directory_failovers;
    uint64_t relay_failover_requests;
    uint64_t tls_handshakes;
    uint32_t tls_last_error_code;
    uint32_t tls_last_verify_flags;
    char tls_last_alpn[16];
    char failed_relay_id[AGENT_RELAY_ID_LEN];
    char last_error[AGENT_RELAY_BOOTSTRAP_ERROR_LEN];
};

static bool read_device_token(const char *path, char token[129])
{
    FILE *file;
    size_t length;

    token[0] = '\0';
    if (path == NULL || path[0] == '\0') return true;
    file = fopen(path, "r");
    if (file == NULL || fgets(token, 129, file) == NULL) {
        if (file != NULL) fclose(file);
        return false;
    }
    fclose(file);
    length = strcspn(token, "\r\n");
    token[length] = '\0';
    return length >= 6U && length <= 128U;
}

static const char *directory_alpn[] = {"http/1.1", NULL};

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        (*value)++;
    }
}

static void set_error(char *target, size_t capacity, const char *message)
{
    if (target != NULL && capacity > 0U) {
        (void)snprintf(target, capacity, "%s",
                       message == NULL ? "unknown error" : message);
    }
}

static const char *phase_name(enum relay_bootstrap_phase phase)
{
    switch (phase) {
    case RELAY_BOOTSTRAP_DISABLED: return "disabled";
    case RELAY_BOOTSTRAP_IDLE: return "idle";
    case RELAY_BOOTSTRAP_TCP: return "tcp-connecting";
    case RELAY_BOOTSTRAP_TLS: return "tls-handshake";
    case RELAY_BOOTSTRAP_WRITE: return "request-write";
    case RELAY_BOOTSTRAP_READ: return "response-read";
    default: return "unknown";
    }
}

static int tls_send(void *context, const unsigned char *buffer, size_t length)
{
    int fd = *(int *)context;
    ssize_t result;

    if (length > (size_t)INT_MAX) length = (size_t)INT_MAX;
    result = send(fd, buffer, length, MSG_NOSIGNAL);
    if (result >= 0) return (int)result;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_receive(void *context, unsigned char *buffer, size_t length)
{
    int fd = *(int *)context;
    ssize_t result;

    if (length > (size_t)INT_MAX) length = (size_t)INT_MAX;
    result = recv(fd, buffer, length, 0);
    if (result >= 0) return (int)result;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static void cleanup_io(struct agent_relay_bootstrap *bootstrap)
{
    if (bootstrap->watcher.registered) {
        uloop_fd_delete(&bootstrap->watcher);
    }
    if (bootstrap->watcher.fd >= 0) {
        close(bootstrap->watcher.fd);
        bootstrap->watcher.fd = -1;
    }
    if (bootstrap->tls_initialized) {
        mbedtls_ssl_free(&bootstrap->tls);
        bootstrap->tls_initialized = false;
    }
    bootstrap->request_offset = 0U;
    bootstrap->response_used = 0U;
    bootstrap->response[0] = '\0';
}

static void schedule_retry(
    struct agent_relay_bootstrap *bootstrap,
    const char *message
)
{
    uint64_t now = bootstrap->now_ms();
    uint32_t delay = bootstrap->config.poll_ms;

    cleanup_io(bootstrap);
    bootstrap->phase = RELAY_BOOTSTRAP_IDLE;
    increment(&bootstrap->failures);
    set_error(bootstrap->last_error, sizeof(bootstrap->last_error), message);
    if (bootstrap->endpoint_count > 1U) {
        bootstrap->endpoint_index =
            (bootstrap->endpoint_index + 1U) % bootstrap->endpoint_count;
        increment(&bootstrap->directory_failovers);
        bootstrap->endpoint_attempts++;
        if (bootstrap->endpoint_attempts < bootstrap->endpoint_count) {
            delay = RELAY_TICK_MS;
        } else {
            bootstrap->endpoint_attempts = 0U;
        }
    }
    if (bootstrap->assignment_active &&
        bootstrap->assignment.expires_at_ms > now) {
        uint64_t remaining = bootstrap->assignment.expires_at_ms - now;
        if (remaining < delay) delay = (uint32_t)remaining;
    }
    if (delay < RELAY_TICK_MS) delay = RELAY_TICK_MS;
    bootstrap->next_query_ms = saturating_add(now, delay);
}

static bool initialize_tls(struct agent_relay_bootstrap *bootstrap)
{
    static const unsigned char personalization[] =
        "nexus-agentd-relay-directory-p37";

    mbedtls_x509_crt_init(&bootstrap->ca_chain);
    mbedtls_x509_crt_init(&bootstrap->client_cert);
    mbedtls_pk_init(&bootstrap->client_key);
    mbedtls_entropy_init(&bootstrap->entropy);
    mbedtls_ctr_drbg_init(&bootstrap->drbg);
    mbedtls_ssl_config_init(&bootstrap->tls_config);
    bootstrap->tls_global_initialized = true;
    if (mbedtls_x509_crt_parse_file(
            &bootstrap->ca_chain, bootstrap->config.ca_file) != 0 ||
        mbedtls_x509_crt_parse_file(
            &bootstrap->client_cert,
            bootstrap->config.client_cert_file) != 0 ||
        mbedtls_ctr_drbg_seed(
            &bootstrap->drbg, mbedtls_entropy_func, &bootstrap->entropy,
            personalization, sizeof(personalization) - 1U) != 0 ||
        mbedtls_pk_parse_keyfile(
            &bootstrap->client_key, bootstrap->config.client_key_file, NULL,
            mbedtls_ctr_drbg_random, &bootstrap->drbg) != 0 ||
        mbedtls_ssl_config_defaults(
            &bootstrap->tls_config, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    {
        size_t index;
        bool has_open_mesh_endpoint = false;

        for (index = 0U; index < bootstrap->endpoint_count; index++) {
            if (bootstrap->endpoints[index].open_mesh) {
                has_open_mesh_endpoint = true;
                break;
            }
        }
        mbedtls_ssl_conf_authmode(
            &bootstrap->tls_config,
            has_open_mesh_endpoint ? MBEDTLS_SSL_VERIFY_OPTIONAL
                                   : MBEDTLS_SSL_VERIFY_REQUIRED);
    }
    mbedtls_ssl_conf_ca_chain(&bootstrap->tls_config,
                              &bootstrap->ca_chain, NULL);
    mbedtls_ssl_conf_rng(&bootstrap->tls_config,
                         mbedtls_ctr_drbg_random, &bootstrap->drbg);
    mbedtls_ssl_conf_min_tls_version(&bootstrap->tls_config,
                                     MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&bootstrap->tls_config,
                                     MBEDTLS_SSL_VERSION_TLS1_3);
    return mbedtls_ssl_conf_own_cert(
               &bootstrap->tls_config, &bootstrap->client_cert,
               &bootstrap->client_key) == 0 &&
           mbedtls_ssl_conf_alpn_protocols(
               &bootstrap->tls_config, directory_alpn) == 0;
}

static void free_tls(struct agent_relay_bootstrap *bootstrap)
{
    if (!bootstrap->tls_global_initialized) return;
    mbedtls_ssl_config_free(&bootstrap->tls_config);
    mbedtls_pk_free(&bootstrap->client_key);
    mbedtls_x509_crt_free(&bootstrap->client_cert);
    mbedtls_x509_crt_free(&bootstrap->ca_chain);
    mbedtls_ctr_drbg_free(&bootstrap->drbg);
    mbedtls_entropy_free(&bootstrap->entropy);
    bootstrap->tls_global_initialized = false;
}

static bool start_tls(struct agent_relay_bootstrap *bootstrap)
{
    const struct agent_relay_directory_endpoint *endpoint =
        &bootstrap->endpoints[bootstrap->endpoint_index];

    mbedtls_ssl_init(&bootstrap->tls);
    if (mbedtls_ssl_setup(&bootstrap->tls, &bootstrap->tls_config) != 0 ||
        mbedtls_ssl_set_hostname(
            &bootstrap->tls, endpoint->server_identity) != 0) {
        mbedtls_ssl_free(&bootstrap->tls);
        return false;
    }
    bootstrap->tls_initialized = true;
    mbedtls_ssl_set_bio(&bootstrap->tls, &bootstrap->watcher.fd,
                        tls_send, tls_receive, NULL);
    bootstrap->phase = RELAY_BOOTSTRAP_TLS;
    return true;
}

static bool header_value(
    const char *headers,
    size_t header_length,
    const char *name,
    char *value,
    size_t value_capacity
)
{
    const char *line = strstr(headers, "\r\n") + 2;
    size_t name_length = strlen(name);

    while (line != NULL && (size_t)(line - headers) < header_length) {
        const char *end = strstr(line, "\r\n");
        const char *start;
        size_t length;

        if (end == NULL || end == line) break;
        if ((size_t)(end - line) > name_length + 1U &&
            strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            start = line + name_length + 1U;
            while (start < end && (*start == ' ' || *start == '\t')) start++;
            length = (size_t)(end - start);
            while (length > 0U &&
                   (start[length - 1U] == ' ' || start[length - 1U] == '\t')) {
                length--;
            }
            if (length == 0U || length >= value_capacity) return false;
            memcpy(value, start, length);
            value[length] = '\0';
            return true;
        }
        line = end + 2;
    }
    return false;
}

static int parse_http_response(
    struct agent_relay_bootstrap *bootstrap,
    struct agent_relay_assignment *assignment
)
{
    const char *header_end = strstr(bootstrap->response, "\r\n\r\n");
    char content_length_text[24];
    char content_type[80];
    char *end = NULL;
    unsigned long content_length;
    size_t header_length;
    size_t available;

    if (header_end == NULL) return 0;
    header_length = (size_t)(header_end - bootstrap->response) + 4U;
    if (strncmp(bootstrap->response, "HTTP/1.1 200 ", 13U) != 0 ||
        !header_value(bootstrap->response, header_length, "Content-Length",
                      content_length_text, sizeof(content_length_text)) ||
        !header_value(bootstrap->response, header_length, "Content-Type",
                      content_type, sizeof(content_type)) ||
        strcmp(content_type,
               "application/vnd.nexus.relay-assignment+json") != 0 ||
        strstr(bootstrap->response, "\r\nTransfer-Encoding:") != NULL ||
        strstr(bootstrap->response, "\r\ntransfer-encoding:") != NULL) {
        return -1;
    }
    errno = 0;
    content_length = strtoul(content_length_text, &end, 10);
    if (errno != 0 || end == content_length_text || *end != '\0' ||
        content_length == 0UL ||
        content_length > AGENT_RELAY_DIRECTORY_BODY_MAX) {
        return -1;
    }
    available = bootstrap->response_used - header_length;
    if (available < (size_t)content_length) return 0;
    if (available != (size_t)content_length) return -1;
    {
        int parsed = agent_relay_directory_parse_assignment(
               bootstrap->response + header_length,
               (size_t)content_length, bootstrap->now_ms(), assignment) ==
           AGENT_RELAY_DIRECTORY_OK ? 1 : -1;
        if (parsed > 0) {
            assignment->open_mesh =
                bootstrap->endpoints[bootstrap->endpoint_index].open_mesh;
            if (bootstrap->mesh_profile.count) {
                const struct agent_mesh_path *path = &bootstrap->mesh_profile.paths[bootstrap->endpoint_index];
                /* Network location comes from the operator-reviewed link, while
                 * the Directory still supplies identity and signed ticket. */
                snprintf(assignment->relay_endpoint, sizeof(assignment->relay_endpoint),
                         "https://relay-seed.mesh.local:%u/arpx/v1", path->relay_port);
                snprintf(assignment->mesh_connect_host, sizeof(assignment->mesh_connect_host), "%s", path->relay_host);
                snprintf(assignment->mesh_tls_sha256, sizeof(assignment->mesh_tls_sha256), "%s", bootstrap->mesh_profile.relay_sha256);
            }
        }
        return parsed;
    }
}

static void accept_assignment(
    struct agent_relay_bootstrap *bootstrap,
    const struct agent_relay_assignment *assignment
)
{
    bool renewal = bootstrap->assignment_active &&
        agent_relay_assignment_equal(&bootstrap->assignment, assignment);
    uint64_t refresh_ms = ((uint64_t)assignment->lease_seconds * 1000U) / 2U;

    /* failed_relay_id is an advisory preference sent to the trusted
     * Directory, not a permanent client-side denylist. A deployment with one
     * trusted Relay must be able to recover when the Directory returns that
     * Relay with a fresh short-lived ticket. Multi-Relay selection remains a
     * Directory policy decision. */
    if (!bootstrap->config.assignment_handler(
            bootstrap->config.event_context, assignment)) {
        schedule_retry(bootstrap, "relay assignment callback rejected");
        return;
    }
    cleanup_io(bootstrap);
    bootstrap->phase = RELAY_BOOTSTRAP_IDLE;
    bootstrap->assignment = *assignment;
    bootstrap->assignment_active = true;
    bootstrap->endpoint_attempts = 0U;
    bootstrap->failed_relay_id[0] = '\0';
    increment(renewal ? &bootstrap->renewals : &bootstrap->assignments);
    bootstrap->last_error[0] = '\0';
    if (refresh_ms > bootstrap->config.poll_ms) {
        refresh_ms = bootstrap->config.poll_ms;
    }
    bootstrap->next_query_ms = saturating_add(
        bootstrap->now_ms(), refresh_ms);
}

static void drive_io(struct agent_relay_bootstrap *bootstrap)
{
    if (bootstrap->phase == RELAY_BOOTSTRAP_TCP) {
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);

        if (getsockopt(bootstrap->watcher.fd, SOL_SOCKET, SO_ERROR,
                       &socket_error, &length) != 0 || socket_error != 0 ||
            !start_tls(bootstrap)) {
            schedule_retry(bootstrap, "directory TCP connection failed");
            return;
        }
    }
    if (bootstrap->phase == RELAY_BOOTSTRAP_TLS) {
        int result = mbedtls_ssl_handshake(&bootstrap->tls);
        uint32_t verify_flags;
        const char *negotiated;

        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) return;
        verify_flags = mbedtls_ssl_get_verify_result(&bootstrap->tls);
        negotiated = mbedtls_ssl_get_alpn_protocol(&bootstrap->tls);
        bootstrap->tls_last_error_code = result < 0
            ? (uint32_t)(-result) : (uint32_t)result;
        bootstrap->tls_last_verify_flags = verify_flags;
        (void)snprintf(bootstrap->tls_last_alpn,
                       sizeof(bootstrap->tls_last_alpn), "%s",
                       negotiated == NULL ? "none" : negotiated);
        {
            const struct agent_relay_directory_endpoint *endpoint =
                &bootstrap->endpoints[bootstrap->endpoint_index];
            uint32_t open_mesh_allowed = MBEDTLS_X509_BADCERT_NOT_TRUSTED |
                MBEDTLS_X509_BADCERT_CN_MISMATCH;
            bool pinned = bootstrap->mesh_profile.count != 0;
            if (pinned) open_mesh_allowed = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
            bool verification_failed = endpoint->open_mesh
                ? (verify_flags & ~open_mesh_allowed) != 0U
                : verify_flags != 0U;

        if (result != 0 || verification_failed || (pinned && !agent_mesh_certificate_matches(
                mbedtls_ssl_get_peer_cert(&bootstrap->tls), bootstrap->mesh_profile.directory_sha256)) || negotiated == NULL ||
            strcmp(negotiated, "http/1.1") != 0) {
            char detail[64];
            char message[AGENT_RELAY_BOOTSTRAP_ERROR_LEN];

            if (result != 0) {
                mbedtls_strerror(result, detail, sizeof(detail));
            } else if (verify_flags != 0U) {
                (void)snprintf(detail, sizeof(detail), "%s",
                               "certificate verification failed");
            } else {
                (void)snprintf(detail, sizeof(detail), "%s",
                               "ALPN mismatch");
            }
            (void)snprintf(
                message, sizeof(message),
                "Directory TLS host=%.40s code=0x%04lx verify=0x%08lx alpn=%.15s: %.48s",
                endpoint->server_identity,
                (unsigned long)bootstrap->tls_last_error_code,
                (unsigned long)verify_flags,
                bootstrap->tls_last_alpn, detail);
            schedule_retry(bootstrap, message);
            return;
        }
        }
        increment(&bootstrap->tls_handshakes);
        bootstrap->phase = RELAY_BOOTSTRAP_WRITE;
    }
    if (bootstrap->phase == RELAY_BOOTSTRAP_WRITE) {
        int result = mbedtls_ssl_write(
            &bootstrap->tls,
            (const unsigned char *)bootstrap->request +
                bootstrap->request_offset,
            bootstrap->request_length - bootstrap->request_offset);

        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) return;
        if (result <= 0) {
            schedule_retry(bootstrap, "directory request write failed");
            return;
        }
        bootstrap->request_offset += (size_t)result;
        if (bootstrap->request_offset < bootstrap->request_length) return;
        bootstrap->phase = RELAY_BOOTSTRAP_READ;
    }
    if (bootstrap->phase == RELAY_BOOTSTRAP_READ) {
        for (;;) {
            int result;
            int parsed;
            struct agent_relay_assignment assignment;

            if (bootstrap->response_used >= RELAY_HTTP_RESPONSE_MAX) {
                schedule_retry(bootstrap, "directory response exceeds bound");
                return;
            }
            result = mbedtls_ssl_read(
                &bootstrap->tls,
                (unsigned char *)bootstrap->response +
                    bootstrap->response_used,
                RELAY_HTTP_RESPONSE_MAX - bootstrap->response_used);
            if (result == MBEDTLS_ERR_SSL_WANT_READ ||
                result == MBEDTLS_ERR_SSL_WANT_WRITE) return;
            if (result <= 0) {
                schedule_retry(bootstrap, "directory response closed early");
                return;
            }
            bootstrap->response_used += (size_t)result;
            bootstrap->response[bootstrap->response_used] = '\0';
            parsed = parse_http_response(bootstrap, &assignment);
            if (parsed < 0) {
                schedule_retry(bootstrap, "invalid directory response");
                return;
            }
            if (parsed > 0) {
                accept_assignment(bootstrap, &assignment);
                return;
            }
        }
    }
}

static void socket_callback(struct uloop_fd *watcher, unsigned int events)
{
    struct agent_relay_bootstrap *bootstrap =
        container_of(watcher, struct agent_relay_bootstrap, watcher);

    (void)events;
    drive_io(bootstrap);
}

static bool resolve_directory_ipv4(
    const struct agent_relay_directory_endpoint *endpoint,
    char resolved[AGENT_RELAY_IPV4_LEN]
)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    bool found = false;

    if (endpoint->connect_ipv4[0] != '\0') {
        return snprintf(resolved, AGENT_RELAY_IPV4_LEN, "%s",
                        endpoint->connect_ipv4) > 0;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(endpoint->server_identity, NULL, &hints, &addresses) != 0) {
        return false;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        const struct sockaddr_in *ipv4;

        if (address->ai_family != AF_INET ||
            address->ai_addrlen < sizeof(struct sockaddr_in)) {
            continue;
        }
        ipv4 = (const struct sockaddr_in *)address->ai_addr;
        if (inet_ntop(AF_INET, &ipv4->sin_addr, resolved,
                      AGENT_RELAY_IPV4_LEN) != NULL) {
            found = true;
            break;
        }
    }
    freeaddrinfo(addresses);
    return found;
}

bool agent_relay_bootstrap_refresh(struct agent_relay_bootstrap *bootstrap)
{
    struct sockaddr_in address;
    struct sockaddr_storage mesh_address;
    socklen_t address_length = sizeof(address);
    const struct sockaddr *connect_address = (const struct sockaddr *)&address;
    int family = AF_INET;
    const struct agent_relay_directory_endpoint *endpoint;
    char connect_ipv4[AGENT_RELAY_IPV4_LEN];
    int flags;
    int result;
    char device_token[129];

    if (bootstrap == NULL || !bootstrap->config.enabled ||
        bootstrap->phase != RELAY_BOOTSTRAP_IDLE) {
        return false;
    }
    endpoint = &bootstrap->endpoints[bootstrap->endpoint_index];
    bootstrap->resolved_directory_ipv4[0] = '\0';
    if (bootstrap->mesh_profile.count) {
        const struct agent_mesh_path *path = &bootstrap->mesh_profile.paths[bootstrap->endpoint_index];
        if (!agent_mesh_resolve(path->directory_host, path->directory_port,
                bootstrap->failures / bootstrap->endpoint_count, &mesh_address, &address_length)) {
            schedule_retry(bootstrap, "Mesh Directory address resolution failed"); return false;
        }
        family = mesh_address.ss_family;
        connect_address = (const struct sockaddr *)&mesh_address;
        connect_ipv4[0] = '\0';
    } else if (!resolve_directory_ipv4(endpoint, connect_ipv4)) {
        schedule_retry(bootstrap, "Directory DNS resolution failed");
        return false;
    }
    (void)snprintf(bootstrap->resolved_directory_ipv4,
                   sizeof(bootstrap->resolved_directory_ipv4), "%s",
                   connect_ipv4);
    if (endpoint->open_mesh) {
        /* The dedicated Open Mesh endpoint authenticates the Router with its
         * generated TLS identity and the short-lived ticket it returns.  It
         * must not depend on, or disclose, a Cloud enrollment credential. */
        device_token[0] = '\0';
    } else if (!read_device_token(bootstrap->config.device_token_file,
                                  device_token)) {
        schedule_retry(bootstrap, "Directory device token is unavailable");
        return false;
    }
    if (agent_relay_directory_build_authenticated_ha_request(
            endpoint, bootstrap->config.router_id,
            bootstrap->config.domain_id,
            bootstrap->assignment_active
                ? bootstrap->assignment.relay_id : "",
            bootstrap->mesh_profile.count ? "" : bootstrap->failed_relay_id, device_token, bootstrap->request,
            sizeof(bootstrap->request), &bootstrap->request_length) !=
        AGENT_RELAY_DIRECTORY_OK) {
        schedule_retry(bootstrap, "failed to build directory request");
        return false;
    }
    bootstrap->watcher.fd = socket(family, SOCK_STREAM, 0);
    if (bootstrap->watcher.fd < 0) {
        schedule_retry(bootstrap, "failed to create directory socket");
        return false;
    }
    flags = fcntl(bootstrap->watcher.fd, F_GETFL, 0);
    if (flags < 0 || fcntl(bootstrap->watcher.fd, F_SETFL,
                           flags | O_NONBLOCK) != 0) {
        schedule_retry(bootstrap, "failed to configure directory socket");
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint->port);
    if (!bootstrap->mesh_profile.count && inet_pton(AF_INET, connect_ipv4,
                  &address.sin_addr) != 1) {
        schedule_retry(bootstrap, "invalid Directory connect address");
        return false;
    }
    result = connect(bootstrap->watcher.fd,
                     connect_address, address_length);
    if (result != 0 && errno != EINPROGRESS) {
        schedule_retry(bootstrap, "directory TCP connect failed");
        return false;
    }
    bootstrap->watcher.cb = socket_callback;
    if (uloop_fd_add(&bootstrap->watcher,
                     ULOOP_READ | ULOOP_WRITE | ULOOP_EDGE_TRIGGER) != 0) {
        schedule_retry(bootstrap, "failed to watch directory socket");
        return false;
    }
    bootstrap->phase = RELAY_BOOTSTRAP_TCP;
    bootstrap->deadline_ms = saturating_add(
        bootstrap->now_ms(), bootstrap->config.timeout_ms);
    increment(&bootstrap->queries);
    if (result == 0) drive_io(bootstrap);
    return true;
}

bool agent_relay_bootstrap_report_relay_failure(
    struct agent_relay_bootstrap *bootstrap,
    const char *relay_id
)
{
    if (bootstrap == NULL || relay_id == NULL ||
        !bootstrap->config.enabled || !bootstrap->assignment_active ||
        strcmp(bootstrap->assignment.relay_id, relay_id) != 0 ||
        !agent_peer_id_valid(relay_id, AGENT_RELAY_ID_LEN)) {
        return false;
    }
    if (strcmp(bootstrap->failed_relay_id, relay_id) != 0) {
        (void)snprintf(bootstrap->failed_relay_id,
                       sizeof(bootstrap->failed_relay_id), "%s", relay_id);
        increment(&bootstrap->relay_failover_requests);
        if (bootstrap->mesh_profile.count) {
            cleanup_io(bootstrap);
            bootstrap->phase = RELAY_BOOTSTRAP_IDLE;
            bootstrap->endpoint_index = (bootstrap->endpoint_index + 1) % bootstrap->endpoint_count;
            increment(&bootstrap->directory_failovers);
        }
    }
    bootstrap->next_query_ms = bootstrap->now_ms();
    return true;
}

static void timer_callback(struct uloop_timeout *timer)
{
    struct agent_relay_bootstrap *bootstrap =
        container_of(timer, struct agent_relay_bootstrap, timer);
    uint64_t now = bootstrap->now_ms();

    if (bootstrap->phase != RELAY_BOOTSTRAP_IDLE &&
        bootstrap->phase != RELAY_BOOTSTRAP_DISABLED &&
        now >= bootstrap->deadline_ms) {
        schedule_retry(bootstrap, "directory request timed out");
    }
    if (bootstrap->assignment_active &&
        now >= bootstrap->assignment.expires_at_ms) {
        bootstrap->assignment_active = false;
        memset(&bootstrap->assignment, 0, sizeof(bootstrap->assignment));
        increment(&bootstrap->expirations);
        (void)bootstrap->config.assignment_handler(
            bootstrap->config.event_context, NULL);
    }
    if (bootstrap->phase == RELAY_BOOTSTRAP_IDLE &&
        now >= bootstrap->next_query_ms) {
        (void)agent_relay_bootstrap_refresh(bootstrap);
    }
    uloop_timeout_set(&bootstrap->timer, (int)RELAY_TICK_MS);
}

static bool config_valid(const struct agent_relay_bootstrap_config *config)
{
    if (config == NULL) return false;
    if (!config->enabled) return true;
    return config->assignment_handler != NULL &&
           config->poll_ms >= 5000U && config->poll_ms <= 3600000U &&
           config->timeout_ms >= 100U && config->timeout_ms <= 30000U &&
           config->ca_file[0] != '\0' &&
           config->client_cert_file[0] != '\0' &&
           config->client_key_file[0] != '\0' &&
           agent_peer_id_valid(config->router_id,
                               sizeof(config->router_id)) &&
           agent_peer_domain_valid(config->domain_id);
}

struct agent_relay_bootstrap *agent_relay_bootstrap_create(
    const struct agent_relay_bootstrap_config *config,
    uint64_t (*now_ms)(void),
    char *error,
    size_t error_capacity
)
{
    struct agent_relay_bootstrap *bootstrap;

    if (!config_valid(config) || now_ms == NULL) {
        set_error(error, error_capacity, "invalid relay bootstrap config");
        return NULL;
    }
    bootstrap = calloc(1U, sizeof(*bootstrap));
    if (bootstrap == NULL) {
        set_error(error, error_capacity, "failed to allocate relay bootstrap");
        return NULL;
    }
    bootstrap->config = *config;
    bootstrap->now_ms = now_ms;
    bootstrap->watcher.fd = -1;
    if (!config->enabled) {
        bootstrap->phase = RELAY_BOOTSTRAP_DISABLED;
        return bootstrap;
    }
    bool endpoints_ok;
    if (config->mesh_profile_json[0]) {
        endpoints_ok = agent_mesh_profile_parse(config->mesh_profile_json, &bootstrap->mesh_profile);
        if (endpoints_ok) {
            bootstrap->endpoint_count = bootstrap->mesh_profile.count;
            for (size_t i = 0; i < bootstrap->endpoint_count; ++i) {
                char endpoint[256];
                snprintf(endpoint, sizeof(endpoint), "https://directory-seed.mesh.local:%u%s",
                         bootstrap->mesh_profile.paths[i].directory_port, AGENT_OPEN_MESH_DIRECTORY_PATH);
                if (!agent_relay_directory_endpoint_parse(endpoint, "", &bootstrap->endpoints[i])) endpoints_ok = false;
            }
        }
    } else endpoints_ok = agent_relay_directory_endpoint_set_parse(
            bootstrap->config.directory_endpoint,
            bootstrap->config.directory_connect_ipv4,
            bootstrap->endpoints, AGENT_RELAY_BOOTSTRAP_MAX_DIRECTORIES,
            &bootstrap->endpoint_count);
    if (!endpoints_ok || !initialize_tls(bootstrap)) {
        set_error(error, error_capacity,
                  "failed to initialize Directory mTLS endpoint");
        free_tls(bootstrap);
        free(bootstrap);
        return NULL;
    }
    bootstrap->phase = RELAY_BOOTSTRAP_IDLE;
    bootstrap->timer.cb = timer_callback;
    bootstrap->next_query_ms = now_ms();
    uloop_timeout_set(&bootstrap->timer, 1);
    return bootstrap;
}

void agent_relay_bootstrap_destroy(struct agent_relay_bootstrap *bootstrap)
{
    if (bootstrap == NULL) return;
    uloop_timeout_cancel(&bootstrap->timer);
    cleanup_io(bootstrap);
    free_tls(bootstrap);
    free(bootstrap);
}

void agent_relay_bootstrap_get_status(
    const struct agent_relay_bootstrap *bootstrap,
    struct agent_relay_bootstrap_status *status
)
{
    uint64_t now;

    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (bootstrap == NULL) return;
    now = bootstrap->now_ms();
    status->enabled = bootstrap->config.enabled;
    status->query_active = bootstrap->phase != RELAY_BOOTSTRAP_DISABLED &&
                           bootstrap->phase != RELAY_BOOTSTRAP_IDLE;
    status->assignment_active = bootstrap->assignment_active;
    status->phase = phase_name(bootstrap->phase);
    status->queries = bootstrap->queries;
    status->assignments = bootstrap->assignments;
    status->renewals = bootstrap->renewals;
    status->failures = bootstrap->failures;
    status->expirations = bootstrap->expirations;
    status->directory_failovers = bootstrap->directory_failovers;
    status->relay_failover_requests = bootstrap->relay_failover_requests;
    status->tls_handshakes = bootstrap->tls_handshakes;
    status->tls_last_error_code = bootstrap->tls_last_error_code;
    status->tls_last_verify_flags = bootstrap->tls_last_verify_flags;
    (void)snprintf(status->tls_last_alpn,
                   sizeof(status->tls_last_alpn), "%s",
                   bootstrap->tls_last_alpn);
    status->directory_count = bootstrap->endpoint_count;
    status->directory_index = bootstrap->endpoint_index;
    if (bootstrap->endpoint_count > 0U) {
        const struct agent_relay_directory_endpoint *endpoint =
            &bootstrap->endpoints[bootstrap->endpoint_index];

        status->directory_dns = endpoint->connect_ipv4[0] == '\0';
        (void)snprintf(status->directory_server_identity,
                       sizeof(status->directory_server_identity), "%s",
                       endpoint->server_identity);
        (void)snprintf(status->directory_resolved_ipv4,
                       sizeof(status->directory_resolved_ipv4), "%s",
                       bootstrap->resolved_directory_ipv4);
    }
    status->assignment = bootstrap->assignment;
    if (bootstrap->assignment_active &&
        bootstrap->assignment.expires_at_ms > now) {
        status->assignment_remaining_ms =
            bootstrap->assignment.expires_at_ms - now;
    }
    set_error(status->last_error, sizeof(status->last_error),
              bootstrap->last_error);
}
