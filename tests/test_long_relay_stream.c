#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../feed/agentd/src/agent_relay_invoke.c"
static uint64_t test_now(void) { return 1000; }
int main(void) {
    struct agent_relay_invoke_manager manager = {0};
    struct relay_invoke_slot slot = {0};
    char data[1024] = {0};
    manager.now_ms = test_now;
    slot.manager = &manager;slot.timeout_ms = 30000;
    slot.stream_pending = malloc(RELAY_STREAM_MAX_EVENT + AGENT_SSE_MAX_HEADER_BYTES);
    assert(slot.stream_pending);
    uloop_init();
    for (unsigned i = 0; i < 4096; ++i) {
        assert(emit_target_stream_event(data, sizeof(data), &slot));
        slot.stream_pending_length = 0; /* Consumer drains bounded pending buffer. */
    }
    assert(slot.stream_bytes == 4194304);
    slot.stream_pending_length = RELAY_STREAM_MAX_EVENT + AGENT_SSE_MAX_HEADER_BYTES;
    assert(!emit_target_stream_event(data, sizeof(data), &slot));
    uloop_timeout_cancel(&slot.timeout);uloop_done();free(slot.stream_pending);
    puts("long Relay stream and pending-buffer bound passed");return 0;
}
