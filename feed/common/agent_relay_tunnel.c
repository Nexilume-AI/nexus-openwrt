#include "agent_relay_tunnel.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

#define OPEN_FIXED_SIZE 16U
#define OPEN_FLAG_FORWARDING_ASSERTION 0x01U
#define OPEN_FLAG_TARGET_AGENT 0x02U

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static uint64_t read_u64(const uint8_t *data)
{
    uint64_t value = 0U;
    size_t i;

    for (i = 0U; i < 8U; i++) {
        value = (value << 8U) | data[i];
    }
    return value;
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static void write_u64(uint8_t *data, uint64_t value)
{
    size_t i;

    for (i = 0U; i < 8U; i++) {
        data[7U - i] = (uint8_t)(value >> (i * 8U));
    }
}

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length;

    if (text == NULL) {
        return capacity;
    }
    for (length = 0U; length < capacity; length++) {
        if (text[length] == '\0') {
            return length;
        }
    }
    return capacity;
}

static bool identifier_valid(const char *text, size_t capacity)
{
    size_t length = bounded_length(text, capacity);
    size_t i;

    if (length == 0U || length >= capacity) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        unsigned char value = (unsigned char)text[i];

        if (!isalnum(value) && value != '.' && value != '-' && value != '_' &&
            value != ':' && value != '/' && value != '@') {
            return false;
        }
    }
    return true;
}

static bool text_valid(const char *text, size_t capacity)
{
    size_t length = bounded_length(text, capacity);
    size_t i;

    if (length == 0U || length >= capacity) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        unsigned char value = (unsigned char)text[i];

        if (value < 0x21U || value > 0x7eU) {
            return false;
        }
    }
    return true;
}

static bool stream_type(enum agent_relay_tunnel_type type)
{
    return (type >= AGENT_RELAY_TUNNEL_OPEN &&
            type <= AGENT_RELAY_TUNNEL_WINDOW_UPDATE) ||
           type == AGENT_RELAY_TUNNEL_RESPONSE_START;
}

