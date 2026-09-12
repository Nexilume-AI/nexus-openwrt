#include "agent_card_authorization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) (*value)++;
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static bool copy_optional(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || capacity == 0U || source == NULL) return false;
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static bool text_valid(const char *text, size_t capacity)
{
    size_t length;

    if (text == NULL) return false;
    length = strnlen(text, capacity);
    return length > 0U && length < capacity;
}

static bool digest_valid(const char *digest)
{
    size_t index;

    if (!text_valid(digest, AGENT_CARD_DIGEST_LEN) ||
        strlen(digest) != AGENT_CARD_DIGEST_LEN - 1U) return false;
    for (index = 0U; index < AGENT_CARD_DIGEST_LEN - 1U; index++) {
        char value = digest[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

static bool parse_mode(
    const char *text,
    enum agent_card_authorization_mode *mode
)
{
    if (text == NULL || mode == NULL) return false;
    if (strcmp(text, "off") == 0) *mode = AGENT_CARD_AUTH_OFF;
    else if (strcmp(text, "same-domain") == 0) {
        *mode = AGENT_CARD_AUTH_SAME_DOMAIN;
    } else if (strcmp(text, "allowlist") == 0) {
        *mode = AGENT_CARD_AUTH_ALLOWLIST;
    } else if (strcmp(text, "directory-trusted") == 0) {
        *mode = AGENT_CARD_AUTH_DIRECTORY_TRUSTED;
    } else if (strcmp(text, "all-signed") == 0) {
        *mode = AGENT_CARD_AUTH_ALL_SIGNED;
    } else return false;
    return true;
}

static bool allowlist_valid(const char *text)
{
    const char *start;
    const char *comma;
    char identifier[AGENT_CROSS_ROUTER_ID_LEN];
    size_t length;

    if (text == NULL || text[0] == '\0') return true;
    if (text[strlen(text) - 1U] == ',') return false;
    for (start = text; *start != '\0'; start = comma + 1U) {
        comma = strchr(start, ',');
        length = comma == NULL ? strlen(start) : (size_t)(comma - start);
        if (length == 0U || length >= sizeof(identifier)) return false;
        memcpy(identifier, start, length);
        identifier[length] = '\0';
        if (!text_valid(identifier, sizeof(identifier))) return false;
        if (comma == NULL) break;
    }
    return true;
}

static bool allowlist_contains(const char *text, const char *router_id)
{
    const char *start;
    const char *comma;
    size_t length;
    size_t router_length;

    if (text == NULL || router_id == NULL) return false;
    router_length = strlen(router_id);
    for (start = text; *start != '\0'; start = comma + 1U) {
        comma = strchr(start, ',');
        length = comma == NULL ? strlen(start) : (size_t)(comma - start);
        if (length == router_length &&
            memcmp(start, router_id, length) == 0) return true;
        if (comma == NULL) break;
    }
    return false;
}

bool agent_card_policy_init(
    struct agent_card_policy *policy,
    const char *mode,
    const char *local_domain,
    const char *allowlist
)
{
    if (policy == NULL || !text_valid(local_domain,
                                      AGENT_CROSS_DOMAIN_ID_LEN) ||
        !allowlist_valid(allowlist)) return false;
    memset(policy, 0, sizeof(*policy));
    return parse_mode(mode, &policy->mode) &&
           copy_optional(policy->local_domain,
                         sizeof(policy->local_domain), local_domain) &&
           copy_optional(policy->allowlist,
                         sizeof(policy->allowlist), allowlist);
}

const char *agent_card_authorization_mode_name(
    enum agent_card_authorization_mode mode
)
{
    switch (mode) {
    case AGENT_CARD_AUTH_OFF: return "off";
    case AGENT_CARD_AUTH_SAME_DOMAIN: return "same-domain";
    case AGENT_CARD_AUTH_ALLOWLIST: return "allowlist";
    case AGENT_CARD_AUTH_DIRECTORY_TRUSTED: return "directory-trusted";
    case AGENT_CARD_AUTH_ALL_SIGNED: return "all-signed";
    default: return "unknown";
    }
}

void agent_card_manager_init(
    struct agent_card_manager *manager,
    size_t max_cards
)
{
    if (manager == NULL) return;
    memset(manager, 0, sizeof(*manager));
    manager->max_cards = max_cards;
}

void agent_card_manager_destroy(struct agent_card_manager *manager)
{
    struct agent_card_record *record;
    struct agent_card_record *next;

    if (manager == NULL) return;
    for (record = manager->head; record != NULL; record = next) {
        next = record->next;
        free(record);
    }
    memset(manager, 0, sizeof(*manager));
}

static struct agent_card_record *find_mutable(
    struct agent_card_manager *manager,
    const char *router_id
)
{
    struct agent_card_record *record;

    if (manager == NULL || router_id == NULL) return NULL;
    for (record = manager->head; record != NULL; record = record->next) {
        if (strcmp(record->document.router_id, router_id) == 0) return record;
    }
    return NULL;
}

const struct agent_card_record *agent_card_find(
    const struct agent_card_manager *manager,
    const char *router_id
)
{
    const struct agent_card_record *record;

    if (manager == NULL || router_id == NULL) return NULL;
    for (record = manager->head; record != NULL; record = record->next) {
        if (strcmp(record->document.router_id, router_id) == 0) return record;
    }
    return NULL;
}

static bool capability_valid(const struct agent_card_capability *capability)
{
    return capability != NULL &&
           text_valid(capability->skill_id,
                      sizeof(capability->skill_id)) &&
           text_valid(capability->intent, sizeof(capability->intent)) &&
           capability->version > 0U &&
           text_valid(capability->tenant, sizeof(capability->tenant));
}

static bool document_valid(const struct agent_card_document *document)
{
    size_t left;
    size_t right;

    if (document == NULL ||
        !text_valid(document->router_id, sizeof(document->router_id)) ||
        !text_valid(document->domain_id, sizeof(document->domain_id)) ||
        !text_valid(document->card_uri, sizeof(document->card_uri)) ||
        !text_valid(document->arpx_endpoint,
                    sizeof(document->arpx_endpoint)) ||
        !text_valid(document->issuer, sizeof(document->issuer)) ||
        !text_valid(document->key_id, sizeof(document->key_id)) ||
        !digest_valid(document->digest_sha256) || document->revision == 0U ||
        document->issued_at_ms >= document->expires_at_ms ||
        document->expires_at_ms - document->issued_at_ms >
            AGENT_CARD_MAX_LIFETIME_MS ||
        document->capability_count == 0U ||
        document->capability_count > AGENT_CARD_MAX_CAPABILITIES) return false;
    for (left = 0U; left < document->capability_count; left++) {
        if (!capability_valid(&document->capabilities[left])) return false;
        for (right = left + 1U; right < document->capability_count; right++) {
            if (strcmp(document->capabilities[left].skill_id,
                       document->capabilities[right].skill_id) == 0 ||
                (strcmp(document->capabilities[left].intent,
                        document->capabilities[right].intent) == 0 &&
                 document->capabilities[left].version ==
                     document->capabilities[right].version &&
                 strcmp(document->capabilities[left].tenant,
                        document->capabilities[right].tenant) == 0)) {
                return false;
            }
        }
    }
    return true;
}

static bool policy_allows(
    const struct agent_card_policy *policy,
    const struct agent_card_document *document
)
{
    switch (policy->mode) {
    case AGENT_CARD_AUTH_SAME_DOMAIN:
        return strcmp(policy->local_domain, document->domain_id) == 0;
    case AGENT_CARD_AUTH_ALLOWLIST:
        return allowlist_contains(policy->allowlist, document->router_id);
    case AGENT_CARD_AUTH_DIRECTORY_TRUSTED:
        return document->directory_trusted;
    case AGENT_CARD_AUTH_ALL_SIGNED:
        return true;
    case AGENT_CARD_AUTH_OFF:
    default:
        return false;
    }
}

static bool candidate_matches(
    const struct agent_cross_candidate *candidate,
    const struct agent_card_document *document
)
{
    char endpoint[AGENT_ARPX_URI_LEN];
    int written;

    if (candidate == NULL || document == NULL ||
        strcmp(candidate->router_id, document->router_id) != 0 ||
        strcmp(candidate->domain_id, document->domain_id) != 0 ||
        strcmp(candidate->agent_card_uri, document->card_uri) != 0) {
        return false;
    }
    written = snprintf(endpoint, sizeof(endpoint), "https://%s:%u/arpx/v1",
                       candidate->target, (unsigned int)candidate->port);
    return written >= 0 && (size_t)written < sizeof(endpoint) &&
           strcmp(endpoint, document->arpx_endpoint) == 0;
}

static enum agent_card_result reject(
    struct agent_card_manager *manager,
    enum agent_card_result result
)
{
    increment(&manager->rejected);
    if (result == AGENT_CARD_SIGNATURE_REQUIRED) {
        increment(&manager->signature_rejected);
    } else if (result == AGENT_CARD_IDENTITY_MISMATCH) {
        increment(&manager->identity_rejected);
    } else if (result == AGENT_CARD_POLICY_REJECTED) {
        increment(&manager->policy_rejected);
    } else if (result == AGENT_CARD_ROLLBACK) {
        increment(&manager->rollback_rejected);
    }
    return result;
}

enum agent_card_result agent_card_ingest(
    struct agent_card_manager *manager,
    const struct agent_card_policy *policy,
    const struct agent_cross_candidate *candidate,
    const struct agent_card_document *document,
    uint64_t candidate_generation,
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms
)
{
    struct agent_card_record *record;
    uint64_t authorized_until;

    if (manager == NULL || policy == NULL || candidate == NULL ||
        !document_valid(document) || candidate_generation == 0U) {
        return manager == NULL ? AGENT_CARD_INVALID :
               reject(manager, AGENT_CARD_INVALID);
    }
    if (!document->signature_verified) {
        return reject(manager, AGENT_CARD_SIGNATURE_REQUIRED);
    }
    if (!candidate_matches(candidate, document)) {
        return reject(manager, AGENT_CARD_IDENTITY_MISMATCH);
    }
    if (!policy_allows(policy, document)) {
        return reject(manager, AGENT_CARD_POLICY_REJECTED);
    }
    if (document->issued_at_ms > now_unix_ms ||
        document->expires_at_ms <= now_unix_ms ||
        candidate->expires_at_ms <= now_monotonic_ms ||
        (document->directory_trusted &&
         document->directory_authorized_until_ms <= now_monotonic_ms)) {
        return reject(manager, AGENT_CARD_EXPIRED);
    }
    record = find_mutable(manager, document->router_id);
    if (record != NULL && document->revision < record->document.revision) {
        return reject(manager, AGENT_CARD_ROLLBACK);
    }
    if (record != NULL && document->revision == record->document.revision &&
        strcmp(document->digest_sha256,
               record->document.digest_sha256) != 0) {
        return reject(manager, AGENT_CARD_CONFLICT);
    }
    if (record != NULL &&
        (strcmp(document->domain_id, record->document.domain_id) != 0 ||
         strcmp(document->issuer, record->document.issuer) != 0)) {
        return reject(manager, AGENT_CARD_CONFLICT);
    }
    if (record == NULL) {
        if (manager->count >= manager->max_cards) {
            return reject(manager, AGENT_CARD_FULL);
        }
        record = calloc(1U, sizeof(*record));
        if (record == NULL) return reject(manager, AGENT_CARD_NO_MEMORY);
        record->next = manager->head;
        manager->head = record;
        manager->count++;
    }
    authorized_until = saturating_add(
        now_monotonic_ms, document->expires_at_ms - now_unix_ms);
    if (candidate->expires_at_ms < authorized_until) {
        authorized_until = candidate->expires_at_ms;
    }
    if (document->directory_trusted &&
        document->directory_authorized_until_ms < authorized_until) {
        authorized_until = document->directory_authorized_until_ms;
    }
    record->document = *document;
    record->candidate_generation = candidate_generation;
    record->authorized_at_ms = now_monotonic_ms;
    record->authorized_until_ms = authorized_until;
    increment(&manager->accepted);
    increment(&manager->generation);
    return AGENT_CARD_OK;
}

bool agent_card_capability_authorized(
    struct agent_card_manager *manager,
    const char *router_id,
    const char *domain_id,
    const char *intent,
    uint32_t version,
    const char *tenant,
    uint64_t now_ms
)
{
    const struct agent_card_record *record;
    size_t index;

    if (manager == NULL || router_id == NULL || domain_id == NULL ||
        intent == NULL || tenant == NULL) return false;
    record = agent_card_find(manager, router_id);
    if (record == NULL || record->authorized_until_ms <= now_ms ||
        strcmp(record->document.domain_id, domain_id) != 0) {
        increment(&manager->route_updates_rejected);
        return false;
    }
    for (index = 0U; index < record->document.capability_count; index++) {
        const struct agent_card_capability *capability =
            &record->document.capabilities[index];
        if (strcmp(capability->intent, intent) == 0 &&
            capability->version == version &&
            (strcmp(capability->tenant, "*") == 0 ||
             strcmp(capability->tenant, tenant) == 0)) return true;
    }
    increment(&manager->route_updates_rejected);
    return false;
}

enum agent_card_result agent_card_revoke(
    struct agent_card_manager *manager,
    const char *router_id,
    agent_card_removing_cb removing,
    void *context
)
{
    struct agent_card_record **link;
    struct agent_card_record *removed;

    if (manager == NULL || router_id == NULL) return AGENT_CARD_INVALID;
    for (link = &manager->head; *link != NULL; link = &(*link)->next) {
        if (strcmp((*link)->document.router_id, router_id) != 0) continue;
        removed = *link;
        *link = removed->next;
        if (removing != NULL) removing(context, removed->document.router_id);
        free(removed);
        manager->count--;
        increment(&manager->revoked);
        increment(&manager->generation);
        return AGENT_CARD_OK;
    }
    return AGENT_CARD_NOT_FOUND;
}

size_t agent_card_prune(
    struct agent_card_manager *manager,
    uint64_t now_ms,
    agent_card_removing_cb removing,
    void *context
)
{
    struct agent_card_record **link;
    struct agent_card_record *removed;
    size_t count = 0U;

    if (manager == NULL) return 0U;
    link = &manager->head;
    while (*link != NULL) {
        if ((*link)->authorized_until_ms > now_ms) {
            link = &(*link)->next;
            continue;
        }
        removed = *link;
        *link = removed->next;
        if (removing != NULL) removing(context, removed->document.router_id);
        free(removed);
        manager->count--;
        count++;
        increment(&manager->expired);
        increment(&manager->generation);
    }
    return count;
}

const char *agent_card_result_name(enum agent_card_result result)
{
    switch (result) {
    case AGENT_CARD_OK: return "ok";
    case AGENT_CARD_INVALID: return "invalid card";
    case AGENT_CARD_SIGNATURE_REQUIRED: return "signature not verified";
    case AGENT_CARD_IDENTITY_MISMATCH: return "candidate identity mismatch";
    case AGENT_CARD_POLICY_REJECTED: return "authorization policy rejected";
    case AGENT_CARD_EXPIRED: return "card or discovery candidate expired";
    case AGENT_CARD_ROLLBACK: return "card revision rollback";
    case AGENT_CARD_CONFLICT: return "card revision or issuer conflict";
    case AGENT_CARD_FULL: return "card table full";
    case AGENT_CARD_NO_MEMORY: return "out of memory";
    case AGENT_CARD_NOT_FOUND: return "card not found";
    default: return "unknown card result";
    }
}
