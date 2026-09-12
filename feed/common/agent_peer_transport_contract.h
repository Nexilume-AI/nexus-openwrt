#ifndef NEXUS_AGENT_PEER_TRANSPORT_CONTRACT_H
#define NEXUS_AGENT_PEER_TRANSPORT_CONTRACT_H

#include "agent_arpx_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_PEER_TRANSPORT_HOST_LEN 128U
#define AGENT_PEER_TRANSPORT_AUTHORITY_LEN 192U
#define AGENT_PEER_TRANSPORT_IPV4_LEN 16U
#define AGENT_PEER_TRANSPORT_PATH "/arpx/v1"
#define AGENT_PEER_TRANSPORT_CONTENT_TYPE "application/arpx+cbor"

enum agent_peer_transport_contract_result {
    AGENT_PEER_TRANSPORT_CONTRACT_OK = 0,
    AGENT_PEER_TRANSPORT_CONTRACT_INVALID = -1,
    AGENT_PEER_TRANSPORT_CONTRACT_FRAME_TOO_LARGE = -2,
    AGENT_PEER_TRANSPORT_CONTRACT_CALLBACK_ERROR = -3
};

struct agent_peer_transport_endpoint {
    char server_identity[AGENT_PEER_TRANSPORT_HOST_LEN];
    char authority[AGENT_PEER_TRANSPORT_AUTHORITY_LEN];
    char connect_ipv4[AGENT_PEER_TRANSPORT_IPV4_LEN];
    uint16_t port;
};

struct agent_peer_transport_ingress {
    uint8_t frame[AGENT_ARPX_MAX_FRAME_SIZE];
    size_t used;
    size_t expected;
};

enum agent_peer_transport_role {
    AGENT_PEER_TRANSPORT_ROLE_INVALID = 0,
    AGENT_PEER_TRANSPORT_ROLE_DIAL,
    AGENT_PEER_TRANSPORT_ROLE_ACCEPT
};

typedef bool (*agent_peer_transport_frame_callback)(
    const uint8_t *frame,
    size_t frame_size,
    void *context
);

bool agent_peer_transport_ipv4_valid(const char *address);

enum agent_peer_transport_role agent_peer_transport_role_select(
    const char *local_router_id,
    const char *remote_router_id
);

bool agent_peer_transport_endpoint_parse(
    const char *endpoint,
    const char *connect_ipv4,
    struct agent_peer_transport_endpoint *parsed
);

void agent_peer_transport_ingress_init(
    struct agent_peer_transport_ingress *ingress
);

enum agent_peer_transport_contract_result agent_peer_transport_ingress_feed(
    struct agent_peer_transport_ingress *ingress,
    const uint8_t *data,
    size_t size,
    agent_peer_transport_frame_callback callback,
    void *context,
    size_t *frames_emitted
);

#endif
