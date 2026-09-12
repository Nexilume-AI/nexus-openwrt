#ifndef NEXUS_AGENT_AUTH_POLICY_H
#define NEXUS_AGENT_AUTH_POLICY_H

#include "agent_gateway_contract.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_AUTH_ISSUER_LEN 128U
#define AGENT_AUTH_AUDIENCE_LEN 128U
#define AGENT_AUTH_SCOPE_LEN 256U
#define AGENT_AUTH_JTI_LEN 128U
#define AGENT_AUTH_TXN_LEN 128U

struct agent_auth_policy {
    char issuer[AGENT_AUTH_ISSUER_LEN];
    char audience[AGENT_AUTH_AUDIENCE_LEN];
    char required_scope[AGENT_AUTH_SCOPE_LEN];
    uint32_t clock_skew_seconds;
    uint32_t max_token_lifetime_seconds;
};

struct agent_auth_claims {
    char issuer[AGENT_AUTH_ISSUER_LEN];
    char audience[AGENT_AUTH_AUDIENCE_LEN];
    char subject[AGENT_IPC_URI_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char scope[AGENT_AUTH_SCOPE_LEN];
    char jti[AGENT_AUTH_JTI_LEN];
    char transaction_id[AGENT_AUTH_TXN_LEN];
    char target_agent[AGENT_IPC_URI_LEN];
    bool transaction_token;
    uint64_t issued_at;
    uint64_t not_before;
    uint64_t expires_at;
};

enum agent_auth_result {
    AGENT_AUTH_OK = 0,
    AGENT_AUTH_INVALID_ARGUMENT,
    AGENT_AUTH_MISSING_CLAIM,
    AGENT_AUTH_ISSUER_MISMATCH,
    AGENT_AUTH_AUDIENCE_MISMATCH,
    AGENT_AUTH_SCOPE_DENIED,
    AGENT_AUTH_NOT_YET_VALID,
    AGENT_AUTH_EXPIRED,
    AGENT_AUTH_LIFETIME_EXCEEDED
};

bool agent_auth_scope_contains(const char *scope, const char *required);

enum agent_auth_result agent_auth_validate_claims(
    const struct agent_auth_policy *policy,
    const struct agent_auth_claims *claims,
    uint64_t now_seconds
);

bool agent_auth_identity_from_claims(
    const struct agent_auth_claims *claims,
    struct gateway_authenticated_identity *identity
);

const char *agent_auth_result_name(enum agent_auth_result result);

#endif
