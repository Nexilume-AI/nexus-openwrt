#include "agent_arpx_protocol.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#define ARPX_CBOR_COMMON_FIELD_COUNT 7U
#define ARPX_CBOR_WITHDRAW_FIELD_COUNT 8U
#define ARPX_CBOR_SNAPSHOT_FIELD_COUNT 8U
#define ARPX_CBOR_UPDATE_FIELD_COUNT 20U

struct cbor_writer {
    uint8_t *data;
    size_t capacity;
    size_t offset;
};

struct cbor_reader {
    const uint8_t *data;
    size_t size;
    size_t offset;
};

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

static bool router_id_valid(const char *identifier)
{
    size_t length = bounded_length(identifier, AGENT_ARPX_ROUTER_ID_LEN);
    size_t i;

    if (length == 0U || length >= AGENT_ARPX_ROUTER_ID_LEN ||
        (!islower((unsigned char)identifier[0]) &&
         !isdigit((unsigned char)identifier[0])) ||
        (!islower((unsigned char)identifier[length - 1U]) &&
         !isdigit((unsigned char)identifier[length - 1U]))) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        unsigned char value = (unsigned char)identifier[i];

        if (!islower(value) && !isdigit(value) && value != '.' &&
            value != '-' && value != '_') {
            return false;
        }
    }
    return true;
}

static bool domain_id_valid(const char *domain)
{
    size_t length = bounded_length(domain, AGENT_ARPX_DOMAIN_ID_LEN);
    size_t label_start = 0U;
    size_t i;

    if (length == 0U || length >= AGENT_ARPX_DOMAIN_ID_LEN) {
        return false;
    }
    for (i = 0U; i <= length; i++) {
        if (i == length || domain[i] == '.') {
            if (i == label_start || i - label_start > 63U ||
                domain[label_start] == '-' || domain[i - 1U] == '-') {
                return false;
            }
            label_start = i + 1U;
            continue;
        }
        if (!islower((unsigned char)domain[i]) &&
            !isdigit((unsigned char)domain[i]) && domain[i] != '-') {
            return false;
        }
    }
    return true;
}

static bool nonempty_text_valid(const char *text, size_t capacity)
{
    size_t length = bounded_length(text, capacity);

    return length > 0U && length < capacity;
}

static bool route_id_valid(const char *route_id)
{
    size_t length = bounded_length(route_id, AGENT_ARPX_ROUTE_ID_LEN);
    size_t i;

    if (length != AGENT_ARPX_ROUTE_ID_LEN - 1U) {
        return false;
    }
    for (i = 0U; i < length; i++) {
        if (!isdigit((unsigned char)route_id[i]) &&
            (route_id[i] < 'a' || route_id[i] > 'f')) {
            return false;
        }
    }
    return true;
}

enum agent_arpx_result agent_arpx_message_validate(
    const struct agent_arpx_message *message
)
{
    if (message == NULL) {
        return AGENT_ARPX_INVALID_ARGUMENT;
    }
    if (message->version != AGENT_ARPX_VERSION ||
        !router_id_valid(message->router_id) ||
        !domain_id_valid(message->domain_id) ||
        message->boot_epoch == 0U || message->sequence == 0U) {
        return AGENT_ARPX_INVALID_MESSAGE;
    }
    if (message->type == AGENT_ARPX_OPEN) {
        if (message->sequence != 1U ||
            message->heartbeat_ms < AGENT_ARPX_MIN_HEARTBEAT_MS ||
            message->heartbeat_ms > AGENT_ARPX_MAX_HEARTBEAT_MS) {
            return AGENT_ARPX_INVALID_MESSAGE;
        }
        return AGENT_ARPX_OK;
    }
    if (message->type == AGENT_ARPX_HEARTBEAT) {
        if (message->sequence < 2U || message->heartbeat_ms != 0U) {
            return AGENT_ARPX_INVALID_MESSAGE;
        }
        return AGENT_ARPX_OK;
    }
    if (message->type == AGENT_ARPX_CAPABILITY_WITHDRAW) {
        if (message->sequence < 2U || message->heartbeat_ms != 0U ||
            !route_id_valid(message->route_id)) {
            return AGENT_ARPX_INVALID_MESSAGE;
        }
        return AGENT_ARPX_OK;
    }
    if (message->type == AGENT_ARPX_SNAPSHOT_REQUEST ||
        message->type == AGENT_ARPX_SNAPSHOT_END) {
        if (message->sequence < 2U || message->heartbeat_ms != 0U ||
            message->snapshot_id == 0U) {
            return AGENT_ARPX_INVALID_MESSAGE;
        }
        return AGENT_ARPX_OK;
    }
    if (message->type == AGENT_ARPX_CAPABILITY_UPDATE) {
        size_t i;
        size_t j;

        if (message->sequence < 2U || message->heartbeat_ms != 0U ||
            !route_id_valid(message->route_id) ||
            !nonempty_text_valid(message->intent, sizeof(message->intent)) ||
            message->capability_version == 0U ||
            !nonempty_text_valid(message->origin, sizeof(message->origin)) ||
            !nonempty_text_valid(message->endpoint,
                                 sizeof(message->endpoint)) ||
            !nonempty_text_valid(message->tenant, sizeof(message->tenant)) ||
            !nonempty_text_valid(message->region, sizeof(message->region)) ||
            message->trust_level > 100U ||
            message->load_permille > 1000U ||
            message->remaining_lease_ms == 0U ||
            message->remaining_lease_ms >
                AGENT_ARPX_MAX_REMAINING_LEASE_MS ||
            message->path_length == 0U ||
            message->path_length > AGENT_ARPX_MAX_PATH) {
            return AGENT_ARPX_INVALID_MESSAGE;
        }
        for (i = 0U; i < message->path_length; i++) {
            if (!router_id_valid(message->path[i])) {
                return AGENT_ARPX_INVALID_MESSAGE;
            }
            for (j = 0U; j < i; j++) {
                if (strcmp(message->path[i], message->path[j]) == 0) {
                    return AGENT_ARPX_INVALID_MESSAGE;
                }
            }
        }
        if (strcmp(message->path[message->path_length - 1U],
                   message->router_id) != 0) {
            return AGENT_ARPX_INVALID_MESSAGE;
        }
        return AGENT_ARPX_OK;
    }
    return AGENT_ARPX_UNSUPPORTED;
}

