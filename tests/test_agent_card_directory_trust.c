#ifdef NDEBUG
#undef NDEBUG
#endif

#include "agent_card_directory_trust.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);
    assert(written >= 0 && (size_t)written < capacity);
}

static struct agent_card_trust_document bundle(void)
{
    struct agent_card_trust_document value;
    struct agent_card_trust_entry *entry;

    memset(&value, 0, sizeof(value));
    set_text(value.directory_id, sizeof(value.directory_id),
             "directory://nexus.example");
    set_text(value.digest_sha256, sizeof(value.digest_sha256),
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    value.revision = 7U;
    value.issued_at_ms = 1000U;
    value.expires_at_ms = 61000U;
    value.signature_verified = true;
    value.entry_count = 2U;
    entry = &value.entries[0];
    set_text(entry->router_id, sizeof(entry->router_id), "router-b");
    set_text(entry->domain_id, sizeof(entry->domain_id), "remote.example");
    set_text(entry->issuer, sizeof(entry->issuer), "issuer-b");
    set_text(entry->key_id, sizeof(entry->key_id), "key-current");
    set_text(entry->public_key_uri, sizeof(entry->public_key_uri),
             "https://directory.example/keys/key-current.pub");
    set_text(entry->public_key_sha256, sizeof(entry->public_key_sha256),
             "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    entry->status = AGENT_CARD_TRUST_KEY_ACTIVE;
    entry->not_before_ms = 500U;
    entry->not_after_ms = 51000U;
    value.entries[1] = value.entries[0];
    set_text(value.entries[1].key_id, sizeof(value.entries[1].key_id),
             "key-retiring");
    set_text(value.entries[1].public_key_uri,
             sizeof(value.entries[1].public_key_uri),
             "https://directory.example/keys/key-retiring.pub");
    set_text(value.entries[1].public_key_sha256,
             sizeof(value.entries[1].public_key_sha256),
             "2123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    value.entries[1].status = AGENT_CARD_TRUST_KEY_RETIRING;
    return value;
}

static struct agent_card_document card(const char *key_id)
{
    struct agent_card_document value;
    memset(&value, 0, sizeof(value));
    set_text(value.router_id, sizeof(value.router_id), "router-b");
    set_text(value.domain_id, sizeof(value.domain_id), "remote.example");
    set_text(value.issuer, sizeof(value.issuer), "issuer-b");
    set_text(value.key_id, sizeof(value.key_id), key_id);
    return value;
}

static void test_rotation_and_expiry(void)
{
    struct agent_card_trust_manager manager;
    struct agent_card_trust_document document = bundle();
    struct agent_card_document current = card("key-current");
    struct agent_card_document retiring = card("key-retiring");
    uint64_t until = 0U;

    agent_card_trust_manager_init(&manager);
    assert(agent_card_trust_replace(&manager, &document, 100U, 1000U) ==
           AGENT_CARD_TRUST_OK);
    assert(agent_card_trust_authorize(&manager, &current, 100U, 1000U,
                                      &until));
    assert(until == 50100U);
    assert(agent_card_trust_authorize(&manager, &retiring, 100U, 1000U,
                                      NULL));
    assert(!agent_card_trust_prune(&manager, 60099U));
    assert(agent_card_trust_prune(&manager, 60100U));
    assert(!agent_card_trust_authorize(&manager, &current, 60100U, 61000U,
                                       NULL));
}

static void test_revoke_and_revision(void)
{
    struct agent_card_trust_manager manager;
    struct agent_card_trust_document document = bundle();
    struct agent_card_document current = card("key-current");

    agent_card_trust_manager_init(&manager);
    assert(agent_card_trust_replace(&manager, &document, 100U, 1000U) ==
           AGENT_CARD_TRUST_OK);
    document.revision = 8U;
    document.entries[0].status = AGENT_CARD_TRUST_KEY_REVOKED;
    set_text(document.digest_sha256, sizeof(document.digest_sha256),
             "3123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    assert(agent_card_trust_replace(&manager, &document, 200U, 2000U) ==
           AGENT_CARD_TRUST_OK);
    assert(!agent_card_trust_authorize(&manager, &current, 200U, 2000U,
                                       NULL));
    document.revision = 7U;
    assert(agent_card_trust_replace(&manager, &document, 300U, 3000U) ==
           AGENT_CARD_TRUST_ROLLBACK);
    assert(manager.rollback_rejected == 1U);
}

static void test_conflict_and_exact_identity_binding(void)
{
    struct agent_card_trust_manager manager;
    struct agent_card_trust_document document = bundle();
    struct agent_card_document authorized = card("key-current");
    struct agent_card_document wrong = card("key-current");

    agent_card_trust_manager_init(&manager);
    assert(agent_card_trust_replace(&manager, &document, 100U, 1000U) ==
           AGENT_CARD_TRUST_OK);
    set_text(document.digest_sha256, sizeof(document.digest_sha256),
             "4123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    assert(agent_card_trust_replace(&manager, &document, 200U, 2000U) ==
           AGENT_CARD_TRUST_CONFLICT);
    assert(agent_card_trust_authorize(&manager, &authorized, 200U, 2000U,
                                      NULL));
    set_text(wrong.domain_id, sizeof(wrong.domain_id), "other.example");
    assert(!agent_card_trust_authorize(&manager, &wrong, 200U, 2000U,
                                       NULL));
    wrong = card("key-current");
    set_text(wrong.issuer, sizeof(wrong.issuer), "issuer-other");
    assert(!agent_card_trust_authorize(&manager, &wrong, 200U, 2000U,
                                       NULL));
}

static void test_fail_closed_inputs(void)
{
    struct agent_card_trust_manager manager;
    struct agent_card_trust_document document = bundle();

    agent_card_trust_manager_init(&manager);
    document.signature_verified = false;
    assert(agent_card_trust_replace(&manager, &document, 100U, 1000U) ==
           AGENT_CARD_TRUST_SIGNATURE_REQUIRED);
    document = bundle();
    document.entries[1] = document.entries[0];
    assert(agent_card_trust_replace(&manager, &document, 100U, 1000U) ==
           AGENT_CARD_TRUST_INVALID);
    document = bundle();
    document.expires_at_ms = 1000U;
    assert(agent_card_trust_replace(&manager, &document, 100U, 1000U) ==
           AGENT_CARD_TRUST_INVALID);
}

int main(void)
{
    test_rotation_and_expiry();
    test_revoke_and_revision();
    test_conflict_and_exact_identity_binding();
    test_fail_closed_inputs();
    puts("agent card directory trust tests passed");
    return 0;
}
