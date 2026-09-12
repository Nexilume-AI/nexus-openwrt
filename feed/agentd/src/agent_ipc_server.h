#ifndef NEXUS_AGENT_IPC_SERVER_H
#define NEXUS_AGENT_IPC_SERVER_H

#include "route_table.h"
#include "agent_ipc_protocol.h"
#include "agent_public_ipv6.h"
#include "peer_table.h"

#include <libubox/uloop.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define AGENT_IPC_SOCKET_PATH_LEN 108U

struct agent_ipc_client;

typedef bool (*agent_ipc_invoke_handler)(
    void *context,
    struct agent_ipc_client *client,
    const struct agent_ipc_invoke_request *request
);

typedef void (*agent_ipc_cancel_handler)(
    void *context,
    struct agent_ipc_client *client
);

typedef bool (*agent_ipc_register_handler)(
    void *context,
    const struct agent_ipc_register_request *request,
    struct agent_ipc_lease_response *response,
    uint32_t *error_code,
    char *error,
    size_t error_capacity
);

typedef bool (*agent_ipc_renew_handler)(
    void *context,
    const struct agent_ipc_renew_request *request,
    struct agent_ipc_lease_response *response,
    uint32_t *error_code,
    char *error,
    size_t error_capacity
);

typedef bool (*agent_ipc_unregister_handler)(
    void *context,
    const struct agent_ipc_unregister_request *request,
    struct agent_ipc_lease_response *response,
    uint32_t *error_code,
    char *error,
    size_t error_capacity
);

struct agent_ipc_server {
    struct uloop_fd listener;
    struct route_table *routes;
    struct peer_table *peers;
    struct agent_public_ipv6_pool *public_ipv6;
    uint64_t (*now_ms)(void);
    uid_t allowed_uid;
    uint64_t accepted_connections;
    uint64_t rejected_credentials;
    uint64_t invoke_requests;
    uint64_t invoke_completions;
    uint64_t stream_starts;
    uint64_t stream_data_frames;
    uint64_t stream_completions;
    agent_ipc_invoke_handler invoke_handler;
    agent_ipc_cancel_handler cancel_handler;
    void *invoke_context;
    agent_ipc_register_handler register_handler;
    agent_ipc_renew_handler renew_handler;
    agent_ipc_unregister_handler unregister_handler;
    void *registration_context;
    char socket_path[AGENT_IPC_SOCKET_PATH_LEN];
};

int agent_ipc_server_start(
    struct agent_ipc_server *server,
    const char *socket_path,
    struct route_table *routes,
    struct peer_table *peers,
    struct agent_public_ipv6_pool *public_ipv6,
    uint64_t (*now_ms)(void),
    uid_t allowed_uid,
    agent_ipc_invoke_handler invoke_handler,
    agent_ipc_cancel_handler cancel_handler,
    void *invoke_context,
    agent_ipc_register_handler register_handler,
    agent_ipc_renew_handler renew_handler,
    agent_ipc_unregister_handler unregister_handler,
    void *registration_context
);

void agent_ipc_server_stop(struct agent_ipc_server *server);

bool agent_ipc_client_respond_invoke(
    struct agent_ipc_client *client,
    const struct agent_ipc_invoke_response *response
);

bool agent_ipc_client_respond_error(
    struct agent_ipc_client *client,
    uint32_t request_id,
    uint32_t code,
    const char *message
);

bool agent_ipc_client_stream_start(
    struct agent_ipc_client *client,
    uint32_t request_id,
    uint16_t status_code,
    const char *content_type
);

bool agent_ipc_client_stream_data(
    struct agent_ipc_client *client,
    uint32_t request_id,
    const uint8_t *data,
    size_t data_length
);

bool agent_ipc_client_stream_end(
    struct agent_ipc_client *client,
    uint32_t request_id,
    uint64_t total_bytes
);

#endif
