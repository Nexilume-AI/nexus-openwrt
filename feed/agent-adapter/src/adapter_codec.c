#include "adapter_codec.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool utf8_bytes_valid(const char *text, size_t length)
{
    size_t index = 0U;

    if (text == NULL) return false;
    while (index < length) {
        unsigned char first = (unsigned char)text[index++];
        unsigned char second;
        unsigned char third;
        unsigned char fourth;

        if (first <= 0x7fU) continue;
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index >= length) return false;
            second = (unsigned char)text[index++];
            if (second < 0x80U || second > 0xbfU) return false;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (length - index < 2U) return false;
            second = (unsigned char)text[index++];
            third = (unsigned char)text[index++];
            if (third < 0x80U || third > 0xbfU ||
                (first == 0xe0U && (second < 0xa0U || second > 0xbfU)) ||
                (first == 0xedU && (second < 0x80U || second > 0x9fU)) ||
                (first != 0xe0U && first != 0xedU &&
                 (second < 0x80U || second > 0xbfU))) return false;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (length - index < 3U) return false;
            second = (unsigned char)text[index++];
            third = (unsigned char)text[index++];
            fourth = (unsigned char)text[index++];
            if (third < 0x80U || third > 0xbfU ||
                fourth < 0x80U || fourth > 0xbfU ||
                (first == 0xf0U && (second < 0x90U || second > 0xbfU)) ||
                (first == 0xf4U && (second < 0x80U || second > 0x8fU)) ||
                (first != 0xf0U && first != 0xf4U &&
                 (second < 0x80U || second > 0xbfU))) return false;
            continue;
        }
        return false;
    }
    return true;
}

static void append_json_unicode_escape(
    char *target,
    size_t *offset,
    uint16_t value
)
{
    static const char hex[] = "0123456789abcdef";

    target[(*offset)++] = '\\';
    target[(*offset)++] = 'u';
    target[(*offset)++] = hex[(value >> 12U) & 0x0fU];
    target[(*offset)++] = hex[(value >> 8U) & 0x0fU];
    target[(*offset)++] = hex[(value >> 4U) & 0x0fU];
    target[(*offset)++] = hex[value & 0x0fU];
}

static bool json_extensions_absent(const char *text, size_t length)
{
    bool in_string = false;
    bool escaped = false;
    size_t index;

    for (index = 0U; index < length; index++) {
        unsigned char current = (unsigned char)text[index];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                in_string = false;
            }
            continue;
        }
        if (current == '"') {
            in_string = true;
            continue;
        }
        if (current == '/') return false;
        if (current == ',') {
            size_t next = index + 1U;

            while (next < length &&
                   (text[next] == ' ' || text[next] == '\t' ||
                    text[next] == '\r' || text[next] == '\n')) next++;
            if (next < length && (text[next] == '}' || text[next] == ']')) {
                return false;
            }
            continue;
        }
        if ((current >= 'A' && current <= 'Z') ||
            (current >= 'a' && current <= 'z')) {
            size_t remaining = length - index;
            size_t token_length = 0U;

            if ((current == 'e' || current == 'E') && index > 0U &&
                text[index - 1U] >= '0' && text[index - 1U] <= '9') {
                size_t next = index + 1U;

                if (next < length &&
                    (text[next] == '+' || text[next] == '-')) next++;
                if (next < length && text[next] >= '0' && text[next] <= '9') {
                    continue;
                }
            }
            if (remaining >= 4U && memcmp(text + index, "true", 4U) == 0) {
                token_length = 4U;
            } else if (remaining >= 5U &&
                       memcmp(text + index, "false", 5U) == 0) {
                token_length = 5U;
            } else if (remaining >= 4U &&
                       memcmp(text + index, "null", 4U) == 0) {
                token_length = 4U;
            } else {
                return false;
            }
            index += token_length - 1U;
        }
    }
    return !in_string && !escaped;
}

