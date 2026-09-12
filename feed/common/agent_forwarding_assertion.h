#ifndef NEXUS_AGENT_FORWARDING_ASSERTION_H
#define NEXUS_AGENT_FORWARDING_ASSERTION_H

#include "agent_ipc_protocol.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_FORWARDING_KID_LEN 65U
#define AGENT_FORWARDING_ISSUER_LEN 128U
#define AGENT_FORWARDING_NONCE_LEN 16U
#define AGENT_FORWARDING_NONCE_HEX_LEN 33U
#define AGENT_FORWARDING_DIGEST_LEN 32U
#define AGENT_FORWARDING_MAX_TTL_SECONDS 300U

struct agent_forwarding_context {
    const char *source_router_id;
    const char *target_router_id;
    const char *source_agent;
    const char *tenant;
    const char *intent;
    const char *task_id;
    uint8_t hop_limit;
    const uint8_t *body;
    size_t body_length;
};

struct agent_forwarding_verified {
    char issuer[AGENT_FORWARDING_ISSUER_LEN];
    char nonce[AGENT_FORWARDING_NONCE_HEX_LEN];
    uint64_t issued_at;
    uint64_t expires_at;
    uint8_t initial_hop_limit;
    uint8_t body_sha256[AGENT_FORWARDING_DIGEST_LEN];
};

struct agent_forwarding_signer {
    mbedtls_pk_context private_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    char key_id[AGENT_FORWARDING_KID_LEN];
    char issuer[AGENT_FORWARDING_ISSUER_LEN];
    uint32_t ttl_seconds;
    bool initialized;
};

struct agent_forwarding_verifier {
    mbedtls_pk_context public_key;
    char key_id[AGENT_FORWARDING_KID_LEN];
    char issuer[AGENT_FORWARDING_ISSUER_LEN];
    uint32_t clock_skew_seconds;
    uint32_t max_ttl_seconds;
    bool initialized;
};

enum agent_forwarding_result {
    AGENT_FORWARDING_OK = 0,
    AGENT_FORWARDING_INVALID_ARGUMENT,
    AGENT_FORWARDING_KEY_LOAD_FAILED,
    AGENT_FORWARDING_CRYPTO_FAILED,
    AGENT_FORWARDING_MALFORMED,
    AGENT_FORWARDING_KEY_ID_MISMATCH,
    AGENT_FORWARDING_SIGNATURE_INVALID,
    AGENT_FORWARDING_ISSUER_MISMATCH,
    AGENT_FORWARDING_EXPIRED,
    AGENT_FORWARDING_LIFETIME_EXCEEDED,
    AGENT_FORWARDING_CONTEXT_MISMATCH,
    AGENT_FORWARDING_BODY_MISMATCH
};

enum agent_forwarding_result agent_forwarding_signer_init(
    struct agent_forwarding_signer *signer,
    const char *private_key_file,
    const char *key_id,
    const char *issuer,
    uint32_t ttl_seconds
);

void agent_forwarding_signer_free(struct agent_forwarding_signer *signer);

enum agent_forwarding_result agent_forwarding_issue(
    struct agent_forwarding_signer *signer,
    const struct agent_forwarding_context *context,
    uint64_t now_seconds,
    char *output,
    size_t capacity
);

enum agent_forwarding_result agent_forwarding_verifier_init(
    struct agent_forwarding_verifier *verifier,
    const char *public_key_file,
    const char *key_id,
    const char *issuer,
    uint32_t clock_skew_seconds,
    uint32_t max_ttl_seconds
);

void agent_forwarding_verifier_free(
    struct agent_forwarding_verifier *verifier
);

enum agent_forwarding_result agent_forwarding_verify(
    struct agent_forwarding_verifier *verifier,
    const char *assertion,
    const struct agent_forwarding_context *expected,
    uint64_t now_seconds,
    struct agent_forwarding_verified *verified
);

enum agent_forwarding_result agent_forwarding_verify_body(
    const struct agent_forwarding_verified *verified,
    const uint8_t *body,
    size_t body_length
);

const char *agent_forwarding_result_name(enum agent_forwarding_result result);

#endif
