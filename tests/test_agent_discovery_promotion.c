#include "agent_discovery_promotion.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct agent_discovery_observation lan_observation(
    const char *last_update,
    uint32_t ttl_seconds
)
{
    struct agent_discovery_observation observation;

    agent_discovery_observation_init(&observation);
    snprintf(observation.instance, sizeof(observation.instance), "%s",
             "router-b");
    snprintf(observation.hostname, sizeof(observation.hostname), "%s",
             "router-b.local");
    snprintf(observation.ipv4, sizeof(observation.ipv4), "%s",
             "192.168.10.2");
    snprintf(observation.iface, sizeof(observation.iface), "%s", "br-lan");
    snprintf(observation.last_update, sizeof(observation.last_update), "%s",
             last_update);
    observation.port = 7444U;
    observation.ttl_seconds = ttl_seconds;
    assert(agent_discovery_observation_add_txt(&observation, "ver=1"));
    assert(agent_discovery_observation_add_txt(
        &observation, "domain=site-b.example"));
    assert(agent_discovery_observation_add_txt(
        &observation, "path=/arpx/v1"));
    assert(agent_discovery_observation_add_txt(
        &observation, "router=router-b"));
    return observation;
}

static void removal_counter(void *context, const char *peer_id)
{
    size_t *count = context;

    assert(strcmp(peer_id, "router-b") == 0);
    (*count)++;
}

static void test_lan_promotion_renewal_and_expiry(void)
{
    struct agent_discovery_promotion_manager manager;
    struct agent_discovery_table candidates;
    struct agent_cross_table cross;
    struct peer_table peers;
    struct agent_discovery_observation observation =
        lan_observation("2026-08-02T00:00:00Z", 10U);
    const struct agent_peer *peer;
    size_t removed = 0U;

    peer_table_init(&peers, 8U);
    agent_discovery_table_init(&candidates, 8U);
    agent_cross_table_init(&cross, 8U);
    agent_discovery_promotion_init(&manager, 4U);
    assert(agent_discovery_table_observe(
               &candidates, &observation, "router-a", 1000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b",
               candidates.generation + 1U, 30U, 1000U) ==
           AGENT_PROMOTION_STALE_GENERATION);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b",
               candidates.generation, 30U, 1000U) == AGENT_PROMOTION_OK);
    assert(manager.count == 1U);
    peer = peer_table_find(&peers, "router-b");
    assert(peer != NULL);
    assert(strcmp(peer->endpoint,
                  "https://router-b.site-b.example:7444/arpx/v1") == 0);
    assert(strcmp(peer->connect_ipv4, "192.168.10.2") == 0);

    assert(peer_table_set_state(&peers, "router-b",
                                AGENT_PEER_STATE_CONNECTING) ==
           PEER_TABLE_OK);
    assert(peer_table_set_state(&peers, "router-b",
                                AGENT_PEER_STATE_ESTABLISHED) ==
           PEER_TABLE_OK);
    observation = lan_observation("2026-08-02T00:00:05Z", 20U);
    assert(agent_discovery_table_observe(
               &candidates, &observation, "router-a", 5000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b",
               candidates.generation, 30U, 5000U) == AGENT_PROMOTION_OK);
    assert(peer_table_find(&peers, "router-b")->state ==
           AGENT_PEER_STATE_ESTABLISHED);
    assert(manager.renewed == 1U);

    assert(agent_discovery_promotion_reconcile(
               &manager, &peers, &candidates, &cross, 24999U,
               removal_counter, &removed) == 0U);
    assert(agent_discovery_promotion_reconcile(
               &manager, &peers, &candidates, &cross, 25000U,
               removal_counter, &removed) == 1U);
    assert(removed == 1U);
    assert(manager.count == 0U);
    assert(peer_table_find(&peers, "router-b") == NULL);

    agent_discovery_promotion_destroy(&manager);
    agent_cross_table_destroy(&cross);
    agent_discovery_table_destroy(&candidates);
    peer_table_destroy(&peers);
}

static void test_svcb_requires_address_and_can_be_revoked(void)
{
    struct agent_discovery_promotion_manager manager;
    struct agent_discovery_table lan;
    struct agent_cross_table candidates;
    struct agent_cross_candidate candidate;
    struct peer_table peers;
    const struct agent_discovery_promotion *promotion;

    memset(&candidate, 0, sizeof(candidate));
    snprintf(candidate.router_id, sizeof(candidate.router_id), "%s",
             "router-c");
    snprintf(candidate.domain_id, sizeof(candidate.domain_id), "%s",
             "remote.example");
    snprintf(candidate.target, sizeof(candidate.target), "%s",
             "router-c.remote.example");
    candidate.port = 7444U;
    candidate.expires_at_ms = 60000U;
    peer_table_init(&peers, 8U);
    agent_discovery_table_init(&lan, 8U);
    agent_cross_table_init(&candidates, 8U);
    agent_discovery_promotion_init(&manager, 4U);
    candidates.head = &candidate;
    candidates.count = 1U;
    candidates.generation = 1U;

    assert(agent_discovery_promote_svcb(
               &manager, &peers, &candidates, "router-c", "router-c",
               1U, 30U, 1000U) == AGENT_PROMOTION_ADDRESS_REQUIRED);
    snprintf(candidate.ipv4_hint, sizeof(candidate.ipv4_hint), "%s",
             "192.0.2.30");
    assert(agent_discovery_promote_svcb(
               &manager, &peers, &candidates, "router-c", "router-c",
               1U, 30U, 1000U) == AGENT_PROMOTION_OK);
    promotion = agent_discovery_promotion_find(&manager, "router-c");
    assert(promotion != NULL);
    assert(promotion->source == AGENT_PROMOTION_SVCB);
    assert(strcmp(promotion->peer.endpoint,
                  "https://router-c.remote.example:7444/arpx/v1") == 0);
    assert(agent_discovery_promotion_remove(
               &manager, &peers, "router-c") == AGENT_PROMOTION_OK);
    assert(peer_table_find(&peers, "router-c") == NULL);
    assert(manager.revoked == 1U);

    assert(agent_discovery_promote_svcb_auto(
               &manager, &peers, &candidates, "router-c", 1U, 30U,
               2000U) == AGENT_PROMOTION_OK);
    promotion = agent_discovery_promotion_find(&manager, "router-c");
    assert(promotion != NULL && promotion->automatic);
    assert(strcmp(promotion->peer.peer_id, "router-c") == 0);

    candidates.head = NULL;
    candidates.count = 0U;
    agent_discovery_promotion_destroy(&manager);
    agent_cross_table_destroy(&candidates);
    agent_discovery_table_destroy(&lan);
    peer_table_destroy(&peers);
}

