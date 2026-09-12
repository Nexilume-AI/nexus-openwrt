#ifdef NDEBUG
#undef NDEBUG
#endif

#include "agent_card_authorization.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);

    assert(written >= 0 && (size_t)written < capacity);
}

static struct agent_cross_candidate candidate(void)
{
    struct agent_cross_candidate value;

    memset(&value, 0, sizeof(value));
    set_text(value.target, sizeof(value.target), "router-b.remote.example");
    set_text(value.router_id, sizeof(value.router_id), "router-b");
    set_text(value.domain_id, sizeof(value.domain_id), "remote.example");
    set_text(value.agent_card_uri, sizeof(value.agent_card_uri),
             "https://router-b.remote.example:7444/.well-known/agent-card.json");
    set_text(value.ipv4_hint, sizeof(value.ipv4_hint), "192.0.2.10");
    value.port = 7444U;
    value.expires_at_ms = 61000U;
    return value;
}

static struct agent_card_document card(void)
{
    struct agent_card_document value;

    memset(&value, 0, sizeof(value));
    set_text(value.router_id, sizeof(value.router_id), "router-b");
    set_text(value.domain_id, sizeof(value.domain_id), "remote.example");
    set_text(value.card_uri, sizeof(value.card_uri),
             "https://router-b.remote.example:7444/.well-known/agent-card.json");
    set_text(value.arpx_endpoint, sizeof(value.arpx_endpoint),
             "https://router-b.remote.example:7444/arpx/v1");
    set_text(value.issuer, sizeof(value.issuer), "directory://nexus-test");
    set_text(value.key_id, sizeof(value.key_id), "card-key-2026");
    set_text(value.digest_sha256, sizeof(value.digest_sha256),
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    value.revision = 1U;
    value.issued_at_ms = 500U;
    value.expires_at_ms = 51000U;
    value.signature_verified = true;
    value.directory_trusted = true;
    value.directory_authorized_until_ms = 51000U;
    value.capability_count = 2U;
    set_text(value.capabilities[0].skill_id,
             sizeof(value.capabilities[0].skill_id), "lint");
    set_text(value.capabilities[0].intent,
             sizeof(value.capabilities[0].intent), "chip.lint.v1");
    value.capabilities[0].version = 1U;
    set_text(value.capabilities[0].tenant,
             sizeof(value.capabilities[0].tenant), "eda");
    set_text(value.capabilities[1].skill_id,
             sizeof(value.capabilities[1].skill_id), "simulate");
    set_text(value.capabilities[1].intent,
             sizeof(value.capabilities[1].intent), "chip.simulate.v1");
    value.capabilities[1].version = 2U;
    set_text(value.capabilities[1].tenant,
             sizeof(value.capabilities[1].tenant), "*");
    return value;
}

static void test_policy_and_ingest(void)
{
    struct agent_card_manager manager;
    struct agent_card_policy policy;
    struct agent_cross_candidate discovery = candidate();
    struct agent_card_document document = card();
    const struct agent_card_record *record;

    assert(agent_card_policy_init(&policy, "directory-trusted",
                                  "local.example", ""));
    assert(!agent_card_policy_init(&policy, "unknown",
                                   "local.example", ""));
    assert(agent_card_policy_init(&policy, "directory-trusted",
                                  "local.example", ""));
    agent_card_manager_init(&manager, 2U);
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             7U, 1000U, 1000U) == AGENT_CARD_OK);
    record = agent_card_find(&manager, "router-b");
    assert(record != NULL && record->authorized_until_ms == 51000U);
    assert(record->candidate_generation == 7U);
    assert(agent_card_capability_authorized(
        &manager, "router-b", "remote.example", "chip.lint.v1", 1U,
        "eda", 2000U));
    assert(agent_card_capability_authorized(
        &manager, "router-b", "remote.example", "chip.simulate.v1", 2U,
        "tenant-b", 2000U));
    assert(!agent_card_capability_authorized(
        &manager, "router-b", "remote.example", "chip.secret.v1", 1U,
        "eda", 2000U));
    assert(manager.route_updates_rejected == 1U);
    agent_card_manager_destroy(&manager);
}

