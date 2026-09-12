#ifndef NEXUS_AGENT_LAN_SESSION_H
#define NEXUS_AGENT_LAN_SESSION_H

#include "agent_ipc_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_LAN_SESSION_SECRET_LEN 64U
#define AGENT_LAN_SESSION_NONCE_LEN 16U
#define AGENT_LAN_SESSION_SOURCE_LEN 46U
#define AGENT_LAN_SESSION_TOKEN_MAX 1024U
#define AGENT_LAN_SESSION_MAX_TTL_SECONDS 300U
#define AGENT_LAN_SESSION_SCOPE "agent.register agent.route agent.invoke"

struct agent_lan_session_claims {
    char source_address[AGENT_LAN_SESSION_SOURCE_LEN];
    char tenant[AGENT_IPC_TENANT_LEN];
    char origin[AGENT_IPC_URI_LEN];
    uint64_t issued_at;
    uint64_t expires_at;
};

enum agent_lan_session_result {
    AGENT_LAN_SESSION_OK = 0,
    AGENT_LAN_SESSION_INVALID_ARGUMENT,
    AGENT_LAN_SESSION_MALFORMED,
    AGENT_LAN_SESSION_SIGNATURE_INVALID,
    AGENT_LAN_SESSION_SOURCE_MISMATCH,
    AGENT_LAN_SESSION_EXPIRED,
    AGENT_LAN_SESSION_LIFETIME_EXCEEDED,
    AGENT_LAN_SESSION_CRYPTO_FAILED
};

bool agent_lan_session_secret_is_valid(const char *secret);

enum agent_lan_session_result agent_lan_session_issue(
    const char *secret,
    const char *source_address,
    const char *tenant,
    const char *origin,
    uint64_t now_seconds,
    uint32_t ttl_seconds,
    const uint8_t nonce[AGENT_LAN_SESSION_NONCE_LEN],
    char *output,
    size_t capacity
);

enum agent_lan_session_result agent_lan_session_verify(
    const char *secret,
    const char *token,
    const char *source_address,
    uint64_t now_seconds,
    struct agent_lan_session_claims *claims
);

const char *agent_lan_session_result_name(enum agent_lan_session_result result);

#endif
