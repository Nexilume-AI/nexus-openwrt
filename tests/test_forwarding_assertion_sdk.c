#include "agent_forwarding_assertion.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    static const uint8_t body[] = "{\"payload\":{\"value\":42}}";
    static const uint8_t changed[] = "{\"payload\":{\"value\":43}}";
    struct agent_forwarding_signer signer;
    struct agent_forwarding_verifier verifier;
    struct agent_forwarding_verifier wrong_issuer_verifier;
    struct agent_forwarding_context issued;
    struct agent_forwarding_context expected;
    struct agent_forwarding_verified verified;
    char assertion[AGENT_IPC_FORWARDING_ASSERTION_LEN];
    char tampered[AGENT_IPC_FORWARDING_ASSERTION_LEN];
    char *signature;

    if (argc != 3) return 2;
    assert(agent_forwarding_signer_init(
               &signer, argv[1], "nexus-cloud-1", "nexus-cloud-e2e", 30U) ==
           AGENT_FORWARDING_OK);
    assert(agent_forwarding_verifier_init(
               &verifier, argv[2], "nexus-cloud-1", "nexus-cloud-e2e",
               2U, 60U) == AGENT_FORWARDING_OK);
    memset(&issued, 0, sizeof(issued));
    issued.source_router_id = "nexus-cloud";
    issued.target_router_id = "router-c";
    issued.source_agent = "agent://tenant-a/client";
    issued.tenant = "tenant-a";
    issued.intent = "chip.verilog.verify.lint.v1";
    issued.task_id = "task-p61";
    issued.hop_limit = 8U;
    issued.body = body;
    issued.body_length = sizeof(body) - 1U;
    assert(agent_forwarding_issue(
               &signer, &issued, 1800000000U,
               assertion, sizeof(assertion)) == AGENT_FORWARDING_OK);
    assert(strncmp(assertion, "nfa1.nexus-cloud-1.",
                   strlen("nfa1.nexus-cloud-1.")) == 0);

    expected = issued;
    expected.hop_limit = 6U;
    expected.body = NULL;
    expected.body_length = 0U;
    assert(agent_forwarding_verify(
               &verifier, assertion, &expected, 1800000001U,
               &verified) == AGENT_FORWARDING_OK);
    assert(verified.initial_hop_limit == 8U);
    assert(verified.expires_at == 1800000030U);
    assert(strlen(verified.nonce) == AGENT_FORWARDING_NONCE_HEX_LEN - 1U);
    assert(agent_forwarding_verify_body(
               &verified, body, sizeof(body) - 1U) == AGENT_FORWARDING_OK);
    assert(agent_forwarding_verify_body(
               &verified, changed, sizeof(changed) - 1U) ==
           AGENT_FORWARDING_BODY_MISMATCH);

    expected.source_router_id = "nexus-cloud-tampered";
    assert(agent_forwarding_verify(
               &verifier, assertion, &expected, 1800000001U,
               &verified) == AGENT_FORWARDING_CONTEXT_MISMATCH);
    expected.source_router_id = "nexus-cloud";
    assert(agent_forwarding_verifier_init(
               &wrong_issuer_verifier, argv[2], "nexus-cloud-1",
               "nexus-cloud-tampered", 2U, 60U) == AGENT_FORWARDING_OK);
    assert(agent_forwarding_verify(
               &wrong_issuer_verifier, assertion, &expected, 1800000001U,
               &verified) == AGENT_FORWARDING_ISSUER_MISMATCH);
    agent_forwarding_verifier_free(&wrong_issuer_verifier);

    expected.tenant = "tenant-b";
    assert(agent_forwarding_verify(
               &verifier, assertion, &expected, 1800000001U,
               &verified) == AGENT_FORWARDING_CONTEXT_MISMATCH);
    expected.tenant = "tenant-a";
    assert(agent_forwarding_verify(
               &verifier, assertion, &expected, 1800000040U,
               &verified) == AGENT_FORWARDING_EXPIRED);

    snprintf(tampered, sizeof(tampered), "%s", assertion);
    signature = strrchr(tampered, '.');
    assert(signature != NULL && strlen(signature) > 12U);
    signature[10] = signature[10] == 'A' ? 'B' : 'A';
    assert(agent_forwarding_verify(
               &verifier, tampered, &expected, 1800000001U,
               &verified) == AGENT_FORWARDING_SIGNATURE_INVALID);

    agent_forwarding_verifier_free(&verifier);
    agent_forwarding_signer_free(&signer);
    puts("P6.1 forwarding assertion SDK runtime tests passed");
    return 0;
}
