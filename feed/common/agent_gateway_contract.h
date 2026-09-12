#ifndef NEXUS_AGENT_GATEWAY_CONTRACT_H
#define NEXUS_AGENT_GATEWAY_CONTRACT_H

#include "agent_ipc_protocol.h"

#include <stdbool.h>
#include <stddef.h>

struct gateway_authenticated_identity {
    bool verified;
    char tenant[AGENT_IPC_TENANT_LEN];
    char source_agent[AGENT_IPC_URI_LEN];
};

enum gateway_ipc_phase {
    GATEWAY_IPC_CONNECTING = 0,
    GATEWAY_IPC_SENDING,
    GATEWAY_IPC_RECEIVING,
    GATEWAY_IPC_FINISHED
};

enum gateway_ipc_event {
    GATEWAY_IPC_CONNECTED = 0,
    GATEWAY_IPC_REQUEST_SENT,
    GATEWAY_IPC_RESPONSE_RECEIVED,
    GATEWAY_IPC_DEADLINE_EXPIRED,
    GATEWAY_IPC_CLIENT_DISCONNECTED,
    GATEWAY_IPC_IO_FAILED
};

enum gateway_ipc_outcome {
    GATEWAY_IPC_ACTIVE = 0,
    GATEWAY_IPC_SUCCEEDED,
    GATEWAY_IPC_TIMED_OUT,
    GATEWAY_IPC_CANCELLED,
    GATEWAY_IPC_FAILED
};

struct gateway_ipc_machine {
    enum gateway_ipc_phase phase;
    enum gateway_ipc_outcome outcome;
};

bool gateway_apply_authenticated_identity(
    struct agent_ipc_lookup_request *request,
    const struct gateway_authenticated_identity *identity,
    bool *tenant_overridden
);

bool gateway_envelope_field_is_reserved(const char *name);

void gateway_ipc_machine_init(struct gateway_ipc_machine *machine);

bool gateway_ipc_machine_transition(
    struct gateway_ipc_machine *machine,
    enum gateway_ipc_event event
);

#endif
