#include "static_peers.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned int test_uci_accesses;

static void gate(const char *value)
{
#ifdef _WIN32
    assert(_putenv_s("NEXUS_ROUTER_NETWORK_ENABLED", value) == 0);
#else
    assert(setenv("NEXUS_ROUTER_NETWORK_ENABLED", value, 1) == 0);
#endif
}

int main(void)
{
    struct peer_table peers;
    struct static_peer_load_result result;
    struct agent_peer peer = {0};
    uint64_t generation;
    snprintf(peer.peer_id, sizeof(peer.peer_id), "static-peer");
    snprintf(peer.router_id, sizeof(peer.router_id), "router-b");
    snprintf(peer.domain_id, sizeof(peer.domain_id), "mesh.local");
    snprintf(peer.endpoint, sizeof(peer.endpoint), "https://router-b.mesh.local:7444/arpx/v1");
    peer.graceful_restart_seconds = 30;
    peer_table_init(&peers, 8);
    assert(peer_table_upsert(&peers, &peer) == PEER_TABLE_OK);
    generation = peers.generation;
    gate("0");
    assert(static_peers_reload(&peers, "agent_peers", &result) == PEER_TABLE_OK);
    assert(peers.count == 0 && peers.generation == generation + 1);
    assert(result.loaded == 0 && result.error[0] == '\0');
    assert(test_uci_accesses == 0); /* even missing/corrupt saved peers stay inert */
    peers.generation = UINT64_MAX;
    assert(static_peers_reload(&peers, "agent_peers", &result) == PEER_TABLE_OK);
    assert(peers.generation == UINT64_MAX);
    assert(peer_table_upsert(&peers, &peer) == PEER_TABLE_OK);
    gate("1");
    assert(static_peers_reload(&peers, "agent_peers", &result) == PEER_TABLE_INVALID);
    assert(test_uci_accesses == 1 && peers.count == 1); /* failed reload is atomic */
    gate("");
    assert(static_peers_reload(&peers, "agent_peers", &result) == PEER_TABLE_INVALID);
    assert(test_uci_accesses == 2); /* legacy unset default still reads UCI */
    peer_table_destroy(&peers);
    puts("static peer pause passed");
    return 0;
}
