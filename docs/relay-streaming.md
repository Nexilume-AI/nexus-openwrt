# Streaming Relay calls

Relay carries MCP streaming responses when inbound IPv6/direct access is blocked.
Upgrade the Cloud runtime adapter, Relay service and OpenWrt agent gateway together.
MCP initialization may return JSON when the client accepts both JSON and SSE;
tool calls can deliver progress followed by the native MCP result through SSE.

## Long-lived Cloud Relay streams

Cloud ingress defaults to `maxStreamDurationMs: 0`: no fixed total stream lifetime.
It also has no cumulative response-byte cap for an SSE sink. The OpenWrt Relay
Agent target, Python SDK SSE writer, and Cloud SSE parser forward events
incrementally without collecting the entire response. `maxResponseBytes` and the
SDK `max_response_bytes` still bound buffered JSON responses. Per-event, request,
frame and pending-buffer limits remain in force; use artifact downloads for a
single result too large to fit in one event.

Response credit is replenished when the HTTP sink drains. A slow consumer stops
credit, which pauses backend reads on OpenWrt. Emit a heartbeat more frequently
than the idle timeout (normally 30 seconds; 5–10 seconds is recommended). Activity
refreshes the idle timer without consuming a total-lifetime budget. An optional
positive `maxStreamDurationMs` (1,000–3,600,000) enables an operator lifetime cap.

Task execution deadlines are separate from stream lifetime. Cancellation,
client disconnect, upstream failure and prolonged stalls still stop the stream;
Agents must cancel cooperative workers and release device ownership during
cleanup. Removing the Relay cap does not make a ten-minute workflow unlimited.
There is no guarantee that a physical connection survives network failure or
service restart. Use background tasks and bounded replay/resumption where
supported, reconnect to the existing task, and never automatically replay a
phone action because its response connection broke.

The change applies to the Cloud Relay → native OpenWrt Agent target path.
The separate legacy HTTP Gateway path retains its existing response limits;
its HTTP library needs explicit output-buffer control before those caps can be
removed safely. Existing Agent containers must upgrade their SDK to obtain the
new SSE writer behavior; an updated Cloud Relay alone cannot remove an old
Agent's cumulative limit.

A green Agent heartbeat confirms registration/presence, not a complete MCP client
handshake or a successful tool call. Diagnose initialization, tools/list and a
small tools/call separately. Preserve error bodies and distinguish account quota,
upstream unavailability and transport timeout. Streaming alone does not increase
account quotas or recover a withdrawn upstream model.

The [OpenROAD SDK example](https://github.com/Nexilume-AI/nexus-agent-sdk-python/tree/main/examples/openroad_mcp_agent)
shows asynchronous jobs, progress polling and resumable artifact retrieval.
