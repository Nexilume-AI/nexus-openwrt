#ifndef NEXUS_AGENT_CARD_AUTHORIZATION_H
#define NEXUS_AGENT_CARD_AUTHORIZATION_H

#include "agent_arpx_protocol.h"
#include "agent_cross_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_CARD_KEY_ID_LEN 65U
#define AGENT_CARD_ISSUER_LEN 128U
#define AGENT_CARD_DIGEST_LEN 65U
#define AGENT_CARD_SKILL_ID_LEN 65U
#define AGENT_CARD_MAX_CAPABILITIES 16U
#define AGENT_CARD_ALLOWLIST_LEN 1025U
#define AGENT_CARD_MAX_LIFETIME_MS 86400000U

enum agent_card_authorization_mode {
    AGENT_CARD_AUTH_OFF = 0,
    AGENT_CARD_AUTH_SAME_DOMAIN = 1,
    AGENT_CARD_AUTH_ALLOWLIST = 2,
    AGENT_CARD_AUTH_DIRECTORY_TRUSTED = 3,
    AGENT_CARD_AUTH_ALL_SIGNED = 4
};

enum agent_card_result {
    AGENT_CARD_OK = 0,
    AGENT_CARD_INVALID = -1,
    AGENT_CARD_SIGNATURE_REQUIRED = -2,
    AGENT_CARD_IDENTITY_MISMATCH = -3,
    AGENT_CARD_POLICY_REJECTED = -4,
    AGENT_CARD_EXPIRED = -5,
    AGENT_CARD_ROLLBACK = -6,
    AGENT_CARD_CONFLICT = -7,
    AGENT_CARD_FULL = -8,
    AGENT_CARD_NO_MEMORY = -9,
    AGENT_CARD_NOT_FOUND = -10
};

struct agent_card_capability {
    char skill_id[AGENT_CARD_SKILL_ID_LEN];
    char intent[AGENT_ARPX_INTENT_LEN];
    uint32_t version;
    char tenant[AGENT_ARPX_TENANT_LEN];
};

/* The fetcher verifies the detached signature over the exact downloaded
 * bytes before constructing this bounded semantic document. */
struct agent_card_document {
    char router_id[AGENT_CROSS_ROUTER_ID_LEN];
    char domain_id[AGENT_CROSS_DOMAIN_ID_LEN];
    char card_uri[AGENT_CROSS_CARD_URI_LEN];
    char arpx_endpoint[AGENT_ARPX_URI_LEN];
    char issuer[AGENT_CARD_ISSUER_LEN];
    char key_id[AGENT_CARD_KEY_ID_LEN];
    char digest_sha256[AGENT_CARD_DIGEST_LEN];
    uint64_t revision;
    uint64_t issued_at_ms;
    uint64_t expires_at_ms;
    bool signature_verified;
    bool directory_trusted;
    uint64_t directory_authorized_until_ms;
    size_t capability_count;
    struct agent_card_capability capabilities[AGENT_CARD_MAX_CAPABILITIES];
};

struct agent_card_policy {
    enum agent_card_authorization_mode mode;
    char local_domain[AGENT_CROSS_DOMAIN_ID_LEN];
    char allowlist[AGENT_CARD_ALLOWLIST_LEN];
};

struct agent_card_record {
    struct agent_card_document document;
    uint64_t candidate_generation;
    uint64_t authorized_at_ms;
    uint64_t authorized_until_ms;
    struct agent_card_record *next;
};

struct agent_card_manager {
    struct agent_card_record *head;
    size_t count;
    size_t max_cards;
    uint64_t generation;
    uint64_t accepted;
    uint64_t rejected;
    uint64_t signature_rejected;
    uint64_t identity_rejected;
    uint64_t policy_rejected;
    uint64_t rollback_rejected;
    uint64_t expired;
    uint64_t revoked;
    uint64_t route_updates_rejected;
};

typedef void (*agent_card_removing_cb)(
    void *context,
    const char *router_id
);

bool agent_card_policy_init(
    struct agent_card_policy *policy,
    const char *mode,
    const char *local_domain,
    const char *allowlist
);

const char *agent_card_authorization_mode_name(
    enum agent_card_authorization_mode mode
);

void agent_card_manager_init(
    struct agent_card_manager *manager,
    size_t max_cards
);

void agent_card_manager_destroy(struct agent_card_manager *manager);

enum agent_card_result agent_card_ingest(
    struct agent_card_manager *manager,
    const struct agent_card_policy *policy,
    const struct agent_cross_candidate *candidate,
    const struct agent_card_document *document,
    uint64_t candidate_generation,
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms
);

const struct agent_card_record *agent_card_find(
    const struct agent_card_manager *manager,
    const char *router_id
);

bool agent_card_capability_authorized(
    struct agent_card_manager *manager,
    const char *router_id,
    const char *domain_id,
    const char *intent,
    uint32_t version,
    const char *tenant,
    uint64_t now_ms
);

enum agent_card_result agent_card_revoke(
    struct agent_card_manager *manager,
    const char *router_id,
    agent_card_removing_cb removing,
    void *context
);

size_t agent_card_prune(
    struct agent_card_manager *manager,
    uint64_t now_ms,
    agent_card_removing_cb removing,
    void *context
);

const char *agent_card_result_name(enum agent_card_result result);

#endif
