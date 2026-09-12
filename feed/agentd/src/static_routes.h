#ifndef NEXUS_AGENT_STATIC_ROUTES_H
#define NEXUS_AGENT_STATIC_ROUTES_H

#include "route_table.h"

#include <stddef.h>

#define STATIC_ROUTE_ERROR_LEN 256

struct static_route_load_result {
    size_t loaded;
    char error[STATIC_ROUTE_ERROR_LEN];
};

enum route_table_result static_routes_reload(
    struct route_table *live_table,
    const char *uci_package_name,
    struct static_route_load_result *load_result
);

#endif

