#include "adapter_registry.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_REGISTRY_PACKAGE "agent_adapter"
#define DEFAULT_MAX_REQUEST_BYTES 65536U
#define MAX_REQUEST_BYTES 1048576U

struct map_options {
    const char *registry_package;
    const char *config_dir;
    enum agent_adapter_protocol protocol;
    const char *authority;
    const char *selector;
    const char *tenant;
    const char *source_agent;
    const char *deadline;
    const char *region;
    uint32_t hop_limit;
    uint32_t max_request_bytes;
    bool validate_only;
};

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: agent-adapter-map -P mcp|a2a -a AUTHORITY -s SELECTOR "
            "-t TENANT -u SOURCE_AGENT [options]\n"
            "       agent-adapter-map -V [-c UCI_PACKAGE] [-D UCI_DIR]\n"
            "options: -c package -D config-dir -H hop-limit -d UTC-deadline -r region "
            "-b max-bytes\n");
}

static bool parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] < '0' || text[0] > '9') {
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

static bool parse_options(int argc, char **argv, struct map_options *options)
{
    int option;

    memset(options, 0, sizeof(*options));
    options->registry_package = DEFAULT_REGISTRY_PACKAGE;
    options->hop_limit = 8U;
    options->max_request_bytes = DEFAULT_MAX_REQUEST_BYTES;
    options->region = "local";
    while ((option = getopt(argc, argv, "P:a:s:t:u:c:D:H:d:r:b:Vh")) != -1) {
        switch (option) {
        case 'P':
            options->protocol = agent_adapter_protocol_parse(optarg);
            break;
        case 'a': options->authority = optarg; break;
        case 's': options->selector = optarg; break;
        case 't': options->tenant = optarg; break;
        case 'u': options->source_agent = optarg; break;
        case 'c': options->registry_package = optarg; break;
        case 'D': options->config_dir = optarg; break;
        case 'H':
            if (!parse_u32(optarg, &options->hop_limit)) return false;
            break;
        case 'd': options->deadline = optarg; break;
        case 'r': options->region = optarg; break;
        case 'b':
            if (!parse_u32(optarg, &options->max_request_bytes)) return false;
            break;
        case 'V': options->validate_only = true; break;
        case 'h': usage(stdout); exit(0);
        default: return false;
        }
    }
    if (optind != argc || options->registry_package[0] == '\0' ||
        options->max_request_bytes == 0U ||
        options->max_request_bytes > MAX_REQUEST_BYTES) {
        return false;
    }
    if (options->validate_only) {
        return true;
    }
    return options->protocol != AGENT_ADAPTER_PROTOCOL_INVALID &&
           options->authority != NULL && options->selector != NULL &&
           options->tenant != NULL && options->source_agent != NULL &&
           options->region != NULL && options->hop_limit >= 2U &&
           options->hop_limit <= 255U;
}

static char *read_request(uint32_t maximum, size_t *length)
{
    char *buffer = malloc((size_t)maximum + 1U);
    size_t used = 0U;

    if (buffer == NULL) {
        return NULL;
    }
    while (used < maximum) {
        size_t count = fread(buffer + used, 1U, (size_t)maximum - used, stdin);

        used += count;
        if (count == 0U) {
            if (ferror(stdin)) {
                free(buffer);
                return NULL;
            }
            break;
        }
    }
    if (used == maximum && fgetc(stdin) != EOF) {
        free(buffer);
        errno = EFBIG;
        return NULL;
    }
    if (used == 0U) {
        free(buffer);
        errno = EINVAL;
        return NULL;
    }
    buffer[used] = '\0';
    *length = used;
    return buffer;
}

static struct json_object *parse_json_exact(const char *text, size_t length)
{
    struct json_tokener *tokener = json_tokener_new_ex(32);
    struct json_object *root;
    enum json_tokener_error error;
    size_t offset;

    if (tokener == NULL || length > INT_MAX) {
        if (tokener != NULL) {
            json_tokener_free(tokener);
        }
        return NULL;
    }
    json_tokener_set_flags(tokener,
                           JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tokener, text, (int)length);
    error = json_tokener_get_error(tokener);
    offset = json_tokener_get_parse_end(tokener);
    while (offset < length &&
           (text[offset] == ' ' || text[offset] == '\t' ||
            text[offset] == '\r' || text[offset] == '\n')) {
        offset++;
    }
    if (error != json_tokener_success || root == NULL || offset != length ||
        json_object_get_type(root) != json_type_object) {
        json_object_put(root);
        root = NULL;
    }
    json_tokener_free(tokener);
    return root;
}

static bool json_string(
    struct json_object *object,
    const char *name,
    const char **value
)
{
    struct json_object *field;

    return json_object_object_get_ex(object, name, &field) &&
           json_object_get_type(field) == json_type_string &&
           (*value = json_object_get_string(field))[0] != '\0';
}

static bool mcp_request_id(
    struct json_object *root,
    const char *selector,
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

    if (!json_string(root, "jsonrpc", &jsonrpc) || strcmp(jsonrpc, "2.0") != 0 ||
        !json_string(root, "method", &method) ||
        strcmp(method, "tools/call") != 0 ||
        !json_object_object_get_ex(root, "id", &id) ||
        !json_object_object_get_ex(root, "params", &params) ||
        json_object_get_type(params) != json_type_object ||
        !json_string(params, "name", &name) || strcmp(name, selector) != 0) {
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

static bool a2a_message_id(
    struct json_object *root,
    char external_id[AGENT_ADAPTER_TASK_ID_LEN]
)
{
    struct json_object *message;
    const char *role;
    const char *message_id;
    int written;

    if (!json_object_object_get_ex(root, "message", &message) ||
        json_object_get_type(message) != json_type_object ||
        !json_string(message, "role", &role) || strcmp(role, "ROLE_USER") != 0 ||
        !json_string(message, "messageId", &message_id)) {
        return false;
    }
    written = snprintf(external_id, AGENT_ADAPTER_TASK_ID_LEN, "%s", message_id);
    return written > 0 && (size_t)written < AGENT_ADAPTER_TASK_ID_LEN;
}

static struct json_object *build_envelope(
    const struct map_options *options,
    const struct agent_adapter_mapping *mapping,
    const char *task_id,
    struct json_object *request
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
    json_object_object_add(root, "task_id", json_object_new_string(task_id));
    json_object_object_add(root, "tenant",
                           json_object_new_string(options->tenant));
    json_object_object_add(root, "source_agent",
                           json_object_new_string(options->source_agent));
    json_object_object_add(root, "hop_limit",
                           json_object_new_uint64(options->hop_limit));
    if (options->deadline != NULL) {
        json_object_object_add(root, "deadline",
                               json_object_new_string(options->deadline));
    }
    json_object_object_add(flags, "idempotent", json_object_new_boolean(false));
    json_object_object_add(flags, "allow_retry", json_object_new_boolean(false));
    json_object_object_add(root, "flags", flags);
    json_object_object_add(constraints, "region",
                           json_object_new_string(options->region));
    json_object_object_add(root, "constraints", constraints);
    json_object_object_add(payload, "protocol",
                           json_object_new_string(
                               agent_adapter_protocol_name(options->protocol)));
    json_object_object_add(payload, "authority",
                           json_object_new_string(options->authority));
    json_object_object_add(payload, "selector",
                           json_object_new_string(options->selector));
    json_object_object_add(payload, "request", json_object_get(request));
    json_object_object_add(root, "payload", payload);
    return root;
}

int main(int argc, char **argv)
{
    struct map_options options;
    struct adapter_registry registry;
    struct adapter_registry_load_result load_result;
    const struct agent_adapter_mapping *mapping = NULL;
    enum agent_adapter_result lookup_result;
    struct json_object *request = NULL;
    struct json_object *envelope = NULL;
    char *request_text = NULL;
    char external_id[AGENT_ADAPTER_TASK_ID_LEN];
    char task_id[AGENT_ADAPTER_TASK_ID_LEN];
    size_t request_length = 0U;
    int exit_code = 1;

    if (!parse_options(argc, argv, &options)) {
        usage(stderr);
        return 2;
    }
    if (!adapter_registry_load(options.registry_package, options.config_dir,
                               &registry,
                               &load_result)) {
        fprintf(stderr, "registry validation failed: %s\n", load_result.error);
        return 3;
    }
    if (options.validate_only) {
        printf("validated_mappings=%zu\n", registry.count);
        return 0;
    }
    lookup_result = agent_adapter_lookup(
        registry.mappings, registry.count, options.protocol,
        options.authority, options.selector, &mapping);
    if (lookup_result != AGENT_ADAPTER_OK) {
        fprintf(stderr, "no unique adapter mapping for protocol authority selector\n");
        return 4;
    }
    request_text = read_request(options.max_request_bytes, &request_length);
    if (request_text == NULL) {
        fprintf(stderr, "protocol request is empty, unreadable or too large\n");
        goto done;
    }
    request = parse_json_exact(request_text, request_length);
    if (request == NULL) {
        fprintf(stderr, "protocol request is not one bounded JSON object\n");
        goto done;
    }
    memset(external_id, 0, sizeof(external_id));
    if ((options.protocol == AGENT_ADAPTER_PROTOCOL_MCP &&
         !mcp_request_id(request, options.selector, external_id)) ||
        (options.protocol == AGENT_ADAPTER_PROTOCOL_A2A &&
         !a2a_message_id(request, external_id)) ||
        !agent_adapter_make_task_id(options.protocol, external_id, task_id)) {
        fprintf(stderr, "protocol request does not match the selected operation\n");
        goto done;
    }
    envelope = build_envelope(&options, mapping, task_id, request);
    if (envelope == NULL) {
        fprintf(stderr, "failed to allocate Envelope\n");
        goto done;
    }
    printf("%s\n", json_object_to_json_string_ext(
               envelope, JSON_C_TO_STRING_PLAIN));
    exit_code = 0;

done:
    json_object_put(envelope);
    json_object_put(request);
    free(request_text);
    return exit_code;
}
