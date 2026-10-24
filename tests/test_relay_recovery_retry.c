#include <assert.h>
#include "../feed/agentd/src/agent_relay_bootstrap.c"
static uint64_t test_clock(void) { return 1000; }
int main(void)
{
    static struct agent_relay_bootstrap bootstrap;
    bootstrap.now_ms = test_clock;
    bootstrap.watcher.fd = -1;
    bootstrap.endpoint_count = 1;
    bootstrap.config.poll_ms = 60000;
    schedule_retry(&bootstrap, "normal renewal failure");
    assert(bootstrap.next_query_ms == 61000);
    strcpy(bootstrap.failed_relay_id, "relay-test");
    schedule_retry(&bootstrap, "recovery request failure");
    assert(bootstrap.next_query_ms == 3000);
    bootstrap.endpoint_count = 2;
    schedule_retry(&bootstrap, "first endpoint");
    assert(bootstrap.next_query_ms == 1000 + RELAY_TICK_MS);
    schedule_retry(&bootstrap, "second endpoint");
    assert(bootstrap.next_query_ms == 3000);
    bootstrap.assignment_active = true;
    bootstrap.assignment.expires_at_ms = 1500;
    schedule_retry(&bootstrap, "lease expiring");
    assert(bootstrap.next_query_ms <= 1500);
    puts("Relay Directory recovery retry bounds passed");
    return 0;
}