static bool writer_byte(struct cbor_writer *writer, uint8_t value)
{
    if (writer->offset >= writer->capacity) {
        return false;
    }
    writer->data[writer->offset++] = value;
    return true;
}

static bool writer_bytes(
    struct cbor_writer *writer,
    const uint8_t *data,
    size_t size
)
{
    if (size > writer->capacity - writer->offset) {
        return false;
    }
    memcpy(writer->data + writer->offset, data, size);
    writer->offset += size;
    return true;
}

static bool writer_head(
    struct cbor_writer *writer,
    uint8_t major,
    uint64_t value
)
{
    size_t i;

    if (value < 24U) {
        return writer_byte(writer, (uint8_t)((major << 5U) | (uint8_t)value));
    }
    if (value <= UINT8_MAX) {
        return writer_byte(writer, (uint8_t)((major << 5U) | 24U)) &&
               writer_byte(writer, (uint8_t)value);
    }
    if (value <= UINT16_MAX) {
        return writer_byte(writer, (uint8_t)((major << 5U) | 25U)) &&
               writer_byte(writer, (uint8_t)(value >> 8U)) &&
               writer_byte(writer, (uint8_t)value);
    }
    if (value <= UINT32_MAX) {
        if (!writer_byte(writer, (uint8_t)((major << 5U) | 26U))) {
            return false;
        }
        for (i = 4U; i > 0U; i--) {
            if (!writer_byte(writer,
                             (uint8_t)(value >> ((i - 1U) * 8U)))) {
                return false;
            }
        }
        return true;
    }
    if (!writer_byte(writer, (uint8_t)((major << 5U) | 27U))) {
        return false;
    }
    for (i = 8U; i > 0U; i--) {
        if (!writer_byte(writer, (uint8_t)(value >> ((i - 1U) * 8U)))) {
            return false;
        }
    }
    return true;
}

static bool writer_uint(struct cbor_writer *writer, uint64_t value)
{
    return writer_head(writer, 0U, value);
}

static bool writer_text(struct cbor_writer *writer, const char *text)
{
    size_t length = strlen(text);

    return writer_head(writer, 3U, (uint64_t)length) &&
           writer_bytes(writer, (const uint8_t *)text, length);
}

static bool writer_path(
    struct cbor_writer *writer,
    const struct agent_arpx_message *message
)
{
    size_t i;

    if (!writer_head(writer, 4U, message->path_length)) {
        return false;
    }
    for (i = 0U; i < message->path_length; i++) {
        if (!writer_text(writer, message->path[i])) {
            return false;
        }
    }
    return true;
}

