#include "agent_discovery.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct agent_discovery_observation valid_observation(
    const char *router_id,
    const char *ipv4
)
{
    struct agent_discovery_observation observation;

    agent_discovery_observation_init(&observation);
    snprintf(observation.instance, sizeof(observation.instance), "%s",
             router_id);
    snprintf(observation.hostname, sizeof(observation.hostname), "%s.local",
             router_id);
    snprintf(observation.ipv4, sizeof(observation.ipv4), "%s", ipv4);
    snprintf(observation.iface, sizeof(observation.iface), "%s", "br-lan");
    snprintf(observation.last_update, sizeof(observation.last_update), "%s",
             "2026-08-02T00:00:00Z");
    observation.port = 7444U;
    observation.ttl_seconds = 120U;
    assert(agent_discovery_observation_add_txt(&observation, "ver=1"));
    assert(agent_discovery_observation_add_txt(&observation,
                                               "domain=lab.example"));
    assert(agent_discovery_observation_add_txt(&observation,
                                               "path=/arpx/v1"));
    {
        char txt[96];
        snprintf(txt, sizeof(txt), "router=%s", router_id);
        assert(agent_discovery_observation_add_txt(&observation, txt));
    }
    return observation;
}

static void test_strict_txt_contract(void)
{
    struct agent_discovery_observation observation =
        valid_observation("router-b", "192.168.10.2");

    assert(agent_discovery_observation_valid(&observation));
    assert(!agent_discovery_observation_add_txt(&observation,
                                                "cap=secret.tool"));
    assert(!agent_discovery_observation_add_txt(&observation, "ver=1"));

    observation = valid_observation("router-b", "192.168.10.2");
    observation.txt_fields = 0U;
    assert(!agent_discovery_observation_valid(&observation));
}

static void test_bounded_table_self_and_expiry(void)
{
    struct agent_discovery_table table;
    struct agent_discovery_observation first =
        valid_observation("router-b", "192.168.10.2");
    struct agent_discovery_observation second =
        valid_observation("router-c", "192.168.10.3");
    struct agent_discovery_observation third =
        valid_observation("router-d", "192.168.10.4");
    struct agent_discovery_observation self =
        valid_observation("router-a", "192.168.10.1");

    agent_discovery_table_init(&table, 2U);
    assert(agent_discovery_table_observe(&table, &self, "router-a", 1000U) ==
           AGENT_DISCOVERY_OK);
    assert(table.count == 0U);
    assert(table.self_suppressed == 1U);
    assert(agent_discovery_table_observe(&table, &first, "router-a", 1000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_table_observe(&table, &second, "router-a", 1000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_table_observe(&table, &third, "router-a", 1000U) ==
           AGENT_DISCOVERY_FULL);
    assert(table.count == 2U);

    first.ttl_seconds = 5U;
    snprintf(first.last_update, sizeof(first.last_update), "%s",
             "2026-08-02T00:00:01Z");
    assert(agent_discovery_table_observe(&table, &first, "router-a", 2000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_table_find(&table, "router-b") != NULL);
    /* Re-reading the same umdns cache record must not renew its lease. */
    assert(agent_discovery_table_observe(&table, &first, "router-a", 6000U) ==
           AGENT_DISCOVERY_OK);
    assert(agent_discovery_table_prune(&table, 7000U) == 1U);
    assert(agent_discovery_table_find(&table, "router-b") == NULL);
    assert(table.count == 1U);
    assert(table.expired == 1U);
    agent_discovery_table_destroy(&table);
}

static void test_invalid_observations_rejected(void)
{
    struct agent_discovery_table table;
    struct agent_discovery_observation observation =
        valid_observation("router-b", "192.168.10.2");

    agent_discovery_table_init(&table, 4U);
    snprintf(observation.instance, sizeof(observation.instance), "%s",
             "different-router");
    assert(agent_discovery_table_observe(&table, &observation, "router-a", 0U) ==
           AGENT_DISCOVERY_INVALID);
    assert(table.rejected == 1U);
    assert(table.count == 0U);
    agent_discovery_table_destroy(&table);
}

int main(void)
{
    test_strict_txt_contract();
    test_bounded_table_self_and_expiry();
    test_invalid_observations_rejected();
    puts("agent discovery tests passed");
    return 0;
}
