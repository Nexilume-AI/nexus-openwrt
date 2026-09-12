#include "static_peers.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uci.h>

static void set_error(
    struct static_peer_load_result *result,
    const char *section,
    const char *message
)
{
    if (result == NULL) {
        return;
    }
    if (section == NULL) {
        snprintf(result->error, sizeof(result->error), "%s", message);
    } else {
        snprintf(result->error, sizeof(result->error),
                 "peer section '%s': %s", section, message);
    }
}

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static const char *option(
    struct uci_context *context,
    struct uci_section *section,
    const char *name
)
{
    return uci_lookup_option_string(context, section, name);
}

static bool parse_bool(const char *text, bool default_value, bool *value)
{
    if (value == NULL) {
        return false;
    }
    if (text == NULL) {
        *value = default_value;
        return true;
    }
    if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
        strcasecmp(text, "yes") == 0 || strcasecmp(text, "on") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
        strcasecmp(text, "no") == 0 || strcasecmp(text, "off") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_peer(
    struct uci_context *context,
    struct uci_section *section,
    struct agent_peer *peer,
    struct static_peer_load_result *result
)
{
    const char *role;
    const char *graceful;
    const char *connect_ipv4;

    memset(peer, 0, sizeof(*peer));
    if (!copy_text(peer->peer_id, sizeof(peer->peer_id),
                   option(context, section, "peer_id")) ||
        !copy_text(peer->router_id, sizeof(peer->router_id),
                   option(context, section, "router_id")) ||
        !copy_text(peer->domain_id, sizeof(peer->domain_id),
                   option(context, section, "domain_id")) ||
        !copy_text(peer->endpoint, sizeof(peer->endpoint),
                   option(context, section, "endpoint"))) {
        set_error(result, section->e.name,
                  "peer_id, router_id, domain_id and endpoint are required");
        return false;
    }

    connect_ipv4 = option(context, section, "connect_ipv4");
    if (connect_ipv4 != NULL &&
        !copy_text(peer->connect_ipv4, sizeof(peer->connect_ipv4),
                   connect_ipv4)) {
        set_error(result, section->e.name,
                  "connect_ipv4 exceeds its bounded field");
        return false;
    }

    role = option(context, section, "role");
    if (role == NULL || strcmp(role, "direct") == 0) {
        peer->role = AGENT_PEER_ROLE_DIRECT;
    } else if (strcmp(role, "reflector") == 0) {
        peer->role = AGENT_PEER_ROLE_REFLECTOR;
    } else {
        set_error(result, section->e.name,
                  "role must be direct or reflector");
        return false;
    }

    graceful = option(context, section, "graceful_restart_seconds");
    if (graceful == NULL) {
        peer->graceful_restart_seconds = 30U;
    } else if (!parse_u32(graceful, &peer->graceful_restart_seconds)) {
        set_error(result, section->e.name,
                  "graceful_restart_seconds must be an unsigned integer");
        return false;
    }
    peer->state = AGENT_PEER_STATE_CONFIGURED;

    if (!agent_peer_valid(peer)) {
        set_error(result, section->e.name,
                  "invalid identifier, domain, ARPX endpoint, connect_ipv4 or graceful timeout");
        return false;
    }
    return true;
}

enum peer_table_result static_peers_reload(
    struct peer_table *live_table,
    const char *uci_package_name,
    struct static_peer_load_result *load_result
)
{
    struct peer_table candidate;
    struct uci_context *context;
    struct uci_package *package = NULL;
    struct uci_element *element;
    struct uci_section *section;
    struct agent_peer peer;
    enum peer_table_result table_result = PEER_TABLE_OK;
    bool enabled;

    if (live_table == NULL || uci_package_name == NULL ||
        load_result == NULL || live_table->max_peers == 0U) {
        return PEER_TABLE_INVALID;
    }
    memset(load_result, 0, sizeof(*load_result));
    peer_table_init(&candidate, live_table->max_peers);

    context = uci_alloc_context();
    if (context == NULL) {
        set_error(load_result, NULL, "failed to allocate UCI context");
        return PEER_TABLE_NO_MEMORY;
    }
    if (uci_load(context, uci_package_name, &package) != UCI_OK) {
        uci_free_context(context);
        set_error(load_result, NULL, "failed to load UCI package");
        return PEER_TABLE_INVALID;
    }

    uci_foreach_element(&package->sections, element) {
        section = uci_to_section(element);
        if (strcmp(section->type, "peer") != 0) {
            continue;
        }
        if (!parse_bool(option(context, section, "enabled"), true, &enabled)) {
            set_error(load_result, section->e.name,
                      "enabled must be a boolean");
            table_result = PEER_TABLE_INVALID;
            goto fail;
        }
        if (!enabled) {
            continue;
        }
        if (!parse_peer(context, section, &peer, load_result)) {
            table_result = PEER_TABLE_INVALID;
            goto fail;
        }
        table_result = peer_table_upsert(&candidate, &peer);
        if (table_result != PEER_TABLE_OK) {
            set_error(load_result, section->e.name,
                      table_result == PEER_TABLE_DUPLICATE
                          ? "peer_id, router_id or endpoint duplicates another peer"
                          : "peer table is full or out of memory");
            goto fail;
        }
        load_result->loaded++;
    }

    candidate.generation = live_table->generation == UINT64_MAX
        ? UINT64_MAX
        : live_table->generation + 1U;
    peer_table_swap(live_table, &candidate);
    peer_table_destroy(&candidate);
    uci_unload(context, package);
    uci_free_context(context);
    return PEER_TABLE_OK;

fail:
    uci_unload(context, package);
    uci_free_context(context);
    peer_table_destroy(&candidate);
    load_result->loaded = 0U;
    return table_result;
}