enum agent_arpx_result agent_arpx_frame_encode(
    const struct agent_arpx_message *message,
    uint8_t *output,
    size_t capacity,
    size_t *written
)
{
    struct cbor_writer writer;
    enum agent_arpx_result result;
    size_t payload_size;

    if (output == NULL || written == NULL) {
        return AGENT_ARPX_INVALID_ARGUMENT;
    }
    *written = 0U;
    result = agent_arpx_message_validate(message);
    if (result != AGENT_ARPX_OK) {
        return result;
    }
    if (capacity < AGENT_ARPX_FRAME_HEADER_SIZE) {
        return AGENT_ARPX_BUFFER_TOO_SMALL;
    }

    writer.data = output;
    writer.capacity = capacity > AGENT_ARPX_MAX_FRAME_SIZE
        ? AGENT_ARPX_MAX_FRAME_SIZE
        : capacity;
    writer.offset = AGENT_ARPX_FRAME_HEADER_SIZE;
    {
        uint64_t field_count = message->type == AGENT_ARPX_CAPABILITY_UPDATE
            ? ARPX_CBOR_UPDATE_FIELD_COUNT
            : (message->type == AGENT_ARPX_CAPABILITY_WITHDRAW
                ? ARPX_CBOR_WITHDRAW_FIELD_COUNT
                : ((message->type == AGENT_ARPX_SNAPSHOT_REQUEST ||
                    message->type == AGENT_ARPX_SNAPSHOT_END)
                    ? ARPX_CBOR_SNAPSHOT_FIELD_COUNT
                    : ARPX_CBOR_COMMON_FIELD_COUNT));

    if (!writer_head(&writer, 5U, field_count) ||
        !writer_uint(&writer, 0U) || !writer_uint(&writer, message->version) ||
        !writer_uint(&writer, 1U) ||
        !writer_uint(&writer, (uint64_t)message->type) ||
        !writer_uint(&writer, 2U) || !writer_text(&writer, message->router_id) ||
        !writer_uint(&writer, 3U) || !writer_text(&writer, message->domain_id) ||
        !writer_uint(&writer, 4U) || !writer_uint(&writer, message->boot_epoch) ||
        !writer_uint(&writer, 5U) || !writer_uint(&writer, message->sequence) ||
        !writer_uint(&writer, 6U) ||
        !writer_uint(&writer, message->heartbeat_ms)) {
        return AGENT_ARPX_BUFFER_TOO_SMALL;
    }
    }
    if (message->type == AGENT_ARPX_CAPABILITY_WITHDRAW &&
        (!writer_uint(&writer, 7U) ||
         !writer_text(&writer, message->route_id))) {
        return AGENT_ARPX_BUFFER_TOO_SMALL;
    }
    if ((message->type == AGENT_ARPX_SNAPSHOT_REQUEST ||
         message->type == AGENT_ARPX_SNAPSHOT_END) &&
        (!writer_uint(&writer, 7U) ||
         !writer_uint(&writer, message->snapshot_id))) {
        return AGENT_ARPX_BUFFER_TOO_SMALL;
    }
    if (message->type == AGENT_ARPX_CAPABILITY_UPDATE &&
        (!writer_uint(&writer, 7U) ||
         !writer_text(&writer, message->route_id) ||
         !writer_uint(&writer, 8U) ||
         !writer_text(&writer, message->intent) ||
         !writer_uint(&writer, 9U) ||
         !writer_uint(&writer, message->capability_version) ||
         !writer_uint(&writer, 10U) ||
         !writer_text(&writer, message->origin) ||
         !writer_uint(&writer, 11U) ||
         !writer_text(&writer, message->endpoint) ||
         !writer_uint(&writer, 12U) ||
         !writer_text(&writer, message->tenant) ||
         !writer_uint(&writer, 13U) ||
         !writer_text(&writer, message->region) ||
         !writer_uint(&writer, 14U) ||
         !writer_uint(&writer, message->cost_microunits) ||
         !writer_uint(&writer, 15U) ||
         !writer_uint(&writer, message->latency_ms) ||
         !writer_uint(&writer, 16U) ||
         !writer_uint(&writer, message->trust_level) ||
         !writer_uint(&writer, 17U) ||
         !writer_uint(&writer, message->load_permille) ||
         !writer_uint(&writer, 18U) ||
         !writer_uint(&writer, message->remaining_lease_ms) ||
         !writer_uint(&writer, 19U) ||
         !writer_path(&writer, message))) {
        return AGENT_ARPX_BUFFER_TOO_SMALL;
    }

    payload_size = writer.offset - AGENT_ARPX_FRAME_HEADER_SIZE;
    output[0] = (uint8_t)(payload_size >> 24U);
    output[1] = (uint8_t)(payload_size >> 16U);
    output[2] = (uint8_t)(payload_size >> 8U);
    output[3] = (uint8_t)payload_size;
    *written = writer.offset;
    return AGENT_ARPX_OK;
}

