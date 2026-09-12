#include "static_routes.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uci.h>

static void set_error(
    struct static_route_load_result *result,
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
                 "route section '%s': %s", section, message);
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

static bool valid_route_id(const char *route_id)
{
    size_t i;

    if (route_id == NULL || strlen(route_id) != 32U) {
        return false;
    }

    for (i = 0U; i < 32U; i++) {
        if (!isdigit((unsigned char)route_id[i]) &&
            (route_id[i] < 'a' || route_id[i] > 'f')) {
            return false;
        }
    }
    return true;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL ||
        value == NULL ||
        text[0] < '0' ||
        text[0] > '9') {
        return false;
    }

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;

    if (!parse_u64(text, &parsed) || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
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

    if (strcmp(text, "1") == 0 ||
        strcasecmp(text, "true") == 0 ||
        strcasecmp(text, "yes") == 0 ||
        strcasecmp(text, "on") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "0") == 0 ||
        strcasecmp(text, "false") == 0 ||
        strcasecmp(text, "no") == 0 ||
        strcasecmp(text, "off") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static const char *option(
    struct uci_context *context,
    struct uci_section *section,
    const char *name
)
{
    return uci_lookup_option_string(context, section, name);
}

static bool parse_optional_u64(
    struct uci_context *context,
    struct uci_section *section,
    const char *name,
    uint64_t default_value,
    uint64_t *value
)
{
    const char *text = option(context, section, name);

    if (text == NULL) {
        *value = default_value;
        return true;
    }
    return parse_u64(text, value);
}

static bool parse_optional_u32(
    struct uci_context *context,
    struct uci_section *section,
    const char *name,
    uint32_t default_value,
    uint32_t *value
)
{
    const char *text = option(context, section, name);

    if (text == NULL) {
        *value = default_value;
        return true;
    }
    return parse_u32(text, value);
}

static bool parse_static_route(
    struct uci_context *context,
    struct uci_section *section,
    struct agent_route *route,
    struct static_route_load_result *result
)
{
    const char *text;
    uint32_t value;

    memset(route, 0, sizeof(*route));
    text = option(context, section, "route_id");
    if (!valid_route_id(text) ||
        !copy_text(route->route_id, sizeof(route->route_id), text)) {
        set_error(result, section->e.name,
                  "route_id must be exactly 32 hexadecimal characters");
        return false;
    }

    if (!copy_text(route->intent, sizeof(route->intent),
                   option(context, section, "intent")) ||
        !copy_text(route->origin, sizeof(route->origin),
                   option(context, section, "origin")) ||
        !copy_text(route->endpoint, sizeof(route->endpoint),
                   option(context, section, "endpoint")) ||
        !copy_text(route->tenant, sizeof(route->tenant),
                   option(context, section, "tenant"))) {
        set_error(result, section->e.name,
                  "intent, origin, endpoint and tenant are required");
        return false;
    }

    text = option(context, section, "region");
    if (!copy_text(route->region, sizeof(route->region),
                   text == NULL ? "local" : text)) {
        set_error(result, section->e.name, "region is invalid or too long");
        return false;
    }

    if (!parse_optional_u32(context, section, "version", 1U, &route->version) ||
        route->version == 0U) {
        set_error(result, section->e.name,
                  "version must be a positive 32-bit integer");
        return false;
    }
    if (!parse_optional_u64(context, section, "cost_microunits", 0U,
                            &route->cost_microunits)) {
        set_error(result, section->e.name,
                  "cost_microunits must be an unsigned integer");
        return false;
    }
    if (!parse_optional_u32(context, section, "latency_ms", 0U,
                            &route->latency_ms)) {
        set_error(result, section->e.name,
                  "latency_ms must be an unsigned 32-bit integer");
        return false;
    }

    if (!parse_optional_u32(context, section, "trust", 50U, &value) ||
        value > 100U) {
        set_error(result, section->e.name,
                  "trust must be between 0 and 100");
        return false;
    }
    route->trust_level = (uint8_t)value;

    if (!parse_optional_u32(context, section, "load_permille", 0U, &value) ||
        value > 1000U) {
        set_error(result, section->e.name,
                  "load_permille must be between 0 and 1000");
        return false;
    }
    route->load_permille = (uint16_t)value;

    if (!parse_optional_u32(context, section, "hop_count", 0U, &value) ||
        value > UINT8_MAX) {
        set_error(result, section->e.name,
                  "hop_count must be between 0 and 255");
        return false;
    }
    route->hop_count = (uint8_t)value;

    route->healthy = true;
    route->lease_expires_ms = UINT64_MAX;
    route->sequence = 1U;
    route->source = AGENT_ROUTE_SOURCE_STATIC;
    return true;
}

enum route_table_result static_routes_reload(
    struct route_table *live_table,
    const char *uci_package_name,
    struct static_route_load_result *load_result
)
{
    struct route_table candidate;
    struct uci_context *context;
    struct uci_package *package = NULL;
    struct uci_element *element;
    struct uci_section *section;
    struct agent_route route;
    enum route_table_result route_result;
    bool enabled;

    if (live_table == NULL ||
        uci_package_name == NULL ||
        load_result == NULL) {
        return ROUTE_TABLE_INVALID;
    }

    memset(load_result, 0, sizeof(*load_result));
    route_result = route_table_clone_excluding_source(
        &candidate, live_table, AGENT_ROUTE_SOURCE_STATIC);
    if (route_result != ROUTE_TABLE_OK) {
        set_error(load_result, NULL,
                  "failed to create atomic candidate route table");
        return route_result;
    }

    context = uci_alloc_context();
    if (context == NULL) {
        route_table_destroy(&candidate);
        set_error(load_result, NULL, "failed to allocate UCI context");
        return ROUTE_TABLE_NO_MEMORY;
    }

    if (uci_load(context, uci_package_name, &package) != UCI_OK) {
        uci_free_context(context);
        route_table_destroy(&candidate);
        set_error(load_result, NULL, "failed to load UCI package");
        return ROUTE_TABLE_INVALID;
    }

    uci_foreach_element(&package->sections, element) {
        section = uci_to_section(element);
        if (strcmp(section->type, "route") != 0) {
            continue;
        }

        if (!parse_bool(option(context, section, "enabled"), true, &enabled)) {
            set_error(load_result, section->e.name,
                      "enabled must be a boolean");
            route_result = ROUTE_TABLE_INVALID;
            goto fail;
        }
        if (!enabled) {
            continue;
        }

        if (!parse_static_route(context, section, &route, load_result)) {
            route_result = ROUTE_TABLE_INVALID;
            goto fail;
        }
        if (route_table_find(&candidate, route.route_id) != NULL) {
            set_error(load_result, section->e.name,
                      "route_id duplicates another static or dynamic route");
            route_result = ROUTE_TABLE_INVALID;
            goto fail;
        }

        route_result = route_table_upsert(&candidate, &route);
        if (route_result != ROUTE_TABLE_OK) {
            set_error(load_result, section->e.name,
                      "route table is full or out of memory");
            goto fail;
        }
        load_result->loaded++;
    }

    candidate.generation = live_table->generation == UINT64_MAX
        ? UINT64_MAX
        : live_table->generation + 1U;
    route_table_swap(live_table, &candidate);
    route_table_destroy(&candidate);
    uci_unload(context, package);
    uci_free_context(context);
    return ROUTE_TABLE_OK;

fail:
    uci_unload(context, package);
    uci_free_context(context);
    route_table_destroy(&candidate);
    load_result->loaded = 0U;
    return route_result;
}
