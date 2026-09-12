#ifndef NEXUS_ADAPTER_REGISTRY_H
#define NEXUS_ADAPTER_REGISTRY_H

#include "agent_adapter_contract.h"

#include <stdbool.h>
#include <stddef.h>

struct adapter_registry {
    struct agent_adapter_mapping mappings[AGENT_ADAPTER_MAX_MAPPINGS];
    bool local_dynamic[AGENT_ADAPTER_MAX_MAPPINGS];
    size_t count;
};

struct adapter_registry_load_result {
    char error[256];
};

struct adapter_registry_merge_result {
    size_t dynamic_added;
    size_t identical_merged;
    size_t static_conflicts;
    size_t invalid_dynamic;
    size_t capacity_rejected;
};

bool adapter_registry_load(
    const char *uci_package,
    const char *uci_config_dir,
    struct adapter_registry *registry,
    struct adapter_registry_load_result *result
);

bool adapter_registry_merge(
    const struct adapter_registry *static_registry,
    const struct agent_adapter_mapping *dynamic_mappings,
    size_t dynamic_count,
    struct adapter_registry *effective,
    struct adapter_registry_merge_result *result
);

#endif
