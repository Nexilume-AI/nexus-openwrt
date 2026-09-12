#ifndef NEXUS_AGENT_RELAY_BOOTSTRAP_H
#define NEXUS_AGENT_RELAY_BOOTSTRAP_H

#include "agent_relay_directory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_RELAY_BOOTSTRAP_FILE_LEN 256U
#define AGENT_RELAY_BOOTSTRAP_ERROR_LEN 160U
#define AGENT_RELAY_BOOTSTRAP_MAX_DIRECTORIES \
    AGENT_RELAY_DIRECTORY_MAX_ENDPOINTS
#define AGENT_RELAY_BOOTSTRAP_ENDPOINT_SET_LEN 1024U
#define AGENT_RELAY_BOOTSTRAP_IPV4_SET_LEN 128U

typedef bool (*agent_relay_assignment_handler)(
    void *context,
    const struct agent_relay_assignment *assignment
);

struct agent_relay_bootstrap_config {
    bool enabled;
    /* Comma-separated endpoints and optional positionally paired IPv4 pins. */
    char directory_endpoint[AGENT_RELAY_BOOTSTRAP_ENDPOINT_SET_LEN];
    char directory_connect_ipv4[AGENT_RELAY_BOOTSTRAP_IPV4_SET_LEN];
    char router_id[AGENT_RELAY_ROUTER_ID_LEN];
    char domain_id[AGENT_RELAY_DOMAIN_ID_LEN];
    char ca_file[AGENT_RELAY_BOOTSTRAP_FILE_LEN];
    char client_cert_file[AGENT_RELAY_BOOTSTRAP_FILE_LEN];
    char client_key_file[AGENT_RELAY_BOOTSTRAP_FILE_LEN];
    char device_token_file[AGENT_RELAY_BOOTSTRAP_FILE_LEN];
    uint32_t poll_ms;
    uint32_t timeout_ms;
    agent_relay_assignment_handler assignment_handler;
    void *event_context;
};

struct agent_relay_bootstrap_status {
    bool enabled;
    bool query_active;
    bool assignment_active;
    const char *phase;
    uint64_t assignment_remaining_ms;
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
    size_t directory_count;
    size_t directory_index;
    bool directory_dns;
    char directory_server_identity[AGENT_RELAY_DIRECTORY_HOST_LEN];
    char directory_resolved_ipv4[AGENT_RELAY_IPV4_LEN];
    char last_error[AGENT_RELAY_BOOTSTRAP_ERROR_LEN];
    struct agent_relay_assignment assignment;
};

struct agent_relay_bootstrap;

struct agent_relay_bootstrap *agent_relay_bootstrap_create(
    const struct agent_relay_bootstrap_config *config,
    uint64_t (*now_ms)(void),
    char *error,
    size_t error_capacity
);

void agent_relay_bootstrap_destroy(struct agent_relay_bootstrap *bootstrap);

bool agent_relay_bootstrap_refresh(struct agent_relay_bootstrap *bootstrap);

bool agent_relay_bootstrap_report_relay_failure(
    struct agent_relay_bootstrap *bootstrap,
    const char *relay_id
);

void agent_relay_bootstrap_get_status(
    const struct agent_relay_bootstrap *bootstrap,
    struct agent_relay_bootstrap_status *status
);

#endif