static char *strict_json_ascii_copy(
    const char *text,
    size_t length,
    size_t *escaped_length
)
{
    char *escaped;
    size_t input = 0U;
    size_t output = 0U;

    if (escaped_length == NULL || !utf8_bytes_valid(text, length) ||
        !json_extensions_absent(text, length) ||
        length > (SIZE_MAX - 1U) / 3U) return NULL;
    escaped = malloc(length * 3U + 1U);
    if (escaped == NULL) return NULL;
    while (input < length) {
        unsigned char first = (unsigned char)text[input++];
        unsigned char second;
        unsigned char third;
        unsigned char fourth;
        uint32_t codepoint;

        if (first <= 0x7fU) {
            escaped[output++] = (char)first;
            continue;
        }
        if (first <= 0xdfU) {
            second = (unsigned char)text[input++];
            codepoint = ((uint32_t)(first & 0x1fU) << 6U) |
                ((uint32_t)second & 0x3fU);
        } else if (first <= 0xefU) {
            second = (unsigned char)text[input++];
            third = (unsigned char)text[input++];
            codepoint = ((uint32_t)(first & 0x0fU) << 12U) |
                (((uint32_t)second & 0x3fU) << 6U) |
                ((uint32_t)third & 0x3fU);
        } else {
            uint16_t high;
            uint16_t low;

            second = (unsigned char)text[input++];
            third = (unsigned char)text[input++];
            fourth = (unsigned char)text[input++];
            codepoint = ((uint32_t)(first & 0x07U) << 18U) |
                (((uint32_t)second & 0x3fU) << 12U) |
                (((uint32_t)third & 0x3fU) << 6U) |
                ((uint32_t)fourth & 0x3fU);
            codepoint -= 0x10000U;
            high = (uint16_t)(0xd800U + (codepoint >> 10U));
            low = (uint16_t)(0xdc00U + (codepoint & 0x03ffU));
            append_json_unicode_escape(escaped, &output, high);
            append_json_unicode_escape(escaped, &output, low);
            continue;
        }
        append_json_unicode_escape(escaped, &output, (uint16_t)codepoint);
    }
    escaped[output] = '\0';
    *escaped_length = output;
    return escaped;
}

static struct json_object *parse_json_exact(const char *text, size_t length)
{
    char *escaped;
    size_t escaped_length;
    struct json_tokener *tokener;
    struct json_object *root;
    enum json_tokener_error error;
    size_t offset;

    if (text == NULL || length == 0U || length > INT_MAX) return NULL;
    escaped = strict_json_ascii_copy(text, length, &escaped_length);
    if (escaped == NULL || escaped_length > INT_MAX) {
        free(escaped);
        return NULL;
    }
    tokener = json_tokener_new_ex(32);
    if (tokener == NULL) {
        free(escaped);
        return NULL;
    }
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    root = json_tokener_parse_ex(tokener, escaped, (int)escaped_length);
    error = json_tokener_get_error(tokener);
    offset = json_tokener_get_parse_end(tokener);
    while (offset < escaped_length &&
           (escaped[offset] == ' ' || escaped[offset] == '\t' ||
            escaped[offset] == '\r' || escaped[offset] == '\n')) {
        offset++;
    }
    if (error != json_tokener_success || root == NULL ||
        offset != escaped_length) {
        json_object_put(root);
        root = NULL;
    }
    json_tokener_free(tokener);
    free(escaped);
    return root;
}

bool adapter_codec_serialize_envelope(
    struct json_object *envelope,
    const char **serialized,
    size_t *serialized_length
)
{
    const char *text;
    size_t length;
    struct json_object *validated;

    if (envelope == NULL || serialized == NULL || serialized_length == NULL ||
        !json_object_is_type(envelope, json_type_object)) {
        return false;
    }
    text = json_object_to_json_string_ext(envelope, JSON_C_TO_STRING_PLAIN);
    if (text == NULL) {
        return false;
    }
    length = strlen(text);
    validated = parse_json_exact(text, length);
    if (validated == NULL || !json_object_is_type(validated, json_type_object)) {
        json_object_put(validated);
        return false;
    }
    json_object_put(validated);
    *serialized = text;
    *serialized_length = length;
    return true;
}

