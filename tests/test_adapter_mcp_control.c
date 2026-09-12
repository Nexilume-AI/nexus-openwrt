#include "adapter_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_text(char *target, size_t capacity, const char *source)
{
    int written = snprintf(target, capacity, "%s", source);

    assert(written > 0 && (size_t)written < capacity);
}

static void add_mapping(
    struct adapter_registry *registry,
    const char *authority,
    const char *selector,
    const char *schema
)
{
    struct agent_adapter_mapping *mapping =
        &registry->mappings[registry->count++];

    memset(mapping, 0, sizeof(*mapping));
    mapping->enabled = true;
    mapping->protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    mapping->intent_version = 1U;
    set_text(mapping->authority, sizeof(mapping->authority), authority);
    set_text(mapping->selector, sizeof(mapping->selector), selector);
    set_text(mapping->intent, sizeof(mapping->intent),
             "nexus.e2e.edge_probe");
    set_text(mapping->title, sizeof(mapping->title), "Edge probe");
    set_text(mapping->description, sizeof(mapping->description),
             "Proves execution on the terminal Agent.");
    if (schema != NULL) {
        set_text(mapping->input_schema_json,
                 sizeof(mapping->input_schema_json), schema);
    }
}

static void test_control_methods(void)
{
    struct adapter_registry registry = {0};
    struct json_object *response = NULL;
    struct json_object *result;
    struct json_object *tools;
    struct json_object *tool;
    struct json_object *schema;
    const char initialize[] =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    const char list[] =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}";
    const char initialized[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    const char call[] =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"edge_probe\",\"arguments\":{}}}";

    add_mapping(&registry, "agent://tenant/edge", "edge_probe",
                "{\"type\":\"object\",\"required\":[\"nonce\"]}");
    add_mapping(&registry, "agent://other/edge", "hidden", NULL);

    assert(adapter_codec_mcp_control(
               &registry, "agent://tenant/edge", initialize,
               sizeof(initialize) - 1U, &response) ==
           ADAPTER_MCP_CONTROL_HANDLED);
    assert(json_object_object_get_ex(response, "result", &result));
    assert(json_object_object_get_ex(result, "serverInfo", &tool));
    json_object_put(response);

    response = NULL;
    assert(adapter_codec_mcp_control(
               &registry, "agent://tenant/edge", list,
               sizeof(list) - 1U, &response) ==
           ADAPTER_MCP_CONTROL_HANDLED);
    assert(json_object_object_get_ex(response, "result", &result));
    assert(json_object_object_get_ex(result, "tools", &tools));
    assert(json_object_array_length(tools) == 1U);
    tool = json_object_array_get_idx(tools, 0U);
    assert(strcmp(json_object_get_string(
                      json_object_object_get(tool, "name")),
                  "edge_probe") == 0);
    assert(json_object_object_get_ex(tool, "inputSchema", &schema));
    assert(json_object_object_get_ex(schema, "required", &result));
    json_object_put(response);

    response = NULL;
    assert(adapter_codec_mcp_control(
               &registry, "agent://tenant/edge", initialized,
               sizeof(initialized) - 1U, &response) ==
           ADAPTER_MCP_CONTROL_NOTIFICATION);
    assert(response == NULL);
    assert(adapter_codec_mcp_control(
               &registry, "agent://tenant/edge", call,
               sizeof(call) - 1U, &response) ==
           ADAPTER_MCP_CONTROL_NOT_CONTROL);
}

static void test_call_envelope_pins_target(void)
{
    struct adapter_registry registry = {0};
    struct adapter_codec_options options = {0};
    struct adapter_normalized_request normalized;
    struct json_object *target;
    struct json_object *constraints;
    struct json_object *region;
    struct json_object *flags;
    struct json_object *interactive;
    const char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"req-1\",\"method\":\"tools/call\",\"params\":{\"name\":\"edge_probe\",\"arguments\":{\"nonce\":\"abc\"}}}";

    add_mapping(&registry, "agent://tenant/edge", "edge_probe", NULL);
    options.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    options.authority = "agent://tenant/edge";
    options.tenant = "tenant";
    options.source_agent = "service://nexus-server";
    options.region = "edge";
    options.hop_limit = 8U;
    assert(adapter_codec_normalize(
               &registry, &options, body, sizeof(body) - 1U, &normalized) ==
           ADAPTER_CODEC_OK);
    assert(json_object_object_get_ex(normalized.envelope,
                                     "target_agent", &target));
    assert(strcmp(json_object_get_string(target), options.authority) == 0);
    assert(json_object_object_get_ex(normalized.envelope,
                                     "constraints", &constraints));
    assert(json_object_object_get_ex(constraints, "region", &region));
    assert(strcmp(json_object_get_string(region), "edge") == 0);
    assert(strcmp(normalized.task_id, "mcp:req-1") == 0);
    assert(!normalized.interactive);
    assert(json_object_object_get_ex(normalized.envelope, "flags", &flags));
    assert(json_object_object_get_ex(flags, "interactive", &interactive));
    assert(!json_object_get_boolean(interactive));
    adapter_codec_request_clear(&normalized);

    registry.local_dynamic[0] = true;
    registry.mappings[0].task = true;
    registry.mappings[0].interactive = true;
    assert(adapter_codec_normalize(
               &registry, &options, body, sizeof(body) - 1U, &normalized) ==
           ADAPTER_CODEC_OK);
    assert(json_object_object_get_ex(normalized.envelope,
                                     "constraints", &constraints));
    assert(json_object_object_get_ex(constraints, "region", &region));
    assert(strcmp(json_object_get_string(region), "local") == 0);
    assert(normalized.interactive);
    assert(json_object_object_get_ex(normalized.envelope, "flags", &flags));
    assert(json_object_object_get_ex(flags, "interactive", &interactive));
    assert(json_object_get_boolean(interactive));
    adapter_codec_request_clear(&normalized);
}

