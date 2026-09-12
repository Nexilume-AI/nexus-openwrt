"use strict";

const fs = require("fs");
const http2 = require("http2");
const { URL } = require("url");
const { FrameDecoder } = require("./relay-tunnel");
const { ControlDecoder, stringifyControl } = require("./relay-federation");

const CONTROL_TYPE = "application/vnd.nexus.relay-federation.v1+jsonl";
const INVOKE_TYPE = "application/vnd.nexus.relay-federation-invoke.v1";
const CONTROL_PATH = "/federation/v1";
const INVOKE_PATH = "/federation/invoke/v1";

class FederationHttp2Network {
  constructor(options) {
    this.relayId = options.relayId;
    this.bridge = options.bridge;
    this.peers = options.peers;
    this.tls = options.tls;
    this.maxQueuedBytes = options.maxQueuedBytes;
    this.heartbeatMs = options.heartbeatMs || 5000;
    this.capture = options.capture || (() => {});
    this.states = new Map();
    this.stopping = false;
    for (const [relayId, config] of Object.entries(this.peers)) {
      const state = {
        relayId, config, session: null, control: null, queue: [], queuedBytes: 0,
        reconnect: null, attempts: 0, invokes: new Set(),
        heartbeat: null, pingSequence: 0,
      };
      this.states.set(relayId, state);
      this.bridge.registerPeer({ relayId, ...config }, {
        sendControl: (message) => this.sendControl(state, message),
        openInvoke: (metadata, handlers) => this.openInvoke(state, metadata, handlers),
      });
    }
  }

  start() {
    for (const state of this.states.values()) this.connect(state);
  }

  connect(state) {
    if (this.stopping || state.session) return;
    const endpoint = new URL(state.config.endpoint);
    const session = http2.connect(endpoint.origin, {
      ca: fs.readFileSync(this.tls.ca), cert: fs.readFileSync(this.tls.cert),
      key: fs.readFileSync(this.tls.key), servername: state.config.certificateDns,
      minVersion: "TLSv1.3", maxVersion: "TLSv1.3",
      lookup: (_hostname, options, callback) => options && options.all
        ? callback(null, [{ address: state.config.connectIpv4, family: 4 }])
        : callback(null, state.config.connectIpv4, 4),
    });
    state.session = session;
    session.on("connect", () => this.openControl(state));
    session.on("error", (error) => this.capture("federation-client-error", {
      peer_relay_id: state.relayId, error: error.message,
    }));
    session.on("close", () => {
      if (state.heartbeat) clearInterval(state.heartbeat);
      state.heartbeat = null;
      if (state.control) state.control = null;
      state.queue = [];
      state.queuedBytes = 0;
      state.session = null;
      for (const call of [...state.invokes]) call.fail("federation-session-closed");
      this.bridge.peerDown(state.relayId, "federation-session-closed");
      this.scheduleReconnect(state);
    });
  }

  openControl(state) {
    if (!state.session || state.session.closed || state.session.destroyed) return;
    const stream = state.session.request({
      ":method": "POST", ":path": CONTROL_PATH,
      "content-type": CONTROL_TYPE, accept: CONTROL_TYPE,
      "nexus-relay-id": this.relayId,
    });
    const decoder = new ControlDecoder((message) =>
      this.bridge.receiveControl(state.relayId, message));
    stream.on("response", (headers) => {
      if (headers[":status"] !== 200 || headers["content-type"] !== CONTROL_TYPE) {
        stream.close(http2.constants.NGHTTP2_REFUSED_STREAM);
        return;
      }
      state.control = stream;
      state.attempts = 0;
      this.flushControl(state);
      this.bridge.transportUp(state.relayId);
      this.startHeartbeat(state);
      this.capture("federation-control-up", { peer_relay_id: state.relayId });
    });
    stream.on("data", (chunk) => {
      try { decoder.push(chunk); } catch (error) {
        this.protocolError(state, stream, error);
      }
    });
    stream.on("error", (error) => this.capture("federation-control-error", {
      peer_relay_id: state.relayId, error: error.message,
    }));
    stream.on("close", () => {
      if (state.control === stream) state.control = null;
    });
  }

  sendControl(state, message) {
    let encoded;
    try { encoded = stringifyControl(message); }
    catch (_error) { return false; }
    if (state.control && !state.control.closed && !state.control.destroyed &&
        state.control.writableLength + encoded.length <= this.maxQueuedBytes) {
      return state.control.write(encoded);
    }
    if (state.queuedBytes + encoded.length > this.maxQueuedBytes) return false;
    state.queue.push(encoded);
    state.queuedBytes += encoded.length;
    return true;
  }

  flushControl(state) {
    while (state.control && state.queue.length) {
      const encoded = state.queue[0];
      if (state.control.writableLength + encoded.length > this.maxQueuedBytes) break;
      state.queue.shift();
      state.queuedBytes -= encoded.length;
      if (!state.control.write(encoded)) break;
    }
  }

  startHeartbeat(state) {
    if (state.heartbeat) clearInterval(state.heartbeat);
    state.heartbeat = setInterval(() => {
      if (!state.session || state.session.closed || state.session.destroyed) return;
      if (Date.now() - this.bridge.peerLastActivity(state.relayId) >
          this.heartbeatMs * 3) {
        state.session.destroy(new Error("federation heartbeat timeout"));
        return;
      }
      state.pingSequence++;
      this.bridge.ping(state.relayId, state.pingSequence);
    }, this.heartbeatMs);
    state.heartbeat.unref();
  }

