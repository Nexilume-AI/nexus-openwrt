#ifndef NEXUS_AGENT_JWT_VERIFIER_H
#define NEXUS_AGENT_JWT_VERIFIER_H

#include "agent_auth_policy.h"

#include <json-c/json.h>
#include <mbedtls/pk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_JWT_KID_LEN 128U
#define AGENT_JWT_MAX_TOKEN_LEN 8192U
#define AGENT_JWT_MAX_KEYS 8U
#define AGENT_JWT_MAX_JWKS_BYTES 65536U

struct agent_jwt_key {
    mbedtls_pk_context public_key;
    char key_id[AGENT_JWT_KID_LEN];
    bool initialized;
};

struct agent_jwt_verifier {
    struct agent_jwt_key keys[AGENT_JWT_MAX_KEYS];
    size_t key_count;
    struct agent_auth_policy policy;
    bool initialized;
};
enum agent_jwt_result {
    AGENT_JWT_OK = 0,
    AGENT_JWT_INVALID_ARGUMENT,
    AGENT_JWT_MALFORMED,
    AGENT_JWT_UNSUPPORTED_ALGORITHM,
    AGENT_JWT_KEY_ID_MISMATCH,
    AGENT_JWT_SIGNATURE_INVALID,
    AGENT_JWT_CLAIMS_INVALID,
    AGENT_JWT_KEY_LOAD_FAILED
};

enum agent_jwt_result agent_jwt_verifier_init(
    struct agent_jwt_verifier *verifier,
    const char *public_key_file,
    const char *key_id,
    const struct agent_auth_policy *policy
);

enum agent_jwt_result agent_jwt_verifier_init_jwks(
    struct agent_jwt_verifier *verifier,
    const char *jwks_file,
    const struct agent_auth_policy *policy
);

void agent_jwt_verifier_free(struct agent_jwt_verifier *verifier);

enum agent_jwt_result agent_jwt_verify(
    struct agent_jwt_verifier *verifier,
    const char *token,
    size_t token_length,
    uint64_t now_seconds,
    struct agent_auth_claims *claims,
    enum agent_auth_result *claim_result
);

const char *agent_jwt_result_name(enum agent_jwt_result result);

#endif