enum agent_relay_tunnel_result agent_relay_tunnel_message_validate(
    const struct agent_relay_tunnel_message *message
)
{
    if (message == NULL) {
        return AGENT_RELAY_TUNNEL_INVALID_ARGUMENT;
    }
    if (stream_type(message->type)) {
        if (message->stream_id == 0U || message->sequence == 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
    } else if ((message->type == AGENT_RELAY_TUNNEL_PING ||
                message->type == AGENT_RELAY_TUNNEL_PONG)) {
        if (message->stream_id != 0U || message->sequence == 0U ||
            message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        return AGENT_RELAY_TUNNEL_OK;
    } else {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }

    switch (message->type) {
    case AGENT_RELAY_TUNNEL_OPEN:
        if (message->sequence != 1U || message->hop_limit == 0U ||
            message->hop_limit > 32U || message->max_latency_ms == 0U ||
            !identifier_valid(message->target_router_id,
                              sizeof(message->target_router_id)) ||
            !text_valid(message->intent_class,
                        sizeof(message->intent_class)) ||
            !identifier_valid(message->task_id, sizeof(message->task_id)) ||
            !identifier_valid(message->source_agent,
                              sizeof(message->source_agent)) ||
            (message->target_agent[0] != '\0' &&
             !identifier_valid(message->target_agent,
                               sizeof(message->target_agent))) ||
            !identifier_valid(message->tenant, sizeof(message->tenant)) ||
            (!identifier_valid(message->region, sizeof(message->region)) &&
             strcmp(message->region, "*") != 0) ||
            (message->forwarding_assertion[0] != '\0' &&
             !text_valid(message->forwarding_assertion,
                         sizeof(message->forwarding_assertion))) ||
            message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    case AGENT_RELAY_TUNNEL_ACCEPT:
        if (message->sequence != 1U || message->status_code < 200U ||
            message->status_code > 599U ||
            message->credit_bytes < AGENT_RELAY_TUNNEL_MIN_WINDOW ||
            message->credit_bytes > AGENT_RELAY_TUNNEL_MAX_WINDOW ||
            message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    case AGENT_RELAY_TUNNEL_DATA:
        if (message->data_length == 0U ||
            message->data_length > AGENT_RELAY_TUNNEL_MAX_DATA) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    case AGENT_RELAY_TUNNEL_END:
        if (message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    case AGENT_RELAY_TUNNEL_RESET:
        if (message->reset_code == 0U || message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    case AGENT_RELAY_TUNNEL_WINDOW_UPDATE:
        if (message->credit_bytes == 0U ||
            message->credit_bytes > AGENT_RELAY_TUNNEL_MAX_WINDOW ||
            message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    case AGENT_RELAY_TUNNEL_RESPONSE_START:
        if (message->status_code != 200U || message->data_length != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        break;
    default:
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    return AGENT_RELAY_TUNNEL_OK;
}

static bool put_text(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *text,
    size_t text_capacity
)
{
    size_t length = bounded_length(text, text_capacity);

    if (length == 0U || length >= text_capacity || length > UINT16_MAX ||
        capacity - *offset < length + 2U) {
        return false;
    }
    write_u16(output + *offset, (uint16_t)length);
    *offset += 2U;
    memcpy(output + *offset, text, length);
    *offset += length;
    return true;
}

static bool get_text(
    const uint8_t *payload,
    size_t payload_size,
    size_t *offset,
    char *output,
    size_t capacity
)
{
    size_t length;

    if (payload_size - *offset < 2U) {
        return false;
    }
    length = read_u16(payload + *offset);
    *offset += 2U;
    if (length == 0U || length >= capacity ||
        payload_size - *offset < length) {
        return false;
    }
    memcpy(output, payload + *offset, length);
    output[length] = '\0';
    *offset += length;
    return true;
}

enum agent_relay_tunnel_result agent_relay_tunnel_frame_encode(
    const struct agent_relay_tunnel_message *message,
    uint8_t *output,
    size_t capacity,
    size_t *written
)
{
    enum agent_relay_tunnel_result result;
    size_t offset = AGENT_RELAY_TUNNEL_HEADER_SIZE;
    size_t payload_length = 0U;

    if (output == NULL || written == NULL) {
        return AGENT_RELAY_TUNNEL_INVALID_ARGUMENT;
    }
    *written = 0U;
    result = agent_relay_tunnel_message_validate(message);
    if (result != AGENT_RELAY_TUNNEL_OK) {
        return result;
    }
    if (capacity < AGENT_RELAY_TUNNEL_HEADER_SIZE) {
        return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
    }
    if (message->type == AGENT_RELAY_TUNNEL_OPEN) {
        if (capacity - offset < OPEN_FIXED_SIZE) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
        output[offset++] = message->hop_limit;
        output[offset++] = message->streaming ? 1U : 0U;
        output[offset++] =
            (message->forwarding_assertion[0] != '\0'
                 ? OPEN_FLAG_FORWARDING_ASSERTION : 0U) |
            (message->target_agent[0] != '\0'
                 ? OPEN_FLAG_TARGET_AGENT : 0U);
        output[offset++] = 0U;
        write_u64(output + offset, message->max_cost_microunits);
        offset += 8U;
        write_u32(output + offset, message->max_latency_ms);
        offset += 4U;
        if (!put_text(output, capacity, &offset, message->target_router_id,
                      sizeof(message->target_router_id)) ||
            !put_text(output, capacity, &offset, message->intent_class,
                      sizeof(message->intent_class)) ||
            !put_text(output, capacity, &offset, message->task_id,
                      sizeof(message->task_id)) ||
            !put_text(output, capacity, &offset, message->source_agent,
                      sizeof(message->source_agent)) ||
            !put_text(output, capacity, &offset, message->tenant,
                      sizeof(message->tenant)) ||
            !put_text(output, capacity, &offset, message->region,
                      sizeof(message->region)) ||
            (message->target_agent[0] != '\0' &&
             !put_text(output, capacity, &offset, message->target_agent,
                       sizeof(message->target_agent))) ||
            (message->forwarding_assertion[0] != '\0' &&
             !put_text(output, capacity, &offset,
                       message->forwarding_assertion,
                       sizeof(message->forwarding_assertion)))) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
    } else if (message->type == AGENT_RELAY_TUNNEL_ACCEPT) {
        if (capacity - offset < 6U) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
        write_u16(output + offset, message->status_code);
        write_u32(output + offset + 2U, message->credit_bytes);
        offset += 6U;
    } else if (message->type == AGENT_RELAY_TUNNEL_DATA) {
        if (capacity - offset < message->data_length) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
        memcpy(output + offset, message->data, message->data_length);
        offset += message->data_length;
    } else if (message->type == AGENT_RELAY_TUNNEL_RESET) {
        if (capacity - offset < 2U) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
        write_u16(output + offset, message->reset_code);
        offset += 2U;
    } else if (message->type == AGENT_RELAY_TUNNEL_WINDOW_UPDATE) {
        if (capacity - offset < 4U) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
        write_u32(output + offset, message->credit_bytes);
        offset += 4U;
    } else if (message->type == AGENT_RELAY_TUNNEL_RESPONSE_START) {
        if (capacity - offset < 2U) {
            return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
        }
        write_u16(output + offset, message->status_code);
        offset += 2U;
    }
    payload_length = offset - AGENT_RELAY_TUNNEL_HEADER_SIZE;
    if (payload_length > AGENT_RELAY_TUNNEL_MAX_DATA) {
        return AGENT_RELAY_TUNNEL_BUFFER_TOO_SMALL;
    }
    write_u32(output, AGENT_RELAY_TUNNEL_MAGIC);
    output[4] = AGENT_RELAY_TUNNEL_VERSION;
    output[5] = (uint8_t)message->type;
    output[6] = 0U;
    output[7] = 0U;
    write_u32(output + 8U, message->stream_id);
    write_u32(output + 12U, (uint32_t)payload_length);
    write_u64(output + 16U, message->sequence);
    *written = offset;
    return AGENT_RELAY_TUNNEL_OK;
}

enum agent_relay_tunnel_result agent_relay_tunnel_frame_decode(
    const uint8_t *frame,
    size_t frame_size,
    struct agent_relay_tunnel_message *message
)
{
    struct agent_relay_tunnel_message decoded;
    size_t payload_size;
    size_t offset = 0U;
    const uint8_t *payload;

    if (frame == NULL || message == NULL) {
        return AGENT_RELAY_TUNNEL_INVALID_ARGUMENT;
    }
    if (frame_size < AGENT_RELAY_TUNNEL_HEADER_SIZE ||
        frame_size > AGENT_RELAY_TUNNEL_MAX_FRAME_SIZE ||
        read_u32(frame) != AGENT_RELAY_TUNNEL_MAGIC ||
        frame[4] != AGENT_RELAY_TUNNEL_VERSION || frame[6] != 0U ||
        frame[7] != 0U) {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    payload_size = read_u32(frame + 12U);
    if (payload_size != frame_size - AGENT_RELAY_TUNNEL_HEADER_SIZE ||
        payload_size > AGENT_RELAY_TUNNEL_MAX_DATA) {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.type = (enum agent_relay_tunnel_type)frame[5];
    decoded.stream_id = read_u32(frame + 8U);
    decoded.sequence = read_u64(frame + 16U);
    payload = frame + AGENT_RELAY_TUNNEL_HEADER_SIZE;
    if (decoded.type == AGENT_RELAY_TUNNEL_OPEN) {
        if (payload_size < OPEN_FIXED_SIZE ||
            (payload[2] & ~(OPEN_FLAG_FORWARDING_ASSERTION |
                            OPEN_FLAG_TARGET_AGENT)) != 0U ||
            payload[3] != 0U || payload[1] > 1U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        decoded.hop_limit = payload[0];
        decoded.streaming = payload[1] == 1U;
        decoded.max_cost_microunits = read_u64(payload + 4U);
        decoded.max_latency_ms = read_u32(payload + 12U);
        offset = OPEN_FIXED_SIZE;
        if (!get_text(payload, payload_size, &offset,
                      decoded.target_router_id,
                      sizeof(decoded.target_router_id)) ||
            !get_text(payload, payload_size, &offset, decoded.intent_class,
                      sizeof(decoded.intent_class)) ||
            !get_text(payload, payload_size, &offset, decoded.task_id,
                      sizeof(decoded.task_id)) ||
            !get_text(payload, payload_size, &offset, decoded.source_agent,
                      sizeof(decoded.source_agent)) ||
            !get_text(payload, payload_size, &offset, decoded.tenant,
                      sizeof(decoded.tenant)) ||
            !get_text(payload, payload_size, &offset, decoded.region,
                      sizeof(decoded.region)) ||
            ((payload[2] & OPEN_FLAG_TARGET_AGENT) != 0U &&
             !get_text(payload, payload_size, &offset,
                       decoded.target_agent,
                       sizeof(decoded.target_agent))) ||
            ((payload[2] & OPEN_FLAG_FORWARDING_ASSERTION) != 0U &&
             !get_text(payload, payload_size, &offset,
                       decoded.forwarding_assertion,
                       sizeof(decoded.forwarding_assertion)))) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
    } else if (decoded.type == AGENT_RELAY_TUNNEL_ACCEPT) {
        if (payload_size != 6U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        decoded.status_code = read_u16(payload);
        decoded.credit_bytes = read_u32(payload + 2U);
        offset = payload_size;
    } else if (decoded.type == AGENT_RELAY_TUNNEL_DATA) {
        if (payload_size == 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        memcpy(decoded.data, payload, payload_size);
        decoded.data_length = payload_size;
        offset = payload_size;
    } else if (decoded.type == AGENT_RELAY_TUNNEL_RESET) {
        if (payload_size != 2U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        decoded.reset_code = read_u16(payload);
        offset = payload_size;
    } else if (decoded.type == AGENT_RELAY_TUNNEL_WINDOW_UPDATE) {
        if (payload_size != 4U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        decoded.credit_bytes = read_u32(payload);
        offset = payload_size;
    } else if (decoded.type == AGENT_RELAY_TUNNEL_RESPONSE_START) {
        if (payload_size != 2U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
        decoded.status_code = read_u16(payload);
        offset = payload_size;
    } else if (decoded.type == AGENT_RELAY_TUNNEL_END ||
               decoded.type == AGENT_RELAY_TUNNEL_PING ||
               decoded.type == AGENT_RELAY_TUNNEL_PONG) {
        if (payload_size != 0U) {
            return AGENT_RELAY_TUNNEL_INVALID_FRAME;
        }
    } else {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    if (offset != payload_size ||
        agent_relay_tunnel_message_validate(&decoded) !=
            AGENT_RELAY_TUNNEL_OK) {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    *message = decoded;
    return AGENT_RELAY_TUNNEL_OK;
}

void agent_relay_tunnel_ingress_init(
    struct agent_relay_tunnel_ingress *ingress
)
{
    if (ingress != NULL) {
        memset(ingress, 0, sizeof(*ingress));
    }
}

static size_t ingress_expected(const uint8_t *header)
{
    uint32_t payload;

    if (read_u32(header) != AGENT_RELAY_TUNNEL_MAGIC ||
        header[4] != AGENT_RELAY_TUNNEL_VERSION) {
        return 0U;
    }
    payload = read_u32(header + 12U);
    if (payload > AGENT_RELAY_TUNNEL_MAX_DATA) {
        return 0U;
    }
    return AGENT_RELAY_TUNNEL_HEADER_SIZE + (size_t)payload;
}

enum agent_relay_tunnel_result agent_relay_tunnel_ingress_feed(
    struct agent_relay_tunnel_ingress *ingress,
    const uint8_t *data,
    size_t size,
    agent_relay_tunnel_frame_callback callback,
    void *context,
    size_t *frames_emitted
)
{
    size_t copied;
    size_t emitted = 0U;

    if (frames_emitted != NULL) {
        *frames_emitted = 0U;
    }
    if (ingress == NULL || (data == NULL && size != 0U) ||
        callback == NULL || frames_emitted == NULL) {
        return AGENT_RELAY_TUNNEL_INVALID_ARGUMENT;
    }
    while (size > 0U) {
        if (ingress->expected == 0U &&
            ingress->used < AGENT_RELAY_TUNNEL_HEADER_SIZE) {
            copied = AGENT_RELAY_TUNNEL_HEADER_SIZE - ingress->used;
            if (copied > size) {
                copied = size;
            }
            memcpy(ingress->frame + ingress->used, data, copied);
            ingress->used += copied;
            data += copied;
            size -= copied;
            if (ingress->used < AGENT_RELAY_TUNNEL_HEADER_SIZE) {
                continue;
            }
            ingress->expected = ingress_expected(ingress->frame);
            if (ingress->expected == 0U) {
                agent_relay_tunnel_ingress_init(ingress);
                return AGENT_RELAY_TUNNEL_INVALID_FRAME;
            }
        }
        copied = ingress->expected - ingress->used;
        if (copied > size) {
            copied = size;
        }
        memcpy(ingress->frame + ingress->used, data, copied);
        ingress->used += copied;
        data += copied;
        size -= copied;
        if (ingress->used == ingress->expected) {
            if (!callback(ingress->frame, ingress->expected, context)) {
                agent_relay_tunnel_ingress_init(ingress);
                return AGENT_RELAY_TUNNEL_CALLBACK_ERROR;
            }
            emitted++;
            agent_relay_tunnel_ingress_init(ingress);
        }
    }
    *frames_emitted = emitted;
    return AGENT_RELAY_TUNNEL_OK;
}

bool agent_relay_mux_init(
    struct agent_relay_mux *mux,
    bool local_odd,
    size_t stream_limit,
    uint32_t initial_window
)
{
    if (mux == NULL || stream_limit == 0U ||
        stream_limit > AGENT_RELAY_TUNNEL_MAX_STREAMS ||
        initial_window < AGENT_RELAY_TUNNEL_MIN_WINDOW ||
        initial_window > AGENT_RELAY_TUNNEL_MAX_WINDOW) {
        return false;
    }
    memset(mux, 0, sizeof(*mux));
    mux->local_odd = local_odd;
    mux->next_stream_id = local_odd ? 1U : 2U;
    mux->stream_limit = stream_limit;
    mux->initial_window = initial_window;
    return true;
}

static struct agent_relay_mux_stream *find_stream(
    struct agent_relay_mux *mux,
    uint32_t stream_id
)
{
    size_t i;

    for (i = 0U; i < mux->stream_limit; i++) {
        if (mux->streams[i].state != AGENT_RELAY_MUX_STREAM_FREE &&
            mux->streams[i].stream_id == stream_id) {
            return &mux->streams[i];
        }
    }
    return NULL;
}

static struct agent_relay_mux_stream *allocate_stream(
    struct agent_relay_mux *mux,
    uint32_t stream_id,
    enum agent_relay_mux_stream_state state
)
{
    size_t i;

    if (mux->active_streams >= mux->stream_limit) {
        return NULL;
    }
    for (i = 0U; i < mux->stream_limit; i++) {
        struct agent_relay_mux_stream *stream = &mux->streams[i];

        if (stream->state == AGENT_RELAY_MUX_STREAM_FREE) {
            memset(stream, 0, sizeof(*stream));
            stream->state = state;
            stream->stream_id = stream_id;
            stream->next_send_sequence = 1U;
            stream->next_receive_sequence = 1U;
            stream->send_credit = mux->initial_window;
            stream->receive_credit = mux->initial_window;
            mux->active_streams++;
            return stream;
        }
    }
    return NULL;
}

static void release_stream(
    struct agent_relay_mux *mux,
    struct agent_relay_mux_stream *stream
)
{
    memset(stream, 0, sizeof(*stream));
    if (mux->active_streams > 0U) {
        mux->active_streams--;
    }
}

enum agent_relay_tunnel_result agent_relay_mux_open(
    struct agent_relay_mux *mux,
    struct agent_relay_tunnel_message *message
)
{
    struct agent_relay_mux_stream *stream;
    uint32_t stream_id;

    if (mux == NULL || message == NULL ||
        message->type != AGENT_RELAY_TUNNEL_OPEN) {
        return AGENT_RELAY_TUNNEL_INVALID_ARGUMENT;
    }
    stream_id = mux->next_stream_id;
    if (stream_id == 0U || stream_id > UINT32_MAX - 2U) {
        return AGENT_RELAY_TUNNEL_STREAM_LIMIT;
    }
    message->stream_id = stream_id;
    message->sequence = 1U;
    if (agent_relay_tunnel_message_validate(message) !=
        AGENT_RELAY_TUNNEL_OK) {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    stream = allocate_stream(mux, stream_id,
                             AGENT_RELAY_MUX_STREAM_OPEN_SENT);
    if (stream == NULL) {
        return AGENT_RELAY_TUNNEL_STREAM_LIMIT;
    }
    stream->next_send_sequence = 2U;
    mux->next_stream_id += 2U;
    mux->opened_local++;
    mux->frames_sent++;
    return AGENT_RELAY_TUNNEL_OK;
}

static bool locally_initiated(
    const struct agent_relay_mux *mux,
    uint32_t stream_id
)
{
    return ((stream_id & 1U) != 0U) == mux->local_odd;
}

static bool previously_opened_stream(
    const struct agent_relay_mux *mux,
    uint32_t stream_id
)
{
    if (locally_initiated(mux, stream_id)) {
        return stream_id < mux->next_stream_id;
    }
    return stream_id <= mux->largest_remote_stream_id;
}

static enum agent_relay_tunnel_result apply_message(
    struct agent_relay_mux *mux,
    const struct agent_relay_tunnel_message *message,
    bool sending
)
{
    struct agent_relay_mux_stream *stream;
    uint64_t *next_sequence;
    uint32_t *credit;

    if (mux == NULL || message == NULL ||
        agent_relay_tunnel_message_validate(message) !=
            AGENT_RELAY_TUNNEL_OK) {
        return AGENT_RELAY_TUNNEL_INVALID_FRAME;
    }
    if (message->type == AGENT_RELAY_TUNNEL_PING ||
        message->type == AGENT_RELAY_TUNNEL_PONG) {
        if (sending) {
            mux->frames_sent++;
        } else {
            mux->frames_received++;
        }
        return AGENT_RELAY_TUNNEL_OK;
    }
    stream = find_stream(mux, message->stream_id);
    if (stream == NULL) {
        /* END frames in opposite directions can cross in flight. A peer may
         * therefore receive credit that was valid when sent after it has
         * observed both ENDs and released the stream. Like HTTP/2, ignore
         * this bounded, state-free update for a previously opened stream. */
        if (!sending &&
            (message->type == AGENT_RELAY_TUNNEL_WINDOW_UPDATE ||
             message->type == AGENT_RELAY_TUNNEL_RESET) &&
            previously_opened_stream(mux, message->stream_id)) {
            mux->frames_received++;
            return AGENT_RELAY_TUNNEL_OK;
        }
        if (sending || message->type != AGENT_RELAY_TUNNEL_OPEN ||
            locally_initiated(mux, message->stream_id) ||
            message->stream_id <= mux->largest_remote_stream_id) {
            mux->protocol_errors++;
            return AGENT_RELAY_TUNNEL_INVALID_STATE;
        }
        stream = allocate_stream(mux, message->stream_id,
                                 AGENT_RELAY_MUX_STREAM_OPEN_RECEIVED);
        if (stream == NULL) {
            return AGENT_RELAY_TUNNEL_STREAM_LIMIT;
        }
        stream->next_receive_sequence = 2U;
        mux->largest_remote_stream_id = message->stream_id;
        mux->opened_remote++;
        mux->frames_received++;
        return AGENT_RELAY_TUNNEL_OK;
    }
    if (message->type == AGENT_RELAY_TUNNEL_OPEN) {
        mux->protocol_errors++;
        return AGENT_RELAY_TUNNEL_INVALID_STATE;
    }
    next_sequence = sending ? &stream->next_send_sequence :
                              &stream->next_receive_sequence;
    if (*next_sequence == UINT64_MAX || message->sequence != *next_sequence) {
        mux->protocol_errors++;
        return AGENT_RELAY_TUNNEL_SEQUENCE_ERROR;
    }
    if (message->type == AGENT_RELAY_TUNNEL_ACCEPT) {
        if ((sending && stream->state != AGENT_RELAY_MUX_STREAM_OPEN_RECEIVED) ||
            (!sending && stream->state != AGENT_RELAY_MUX_STREAM_OPEN_SENT)) {
            mux->protocol_errors++;
            return AGENT_RELAY_TUNNEL_INVALID_STATE;
        }
        stream->state = AGENT_RELAY_MUX_STREAM_ESTABLISHED;
        if (sending) {
            stream->receive_credit = message->credit_bytes;
        } else {
            stream->send_credit = message->credit_bytes;
        }
    } else if (stream->state != AGENT_RELAY_MUX_STREAM_ESTABLISHED) {
        if (message->type != AGENT_RELAY_TUNNEL_RESET) {
            mux->protocol_errors++;
            return AGENT_RELAY_TUNNEL_INVALID_STATE;
        }
    }
    if (message->type == AGENT_RELAY_TUNNEL_DATA) {
        if ((sending && stream->local_closed) ||
            (!sending && stream->remote_closed)) {
            mux->protocol_errors++;
            return AGENT_RELAY_TUNNEL_INVALID_STATE;
        }
        credit = sending ? &stream->send_credit : &stream->receive_credit;
        if (message->data_length > *credit) {
            mux->protocol_errors++;
            return AGENT_RELAY_TUNNEL_FLOW_CONTROL;
        }
        *credit -= (uint32_t)message->data_length;
    } else if (message->type == AGENT_RELAY_TUNNEL_WINDOW_UPDATE) {
        credit = sending ? &stream->receive_credit : &stream->send_credit;
        if (message->credit_bytes > AGENT_RELAY_TUNNEL_MAX_WINDOW - *credit) {
            mux->protocol_errors++;
            return AGENT_RELAY_TUNNEL_FLOW_CONTROL;
        }
        *credit += message->credit_bytes;
    } else if (message->type == AGENT_RELAY_TUNNEL_END) {
        if (sending) {
            if (stream->local_closed) {
                mux->protocol_errors++;
                return AGENT_RELAY_TUNNEL_INVALID_STATE;
            }
            stream->local_closed = true;
        } else {
            if (stream->remote_closed) {
                mux->protocol_errors++;
                return AGENT_RELAY_TUNNEL_INVALID_STATE;
            }
            stream->remote_closed = true;
        }
    }
    *next_sequence += 1U;
    if (sending) {
        mux->frames_sent++;
    } else {
        mux->frames_received++;
    }
    if (message->type == AGENT_RELAY_TUNNEL_RESET) {
        mux->resets++;
        release_stream(mux, stream);
    } else if (stream->local_closed && stream->remote_closed) {
        release_stream(mux, stream);
    }
    return AGENT_RELAY_TUNNEL_OK;
}

enum agent_relay_tunnel_result agent_relay_mux_on_send(
    struct agent_relay_mux *mux,
    const struct agent_relay_tunnel_message *message
)
{
    return apply_message(mux, message, true);
}

enum agent_relay_tunnel_result agent_relay_mux_on_receive(
    struct agent_relay_mux *mux,
    const struct agent_relay_tunnel_message *message
)
{
    return apply_message(mux, message, false);
}

const char *agent_relay_tunnel_type_name(enum agent_relay_tunnel_type type)
{
    switch (type) {
    case AGENT_RELAY_TUNNEL_OPEN:
        return "open";
    case AGENT_RELAY_TUNNEL_ACCEPT:
        return "accept";
    case AGENT_RELAY_TUNNEL_DATA:
        return "data";
    case AGENT_RELAY_TUNNEL_END:
        return "end";
    case AGENT_RELAY_TUNNEL_RESET:
        return "reset";
    case AGENT_RELAY_TUNNEL_WINDOW_UPDATE:
        return "window-update";
    case AGENT_RELAY_TUNNEL_PING:
        return "ping";
    case AGENT_RELAY_TUNNEL_PONG:
        return "pong";
    case AGENT_RELAY_TUNNEL_RESPONSE_START:
        return "response-start";
    default:
        return "unknown";
    }
}
