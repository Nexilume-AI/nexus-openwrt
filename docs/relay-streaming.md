# Streaming Relay calls

Relay carries MCP streaming responses when inbound IPv6/direct access is blocked.
Upgrade the Cloud runtime adapter, Relay service and OpenWrt agent gateway together.
MCP initialization may return JSON when the client accepts both JSON and SSE;
tool calls can deliver progress followed by the native MCP result through SSE.

Response credit is replenished as buffered data is consumed. Backpressure bounds
queued data instead of requiring an entire large response to fit in the initial
credit window. The example Cloud ingress response limit is 1 MiB. Request limits,
per-event limits and deadlines still apply; oversized responses fail explicitly.
Use pagination, chunked artifacts or authenticated file downloads for larger data.

A green Agent heartbeat confirms registration/presence, not a complete MCP client
handshake or a successful tool call. Diagnose initialization, tools/list and a
small tools/call separately. Preserve error bodies and distinguish account quota,
upstream unavailability and transport timeout. Streaming alone does not increase
account quotas or recover a withdrawn upstream model.

The [OpenROAD SDK example](https://github.com/Nexilume-AI/nexus-agent-sdk-python/tree/main/examples/openroad_mcp_agent)
shows asynchronous jobs, progress polling and resumable artifact retrieval.
