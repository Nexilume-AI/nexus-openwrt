#include "adapter_registry.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <uci.h>
#include <json-c/json.h>

static void set_error(
    struct adapter_registry_load_result *result,
    const char *section,
    const char *message
)
{
    if (result == NULL) {
        return;
    }
    if (section == NULL) {
        (void)snprintf(result->error, sizeof(result->error), "%s", message);
    } else {
        (void)snprintf(result->error, sizeof(result->error),
                       "mapping section '%s': %s", section, message);
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

static bool copy_optional_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || capacity == 0U) {
        return false;
    }
    if (source == NULL || source[0] == '\0') {
        target[0] = '\0';
        return true;
    }
    written = snprintf(target, capacity, "%s", source);
    return written >= 0 && (size_t)written < capacity;
}

static bool input_schema_valid(const char *text)
{
    struct json_tokener *tokener;
    struct json_object *schema;
    enum json_tokener_error error;

    if (text == NULL || text[0] == '\0') {
        return true;
    }
    tokener = json_tokener_new_ex(32);
    if (tokener == NULL) {
        return false;
    }
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    schema = json_tokener_parse_ex(tokener, text, (int)strlen(text));
    error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (error != json_tokener_success || schema == NULL ||
        !json_object_is_type(schema, json_type_object)) {
        json_object_put(schema);
        return false;
    }
    json_object_put(schema);
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

static bool parse_u32(const char *text, uint32_t default_value, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL) {
        return false;
    }
    if (text == NULL) {
        *value = default_value;
        return true;
    }
    if (text[0] < '0' || text[0] > '9') {
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

bool adapter_registry_load(
    const char *uci_package,
    const char *uci_config_dir,
    struct adapter_registry *registry,
    struct adapter_registry_load_result *result
)
{
    struct uci_context *context;
    struct uci_package *package = NULL;
    struct uci_element *element;

    if (uci_package == NULL || uci_package[0] == '\0' ||
        registry == NULL || result == NULL) {
        return false;
    }
    memset(registry, 0, sizeof(*registry));
    memset(result, 0, sizeof(*result));
    context = uci_alloc_context();
    if (context == NULL) {
        set_error(result, NULL, "failed to allocate UCI context");
        return false;
    }
    if (uci_config_dir != NULL) {
        uci_set_confdir(context, uci_config_dir);
    }
    if (uci_load(context, uci_package, &package) != UCI_OK) {
        set_error(result, NULL, "failed to load UCI package");
        uci_free_context(context);
        return false;
    }
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        struct agent_adapter_mapping *mapping;
        const char *text;
        bool enabled;

        if (strcmp(section->type, "mapping") != 0) {
            continue;
        }
        text = uci_lookup_option_string(context, section, "enabled");
        if (!parse_bool(text, false, &enabled)) {
            set_error(result, section->e.name, "enabled must be boolean");
            goto failed;
        }
        if (!enabled) {
            continue;
        }
        if (registry->count >= AGENT_ADAPTER_MAX_MAPPINGS) {
            set_error(result, section->e.name,
                      "enabled mapping count exceeds 256");
            goto failed;
        }
        mapping = &registry->mappings[registry->count];
        memset(mapping, 0, sizeof(*mapping));
        mapping->enabled = true;
        mapping->protocol = agent_adapter_protocol_parse(
            uci_lookup_option_string(context, section, "protocol"));
        if (!copy_text(mapping->authority, sizeof(mapping->authority),
                       uci_lookup_option_string(context, section,
                                                "authority")) ||
            !copy_text(mapping->selector, sizeof(mapping->selector),
                       uci_lookup_option_string(context, section,
                                                "selector")) ||
            !copy_text(mapping->intent, sizeof(mapping->intent),
                       uci_lookup_option_string(context, section, "intent"))) {
            set_error(result, section->e.name,
                      "authority, selector and intent are required");
            goto failed;
        }
        if (!parse_u32(uci_lookup_option_string(context, section, "version"),
                       1U, &mapping->intent_version) ||
            !copy_optional_text(
                mapping->title, sizeof(mapping->title),
                uci_lookup_option_string(context, section, "title")) ||
            !copy_optional_text(
                mapping->description, sizeof(mapping->description),
                uci_lookup_option_string(context, section, "description")) ||
            !copy_optional_text(
                mapping->input_schema_json,
                sizeof(mapping->input_schema_json),
                uci_lookup_option_string(context, section,
                                         "input_schema_json")) ||
            !parse_bool(uci_lookup_option_string(context, section, "task"),
                        false, &mapping->task) ||
            !parse_bool(uci_lookup_option_string(context, section, "resumable"),
                        false, &mapping->resumable) ||
            !parse_bool(uci_lookup_option_string(context, section, "demo"),
                        false, &mapping->demo) ||
            !parse_bool(uci_lookup_option_string(context, section, "chat"),
                        false, &mapping->chat) ||
            !parse_bool(uci_lookup_option_string(context, section, "interactive"),
                        false, &mapping->interactive) ||
            !input_schema_valid(mapping->input_schema_json) ||
            (mapping->resumable && !mapping->task) ||
            (mapping->chat && (!mapping->task || !mapping->interactive)) ||
            !agent_adapter_mapping_valid(mapping)) {
            set_error(result, section->e.name,
                      "protocol, identifiers, version or input schema are invalid");
            goto failed;
        }
        registry->count++;
    }
    if (agent_adapter_registry_validate(registry->mappings, registry->count) !=
        AGENT_ADAPTER_OK) {
        set_error(result, NULL,
                  "registry contains invalid or duplicate mapping keys");
        goto failed;
    }
    uci_unload(context, package);
    uci_free_context(context);
    return true;

failed:
    uci_unload(context, package);
    uci_free_context(context);
    memset(registry, 0, sizeof(*registry));
    return false;
}

static bool same_key(
    const struct agent_adapter_mapping *left,
    const struct agent_adapter_mapping *right
)
{
    return left->protocol == right->protocol &&
           strcmp(left->authority, right->authority) == 0 &&
           strcmp(left->selector, right->selector) == 0;
}

static bool same_mapping(
    const struct agent_adapter_mapping *left,
    const struct agent_adapter_mapping *right
)
{
    return same_key(left, right) &&
           strcmp(left->intent, right->intent) == 0 &&
           left->intent_version == right->intent_version &&
           strcmp(left->title, right->title) == 0 &&
           strcmp(left->description, right->description) == 0 &&
           strcmp(left->input_schema_json, right->input_schema_json) == 0 &&
           left->task == right->task &&
           left->resumable == right->resumable &&
           left->demo == right->demo &&
           left->chat == right->chat &&
           left->interactive == right->interactive;
}

bool adapter_registry_merge(
    const struct adapter_registry *static_registry,
    const struct agent_adapter_mapping *dynamic_mappings,
    size_t dynamic_count,
    struct adapter_registry *effective,
    struct adapter_registry_merge_result *result
)
{
    size_t dynamic_index;

    if (static_registry == NULL || effective == NULL || result == NULL ||
        (dynamic_mappings == NULL && dynamic_count != 0U) ||
        static_registry->count > AGENT_ADAPTER_MAX_MAPPINGS ||
        dynamic_count > AGENT_ADAPTER_MAX_MAPPINGS ||
        agent_adapter_registry_validate(static_registry->mappings,
                                        static_registry->count) !=
            AGENT_ADAPTER_OK) return false;
    memset(effective, 0, sizeof(*effective));
    memset(result, 0, sizeof(*result));
    memcpy(effective->mappings, static_registry->mappings,
           static_registry->count * sizeof(effective->mappings[0]));
    memcpy(effective->local_dynamic, static_registry->local_dynamic,
           static_registry->count * sizeof(effective->local_dynamic[0]));
    effective->count = static_registry->count;
    for (dynamic_index = 0U; dynamic_index < dynamic_count; dynamic_index++) {
        const struct agent_adapter_mapping *candidate =
            &dynamic_mappings[dynamic_index];
        size_t current_index;
        bool duplicate = false;

        if (!agent_adapter_mapping_valid(candidate) ||
            candidate->protocol != AGENT_ADAPTER_PROTOCOL_MCP ||
            !input_schema_valid(candidate->input_schema_json) ||
            (candidate->resumable && !candidate->task) ||
            (candidate->chat &&
             (!candidate->task || !candidate->interactive))) {
            result->invalid_dynamic++;
            continue;
        }
        for (current_index = 0U; current_index < effective->count;
             current_index++) {
            const struct agent_adapter_mapping *current =
                &effective->mappings[current_index];

            if (!same_key(current, candidate)) continue;
            duplicate = true;
            if (same_mapping(current, candidate)) {
                effective->local_dynamic[current_index] = true;
                result->identical_merged++;
            } else {
                result->static_conflicts++;
            }
            break;
        }
        if (duplicate) continue;
        if (effective->count >= AGENT_ADAPTER_MAX_MAPPINGS) {
            result->capacity_rejected++;
            continue;
        }
        effective->mappings[effective->count] = *candidate;
        effective->local_dynamic[effective->count] = true;
        effective->count++;
        result->dynamic_added++;
    }
    return agent_adapter_registry_validate(
               effective->mappings, effective->count) == AGENT_ADAPTER_OK;
}
