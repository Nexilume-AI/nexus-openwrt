#include "agent_sse_contract.h"

#include <limits.h>
#include <string.h>

static bool ascii_equal_case(
    const char *left,
    size_t left_length,
    const char *right
)
{
    size_t index;

    if (strlen(right) != left_length) {
        return false;
    }
    for (index = 0U; index < left_length; index++) {
        char a = left[index];
        char b = right[index];

        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static const char *find_header_end(const char *buffer, size_t length)
{
    size_t index;

    for (index = 0U; index + 3U < length; index++) {
        if (memcmp(buffer + index, "\r\n\r\n", 4U) == 0) {
            return buffer + index + 4U;
        }
    }
    return NULL;
}

static bool parse_status(const char *line, size_t length, int *status)
{
    if (length < 12U ||
        (memcmp(line, "HTTP/1.1 ", 9U) != 0 &&
         memcmp(line, "HTTP/1.0 ", 9U) != 0) ||
        line[9] < '1' || line[9] > '5' ||
        line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9' ||
        (length > 12U && line[12] != ' ')) {
        return false;
    }
    *status = (line[9] - '0') * 100 +
        (line[10] - '0') * 10 + (line[11] - '0');
    return true;
}

static void trim_value(const char **start, const char **end)
{
    while (*start < *end && (**start == ' ' || **start == '\t')) (*start)++;
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t')) (*end)--;
}

static bool parse_size(const char *start, const char *end, size_t *value)
{
    size_t parsed = 0U;

    if (start == end) return false;
    while (start < end) {
        size_t digit;

        if (*start < '0' || *start > '9') return false;
        digit = (size_t)(*start++ - '0');
        if (parsed > (SIZE_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

static bool value_has_token(
    const char *start,
    const char *end,
    const char *token
)
{
    while (start < end) {
        const char *token_end = start;

        while (token_end < end && *token_end != ',') token_end++;
        trim_value(&start, &token_end);
        if (ascii_equal_case(start, (size_t)(token_end - start), token)) {
            return true;
        }
        start = token_end < end ? token_end + 1U : end;
    }
    return false;
}

enum agent_sse_head_result agent_sse_parse_response_head(
    const char *buffer,
    size_t length,
    struct agent_sse_response_head *head
)
{
    const char *header_end;
    const char *cursor;
    const char *line_end;
    bool have_content_type = false;
    bool have_transfer_encoding = false;
    bool have_content_length = false;

    if (buffer == NULL || head == NULL) return AGENT_SSE_HEAD_INVALID;
    memset(head, 0, sizeof(*head));
    header_end = find_header_end(buffer, length);
    if (header_end == NULL) {
        return length > AGENT_SSE_MAX_HEADER_BYTES
            ? AGENT_SSE_HEAD_TOO_LARGE : AGENT_SSE_HEAD_INCOMPLETE;
    }
    head->header_length = (size_t)(header_end - buffer);
    if (head->header_length > AGENT_SSE_MAX_HEADER_BYTES) {
        return AGENT_SSE_HEAD_TOO_LARGE;
    }
    line_end = strstr(buffer, "\r\n");
    if (line_end == NULL || line_end >= header_end ||
        !parse_status(buffer, (size_t)(line_end - buffer), &head->status)) {
        return AGENT_SSE_HEAD_INVALID;
    }
    cursor = line_end + 2U;
    while (cursor < header_end - 2U) {
        const char *colon;
        const char *value_start;
        const char *value_end;

        line_end = strstr(cursor, "\r\n");
        if (line_end == NULL || line_end > header_end || line_end == cursor) {
            return AGENT_SSE_HEAD_INVALID;
        }
        colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon == NULL || colon == cursor) return AGENT_SSE_HEAD_INVALID;
        value_start = colon + 1U;
        value_end = line_end;
        trim_value(&value_start, &value_end);
        if (ascii_equal_case(cursor, (size_t)(colon - cursor),
                             "Content-Type")) {
            static const char sse[] = "text/event-stream";
            if (have_content_type) return AGENT_SSE_HEAD_INVALID;
            have_content_type = true;
            head->event_stream =
                (size_t)(value_end - value_start) >= sizeof(sse) - 1U &&
                ascii_equal_case(value_start, sizeof(sse) - 1U, sse) &&
                ((size_t)(value_end - value_start) == sizeof(sse) - 1U ||
                 value_start[sizeof(sse) - 1U] == ';');
        } else if (ascii_equal_case(cursor, (size_t)(colon - cursor),
                                    "Transfer-Encoding")) {
            if (have_transfer_encoding || value_start == value_end ||
                !ascii_equal_case(value_start,
                                  (size_t)(value_end - value_start),
                                  "chunked")) {
                return AGENT_SSE_HEAD_INVALID;
            }
            have_transfer_encoding = true;
            head->chunked = true;
        } else if (ascii_equal_case(cursor, (size_t)(colon - cursor),
                                    "Content-Length")) {
            if (have_content_length ||
                !parse_size(value_start, value_end, &head->content_length)) {
                return AGENT_SSE_HEAD_INVALID;
            }
            have_content_length = true;
            head->content_length_present = true;
        } else if (ascii_equal_case(cursor, (size_t)(colon - cursor),
                                    "Connection")) {
            head->connection_close = value_has_token(
                value_start, value_end, "close");
        }
        cursor = line_end + 2U;
    }
    if (head->chunked && head->content_length_present) {
        return AGENT_SSE_HEAD_INVALID;
    }
    return AGENT_SSE_HEAD_OK;
}

bool agent_sse_response_is_streamable(
    const struct agent_sse_response_head *head
)
{
    return head != NULL && head->status >= 200 && head->status < 300 &&
        head->event_stream && !head->content_length_present &&
        (head->chunked || head->connection_close);
}

void agent_sse_relay_init(
    struct agent_sse_relay *relay,
    bool chunked,
    char *event_buffer,
    size_t event_capacity
)
{
    memset(relay, 0, sizeof(*relay));
    relay->chunked = chunked;
    relay->chunk_phase = chunked
        ? AGENT_SSE_CHUNK_SIZE : AGENT_SSE_CHUNK_DATA;
    relay->event_buffer = event_buffer;
    relay->event_capacity = event_capacity;
}

static enum agent_sse_relay_result emit_byte(
    struct agent_sse_relay *relay,
    unsigned char byte,
    agent_sse_emit_fn emit,
    void *context
)
{
    bool blank_line;
    bool cr_ending = false;

    if (byte == 0U) return AGENT_SSE_RELAY_INVALID;
    if (relay->event_length >= relay->event_capacity) {
        return AGENT_SSE_RELAY_TOO_LARGE;
    }
    relay->event_buffer[relay->event_length++] = (char)byte;
    if (relay->pending_cr) {
        if (byte != '\n') return AGENT_SSE_RELAY_INVALID;
        blank_line = relay->line_length == 0U;
        cr_ending = true;
        relay->pending_cr = false;
    } else if (byte == '\r') {
        relay->pending_cr = true;
        return AGENT_SSE_RELAY_OK;
    } else if (byte == '\n') {
        blank_line = relay->line_length == 0U;
    } else {
        relay->line_length++;
        return AGENT_SSE_RELAY_OK;
    }
    relay->line_length = 0U;
    if (!blank_line) return AGENT_SSE_RELAY_OK;
    if (relay->event_length > (cr_ending ? 2U : 1U)) {
        if (!emit(relay->event_buffer, relay->event_length, context)) {
            return AGENT_SSE_RELAY_EMIT_FAILED;
        }
        relay->emitted_events++;
        relay->emitted_bytes += relay->event_length;
    }
    relay->event_length = 0U;
    return AGENT_SSE_RELAY_OK;
}

static bool parse_chunk_size(
    const char *line,
    size_t length,
    uint64_t *value
)
{
    size_t index;
    uint64_t parsed = 0U;

    if (length == 0U) return false;
    for (index = 0U; index < length; index++) {
        unsigned int digit;
        char c = line[index];

        if (c >= '0' && c <= '9') digit = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned int)(c - 'A' + 10);
        else return false;
        if (parsed > (UINT64_MAX - digit) / 16U) return false;
        parsed = parsed * 16U + digit;
    }
    *value = parsed;
    return true;
}

enum agent_sse_relay_result agent_sse_relay_feed(
    struct agent_sse_relay *relay,
    const char *input,
    size_t input_length,
    agent_sse_emit_fn emit,
    void *context
)
{
    size_t index = 0U;

    if (relay == NULL || input == NULL || emit == NULL ||
        relay->event_buffer == NULL || relay->event_capacity == 0U) {
        return AGENT_SSE_RELAY_INVALID;
    }
    if (!relay->chunked) {
        while (index < input_length) {
            enum agent_sse_relay_result result = emit_byte(
                relay, (unsigned char)input[index++], emit, context);
            if (result != AGENT_SSE_RELAY_OK) return result;
        }
        return AGENT_SSE_RELAY_OK;
    }
    while (index < input_length) {
        unsigned char byte = (unsigned char)input[index++];

        switch (relay->chunk_phase) {
        case AGENT_SSE_CHUNK_SIZE:
            if (byte == '\r') {
                if (!parse_chunk_size(relay->chunk_line,
                                      relay->chunk_line_length,
                                      &relay->chunk_remaining)) {
                    return AGENT_SSE_RELAY_INVALID;
                }
                relay->chunk_line_length = 0U;
                relay->chunk_phase = AGENT_SSE_CHUNK_SIZE_LF;
            } else if (relay->chunk_line_length >=
                       sizeof(relay->chunk_line)) {
                return AGENT_SSE_RELAY_INVALID;
            } else {
                relay->chunk_line[relay->chunk_line_length++] = (char)byte;
            }
            break;
        case AGENT_SSE_CHUNK_SIZE_LF:
            if (byte != '\n') return AGENT_SSE_RELAY_INVALID;
            relay->chunk_phase = relay->chunk_remaining == 0U
                ? AGENT_SSE_CHUNK_FINAL_CR : AGENT_SSE_CHUNK_DATA;
            break;
        case AGENT_SSE_CHUNK_DATA: {
            enum agent_sse_relay_result result = emit_byte(
                relay, byte, emit, context);
            if (result != AGENT_SSE_RELAY_OK) return result;
            relay->chunk_remaining--;
            if (relay->chunk_remaining == 0U) {
                relay->chunk_phase = AGENT_SSE_CHUNK_DATA_CR;
            }
            break;
        }
        case AGENT_SSE_CHUNK_DATA_CR:
            if (byte != '\r') return AGENT_SSE_RELAY_INVALID;
            relay->chunk_phase = AGENT_SSE_CHUNK_DATA_LF;
            break;
        case AGENT_SSE_CHUNK_DATA_LF:
            if (byte != '\n') return AGENT_SSE_RELAY_INVALID;
            relay->chunk_phase = AGENT_SSE_CHUNK_SIZE;
            break;
        case AGENT_SSE_CHUNK_FINAL_CR:
            if (byte != '\r') return AGENT_SSE_RELAY_INVALID;
            relay->chunk_phase = AGENT_SSE_CHUNK_FINAL_LF;
            break;
        case AGENT_SSE_CHUNK_FINAL_LF:
            if (byte != '\n' || index != input_length ||
                relay->event_length != 0U) {
                return AGENT_SSE_RELAY_INVALID;
            }
            relay->chunk_phase = AGENT_SSE_CHUNK_DONE;
            return AGENT_SSE_RELAY_DONE;
        case AGENT_SSE_CHUNK_DONE:
            return AGENT_SSE_RELAY_INVALID;
        }
    }
    return AGENT_SSE_RELAY_OK;
}

enum agent_sse_relay_result agent_sse_relay_finish(
    struct agent_sse_relay *relay
)
{
    if (relay == NULL || relay->pending_cr || relay->event_length != 0U) {
        return AGENT_SSE_RELAY_INVALID;
    }
    if (relay->chunked && relay->chunk_phase != AGENT_SSE_CHUNK_DONE) {
        return AGENT_SSE_RELAY_INVALID;
    }
    return AGENT_SSE_RELAY_DONE;
}