  openInvoke(state, metadata, handlers) {
    if (!state.session || state.session.closed || state.session.destroyed) return null;
    const stream = state.session.request({
      ":method": "POST", ":path": INVOKE_PATH,
      "content-type": INVOKE_TYPE, accept: INVOKE_TYPE,
      "nexus-relay-id": this.relayId,
      "nexus-target-router": metadata.target_router_id,
    });
    let accepted = false;
    let closed = false;
    const decoder = new FrameDecoder((frame) => handlers.onFrame(frame));
    const call = {
      fail: (reason) => {
        if (closed) return;
        closed = true;
        state.invokes.delete(call);
        handlers.onClose(reason);
        if (!stream.closed && !stream.destroyed) stream.close();
      },
    };
    state.invokes.add(call);
    stream.on("response", (headers) => {
      if (headers[":status"] !== 200 || headers["content-type"] !== INVOKE_TYPE) {
        call.fail("federation-invoke-rejected");
      } else accepted = true;
    });
    stream.on("data", (chunk) => {
      try { decoder.push(chunk); } catch (_error) { call.fail("invalid-federation-frame"); }
    });
    stream.on("error", () => call.fail("federation-invoke-stream-error"));
    stream.on("close", () => call.fail("federation-invoke-stream-closed"));
    return {
      send: (frame) => !closed && stream.writableLength + frame.length <=
        this.maxQueuedBytes && stream.write(frame),
      close: () => {
        if (closed) return;
        closed = true;
        state.invokes.delete(call);
        if (!stream.closed && !stream.destroyed) stream.close();
      },
      accepted: () => accepted,
    };
  }

  acceptStream(stream, headers, certificateDns) {
    const path = headers[":path"];
    if (path !== CONTROL_PATH && path !== INVOKE_PATH) return false;
    const relayId = headers["nexus-relay-id"];
    const state = typeof relayId === "string" ? this.states.get(relayId) : null;
    if (!state || state.config.certificateDns !== certificateDns ||
        headers[":method"] !== "POST") {
      stream.respond({ ":status": 403 }); stream.end(); return true;
    }
    if (path === CONTROL_PATH) this.acceptControl(state, stream, headers);
    else this.acceptInvoke(state, stream, headers);
    return true;
  }

  acceptControl(state, stream, headers) {
    if (headers["content-type"] !== CONTROL_TYPE || headers.accept !== CONTROL_TYPE) {
      stream.respond({ ":status": 400 }); stream.end(); return;
    }
    const decoder = new ControlDecoder((message) =>
      this.bridge.receiveControl(state.relayId, message));
    stream.respond({ ":status": 200, "content-type": CONTROL_TYPE });
    stream.on("data", (chunk) => {
      try { decoder.push(chunk); } catch (error) {
        this.protocolError(state, stream, error);
      }
    });
    stream.on("error", (error) => this.capture("federation-server-control-error", {
      peer_relay_id: state.relayId, error: error.message,
    }));
  }

  acceptInvoke(state, stream, headers) {
    if (headers["content-type"] !== INVOKE_TYPE || headers.accept !== INVOKE_TYPE ||
        typeof headers["nexus-target-router"] !== "string") {
      stream.respond({ ":status": 400 }); stream.end(); return;
    }
    let frameReceiver = null;
    let closeReceiver = null;
    const decoder = new FrameDecoder((frame) => {
      if (frameReceiver) frameReceiver(frame);
    });
    const channel = {
      targetRouterId: headers["nexus-target-router"],
      send: (frame) => !stream.closed && !stream.destroyed &&
        stream.writableLength + frame.length <= this.maxQueuedBytes && stream.write(frame),
      close: () => { if (!stream.closed && !stream.destroyed) stream.close(); },
      onFrame: (callback) => { frameReceiver = callback; },
      onClose: (callback) => { closeReceiver = callback; },
    };
    try { this.bridge.acceptInvoke(state.relayId, channel); }
    catch (_error) { stream.respond({ ":status": 503 }); stream.end(); return; }
    stream.respond({ ":status": 200, "content-type": INVOKE_TYPE });
    stream.on("data", (chunk) => {
      try { decoder.push(chunk); } catch (error) {
        this.protocolError(state, stream, error);
      }
    });
    stream.on("error", () => { if (closeReceiver) closeReceiver("stream-error"); });
    stream.on("close", () => { if (closeReceiver) closeReceiver("stream-close"); });
  }

  protocolError(state, stream, error) {
    this.capture("federation-protocol-error", {
      peer_relay_id: state.relayId, error: error.message,
    });
    stream.close(http2.constants.NGHTTP2_PROTOCOL_ERROR);
  }

  scheduleReconnect(state) {
    if (this.stopping || state.reconnect) return;
    state.attempts = Math.min(state.attempts + 1, 6);
    const delay = Math.min(30000, 250 * (2 ** state.attempts));
    state.reconnect = setTimeout(() => {
      state.reconnect = null;
      this.connect(state);
    }, delay);
    state.reconnect.unref();
  }

  stop() {
    this.stopping = true;
    for (const state of this.states.values()) {
      if (state.reconnect) clearTimeout(state.reconnect);
      if (state.heartbeat) clearInterval(state.heartbeat);
      if (state.control && !state.control.closed) state.control.close();
      if (state.session) state.session.destroy();
      this.bridge.unregisterPeer(state.relayId, "shutdown");
    }
    this.states.clear();
  }
}

module.exports = {
  CONTROL_TYPE, INVOKE_TYPE, CONTROL_PATH, INVOKE_PATH, FederationHttp2Network,
};
