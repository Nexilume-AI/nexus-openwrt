#include "agent_gateway_contract.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);

    assert(written >= 0);
    assert((size_t)written < capacity);
}

static void test_unverified_identity_preserves_local_claim(void)
{
    struct agent_ipc_lookup_request request = {0};
    struct gateway_authenticated_identity identity = {0};
    bool overridden = true;

    set_text(request.tenant, sizeof(request.tenant), "tenant-a");
    assert(gateway_apply_authenticated_identity(
        &request, &identity, &overridden));
    assert(!overridden);
    assert(strcmp(request.tenant, "tenant-a") == 0);
}

static void test_verified_identity_overrides_claim(void)
{
    struct agent_ipc_lookup_request request = {0};
    struct gateway_authenticated_identity identity = {0};
    bool overridden = false;

    set_text(request.tenant, sizeof(request.tenant), "forged-tenant");
    identity.verified = true;
    set_text(identity.tenant, sizeof(identity.tenant), "tenant-a");
    set_text(identity.source_agent, sizeof(identity.source_agent),
             "agent://tenant-a/client-1");

    assert(gateway_apply_authenticated_identity(
        &request, &identity, &overridden));
    assert(overridden);
    assert(strcmp(request.tenant, "tenant-a") == 0);
}

static void test_verified_identity_requires_complete_claims(void)
{
    struct agent_ipc_lookup_request request = {0};
    struct gateway_authenticated_identity identity = {0};
    bool overridden = false;

    set_text(request.tenant, sizeof(request.tenant), "tenant-a");
    identity.verified = true;
    set_text(identity.tenant, sizeof(identity.tenant), "tenant-a");
    assert(!gateway_apply_authenticated_identity(
        &request, &identity, &overridden));
}

static void test_verified_identity_supplies_missing_tenant(void)
{
    struct agent_ipc_lookup_request request = {0};
    struct gateway_authenticated_identity identity = {0};
    bool overridden = true;

    identity.verified = true;
    set_text(identity.tenant, sizeof(identity.tenant), "tenant-a");
    set_text(identity.source_agent, sizeof(identity.source_agent),
             "agent://tenant-a/client-1");
    assert(gateway_apply_authenticated_identity(
        &request, &identity, &overridden));
    assert(!overridden);
    assert(strcmp(request.tenant, "tenant-a") == 0);
}

static void test_reserved_fields(void)
{
    assert(gateway_envelope_field_is_reserved("claims"));
    assert(gateway_envelope_field_is_reserved("authenticated_tenant"));
    assert(gateway_envelope_field_is_reserved("route_path"));
    assert(!gateway_envelope_field_is_reserved("tenant"));
    assert(!gateway_envelope_field_is_reserved("source_agent"));
}

static void test_successful_ipc_lifecycle(void)
{
    struct gateway_ipc_machine machine;

    gateway_ipc_machine_init(&machine);
    assert(machine.phase == GATEWAY_IPC_CONNECTING);
    assert(machine.outcome == GATEWAY_IPC_ACTIVE);
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_CONNECTED));
    assert(machine.phase == GATEWAY_IPC_SENDING);
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_REQUEST_SENT));
    assert(machine.phase == GATEWAY_IPC_RECEIVING);
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_RESPONSE_RECEIVED));
    assert(machine.phase == GATEWAY_IPC_FINISHED);
    assert(machine.outcome == GATEWAY_IPC_SUCCEEDED);
    assert(!gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_IO_FAILED));
}

static void test_timeout_failure_and_disconnect(void)
{
    struct gateway_ipc_machine machine;

    gateway_ipc_machine_init(&machine);
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_DEADLINE_EXPIRED));
    assert(machine.outcome == GATEWAY_IPC_TIMED_OUT);

    gateway_ipc_machine_init(&machine);
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_CONNECTED));
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_IO_FAILED));
    assert(machine.outcome == GATEWAY_IPC_FAILED);

    gateway_ipc_machine_init(&machine);
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_CONNECTED));
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_REQUEST_SENT));
    assert(gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_CLIENT_DISCONNECTED));
    assert(machine.outcome == GATEWAY_IPC_CANCELLED);
}

static void test_invalid_transition_is_rejected(void)
{
    struct gateway_ipc_machine machine;

    gateway_ipc_machine_init(&machine);
    assert(!gateway_ipc_machine_transition(
        &machine, GATEWAY_IPC_REQUEST_SENT));
    assert(machine.phase == GATEWAY_IPC_CONNECTING);
    assert(machine.outcome == GATEWAY_IPC_ACTIVE);
}

int main(void)
{
    test_unverified_identity_preserves_local_claim();
    test_verified_identity_overrides_claim();
    test_verified_identity_requires_complete_claims();
    test_verified_identity_supplies_missing_tenant();
    test_reserved_fields();
    test_successful_ipc_lifecycle();
    test_timeout_failure_and_disconnect();
    test_invalid_transition_is_rejected();
    return 0;
}