static void test_rejection_and_revision(void)
{
    struct agent_card_manager manager;
    struct agent_card_policy policy;
    struct agent_cross_candidate discovery = candidate();
    struct agent_card_document document = card();

    assert(agent_card_policy_init(&policy, "all-signed",
                                  "local.example", ""));
    agent_card_manager_init(&manager, 2U);
    document.signature_verified = false;
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             1U, 1000U, 1000U) == AGENT_CARD_SIGNATURE_REQUIRED);
    document = card();
    set_text(document.domain_id, sizeof(document.domain_id), "wrong.example");
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             1U, 1000U, 1000U) == AGENT_CARD_IDENTITY_MISMATCH);
    document = card();
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             1U, 1000U, 1000U) == AGENT_CARD_OK);
    document.revision = 2U;
    set_text(document.digest_sha256, sizeof(document.digest_sha256),
             "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             2U, 2000U, 2000U) == AGENT_CARD_OK);
    document.revision = 1U;
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             3U, 3000U, 3000U) == AGENT_CARD_ROLLBACK);
    assert(manager.signature_rejected == 1U);
    assert(manager.identity_rejected == 1U);
    assert(manager.rollback_rejected == 1U);
    agent_card_manager_destroy(&manager);
}

static void removed(void *context, const char *router_id)
{
    size_t *count = context;

    assert(strcmp(router_id, "router-b") == 0);
    (*count)++;
}

static void test_policy_expiry_and_revoke(void)
{
    struct agent_card_manager manager;
    struct agent_card_policy policy;
    struct agent_cross_candidate discovery = candidate();
    struct agent_card_document document = card();
    size_t removed_count = 0U;

    assert(agent_card_policy_init(&policy, "same-domain",
                                  "local.example", ""));
    agent_card_manager_init(&manager, 2U);
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             1U, 1000U, 1000U) == AGENT_CARD_POLICY_REJECTED);
    assert(agent_card_policy_init(&policy, "directory-trusted",
                                  "local.example", ""));
    document.directory_authorized_until_ms = 1000U;
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             1U, 1000U, 1000U) == AGENT_CARD_EXPIRED);
    document.directory_authorized_until_ms = 51000U;
    assert(agent_card_policy_init(&policy, "allowlist", "local.example",
                                  "router-a,router-b"));
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             1U, 1000U, 1000U) == AGENT_CARD_OK);
    assert(agent_card_prune(&manager, 50999U, removed, &removed_count) == 0U);
    assert(agent_card_prune(&manager, 51000U, removed, &removed_count) == 1U);
    assert(removed_count == 1U && manager.expired == 1U);
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             2U, 1000U, 1000U) == AGENT_CARD_OK);
    assert(agent_card_revoke(&manager, "router-b", removed,
                             &removed_count) == AGENT_CARD_OK);
    assert(removed_count == 2U && manager.revoked == 1U);
    agent_card_manager_destroy(&manager);
}

static void test_wall_clock_is_converted_to_monotonic_expiry(void)
{
    struct agent_card_manager manager;
    struct agent_card_policy policy;
    struct agent_cross_candidate discovery = candidate();
    struct agent_card_document document = card();
    const struct agent_card_record *record;
    const uint64_t now_unix_ms = 1700000000000ULL;

    discovery.expires_at_ms = 100000U;
    document.issued_at_ms = now_unix_ms - 1000U;
    document.expires_at_ms = now_unix_ms + 5000U;
    assert(agent_card_policy_init(&policy, "all-signed",
                                  "local.example", ""));
    agent_card_manager_init(&manager, 1U);
    assert(agent_card_ingest(&manager, &policy, &discovery, &document,
                             3U, 2000U, now_unix_ms) == AGENT_CARD_OK);
    record = agent_card_find(&manager, "router-b");
    assert(record != NULL && record->authorized_until_ms == 7000U);
    assert(agent_card_prune(&manager, 6999U, NULL, NULL) == 0U);
    assert(agent_card_prune(&manager, 7000U, NULL, NULL) == 1U);
    agent_card_manager_destroy(&manager);
}

int main(void)
{
    test_policy_and_ingest();
    test_rejection_and_revision();
    test_policy_expiry_and_revoke();
    test_wall_clock_is_converted_to_monotonic_expiry();
    puts("agent card authorization tests passed");
    return 0;
}