static bool reader_byte(struct cbor_reader *reader, uint8_t *value)
{
    if (reader->offset >= reader->size) {
        return false;
    }
    *value = reader->data[reader->offset++];
    return true;
}

static bool reader_head(
    struct cbor_reader *reader,
    uint8_t expected_major,
    uint64_t *value
)
{
    uint8_t initial;
    uint8_t additional;
    uint8_t byte;
    size_t count;
    size_t i;

    if (!reader_byte(reader, &initial) || (initial >> 5U) != expected_major) {
        return false;
    }
    additional = initial & 0x1fU;
    if (additional < 24U) {
        *value = additional;
        return true;
    }
    if (additional == 24U) {
        count = 1U;
    } else if (additional == 25U) {
        count = 2U;
    } else if (additional == 26U) {
        count = 4U;
    } else if (additional == 27U) {
        count = 8U;
    } else {
        return false;
    }
    *value = 0U;
    for (i = 0U; i < count; i++) {
        if (!reader_byte(reader, &byte)) {
            return false;
        }
        *value = (*value << 8U) | byte;
    }
    if ((count == 1U && *value < 24U) ||
        (count == 2U && *value <= UINT8_MAX) ||
        (count == 4U && *value <= UINT16_MAX) ||
        (count == 8U && *value <= UINT32_MAX)) {
        return false;
    }
    return true;
}

static bool reader_uint(struct cbor_reader *reader, uint64_t *value)
{
    return reader_head(reader, 0U, value);
}

static bool reader_text(
    struct cbor_reader *reader,
    char *output,
    size_t capacity
)
{
    uint64_t length;

    if (!reader_head(reader, 3U, &length) || length == 0U ||
        length >= capacity || length > reader->size - reader->offset) {
        return false;
    }
    memcpy(output, reader->data + reader->offset, (size_t)length);
    output[(size_t)length] = '\0';
    reader->offset += (size_t)length;
    return true;
}

static bool reader_path(
    struct cbor_reader *reader,
    struct agent_arpx_message *message
)
{
    uint64_t count;
    size_t i;

    if (!reader_head(reader, 4U, &count) || count == 0U ||
        count > AGENT_ARPX_MAX_PATH) {
        return false;
    }
    message->path_length = (uint8_t)count;
    for (i = 0U; i < (size_t)count; i++) {
        if (!reader_text(reader, message->path[i],
                         sizeof(message->path[i]))) {
            return false;
        }
    }
    return true;
}