static void test_unicode_call_envelope_is_valid_utf8(void)
{
    struct adapter_registry registry = {0};
    struct adapter_codec_options options = {0};
    struct adapter_normalized_request normalized;
    const char *serialized = NULL;
    size_t serialized_length = 0U;
    struct json_object *root;
    struct json_object *payload;
    struct json_object *request;
    struct json_object *params;
    struct json_object *arguments;
    struct json_object *message;
    const char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"unicode-1\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"edge_probe\",\"arguments\":{\"message\":\"中文老师🙂\"}}}";

    add_mapping(&registry, "agent://tenant/edge", "edge_probe", NULL);
    options.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    options.authority = "agent://tenant/edge";
    options.tenant = "tenant";
    options.source_agent = "service://nexus-server";
    options.region = "edge";
    options.hop_limit = 8U;
    assert(adapter_codec_normalize(
               &registry, &options, body, sizeof(body) - 1U, &normalized) ==
           ADAPTER_CODEC_OK);
    assert(adapter_codec_serialize_envelope(
        normalized.envelope, &serialized, &serialized_length));
    assert(serialized_length == strlen(serialized));
    root = json_tokener_parse(serialized);
    assert(root != NULL);
    assert(json_object_object_get_ex(root, "payload", &payload));
    assert(json_object_object_get_ex(payload, "request", &request));
    assert(json_object_object_get_ex(request, "params", &params));
    assert(json_object_object_get_ex(params, "arguments", &arguments));
    assert(json_object_object_get_ex(arguments, "message", &message));
    assert(strcmp(json_object_get_string(message), "中文老师🙂") == 0);
    json_object_put(root);
    adapter_codec_request_clear(&normalized);
}

static void test_invalid_utf8_call_is_rejected(void)
{
    struct adapter_registry registry = {0};
    struct adapter_codec_options options = {0};
    struct adapter_normalized_request normalized;
    const char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"invalid-utf8\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"edge_probe\",\"arguments\":{\"message\":\"\xc0\xaf\"}}}";

    add_mapping(&registry, "agent://tenant/edge", "edge_probe", NULL);
    options.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    options.authority = "agent://tenant/edge";
    options.tenant = "tenant";
    options.source_agent = "service://nexus-server";
    options.region = "edge";
    options.hop_limit = 8U;
    assert(adapter_codec_normalize(
               &registry, &options, body, sizeof(body) - 1U, &normalized) ==
           ADAPTER_CODEC_INVALID_JSON);
}

static void test_nonstandard_json_is_rejected(void)
{
    struct adapter_registry registry = {0};
    struct adapter_codec_options options = {0};
    struct adapter_normalized_request normalized;
    const char *invalid[] = {
        "{/*comment*/\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"edge_probe\",\"arguments\":{}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"edge_probe\",\"arguments\":{},}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"edge_probe\",\"arguments\":{\"value\":NaN}}}"
    };
    size_t index;

    add_mapping(&registry, "agent://tenant/edge", "edge_probe", NULL);
    options.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    options.authority = "agent://tenant/edge";
    options.tenant = "tenant";
    options.source_agent = "service://nexus-server";
    options.region = "edge";
    options.hop_limit = 8U;
    for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        assert(adapter_codec_normalize(
                   &registry, &options, invalid[index], strlen(invalid[index]),
                   &normalized) == ADAPTER_CODEC_INVALID_JSON);
    }
}

int main(void)
{
    test_control_methods();
    test_call_envelope_pins_target();
    test_unicode_call_envelope_is_valid_utf8();
    test_invalid_utf8_call_is_rejected();
    test_nonstandard_json_is_rejected();
    return 0;
}
