/* Exercise production timeout/reconnect logic, including Directory notification. */
#include <assert.h>
#include "../feed/agentd/src/agent_peer_transport.c"

static uint64_t clock_ms = 1000;
static unsigned int notifications;
static uint64_t test_clock(void) { return clock_ms; }
static void changed(void *context, const char *id, bool up, uint64_t now)
{
    (void)context;
    assert(strcmp(id, "relay-test") == 0 && !up && now == clock_ms);
    notifications++;
}
int main(void)
{
    static struct agent_peer_transport_manager manager;
    static struct peer_transport_slot slot;
    static struct peer_table peers;
    assert(uloop_init() == 0);
    manager.now_ms = test_clock;
    manager.peers = &peers;
    manager.config.session_handler = changed;
    manager.config.tick_ms = 100;
    manager.slots = &slot;
    manager.slot_count = 1;
    slot.manager = &manager;
    slot.eligible = slot.relay = true;
    slot.watcher.fd = -1;
    strcpy(slot.peer_id, "relay-test");
    strcpy(slot.relay_session_ticket, "old-ticket");
    slot.phase = TRANSPORT_ESTABLISHED;
    slot.session.state = AGENT_PEER_SESSION_ESTABLISHED;
    slot.session.deadline_ms = 999;
    slot.session.config.initial_backoff_ms = 1000;
    slot.session.config.max_backoff_ms = 30000;
    manager_timer_callback(&manager.timer);
    assert(notifications == 1 && slot.relay_ticket_waiting);
    assert(slot.phase == TRANSPORT_BACKOFF && slot.session.timeouts == 1);
    clock_ms += 60000;
    manager_timer_callback(&manager.timer);
    assert(slot.session.reconnects == 0 && notifications == 1);
    assert(agent_peer_transport_set_relay_ticket(&manager, "relay-test", "old-ticket"));
    assert(slot.relay_ticket_waiting);
    assert(agent_peer_transport_set_relay_ticket(&manager, "relay-test", "new-ticket"));
    assert(!slot.relay_ticket_waiting && !slot.failure_notified);
    assert(slot.session.next_action_ms == clock_ms);
    slot_fail(&slot, "another connection failure");
    assert(notifications == 2 && slot.relay_ticket_waiting);
    slot_fail(&slot, "duplicate callback");
    assert(notifications == 2);
    uloop_timeout_cancel(&manager.timer);
    uloop_done();
    puts("Relay timeout, replay prevention and fresh-ticket recovery passed");
    return 0;
}
