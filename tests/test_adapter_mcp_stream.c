#include "adapter_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct adapter_normalized_request request(const char *meta)
{
    struct adapter_normalized_request n = {0};
    char body[512];
    n.protocol = AGENT_ADAPTER_PROTOCOL_MCP;
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"stages\"%s}}", meta);
    n.request = json_tokener_parse(body);
    assert(n.request != NULL);
    return n;
}

static bool event(struct adapter_normalized_request *n,
                  struct adapter_mcp_stream_state *s, const char *wire,
                  struct json_object **message)
{
    return adapter_codec_mcp_event(n, s, wire, strlen(wire), message);
}

static void test_native_results(void)
{
    struct adapter_normalized_request n = request("");
    struct json_object *m, *r, *v;
    struct adapter_mcp_stream_state s = {0};
    const char body[] = "{\"nexus_mcp_result_version\":1,\"result\":{\"content\":["
        "{\"type\":\"text\",\"text\":\"工具\"},"
        "{\"type\":\"image\",\"data\":\"YWJj\",\"mimeType\":\"image/png\"},"
        "{\"type\":\"resource\",\"resource\":{\"uri\":\"test://asset\",\"text\":\"body\"}}],"
        "\"isError\":true,\"structuredContent\":{\"marker\":\"native\"}}}";
    m = adapter_codec_map_response(&n, 200, "application/json", body, strlen(body));
    assert(m && json_object_object_get_ex(m, "result", &r));
    assert(json_object_object_get_ex(r, "content", &v) && json_object_array_length(v) == 3U);
    assert(json_object_get_boolean(json_object_object_get(r, "isError")));
    assert(json_object_get_int(json_object_object_get(m, "id")) == 2);
    assert(strcmp(json_object_get_string(json_object_object_get(
        json_object_object_get(r, "structuredContent"), "marker")), "native") == 0);
    json_object_put(m);
    assert(event(&n, &s, "event: result\ndata: {\"result\":{\"nexus_mcp_result_version\":1,"
        "\"result\":{\"content\":[],\"isError\":true}}}\n\n", &m));
    assert(json_object_get_boolean(json_object_object_get(json_object_object_get(m, "result"), "isError")));
    json_object_put(m);

    /* Business JSON that happens to contain MCP field names is unchanged. */
    const char business[] = "{\"content\":[],\"isError\":true}";
    m = adapter_codec_map_response(&n, 200, "application/json", business, strlen(business));
    assert(!json_object_get_boolean(json_object_object_get(json_object_object_get(m, "result"), "isError")));
    assert(json_object_object_get_ex(json_object_object_get(m, "result"), "structuredContent", &v));
    json_object_put(m);
    const char *invalid[] = {
        "{\"nexus_mcp_result_version\":2,\"result\":{\"content\":[]}}",
        "{\"nexus_mcp_result_version\":1,\"result\":{\"content\":[],\"isError\":\"false\"}}",
        "{\"nexus_mcp_result_version\":1,\"result\":{\"content\":[{\"type\":\"other\"}]}}",
        "{\"nexus_mcp_result_version\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":1}]}}",
        "{\"nexus_mcp_result_version\":1,\"result\":{\"content\":[],\"id\":\"forged\"}}",
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        m = adapter_codec_map_response(&n, 200, "application/json", invalid[i], strlen(invalid[i]));
        assert(m && json_object_object_get_ex(m, "error", &v));
        assert(json_object_get_int(json_object_object_get(m, "id")) == 2);
        json_object_put(m);
    }
    adapter_codec_request_clear(&n);
}

int main(void)
{
    test_native_results();
    struct adapter_normalized_request n = request(
        ",\"_meta\":{\"progressToken\":\"caller-token\"}");
    struct adapter_mcp_stream_state s = {0};
    struct json_object *m = NULL, *p, *v, *r;
    assert(event(&n, &s, ": heartbeat\n\n", &m) && m == NULL);
    assert(event(&n, &s,
        "event: progress\r\nid: 1\r\ndata: {\"progress\":1,\r\n"
        "data: \"total\":16,\"message\":\"工具完成\"}\r\n\r\n", &m));
    assert(json_object_object_get_ex(m, "method", &v));
    assert(strcmp(json_object_get_string(v), "notifications/progress") == 0);
    assert(json_object_object_get_ex(m, "params", &p));
    assert(json_object_object_get_ex(p, "progressToken", &v));
    assert(strcmp(json_object_get_string(v), "caller-token") == 0);
    assert(!json_object_object_get_ex(m, "id", &v));
    json_object_put(m);
    assert(!event(&n, &s, "event: progress\ndata: {\"progress\":1}\n\n", &m));
    assert(!event(&n, &s, "event: progress\ndata: {\"progress\":2,\"total\":\"bad\"}\n\n", &m));
    assert(event(&n, &s, "event: result\ndata: {\"result\":{\"answer\":42}}\n\n", &m));
    assert(s.finished);
    assert(json_object_object_get_ex(m, "id", &v) && json_object_get_int(v) == 2);
    assert(json_object_object_get_ex(m, "result", &r));
    assert(json_object_object_get_ex(r, "structuredContent", &p));
    assert(json_object_object_get_ex(p, "answer", &v) && json_object_get_int(v) == 42);
    json_object_put(m);
    assert(!event(&n, &s, "event: result\ndata: {\"result\":1}\n\n", &m));
    adapter_codec_request_clear(&n);

    n = request(""); memset(&s, 0, sizeof(s));
    assert(event(&n, &s, "event: progress\ndata: {\"progress\":1}\n\n", &m));
    assert(m == NULL && s.has_progress);
    assert(!event(&n, &s, "event: result\ndata: {}\n\n", &m));
    assert(!event(&n, &s, "event: request_input\ndata: {}\n\n", &m));
    assert(!event(&n, &s, "event: result\ndata: not-json\n\n", &m));
    assert(event(&n, &s, "event: error\ndata: {\"code\":\"FAILED\"}\n\n", &m));
    assert(s.finished && json_object_object_get_ex(m, "error", &v));
    json_object_put(m); adapter_codec_request_clear(&n);

    n = request(",\"_meta\":{\"progressToken\":0}"); memset(&s, 0, sizeof(s));
    assert(event(&n, &s, "event: progress\ndata: {\"progress\":0}\n\n", &m));
    assert(json_object_object_get_ex(m, "params", &p));
    assert(json_object_object_get_ex(p, "progressToken", &v));
    assert(json_object_get_type(v) == json_type_int && json_object_get_int(v) == 0);
    json_object_put(m); adapter_codec_request_clear(&n);
    puts("MCP stream mapping: progress tokens, UTF-8, multiline SSE, final/error and rejection cases passed");
    return 0;
}
