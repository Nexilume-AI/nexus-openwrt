#include "agent_card_directory_trust.h"

#include <string.h>

static void increment(uint64_t *value)
{
    if (*value != UINT64_MAX) (*value)++;
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
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
        const char value = digest[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

static bool https_uri_valid(const char *uri)
{
    return text_valid(uri, AGENT_CARD_KEY_URI_LEN) &&
           strncmp(uri, "https://", 8U) == 0;
}

bool agent_card_trust_key_status_parse(
    const char *text,
    enum agent_card_trust_key_status *status
)
{
    if (text == NULL || status == NULL) return false;
    if (strcmp(text, "active") == 0) {
        *status = AGENT_CARD_TRUST_KEY_ACTIVE;
    } else if (strcmp(text, "retiring") == 0) {
        *status = AGENT_CARD_TRUST_KEY_RETIRING;
    } else if (strcmp(text, "revoked") == 0) {
        *status = AGENT_CARD_TRUST_KEY_REVOKED;
    } else return false;
    return true;
}

const char *agent_card_trust_key_status_name(
    enum agent_card_trust_key_status status
)
{
    switch (status) {
    case AGENT_CARD_TRUST_KEY_ACTIVE: return "active";
    case AGENT_CARD_TRUST_KEY_RETIRING: return "retiring";
    case AGENT_CARD_TRUST_KEY_REVOKED: return "revoked";
    default: return "unknown";
    }
}

static bool entry_valid(const struct agent_card_trust_entry *entry)
{
    return entry != NULL &&
           text_valid(entry->router_id, sizeof(entry->router_id)) &&
           text_valid(entry->domain_id, sizeof(entry->domain_id)) &&
           text_valid(entry->issuer, sizeof(entry->issuer)) &&
           text_valid(entry->key_id, sizeof(entry->key_id)) &&
           https_uri_valid(entry->public_key_uri) &&
           digest_valid(entry->public_key_sha256) &&
           (entry->status == AGENT_CARD_TRUST_KEY_ACTIVE ||
            entry->status == AGENT_CARD_TRUST_KEY_RETIRING ||
            entry->status == AGENT_CARD_TRUST_KEY_REVOKED) &&
           entry->not_before_ms < entry->not_after_ms;
}

static bool document_valid(const struct agent_card_trust_document *document)
{
    size_t left;
    size_t right;

    if (document == NULL ||
        !text_valid(document->directory_id, sizeof(document->directory_id)) ||
        !digest_valid(document->digest_sha256) || document->revision == 0U ||
        document->issued_at_ms >= document->expires_at_ms ||
        document->expires_at_ms - document->issued_at_ms >
            AGENT_CARD_TRUST_MAX_LIFETIME_MS ||
        document->entry_count == 0U ||
        document->entry_count > AGENT_CARD_TRUST_MAX_ENTRIES) return false;
    for (left = 0U; left < document->entry_count; left++) {
        if (!entry_valid(&document->entries[left])) return false;
        for (right = left + 1U; right < document->entry_count; right++) {
            const struct agent_card_trust_entry *a = &document->entries[left];
            const struct agent_card_trust_entry *b = &document->entries[right];
            if (strcmp(a->router_id, b->router_id) == 0 &&
                strcmp(a->domain_id, b->domain_id) == 0 &&
                strcmp(a->issuer, b->issuer) == 0 &&
                strcmp(a->key_id, b->key_id) == 0) return false;
        }
    }
    return true;
}

void agent_card_trust_manager_init(struct agent_card_trust_manager *manager)
{
    if (manager != NULL) memset(manager, 0, sizeof(*manager));
}

enum agent_card_trust_result agent_card_trust_replace(
    struct agent_card_trust_manager *manager,
    const struct agent_card_trust_document *document,
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms
)
{
    uint64_t lifetime_ms;

    if (manager == NULL || !document_valid(document)) {
        if (manager != NULL) increment(&manager->rejected);
        return AGENT_CARD_TRUST_INVALID;
    }
    if (!document->signature_verified) {
        increment(&manager->rejected);
        return AGENT_CARD_TRUST_SIGNATURE_REQUIRED;
    }
    if (document->issued_at_ms > now_unix_ms ||
        document->expires_at_ms <= now_unix_ms) {
        increment(&manager->rejected);
        increment(&manager->expired);
        return AGENT_CARD_TRUST_EXPIRED;
    }
    if (manager->loaded && document->revision < manager->document.revision) {
        increment(&manager->rejected);
        increment(&manager->rollback_rejected);
        return AGENT_CARD_TRUST_ROLLBACK;
    }
    if (manager->loaded && document->revision == manager->document.revision) {
        if (strcmp(document->directory_id,
                   manager->document.directory_id) != 0 ||
            strcmp(document->digest_sha256,
                   manager->document.digest_sha256) != 0) {
            increment(&manager->rejected);
            return AGENT_CARD_TRUST_CONFLICT;
        }
        return AGENT_CARD_TRUST_OK;
    }
    if (manager->loaded && strcmp(document->directory_id,
                                  manager->document.directory_id) != 0) {
        increment(&manager->rejected);
        return AGENT_CARD_TRUST_CONFLICT;
    }
    lifetime_ms = document->expires_at_ms - now_unix_ms;
    manager->document = *document;
    manager->loaded = true;
    manager->authorized_until_ms = saturating_add(now_monotonic_ms,
                                                  lifetime_ms);
    increment(&manager->generation);
    increment(&manager->accepted);
    return AGENT_CARD_TRUST_OK;
}

bool agent_card_trust_authorize(
    const struct agent_card_trust_manager *manager,
    const struct agent_card_document *card,
    uint64_t now_monotonic_ms,
    uint64_t now_unix_ms,
    uint64_t *authorized_until_ms
)
{
    size_t index;

    if (authorized_until_ms != NULL) *authorized_until_ms = 0U;
    if (manager == NULL || card == NULL || !manager->loaded ||
        manager->authorized_until_ms <= now_monotonic_ms) return false;
    for (index = 0U; index < manager->document.entry_count; index++) {
        const struct agent_card_trust_entry *entry =
            &manager->document.entries[index];
        uint64_t key_remaining;
        uint64_t key_until;

        if (entry->status == AGENT_CARD_TRUST_KEY_REVOKED ||
            entry->not_before_ms > now_unix_ms ||
            entry->not_after_ms <= now_unix_ms ||
            strcmp(entry->router_id, card->router_id) != 0 ||
            strcmp(entry->domain_id, card->domain_id) != 0 ||
            strcmp(entry->issuer, card->issuer) != 0 ||
            strcmp(entry->key_id, card->key_id) != 0) continue;
        key_remaining = entry->not_after_ms - now_unix_ms;
        key_until = saturating_add(now_monotonic_ms, key_remaining);
        if (authorized_until_ms != NULL) {
            *authorized_until_ms = key_until < manager->authorized_until_ms
                ? key_until : manager->authorized_until_ms;
        }
        return true;
    }
    return false;
}

bool agent_card_trust_prune(
    struct agent_card_trust_manager *manager,
    uint64_t now_monotonic_ms
)
{
    if (manager == NULL || !manager->loaded ||
        manager->authorized_until_ms > now_monotonic_ms) return false;
    manager->loaded = false;
    manager->authorized_until_ms = 0U;
    increment(&manager->generation);
    increment(&manager->expired);
    return true;
}

const char *agent_card_trust_result_name(enum agent_card_trust_result result)
{
    switch (result) {
    case AGENT_CARD_TRUST_OK: return "ok";
    case AGENT_CARD_TRUST_INVALID: return "invalid trust document";
    case AGENT_CARD_TRUST_SIGNATURE_REQUIRED: return "signature not verified";
    case AGENT_CARD_TRUST_EXPIRED: return "trust document expired";
    case AGENT_CARD_TRUST_ROLLBACK: return "trust revision rollback";
    case AGENT_CARD_TRUST_CONFLICT: return "trust revision conflict";
    default: return "unknown trust result";
    }
}