static void test_static_conflict_and_reapply(void)
{
    struct agent_discovery_promotion_manager manager;
    struct agent_discovery_table candidates;
    struct agent_discovery_observation observation =
        lan_observation("2026-08-02T00:00:00Z", 30U);
    struct peer_table peers;
    struct peer_table reloaded;
    struct agent_peer static_peer;

    peer_table_init(&peers, 8U);
    peer_table_init(&reloaded, 8U);
    agent_discovery_table_init(&candidates, 8U);
    agent_discovery_promotion_init(&manager, 4U);
    assert(agent_discovery_table_observe(
               &candidates, &observation, "router-a", 1000U) ==
           AGENT_DISCOVERY_OK);
    memset(&static_peer, 0, sizeof(static_peer));
    snprintf(static_peer.peer_id, sizeof(static_peer.peer_id), "%s",
             "router-b");
    snprintf(static_peer.router_id, sizeof(static_peer.router_id), "%s",
             "router-z");
    snprintf(static_peer.domain_id, sizeof(static_peer.domain_id), "%s",
             "site-z.example");
    snprintf(static_peer.endpoint, sizeof(static_peer.endpoint), "%s",
             "https://router-z.example:7444/arpx/v1");
    snprintf(static_peer.connect_ipv4, sizeof(static_peer.connect_ipv4), "%s",
             "192.0.2.40");
    static_peer.role = AGENT_PEER_ROLE_DIRECT;
    static_peer.state = AGENT_PEER_STATE_CONFIGURED;
    static_peer.graceful_restart_seconds = 30U;
    assert(peer_table_upsert(&peers, &static_peer) == PEER_TABLE_OK);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b",
               candidates.generation, 30U, 1000U) ==
           AGENT_PROMOTION_CONFLICT);
    assert(peer_table_remove(&peers, "router-b") == PEER_TABLE_OK);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b",
               candidates.generation, 30U, 1000U) == AGENT_PROMOTION_OK);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b-alias",
               candidates.generation, 30U, 1000U) ==
           AGENT_PROMOTION_CONFLICT);
    assert(manager.count == 1U);
    assert(agent_discovery_promotion_reapply(&manager, &reloaded) ==
           AGENT_PROMOTION_OK);
    assert(peer_table_find(&reloaded, "router-b") != NULL);

    agent_discovery_promotion_destroy(&manager);
    agent_discovery_table_destroy(&candidates);
    peer_table_destroy(&reloaded);
    peer_table_destroy(&peers);
}

static void test_auto_promotion_and_explicit_takeover(void)
{
    struct agent_discovery_promotion_manager manager;
    struct agent_discovery_table candidates;
    struct agent_discovery_observation observation =
        lan_observation("2026-08-02T00:00:00Z", 30U);
    struct peer_table peers;
    const struct agent_discovery_promotion *promotion;

    peer_table_init(&peers, 8U);
    agent_discovery_table_init(&candidates, 8U);
    agent_discovery_promotion_init(&manager, 4U);
    assert(agent_discovery_table_observe(
               &candidates, &observation, "router-a", 1000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_promote_lan_auto(
               &manager, &peers, &candidates, "router-b",
               candidates.generation, 30U, 1000U) == AGENT_PROMOTION_OK);
    promotion = agent_discovery_promotion_find(&manager, "router-b");
    assert(promotion != NULL && promotion->automatic);
    assert(agent_discovery_promote_lan(
               &manager, &peers, &candidates, "router-b", "router-b",
               candidates.generation, 30U, 1000U) == AGENT_PROMOTION_OK);
    promotion = agent_discovery_promotion_find(&manager, "router-b");
    assert(promotion != NULL && !promotion->automatic);

    agent_discovery_promotion_destroy(&manager);
    agent_discovery_table_destroy(&candidates);
    peer_table_destroy(&peers);
}

int main(void)
{
    test_lan_promotion_renewal_and_expiry();
    test_svcb_requires_address_and_can_be_revoked();
    test_static_conflict_and_reapply();
    test_auto_promotion_and_explicit_takeover();
    puts("agent discovery promotion tests passed");
    return 0;
}
