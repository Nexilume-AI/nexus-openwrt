#include "agent_sse_contract.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct capture {
    char data[512];
    size_t length;
    unsigned int events;
};

static bool capture_event(const char *event, size_t length, void *context)
{
    struct capture *capture = context;

    if (capture->length + length > sizeof(capture->data)) return false;
    memcpy(capture->data + capture->length, event, length);
    capture->length += length;
    capture->events++;
    return true;
}

static void test_heads(void)
{
    struct agent_sse_response_head head;
    const char valid[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    const char fixed[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: 10\r\n\r\n";
    const char close_delimited[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n";

    assert(agent_sse_parse_response_head(
        valid, sizeof(valid) - 1U, &head) == AGENT_SSE_HEAD_OK);
    assert(head.header_length == sizeof(valid) - 1U);
    assert(head.chunked && head.event_stream);
    assert(agent_sse_response_is_streamable(&head));
    assert(agent_sse_parse_response_head(
        valid, 16U, &head) == AGENT_SSE_HEAD_INCOMPLETE);
    assert(agent_sse_parse_response_head(
        fixed, sizeof(fixed) - 1U, &head) == AGENT_SSE_HEAD_OK);
    assert(!agent_sse_response_is_streamable(&head));
    assert(agent_sse_parse_response_head(
        close_delimited, sizeof(close_delimited) - 1U,
        &head) == AGENT_SSE_HEAD_OK);
    assert(agent_sse_response_is_streamable(&head));
}

static void test_chunked_events(void)
{
    struct agent_sse_relay relay;
    struct capture capture = {0};
    char event_buffer[128];
    const char first[] = "B\r\ndata: one\n\n\r\n";
    const char second[] = "B\r\ndata: two\n\n\r\n0\r\n\r\n";

    agent_sse_relay_init(&relay, true, event_buffer, sizeof(event_buffer));
    assert(agent_sse_relay_feed(
        &relay, first, sizeof(first) - 1U, capture_event,
        &capture) == AGENT_SSE_RELAY_OK);
    assert(capture.events == 1U);
    assert(agent_sse_relay_feed(
        &relay, second, sizeof(second) - 1U, capture_event,
        &capture) == AGENT_SSE_RELAY_DONE);
    assert(capture.events == 2U);
    assert(relay.emitted_events == 2U);
    assert(memcmp(capture.data, "data: one\n\ndata: two\n\n",
                  capture.length) == 0);
    assert(agent_sse_relay_finish(&relay) == AGENT_SSE_RELAY_DONE);
}

static void test_invalid_and_bound(void)
{
    struct agent_sse_relay relay;
    struct capture capture = {0};
    char event_buffer[8];
    const char oversized[] = "data: too-long\n\n";
    const char bad_chunk[] = "Z\r\ndata\r\n";

    agent_sse_relay_init(&relay, false, event_buffer, sizeof(event_buffer));
    assert(agent_sse_relay_feed(
        &relay, oversized, sizeof(oversized) - 1U, capture_event,
        &capture) == AGENT_SSE_RELAY_TOO_LARGE);
    agent_sse_relay_init(&relay, true, event_buffer, sizeof(event_buffer));
    assert(agent_sse_relay_feed(
        &relay, bad_chunk, sizeof(bad_chunk) - 1U, capture_event,
        &capture) == AGENT_SSE_RELAY_INVALID);
}

static void test_completion_comment_is_relayed(void)
{
    struct agent_sse_relay relay;
    struct capture capture = {0};
    char event_buffer[128];
    const char input[] = ": nexus-stream-complete\n\n";

    agent_sse_relay_init(&relay, false, event_buffer, sizeof(event_buffer));
    assert(agent_sse_relay_feed(
        &relay, input, sizeof(input) - 1U, capture_event,
        &capture) == AGENT_SSE_RELAY_OK);
    assert(agent_sse_relay_finish(&relay) == AGENT_SSE_RELAY_DONE);
    assert(capture.events == 1U);
    assert(capture.length == sizeof(input) - 1U);
    assert(memcmp(capture.data, input, sizeof(input) - 1U) == 0);
}

int main(void)
{
    test_heads();
    test_chunked_events();
    test_invalid_and_bound();
    test_completion_comment_is_relayed();
    puts("SSE stream contract tests passed");
    return 0;
}
