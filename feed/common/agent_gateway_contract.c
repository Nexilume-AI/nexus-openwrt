#include "agent_gateway_contract.h"

#include <stdio.h>
#include <string.h>

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

bool gateway_apply_authenticated_identity(
    struct agent_ipc_lookup_request *request,
    const struct gateway_authenticated_identity *identity,
    bool *tenant_overridden
)
{
    if (request == NULL || identity == NULL || tenant_overridden == NULL) {
        return false;
    }

    *tenant_overridden = false;
    if (!identity->verified) {
        return request->tenant[0] != '\0';
    }
    if (identity->tenant[0] == '\0' ||
        identity->source_agent[0] == '\0') {
        return false;
    }

    *tenant_overridden =
        request->tenant[0] != '\0' &&
        strcmp(request->tenant, identity->tenant) != 0;
    return copy_text(request->tenant, sizeof(request->tenant),
                     identity->tenant);
}

bool gateway_envelope_field_is_reserved(const char *name)
{
    static const char *const reserved[] = {
        "auth_context",
        "authenticated_identity",
        "authenticated_subject",
        "authenticated_tenant",
        "claims",
        "policy_decision_id",
        "principal",
        "route_path"
    };
    size_t index;

    if (name == NULL) {
        return false;
    }
    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]);
         index++) {
        if (strcmp(name, reserved[index]) == 0) {
            return true;
        }
    }
    return false;
}

void gateway_ipc_machine_init(struct gateway_ipc_machine *machine)
{
    if (machine == NULL) {
        return;
    }
    machine->phase = GATEWAY_IPC_CONNECTING;
    machine->outcome = GATEWAY_IPC_ACTIVE;
}

bool gateway_ipc_machine_transition(
    struct gateway_ipc_machine *machine,
    enum gateway_ipc_event event
)
{
    if (machine == NULL ||
        machine->outcome != GATEWAY_IPC_ACTIVE) {
        return false;
    }

    if (event == GATEWAY_IPC_DEADLINE_EXPIRED) {
        machine->phase = GATEWAY_IPC_FINISHED;
        machine->outcome = GATEWAY_IPC_TIMED_OUT;
        return true;
    }
    if (event == GATEWAY_IPC_CLIENT_DISCONNECTED) {
        machine->phase = GATEWAY_IPC_FINISHED;
        machine->outcome = GATEWAY_IPC_CANCELLED;
        return true;
    }
    if (event == GATEWAY_IPC_IO_FAILED) {
        machine->phase = GATEWAY_IPC_FINISHED;
        machine->outcome = GATEWAY_IPC_FAILED;
        return true;
    }

    switch (machine->phase) {
    case GATEWAY_IPC_CONNECTING:
        if (event != GATEWAY_IPC_CONNECTED) {
            return false;
        }
        machine->phase = GATEWAY_IPC_SENDING;
        return true;
    case GATEWAY_IPC_SENDING:
        if (event != GATEWAY_IPC_REQUEST_SENT) {
            return false;
        }
        machine->phase = GATEWAY_IPC_RECEIVING;
        return true;
    case GATEWAY_IPC_RECEIVING:
        if (event != GATEWAY_IPC_RESPONSE_RECEIVED) {
            return false;
        }
        machine->phase = GATEWAY_IPC_FINISHED;
        machine->outcome = GATEWAY_IPC_SUCCEEDED;
        return true;
    case GATEWAY_IPC_FINISHED:
    default:
        return false;
    }
}
