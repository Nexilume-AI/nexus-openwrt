#ifndef NEXUS_AGENT_SSE_CONTRACT_H
#define NEXUS_AGENT_SSE_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_SSE_MAX_HEADER_BYTES 8192U
#define AGENT_SSE_MAX_CHUNK_LINE 32U

enum agent_sse_head_result {
    AGENT_SSE_HEAD_INCOMPLETE = 0,
    AGENT_SSE_HEAD_OK,
    AGENT_SSE_HEAD_INVALID,
    AGENT_SSE_HEAD_TOO_LARGE
};

struct agent_sse_response_head {
    int status;
    size_t header_length;
    bool event_stream;
    bool chunked;
    bool connection_close;
    bool content_length_present;
    size_t content_length;
};

enum agent_sse_relay_result {
    AGENT_SSE_RELAY_OK = 0,
    AGENT_SSE_RELAY_DONE,
    AGENT_SSE_RELAY_INVALID,
    AGENT_SSE_RELAY_TOO_LARGE,
    AGENT_SSE_RELAY_EMIT_FAILED
};

typedef bool (*agent_sse_emit_fn)(
    const char *event,
    size_t event_length,
    void *context
);

enum agent_sse_chunk_phase {
    AGENT_SSE_CHUNK_SIZE = 0,
    AGENT_SSE_CHUNK_SIZE_LF,
    AGENT_SSE_CHUNK_DATA,
    AGENT_SSE_CHUNK_DATA_CR,
    AGENT_SSE_CHUNK_DATA_LF,
    AGENT_SSE_CHUNK_FINAL_CR,
    AGENT_SSE_CHUNK_FINAL_LF,
    AGENT_SSE_CHUNK_DONE
};

struct agent_sse_relay {
    bool chunked;
    enum agent_sse_chunk_phase chunk_phase;
    uint64_t chunk_remaining;
    char chunk_line[AGENT_SSE_MAX_CHUNK_LINE];
    size_t chunk_line_length;
    char *event_buffer;
    size_t event_capacity;
    size_t event_length;
    size_t line_length;
    bool pending_cr;
    uint64_t emitted_events;
    uint64_t emitted_bytes;
};

enum agent_sse_head_result agent_sse_parse_response_head(
    const char *buffer,
    size_t length,
    struct agent_sse_response_head *head
);

bool agent_sse_response_is_streamable(
    const struct agent_sse_response_head *head
);

void agent_sse_relay_init(
    struct agent_sse_relay *relay,
    bool chunked,
    char *event_buffer,
    size_t event_capacity
);

enum agent_sse_relay_result agent_sse_relay_feed(
    struct agent_sse_relay *relay,
    const char *input,
    size_t input_length,
    agent_sse_emit_fn emit,
    void *context
);

enum agent_sse_relay_result agent_sse_relay_finish(
    struct agent_sse_relay *relay
);

#endif
