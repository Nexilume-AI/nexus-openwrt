#include "agent_jwt_verifier.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct agent_auth_policy policy;
    struct agent_jwt_verifier verifier;
    enum agent_jwt_result result;

    if (argc != 2) {
        fprintf(stderr, "usage: agent-jwks-validate <jwks-file>\n");
        return 2;
    }
    memset(&policy, 0, sizeof(policy));
    snprintf(policy.issuer, sizeof(policy.issuer), "%s",
             "https://validation.invalid");
    snprintf(policy.audience, sizeof(policy.audience), "%s",
             "nexus-agent-router");
    snprintf(policy.required_scope, sizeof(policy.required_scope), "%s",
             "agent.route");
    policy.clock_skew_seconds = 30U;
    policy.max_token_lifetime_seconds = 300U;

    result = agent_jwt_verifier_init_jwks(
        &verifier, argv[1], &policy);
    if (result != AGENT_JWT_OK) {
        fprintf(stderr, "JWKS validation failed: %s\n",
                agent_jwt_result_name(result));
        return 1;
    }
    printf("JWKS valid: %zu usable key(s)\n", verifier.key_count);
    agent_jwt_verifier_free(&verifier);
    return 0;
}
