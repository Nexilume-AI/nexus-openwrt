#ifndef NEXUS_AGENT_CARD_DIRECTORY_TRUST_H
#define NEXUS_AGENT_CARD_DIRECTORY_TRUST_H

#include "agent_card_authorization.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_CARD_DIRECTORY_ID_LEN 128U
#define AGENT_CARD_KEY_URI_LEN 512U
#define AGENT_CARD_TRUST_MAX_ENTRIES 32U
#define AGENT_CARD_TRUST_MAX_LIFETIME_MS 86400000U

enum agent_card_trust_key_status {
    AGENT_CARD_TRUST_KEY_ACTIVE = 1,
    AGENT_CARD_TRUST_KEY_RETIRING = 2,
    AGENT_CARD_TRUST_KEY_REVOKED = 3
};

enum agent_card_trust_result {
    AGENT_CARD_TRUST_OK = 0,
    AGENT_CARD_TRUST_INVALID = -1,
    AGENT_CARD_TRUST_SIGNATURE_REQUIRED = -2,
    AGENT_CARD_TRUST_EXPIRED = -3,
    AGENT_CARD_TRUST_ROLLBACK = -4,
    AGENT_CARD_TRUST_CONFLICT = -5
};

struct agent_card_trust_entry {
    char router_id[AGENT_CROSS_ROUTER_ID_LEN];
    char domain_id[AGENT_CROSS_DOMAIN_ID_LEN];
    char issuer[AGENT_CARD_ISSUER_LEN];
    char key_id[AGENT_CARD_KEY_ID_LEN];
    char public_key_uri[AGENT_CARD_KEY_URI_LEN];
    char public_key_sha256[AGENT_CARD_DIGEST_LEN];
    enum agent_card_trust_key_status status;
    uint64_t not_before_ms;
    uint64_t not_after_ms;
};

struct agent_card_trust_document {
    char directory_id[AGENT_CARD_DIRECTORY_ID_LEN];
    char digest_sha256[AGENT_CARD_DIGEST_LEN];
    uint64_t revision;
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;
    bool signature_verified;
    size_t entry_count;
    struct agent_card_trust_entry entries[AGENT_CARD_TRUST_MAX_ENTRIES];
};

struct agent_card_trust_manager {
    struct agent_card_trust_document document;
    bool loaded;
    uint64_t authorized_until_ms;
    uint64_t generation;
    uint64_t accepted;
    uint64_t rejected;
    uint64_t rollback_rejected;
    uint64_t expired;
};

void agent_card_trust_manager_init(struct agent_card_trust_manager *manager);

bool agent_card_trust_key_status_parse(
    const char *text,
    enum agent_card_trust_key_status *status
);

const char *agent_card_trust_key_status_name(
    enum agent_card_trust_key_status status
);

enum agent_card_trust_result agent_card_trust_replace(
    struct agent_card_trust_manager *manager,
    const struct agent_card_trust_document *document,
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms
);

bool agent_card_trust_authorize(
    const struct agent_card_trust_manager *manager,
    const struct agent_card_document *card,
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms,
    uint64_t *authorized_until_ms
);

bool agent_card_trust_prune(
    struct agent_card_trust_manager *manager,
    uint64_t now_monotonic_ms
);

const char *agent_card_trust_result_name(enum agent_card_trust_result result);

#endif