enum agent_arpx_result agent_arpx_frame_decode(
    const uint8_t *frame,
    size_t frame_size,
    struct agent_arpx_message *message
)
{
    struct cbor_reader reader;
    struct agent_arpx_message decoded;
    uint64_t payload_size;
    uint64_t value;
    uint64_t key;
    size_t i;
    enum agent_arpx_result result;

    if (frame == NULL || message == NULL) {
        return AGENT_ARPX_INVALID_ARGUMENT;
    }
    if (frame_size < AGENT_ARPX_FRAME_HEADER_SIZE ||
        frame_size > AGENT_ARPX_MAX_FRAME_SIZE) {
        return AGENT_ARPX_INVALID_FRAME;
    }
    payload_size = ((uint64_t)frame[0] << 24U) |
                   ((uint64_t)frame[1] << 16U) |
                   ((uint64_t)frame[2] << 8U) | frame[3];
    if (payload_size != frame_size - AGENT_ARPX_FRAME_HEADER_SIZE) {
        return AGENT_ARPX_INVALID_FRAME;
    }

    memset(&decoded, 0, sizeof(decoded));
    reader.data = frame + AGENT_ARPX_FRAME_HEADER_SIZE;
    reader.size = (size_t)payload_size;
    reader.offset = 0U;
    if (!reader_head(&reader, 5U, &value) ||
        (value != ARPX_CBOR_COMMON_FIELD_COUNT &&
         value != ARPX_CBOR_WITHDRAW_FIELD_COUNT &&
         value != ARPX_CBOR_UPDATE_FIELD_COUNT)) {
        return AGENT_ARPX_INVALID_FRAME;
    }
    {
    size_t field_count = (size_t)value;
    for (i = 0U; i < field_count; i++) {
        if (!reader_uint(&reader, &key) || key != i) {
            return AGENT_ARPX_INVALID_FRAME;
        }
        switch (i) {
        case 0U:
            if (!reader_uint(&reader, &value) || value > UINT32_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.version = (uint32_t)value;
            break;
        case 1U:
            if (!reader_uint(&reader, &value) || value > UINT32_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.type = (enum agent_arpx_message_type)value;
            break;
        case 2U:
            if (!reader_text(&reader, decoded.router_id,
                             sizeof(decoded.router_id))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 3U:
            if (!reader_text(&reader, decoded.domain_id,
                             sizeof(decoded.domain_id))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 4U:
            if (!reader_uint(&reader, &decoded.boot_epoch)) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 5U:
            if (!reader_uint(&reader, &decoded.sequence)) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 6U:
            if (!reader_uint(&reader, &value) || value > UINT32_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.heartbeat_ms = (uint32_t)value;
            break;
        case 7U:
            if (decoded.type == AGENT_ARPX_SNAPSHOT_REQUEST ||
                decoded.type == AGENT_ARPX_SNAPSHOT_END) {
                if (!reader_uint(&reader, &decoded.snapshot_id)) {
                    return AGENT_ARPX_INVALID_FRAME;
                }
            } else if (!reader_text(&reader, decoded.route_id,
                                    sizeof(decoded.route_id))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 8U:
            if (!reader_text(&reader, decoded.intent,
                             sizeof(decoded.intent))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 9U:
            if (!reader_uint(&reader, &value) || value > UINT32_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.capability_version = (uint32_t)value;
            break;
        case 10U:
            if (!reader_text(&reader, decoded.origin,
                             sizeof(decoded.origin))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 11U:
            if (!reader_text(&reader, decoded.endpoint,
                             sizeof(decoded.endpoint))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 12U:
            if (!reader_text(&reader, decoded.tenant,
                             sizeof(decoded.tenant))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 13U:
            if (!reader_text(&reader, decoded.region,
                             sizeof(decoded.region))) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 14U:
            if (!reader_uint(&reader, &decoded.cost_microunits)) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        case 15U:
            if (!reader_uint(&reader, &value) || value > UINT32_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.latency_ms = (uint32_t)value;
            break;
        case 16U:
            if (!reader_uint(&reader, &value) || value > UINT8_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.trust_level = (uint8_t)value;
            break;
        case 17U:
            if (!reader_uint(&reader, &value) || value > UINT16_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.load_permille = (uint16_t)value;
            break;
        case 18U:
            if (!reader_uint(&reader, &value) || value > UINT32_MAX) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            decoded.remaining_lease_ms = (uint32_t)value;
            break;
        case 19U:
            if (!reader_path(&reader, &decoded)) {
                return AGENT_ARPX_INVALID_FRAME;
            }
            break;
        default:
            return AGENT_ARPX_INVALID_FRAME;
        }
    }
    if ((decoded.type == AGENT_ARPX_CAPABILITY_UPDATE &&
         field_count != ARPX_CBOR_UPDATE_FIELD_COUNT) ||
        (decoded.type == AGENT_ARPX_CAPABILITY_WITHDRAW &&
         field_count != ARPX_CBOR_WITHDRAW_FIELD_COUNT) ||
        ((decoded.type == AGENT_ARPX_SNAPSHOT_REQUEST ||
          decoded.type == AGENT_ARPX_SNAPSHOT_END) &&
         field_count != ARPX_CBOR_SNAPSHOT_FIELD_COUNT) ||
        ((decoded.type == AGENT_ARPX_OPEN ||
          decoded.type == AGENT_ARPX_HEARTBEAT) &&
         field_count != ARPX_CBOR_COMMON_FIELD_COUNT)) {
        return AGENT_ARPX_INVALID_FRAME;
    }
    }
    if (reader.offset != reader.size) {
        return AGENT_ARPX_INVALID_FRAME;
    }
    result = agent_arpx_message_validate(&decoded);
    if (result != AGENT_ARPX_OK) {
        return result;
    }
    *message = decoded;
    return AGENT_ARPX_OK;
}

const char *agent_arpx_message_type_name(enum agent_arpx_message_type type)
{
    switch (type) {
    case AGENT_ARPX_OPEN:
        return "OPEN";
    case AGENT_ARPX_CAPABILITY_UPDATE:
        return "CAPABILITY_UPDATE";
    case AGENT_ARPX_CAPABILITY_WITHDRAW:
        return "CAPABILITY_WITHDRAW";
    case AGENT_ARPX_HEARTBEAT:
        return "HEARTBEAT";
    case AGENT_ARPX_SNAPSHOT_REQUEST:
        return "SNAPSHOT_REQUEST";
    case AGENT_ARPX_SNAPSHOT_END:
        return "SNAPSHOT_END";
    case AGENT_ARPX_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
