#ifndef NEXUS_AGENT_STATIC_PEERS_H
#define NEXUS_AGENT_STATIC_PEERS_H

#include "peer_table.h"

#include <stddef.h>

#define STATIC_PEER_ERROR_LEN 256

struct static_peer_load_result {
    size_t loaded;
    char error[STATIC_PEER_ERROR_LEN];
};

enum peer_table_result static_peers_reload(
    struct peer_table *live_table,
    const char *uci_package_name,
    struct static_peer_load_result *load_result
);

#endif