static bool json_string(
    struct json_object *object,
    const char *name,
    const char **value
)
{
    struct json_object *field;

    return object != NULL &&
           json_object_object_get_ex(object, name, &field) &&
           json_object_get_type(field) == json_type_string &&
           (*value = json_object_get_string(field))[0] != '\0';
}

static bool copy_text(char *target, size_t capacity, const char *source)
{
    int written;

    if (target == NULL || source == NULL || source[0] == '\0') {
        return false;
    }
    written = snprintf(target, capacity, "%s", source);
    return written > 0 && (size_t)written < capacity;
}

static struct json_object *mcp_result(
    struct json_object *request,
    struct json_object *result
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *id;

    if (root == NULL || result == NULL ||
        !json_object_object_get_ex(request, "id", &id) ||
        (json_object_get_type(id) != json_type_string &&
         json_object_get_type(id) != json_type_int)) {
        json_object_put(root);
        json_object_put(result);
        return NULL;
    }
    json_object_object_add(root, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(root, "result", result);
    json_object_object_add(root, "id", json_object_get(id));
    return root;
}

enum adapter_mcp_control_result adapter_codec_mcp_control(
    const struct adapter_registry *registry,
    const char *authority,
    const char *body,
    size_t body_length,
    struct json_object **response
)
{
    struct json_object *request = NULL;
    struct json_object *result = NULL;
    struct json_object *tools = NULL;
    const char *jsonrpc;
    const char *method;
    size_t index;

    if (response == NULL) {
        return ADAPTER_MCP_CONTROL_INVALID;
    }
    *response = NULL;
    request = parse_json_exact(body, body_length);
    if (request == NULL || !json_object_is_type(request, json_type_object) ||
        !json_string(request, "jsonrpc", &jsonrpc) ||
        strcmp(jsonrpc, "2.0") != 0 ||
        !json_string(request, "method", &method)) {
        json_object_put(request);
        return ADAPTER_MCP_CONTROL_INVALID;
    }
    if (strcmp(method, "tools/call") == 0) {
        json_object_put(request);
        return ADAPTER_MCP_CONTROL_NOT_CONTROL;
    }
    if (strcmp(method, "notifications/initialized") == 0) {
        struct json_object *id;
        bool has_id = json_object_object_get_ex(request, "id", &id);

        json_object_put(request);
        return has_id ? ADAPTER_MCP_CONTROL_INVALID :
                        ADAPTER_MCP_CONTROL_NOTIFICATION;
    }
    if (strcmp(method, "initialize") == 0) {
        struct json_object *server_info = json_object_new_object();
        struct json_object *capabilities = json_object_new_object();
        struct json_object *tool_capability = json_object_new_object();

        result = json_object_new_object();
        if (result == NULL || server_info == NULL || capabilities == NULL ||
            tool_capability == NULL) {
            json_object_put(result);
            json_object_put(server_info);
            json_object_put(capabilities);
            json_object_put(tool_capability);
            json_object_put(request);
            return ADAPTER_MCP_CONTROL_OUT_OF_MEMORY;
        }
        json_object_object_add(result, "protocolVersion",
                               json_object_new_string("2025-03-26"));
        json_object_object_add(server_info, "name",
                               json_object_new_string("nexus-openwrt-agent-router"));
        json_object_object_add(server_info, "version",
                               json_object_new_string("0.6.0"));
        json_object_object_add(result, "serverInfo", server_info);
        json_object_object_add(tool_capability, "listChanged",
                               json_object_new_boolean(false));
        json_object_object_add(capabilities, "tools", tool_capability);
        json_object_object_add(result, "capabilities", capabilities);
    } else if (strcmp(method, "tools/list") == 0) {
        if (registry == NULL || authority == NULL) {
            json_object_put(request);
            return ADAPTER_MCP_CONTROL_INVALID;
        }
        result = json_object_new_object();
        tools = json_object_new_array();
        if (result == NULL || tools == NULL) {
            json_object_put(result);
            json_object_put(tools);
            json_object_put(request);
            return ADAPTER_MCP_CONTROL_OUT_OF_MEMORY;
        }
        for (index = 0U; index < registry->count; index++) {
            const struct agent_adapter_mapping *mapping =
                &registry->mappings[index];
            struct json_object *tool;
            struct json_object *schema;
            struct json_object *metadata;
            struct json_object *nexus;

            if (!mapping->enabled ||
                mapping->protocol != AGENT_ADAPTER_PROTOCOL_MCP ||
                strcmp(mapping->authority, authority) != 0) {
                continue;
            }
            tool = json_object_new_object();
            schema = mapping->input_schema_json[0] != '\0'
                ? parse_json_exact(mapping->input_schema_json,
                                   strlen(mapping->input_schema_json))
                : json_tokener_parse(
                    "{\"type\":\"object\",\"additionalProperties\":true}");
            if (tool == NULL || schema == NULL) {
                json_object_put(tool);
                json_object_put(schema);
                json_object_put(result);
                json_object_put(tools);
                json_object_put(request);
                return ADAPTER_MCP_CONTROL_OUT_OF_MEMORY;
            }
            json_object_object_add(tool, "name",
                                   json_object_new_string(mapping->selector));
            if (mapping->title[0] != '\0') {
                json_object_object_add(tool, "title",
                                       json_object_new_string(mapping->title));
            }
            json_object_object_add(
                tool, "description",
                json_object_new_string(mapping->description[0] != '\0'
                    ? mapping->description : mapping->intent));
            json_object_object_add(tool, "inputSchema", schema);
            metadata = json_object_new_object();
            nexus = json_object_new_object();
            if (metadata == NULL || nexus == NULL) {
                json_object_put(metadata);
                json_object_put(nexus);
                json_object_put(tool);
                json_object_put(tools);
                json_object_put(result);
                json_object_put(request);
                return ADAPTER_MCP_CONTROL_OUT_OF_MEMORY;
            }
            json_object_object_add(nexus, "task",
                                   json_object_new_boolean(mapping->task));
            json_object_object_add(nexus, "resumable",
                                   json_object_new_boolean(mapping->resumable));
            json_object_object_add(nexus, "demo",
                                   json_object_new_boolean(mapping->demo));
            json_object_object_add(nexus, "chat",
                                   json_object_new_boolean(mapping->chat));
            json_object_object_add(nexus, "interactive",
                                   json_object_new_boolean(mapping->interactive));
            json_object_object_add(metadata, "nexus", nexus);
            json_object_object_add(tool, "_meta", metadata);
            json_object_array_add(tools, tool);
        }
        json_object_object_add(result, "tools", tools);
        tools = NULL;
    } else {
        json_object_put(request);
        return ADAPTER_MCP_CONTROL_INVALID;
    }
    *response = mcp_result(request, result);
    json_object_put(request);
    return *response != NULL ? ADAPTER_MCP_CONTROL_HANDLED :
                               ADAPTER_MCP_CONTROL_INVALID;
}

static bool extract_mcp(
    struct json_object *root,
    char selector[AGENT_ADAPTER_SELECTOR_LEN],
    char external_id[AGENT_ADAPTER_TASK_ID_LEN]
)
{
    struct json_object *id;
    struct json_object *params;
    struct json_object *arguments;
    const char *jsonrpc;
    const char *method;
    const char *name;
    int written;

    if (!json_string(root, "jsonrpc", &jsonrpc) ||
        strcmp(jsonrpc, "2.0") != 0 ||
        !json_string(root, "method", &method) ||
        strcmp(method, "tools/call") != 0 ||
        !json_object_object_get_ex(root, "id", &id) ||
        !json_object_object_get_ex(root, "params", &params) ||
        json_object_get_type(params) != json_type_object ||
        !json_string(params, "name", &name) ||
        !copy_text(selector, AGENT_ADAPTER_SELECTOR_LEN, name)) {
        return false;
    }
    if (json_object_object_get_ex(params, "arguments", &arguments) &&
        json_object_get_type(arguments) != json_type_object) {
        return false;
    }
    if (json_object_get_type(id) == json_type_string) {
        written = snprintf(external_id, AGENT_ADAPTER_TASK_ID_LEN, "%s",
                           json_object_get_string(id));
    } else if (json_object_get_type(id) == json_type_int) {
        written = snprintf(external_id, AGENT_ADAPTER_TASK_ID_LEN, "%" PRId64,
                           json_object_get_int64(id));
    } else {
        return false;
    }
    return written > 0 && (size_t)written < AGENT_ADAPTER_TASK_ID_LEN;
}

static bool extract_a2a(
    struct json_object *root,
    const char *configured_selector,
    char selector[AGENT_ADAPTER_SELECTOR_LEN],
    char external_id[AGENT_ADAPTER_TASK_ID_LEN]
)
{
    struct json_object *message;
    const char *role;
    const char *message_id;

    return configured_selector != NULL &&
           copy_text(selector, AGENT_ADAPTER_SELECTOR_LEN,
                     configured_selector) &&
           json_object_object_get_ex(root, "message", &message) &&
           json_object_get_type(message) == json_type_object &&
           json_string(message, "role", &role) &&
           strcmp(role, "ROLE_USER") == 0 &&
           json_string(message, "messageId", &message_id) &&
           copy_text(external_id, AGENT_ADAPTER_TASK_ID_LEN, message_id);
}

static struct json_object *build_envelope(
    const struct adapter_codec_options *options,
    const struct agent_adapter_mapping *mapping,
    const struct adapter_normalized_request *normalized,
    bool local_dynamic
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *flags = json_object_new_object();
    struct json_object *constraints = json_object_new_object();
    struct json_object *payload = json_object_new_object();

    if (root == NULL || flags == NULL || constraints == NULL || payload == NULL) {
        json_object_put(root);
        json_object_put(flags);
        json_object_put(constraints);
        json_object_put(payload);
        return NULL;
    }
    json_object_object_add(root, "version", json_object_new_string("1.0"));
    json_object_object_add(root, "intent",
                           json_object_new_string(mapping->intent));
    json_object_object_add(root, "intent_version",
                           json_object_new_uint64(mapping->intent_version));
    json_object_object_add(root, "task_id",
                           json_object_new_string(normalized->task_id));
    json_object_object_add(root, "tenant",
                           json_object_new_string(options->tenant));
    json_object_object_add(root, "source_agent",
                           json_object_new_string(options->source_agent));
    json_object_object_add(root, "target_agent",
                           json_object_new_string(options->authority));
    json_object_object_add(root, "hop_limit",
                           json_object_new_uint64(options->hop_limit));
    if (options->deadline != NULL && options->deadline[0] != '\0') {
        json_object_object_add(root, "deadline",
                               json_object_new_string(options->deadline));
    }
    json_object_object_add(flags, "idempotent", json_object_new_boolean(false));
    json_object_object_add(flags, "allow_retry", json_object_new_boolean(false));
    json_object_object_add(flags, "interactive",
                           json_object_new_boolean(
                               mapping->task || mapping->interactive ||
                               mapping->chat));
    json_object_object_add(root, "flags", flags);
    json_object_object_add(constraints, "region",
                           json_object_new_string(
                               local_dynamic ? "local" : options->region));
    json_object_object_add(root, "constraints", constraints);
    if (options->run_context_url != NULL &&
        options->run_context_url[0] != '\0' &&
        options->run_context_token != NULL &&
        options->run_context_token[0] != '\0') {
        struct json_object *run_context = json_object_new_object();

        if (run_context == NULL) {
            json_object_put(root);
            json_object_put(payload);
            return NULL;
        }
        json_object_object_add(
            run_context, "exchange_url",
            json_object_new_string(options->run_context_url));
        json_object_object_add(
            run_context, "exchange_token",
            json_object_new_string(options->run_context_token));
        json_object_object_add(root, "nexus_run_context", run_context);
    }
    json_object_object_add(payload, "protocol",
                           json_object_new_string(
                               agent_adapter_protocol_name(options->protocol)));
    json_object_object_add(payload, "authority",
                           json_object_new_string(options->authority));
    json_object_object_add(payload, "selector",
                           json_object_new_string(normalized->selector));
    json_object_object_add(payload, "request",
                           json_object_get(normalized->request));
    json_object_object_add(root, "payload", payload);
    return root;
}

enum adapter_codec_result adapter_codec_normalize(
    const struct adapter_registry *registry,
    const struct adapter_codec_options *options,
    const char *body,
    size_t body_length,
    struct adapter_normalized_request *normalized
)
{
    const struct agent_adapter_mapping *mapping = NULL;
    enum agent_adapter_result lookup_result;
    size_t mapping_index;

    if (registry == NULL || options == NULL || normalized == NULL ||
        options->authority == NULL || options->tenant == NULL ||
        options->source_agent == NULL || options->region == NULL ||
        options->hop_limit < 2U || options->hop_limit > 255U) {
        return ADAPTER_CODEC_INVALID_REQUEST;
    }
    memset(normalized, 0, sizeof(*normalized));
    normalized->protocol = options->protocol;
    normalized->request = parse_json_exact(body, body_length);
    if (normalized->request == NULL ||
        json_object_get_type(normalized->request) != json_type_object) {
        adapter_codec_request_clear(normalized);
        return ADAPTER_CODEC_INVALID_JSON;
    }
    if ((options->protocol == AGENT_ADAPTER_PROTOCOL_MCP &&
         !extract_mcp(normalized->request, normalized->selector,
                      normalized->external_id)) ||
        (options->protocol == AGENT_ADAPTER_PROTOCOL_A2A &&
         !extract_a2a(normalized->request, options->selector,
                      normalized->selector, normalized->external_id)) ||
        (options->protocol != AGENT_ADAPTER_PROTOCOL_MCP &&
         options->protocol != AGENT_ADAPTER_PROTOCOL_A2A) ||
        !agent_adapter_make_task_id(options->protocol,
                                    normalized->external_id,
                                    normalized->task_id)) {
        adapter_codec_request_clear(normalized);
        return ADAPTER_CODEC_INVALID_REQUEST;
    }
    lookup_result = agent_adapter_lookup(
        registry->mappings, registry->count, options->protocol,
        options->authority, normalized->selector, &mapping);
    if (lookup_result != AGENT_ADAPTER_OK) {
        if (lookup_result == AGENT_ADAPTER_NOT_FOUND) {
            return ADAPTER_CODEC_ROUTE_NOT_FOUND;
        }
        adapter_codec_request_clear(normalized);
        return ADAPTER_CODEC_INVALID_REQUEST;
    }
    mapping_index = (size_t)(mapping - registry->mappings);
    if (mapping_index >= registry->count) {
        adapter_codec_request_clear(normalized);
        return ADAPTER_CODEC_INVALID_REQUEST;
    }
    normalized->interactive = mapping->task || mapping->interactive ||
        mapping->chat;
    normalized->envelope = build_envelope(
        options, mapping, normalized, registry->local_dynamic[mapping_index]);
    if (normalized->envelope == NULL) {
        adapter_codec_request_clear(normalized);
        return ADAPTER_CODEC_OUT_OF_MEMORY;
    }
    return ADAPTER_CODEC_OK;
}

static bool content_type_is_json(const char *content_type)
{
    return content_type != NULL &&
           (strncmp(content_type, "application/json", 16U) == 0 ||
            strncmp(content_type, "application/vnd.", 16U) == 0) &&
           strstr(content_type, "json") != NULL;
}

static struct json_object *gateway_detail(
    int status,
    const char *content_type,
    const char *body,
    size_t body_length
)
{
    struct json_object *detail = json_object_new_object();
    struct json_object *parsed = NULL;

    if (detail == NULL) {
        return NULL;
    }
    json_object_object_add(detail, "gatewayStatus", json_object_new_int(status));
    if (body != NULL && body_length > 0U && content_type_is_json(content_type)) {
        parsed = parse_json_exact(body, body_length);
    }
    if (parsed != NULL) {
        json_object_object_add(detail, "gatewayBody", parsed);
    } else {
        json_object_object_add(
            detail, "gatewayBody",
            json_object_new_string_len(body != NULL ? body : "",
                                       (int)body_length));
    }
    return detail;
}

static int mcp_error_code(int status)
{
    if (status == 401 || status == 403) return -32001;
    if (status == 404) return -32004;
    if (status == 408 || status == 504) return -32008;
    if (status == 429) return -32029;
    return -32000;
}

static int a2a_error_code(int status)
{
    if (status == 401) return 16;
    if (status == 403) return 7;
    if (status == 404) return 5;
    if (status == 408 || status == 504) return 4;
    if (status == 429) return 8;
    return 13;
}

static struct json_object *mcp_response(
    const struct adapter_normalized_request *normalized,
    int status,
    const char *content_type,
    const char *body,
    size_t body_length
)
{
    struct json_object *root = json_object_new_object();
    struct json_object *id;

    if (root == NULL ||
        !json_object_object_get_ex(normalized->request, "id", &id)) {
        json_object_put(root);
        return NULL;
    }
    json_object_object_add(root, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(root, "id", json_object_get(id));
    if (status < 200 || status >= 300) {
        struct json_object *error = json_object_new_object();
        struct json_object *detail = gateway_detail(
            status, content_type, body, body_length);

        if (error == NULL || detail == NULL) {
            json_object_put(error);
            json_object_put(detail);
            json_object_put(root);
            return NULL;
        }
        json_object_object_add(error, "code",
                               json_object_new_int(mcp_error_code(status)));
        json_object_object_add(error, "message",
                               json_object_new_string(
                                   "Nexus Agent invocation failed"));
        json_object_object_add(error, "data", detail);
        json_object_object_add(root, "error", error);
        return root;
    }
    {
        struct json_object *result = json_object_new_object();
        struct json_object *content = json_object_new_array();
        struct json_object *part = json_object_new_object();
        struct json_object *structured = NULL;

        if (result == NULL || content == NULL || part == NULL) {
            json_object_put(result);
            json_object_put(content);
            json_object_put(part);
            json_object_put(root);
            return NULL;
        }
        json_object_object_add(part, "type", json_object_new_string("text"));
        json_object_object_add(
            part, "text", json_object_new_string_len(
                body != NULL ? body : "", (int)body_length));
        json_object_array_add(content, part);
        json_object_object_add(result, "content", content);
        json_object_object_add(result, "isError", json_object_new_boolean(false));
        if (body != NULL && body_length > 0U &&
            content_type_is_json(content_type)) {
            structured = parse_json_exact(body, body_length);
        }
        if (structured != NULL &&
            json_object_get_type(structured) == json_type_object) {
            json_object_object_add(result, "structuredContent", structured);
        } else {
            json_object_put(structured);
        }
        json_object_object_add(root, "result", result);
    }
    return root;
}

static struct json_object *a2a_response(
    const struct adapter_normalized_request *normalized,
    int status,
    const char *content_type,
    const char *body,
    size_t body_length
)
{
    if (status < 200 || status >= 300) {
        struct json_object *root = json_object_new_object();
        struct json_object *details = json_object_new_array();
        struct json_object *detail = gateway_detail(
            status, content_type, body, body_length);

        if (root == NULL || details == NULL || detail == NULL) {
            json_object_put(root);
            json_object_put(details);
            json_object_put(detail);
            return NULL;
        }
        json_object_object_add(detail, "@type", json_object_new_string(
            "type.googleapis.com/nexus.agent.GatewayError"));
        json_object_array_add(details, detail);
        json_object_object_add(root, "code",
                               json_object_new_int(a2a_error_code(status)));
        json_object_object_add(root, "message", json_object_new_string(
            "Nexus Agent invocation failed"));
        json_object_object_add(root, "details", details);
        return root;
    }
    {
        struct json_object *root = json_object_new_object();
        struct json_object *message = json_object_new_object();
        struct json_object *parts = json_object_new_array();
        struct json_object *part = json_object_new_object();
        struct json_object *request_message;
        struct json_object *request_context;
        struct json_object *parsed = NULL;

        if (root == NULL || message == NULL || parts == NULL || part == NULL ||
            !json_object_object_get_ex(normalized->request, "message",
                                       &request_message)) {
            json_object_put(root);
            json_object_put(message);
            json_object_put(parts);
            json_object_put(part);
            return NULL;
        }
        json_object_object_add(message, "messageId",
                               json_object_new_string(normalized->task_id));
        if (json_object_object_get_ex(request_message, "contextId",
                                      &request_context) &&
            json_object_get_type(request_context) == json_type_string &&
            json_object_get_string(request_context)[0] != '\0') {
            json_object_object_add(message, "contextId",
                                   json_object_get(request_context));
        } else {
            json_object_object_add(message, "contextId",
                                   json_object_new_string(
                                       normalized->external_id));
        }
        json_object_object_add(message, "role",
                               json_object_new_string("ROLE_AGENT"));
        if (body != NULL && body_length > 0U &&
            content_type_is_json(content_type)) {
            parsed = parse_json_exact(body, body_length);
        }
        if (parsed != NULL) {
            json_object_object_add(part, "data", parsed);
        } else {
            json_object_object_add(
                part, "text", json_object_new_string_len(
                    body != NULL ? body : "", (int)body_length));
        }
        json_object_object_add(part, "mediaType", json_object_new_string(
            content_type != NULL && content_type[0] != '\0'
                ? content_type : "application/octet-stream"));
        json_object_array_add(parts, part);
        json_object_object_add(message, "parts", parts);
        json_object_object_add(root, "message", message);
        return root;
    }
}

struct json_object *adapter_codec_map_response(
    const struct adapter_normalized_request *normalized,
    int gateway_status,
    const char *gateway_content_type,
    const char *body,
    size_t body_length
)
{
    if (normalized == NULL || normalized->request == NULL ||
        gateway_status < 100 || gateway_status > 599 || body_length > INT_MAX) {
        return NULL;
    }
    if (normalized->protocol == AGENT_ADAPTER_PROTOCOL_MCP) {
        return mcp_response(normalized, gateway_status,
                            gateway_content_type, body, body_length);
    }
    if (normalized->protocol == AGENT_ADAPTER_PROTOCOL_A2A) {
        return a2a_response(normalized, gateway_status,
                            gateway_content_type, body, body_length);
    }
    return NULL;
}

const char *adapter_codec_response_content_type(
    enum agent_adapter_protocol protocol
)
{
    return protocol == AGENT_ADAPTER_PROTOCOL_A2A
        ? "application/a2a+json; charset=utf-8"
        : "application/json; charset=utf-8";
}

void adapter_codec_request_clear(
    struct adapter_normalized_request *normalized
)
{
    if (normalized == NULL) {
        return;
    }
    json_object_put(normalized->envelope);
    json_object_put(normalized->request);
    memset(normalized, 0, sizeof(*normalized));
}
