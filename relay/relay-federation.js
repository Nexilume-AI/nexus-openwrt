"use strict";

const crypto = require("crypto");
const {
  TYPE, RESET, decodeFrame, rewriteStreamId, decrementOpenHopLimit, encodeReset,
} = require("./relay-tunnel");
const { validRouterId } = require("./relay-core");

const FEDERATION_VERSION = 1;
const MAX_CONTROL_BYTES = 65536;
const MAX_PATH = 8;

function validDnsName(value) {
  return typeof value === "string" && value.length >= 1 && value.length <= 253 &&
    value.split(".").every((label) => label.length >= 1 && label.length <= 63 &&
      /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(label));
}

function validPath(path) {
  return Array.isArray(path) && path.length >= 1 && path.length <= MAX_PATH &&
    new Set(path).size === path.length && path.every(validRouterId);
}

function stringifyControl(message) {
  const encoded = Buffer.from(JSON.stringify(message, (_key, value) =>
    typeof value === "bigint" ? `${value}n` : value), "utf8");
  if (encoded.length < 2 || encoded.length > MAX_CONTROL_BYTES - 1) {
    throw new Error("federation control message is out of bounds");
  }
  return Buffer.concat([encoded, Buffer.from("\n")]);
}

function parseControl(line) {
  if (!Buffer.isBuffer(line) || line.length < 2 || line.length > MAX_CONTROL_BYTES) {
    throw new Error("invalid federation control message size");
  }
  const value = JSON.parse(line.toString("utf8"), (_key, item) =>
    typeof item === "string" && /^[0-9]+n$/.test(item)
      ? BigInt(item.slice(0, -1)) : item);
  if (!value || Array.isArray(value) || typeof value !== "object" ||
      value.version !== FEDERATION_VERSION || typeof value.type !== "string") {
    throw new Error("invalid federation control envelope");
  }
  return value;
}

class ControlDecoder {
  constructor(callback) {
    if (typeof callback !== "function") throw new Error("callback is required");
    this.callback = callback;
    this.buffer = Buffer.alloc(0);
  }

  push(chunk) {
    if (!Buffer.isBuffer(chunk)) throw new Error("control chunk must be a Buffer");
    if (this.buffer.length + chunk.length > MAX_CONTROL_BYTES) {
      throw new Error("federation control buffer exceeded");
    }
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      const newline = this.buffer.indexOf(0x0a);
      if (newline < 0) break;
      if (newline === 0) throw new Error("empty federation control message");
      const line = this.buffer.subarray(0, newline);
      this.buffer = this.buffer.subarray(newline + 1);
      this.callback(parseControl(line));
    }
  }
}

function peerConfig(value) {
  if (!value || typeof value !== "object" || Array.isArray(value) ||
      !validRouterId(value.relayId) || !validRouterId(value.relayRouterId) ||
      !validDnsName(value.domainId) || !validDnsName(value.certificateDns) ||
      !Number.isInteger(value.priority) || value.priority < 0 || value.priority > 65535) {
    throw new Error("invalid federation peer identity");
  }
  return { ...value };
}

class FederationBridge {
  constructor(options) {
    if (!options || !validRouterId(options.relayId) ||
        !validRouterId(options.relayRouterId) || !options.core) {
      throw new Error("invalid federation bridge options");
    }
    this.relayId = options.relayId;
    this.relayRouterId = options.relayRouterId;
    this.core = options.core;
    this.reflector = options.reflector || null;
    this.maxRemoteRouters = options.maxRemoteRouters || 4096;
    this.maxTransitHops = options.maxTransitHops || MAX_PATH;
    this.capture = options.capture || (() => {});
    if (!Number.isInteger(this.maxRemoteRouters) || this.maxRemoteRouters < 1 ||
        this.maxRemoteRouters > 65536 || !Number.isInteger(this.maxTransitHops) ||
        this.maxTransitHops < 1 || this.maxTransitHops > MAX_PATH) {
      throw new Error("invalid federation bounds");
    }
    this.bootEpoch = crypto.randomBytes(8).readBigUInt64BE() & 0x7fffffffffffffffn;
    this.peers = new Map();
    this.localNodes = new Map();
    this.memberSequences = new Map();
    this.candidates = new Map();
    this.selected = new Map();
    this.outbound = new Map();
    this.inbound = new Map();
    this.nextIngress = 1;
    this.metrics = {
      peersUp: 0, peersDown: 0, membersLearned: 0, membersWithdrawn: 0,
      memberLoopsRejected: 0, virtualNodes: 0, invokesOpened: 0,
      invokesCompleted: 0, invokesFailed: 0, hopLimitRejected: 0,
    };
  }

  registerPeer(configValue, transport) {
    const config = peerConfig(configValue);
    if (!transport || typeof transport.sendControl !== "function" ||
        typeof transport.openInvoke !== "function" ||
        this.peers.has(config.relayId) || config.relayId === this.relayId) {
      throw new Error("invalid or duplicate federation transport");
    }
    const peer = {
      config, transport, ready: false, sequences: new Map(),
      lastActivity: Date.now(),
    };
    this.peers.set(config.relayId, peer);
    if (this.reflector) this.registerReflectorPeer(peer);
    this.sendControl(config.relayId, {
      type: "hello", relay_id: this.relayId,
      relay_router_id: this.relayRouterId, boot_epoch: this.bootEpoch,
    });
    for (const [routerId, sequence] of this.localNodes) {
      this.sendMember(config.relayId, "member_up", routerId, this.relayId,
        sequence, [this.relayId]);
    }
    return true;
  }

  registerReflectorPeer(peer) {
    if (!this.reflector || this.reflector.nodes.has(peer.config.relayRouterId)) return;
    this.reflector.registerFederationPeer(peer.config.relayRouterId, {
        sendRoute: (route) => this.sendControl(peer.config.relayId, {
          type: "route_update", route,
        }),
        sendWithdraw: (routeId) => this.sendControl(peer.config.relayId, {
          type: "route_withdraw", route_id: routeId,
        }),
        sendSnapshotEnd: () => true,
        fail: (error) => this.capture("federation-route-output-error", {
          peer_relay_id: peer.config.relayId, error: error.message,
        }),
      });
    this.reflector.snapshotTo(peer.config.relayRouterId, 1n);
  }

  unregisterPeer(relayId, reason = "peer-down") {
    const peer = this.peers.get(relayId);
    if (!peer) return false;
    this.peers.delete(relayId);
    if (this.reflector) this.reflector.unregisterNode(peer.config.relayRouterId);
    for (const [routerId, choices] of [...this.candidates]) {
      if (choices.delete(relayId)) {
        this.metrics.membersWithdrawn++;
        if (choices.size === 0) this.candidates.delete(routerId);
        this.selectRouter(routerId);
      }
    }
    for (const [key, call] of [...this.outbound]) {
      if (call.peerId === relayId) this.failOutbound(key, call, reason);
    }
    this.metrics.peersDown++;
    this.capture("federation-peer-down", { peer_relay_id: relayId, reason });
    return true;
  }

  peerDown(relayId, reason = "transport-down") {
    const peer = this.peers.get(relayId);
    if (!peer || !peer.ready) return false;
    peer.ready = false;
    if (this.reflector && this.reflector.nodes.has(peer.config.relayRouterId)) {
      this.reflector.unregisterNode(peer.config.relayRouterId);
    }
    for (const [routerId, choices] of [...this.candidates]) {
      if (choices.delete(relayId)) {
        this.metrics.membersWithdrawn++;
        if (choices.size === 0) this.candidates.delete(routerId);
        this.selectRouter(routerId);
      }
    }
    for (const [key, call] of [...this.outbound]) {
      if (call.peerId === relayId) this.failOutbound(key, call, reason);
    }
    this.metrics.peersDown++;
    this.capture("federation-peer-down", { peer_relay_id: relayId, reason });
    return true;
  }

  transportUp(relayId) {
    const peer = this.peers.get(relayId);
    if (!peer) return false;
    this.sendControl(relayId, {
      type: "hello", relay_id: this.relayId,
      relay_router_id: this.relayRouterId, boot_epoch: this.bootEpoch,
    });
    for (const [routerId, sequence] of this.localNodes) {
      this.sendMember(relayId, "member_up", routerId, this.relayId,
        sequence, [this.relayId]);
    }
    return true;
  }

  ping(relayId, nonce) {
    if (!Number.isSafeInteger(nonce) || nonce < 1) return false;
    return this.sendControl(relayId, { type: "ping", nonce });
  }

  peerLastActivity(relayId) {
    const peer = this.peers.get(relayId);
    return peer ? peer.lastActivity : 0;
  }

  receiveControl(relayId, message) {
    const peer = this.peers.get(relayId);
    if (!peer || !message || message.version !== FEDERATION_VERSION) {
      throw new Error("control message from unknown federation peer");
    }
    peer.lastActivity = Date.now();
    if (message.type === "hello") {
      if (message.relay_id !== relayId ||
          message.relay_router_id !== peer.config.relayRouterId ||
          typeof message.boot_epoch !== "bigint") {
        throw new Error("federation hello identity mismatch");
      }
      if (!peer.ready) {
        peer.ready = true;
        this.registerReflectorPeer(peer);
        this.metrics.peersUp++;
        this.capture("federation-peer-up", { peer_relay_id: relayId });
      }
      return true;
    }
    if (message.type === "member_up" || message.type === "member_down") {
      return this.receiveMember(peer, message);
    }
    if (message.type === "ping") {
      if (!Number.isSafeInteger(message.nonce) || message.nonce < 1) {
        throw new Error("invalid federation ping");
      }
      return this.sendControl(relayId, { type: "pong", nonce: message.nonce });
    }
    if (message.type === "pong") {
      if (!Number.isSafeInteger(message.nonce) || message.nonce < 1) {
        throw new Error("invalid federation pong");
      }
      return true;
    }
    if (message.type === "route_update") {
      if (!this.reflector) return false;
      return this.reflector.receiveFederated(peer.config.relayRouterId,
        message.route);
    }
    if (message.type === "route_withdraw") {
      if (!this.reflector) return false;
      return this.reflector.receiveFederatedWithdraw(peer.config.relayRouterId,
        message.route_id);
    }
    throw new Error("unsupported federation control message");
  }

  localNodeUp(routerId) {
    if (!validRouterId(routerId) || !this.core.nodes.has(routerId)) {
      throw new Error("local federation member is not registered in RelayCore");
    }
    const sequence = (this.memberSequences.get(routerId) || 0) + 1;
    this.memberSequences.set(routerId, sequence);
    this.localNodes.set(routerId, sequence);
    this.removeVirtual(routerId);
    for (const relayId of this.peers.keys()) {
      this.sendMember(relayId, "member_up", routerId, this.relayId,
        sequence, [this.relayId]);
    }
    return true;
  }

  prepareLocalNode(routerId) {
    if (!validRouterId(routerId)) return false;
    this.removeVirtual(routerId);
    return true;
  }

  localNodeDown(routerId) {
    const activeSequence = this.localNodes.get(routerId);
    if (!activeSequence) return false;
    const sequence = (this.memberSequences.get(routerId) || activeSequence) + 1;
    this.memberSequences.set(routerId, sequence);
    this.localNodes.delete(routerId);
    for (const relayId of this.peers.keys()) {
      this.sendMember(relayId, "member_down", routerId, this.relayId,
        sequence, [this.relayId]);
    }
    this.selectRouter(routerId);
    return true;
  }

  receiveMember(peer, message) {
    if (!validRouterId(message.router_id) || !validRouterId(message.origin_relay_id) ||
        !Number.isSafeInteger(message.sequence) || message.sequence < 1 ||
        !validPath(message.path) || message.path.at(-1) !== peer.config.relayId ||
        message.path[0] !== message.origin_relay_id) {
      throw new Error("invalid federation membership advertisement");
    }
    if (message.path.includes(this.relayId) ||
        message.path.length >= this.maxTransitHops) {
      this.metrics.memberLoopsRejected++;
      return false;
    }
    const sequenceKey = `${message.origin_relay_id}\u0000${message.router_id}`;
    const previousSequence = peer.sequences.get(sequenceKey) || 0;
    if (message.sequence <= previousSequence) return false;
    peer.sequences.set(sequenceKey, message.sequence);
    let choices = this.candidates.get(message.router_id);
    if (!choices) {
      if (this.candidates.size >= this.maxRemoteRouters) {
        throw new Error("federation remote Router limit exceeded");
      }
      choices = new Map();
      this.candidates.set(message.router_id, choices);
    }
    if (message.type === "member_up") {
      choices.set(peer.config.relayId, {
        peerId: peer.config.relayId, originRelayId: message.origin_relay_id,
        sequence: message.sequence, path: [...message.path],
        priority: peer.config.priority,
      });
      this.metrics.membersLearned++;
    } else {
      choices.delete(peer.config.relayId);
      this.metrics.membersWithdrawn++;
      if (choices.size === 0) this.candidates.delete(message.router_id);
    }
    this.selectRouter(message.router_id);
    const forwardedPath = [...message.path, this.relayId];
    for (const relayId of this.peers.keys()) {
      if (relayId !== peer.config.relayId && !forwardedPath.includes(relayId)) {
        this.sendMember(relayId, message.type, message.router_id,
          message.origin_relay_id, message.sequence, forwardedPath);
      }
    }
    return true;
  }

  selectRouter(routerId) {
    if (this.localNodes.has(routerId)) return;
    const choices = [...(this.candidates.get(routerId) || new Map()).values()];
    choices.sort((left, right) => left.path.length - right.path.length ||
      right.priority - left.priority || left.peerId.localeCompare(right.peerId));
    const next = choices[0] || null;
    const current = this.selected.get(routerId) || null;
    if (current && next && current.peerId === next.peerId &&
        current.sequence === next.sequence) return;
    this.removeVirtual(routerId);
    if (!next) return;
    if (!this.core.registerNode(routerId,
      (frame) => this.sendRemoteFrame(routerId, next.peerId, frame))) {
      throw new Error("failed to install federated virtual Router");
    }
    this.selected.set(routerId, next);
    this.metrics.virtualNodes = this.selected.size;
    this.capture("federation-member-selected", {
      router_id: routerId, peer_relay_id: next.peerId, path: next.path,
    });
  }

  removeVirtual(routerId) {
    if (!this.selected.has(routerId)) return false;
    this.core.unregisterNode(routerId);
    this.selected.delete(routerId);
    for (const [key, call] of [...this.outbound]) {
      if (call.routerId === routerId) this.failOutbound(key, call, "member-withdrawn");
    }
    this.metrics.virtualNodes = this.selected.size;
    return true;
  }

  sendRemoteFrame(routerId, peerId, frame) {
    const message = decodeFrame(frame);
    const key = `${routerId}\u0000${message.streamId}`;
    let call = this.outbound.get(key);
    if (message.type === TYPE.OPEN) {
      if (call) throw new Error("duplicate federated invoke OPEN");
      const peer = this.peers.get(peerId);
      if (!peer) return false;
      if (message.hopLimit <= 1) {
        this.metrics.hopLimitRejected++;
        this.core.receive(routerId,
          encodeReset(message.streamId, 1n, RESET.FEDERATION_HOP_LIMIT));
        return true;
      }
      call = { routerId, peerId, streamId: message.streamId, channel: null };
      const handlers = {
        onFrame: (incoming) => this.receiveRemoteFrame(key, call, incoming),
        onClose: (reason) => this.failOutbound(key, call, reason || "peer-stream-closed"),
      };
      call.channel = peer.transport.openInvoke({
        source_relay_id: this.relayId, target_router_id: routerId,
      }, handlers);
      if (!call.channel || typeof call.channel.send !== "function") return false;
      this.outbound.set(key, call);
      this.metrics.invokesOpened++;
      this.capture("federation-invoke-opened", {
        peer_relay_id: peerId, target_router_id: routerId,
      });
      frame = decrementOpenHopLimit(frame);
    } else if (!call) {
      throw new Error("unknown federated invoke stream");
    }
    const sent = call.channel.send(rewriteStreamId(frame, 1));
    if (message.type === TYPE.RESET) this.finishOutbound(key, call);
    return sent !== false;
  }

  receiveRemoteFrame(key, call, frame) {
    const message = decodeFrame(frame);
    if (message.streamId !== 1) throw new Error("invalid federated response stream id");
    this.core.receive(call.routerId, rewriteStreamId(frame, call.streamId));
    if (message.type === TYPE.RESET || message.type === TYPE.END) {
      this.finishOutbound(key, call);
    }
  }

  finishOutbound(key, call) {
    if (this.outbound.get(key) !== call) return;
    this.outbound.delete(key);
    this.metrics.invokesCompleted++;
    this.capture("federation-invoke-completed", {
      peer_relay_id: call.peerId, target_router_id: call.routerId,
    });
    if (call.channel && typeof call.channel.close === "function") call.channel.close();
  }

  failOutbound(key, call, reason) {
    if (this.outbound.get(key) !== call) return;
    this.outbound.delete(key);
    this.metrics.invokesFailed++;
    try {
      this.core.receive(call.routerId,
        encodeReset(call.streamId, 1n, RESET.NODE_DISCONNECTED));
    } catch (_error) {}
    if (call.channel && typeof call.channel.close === "function") call.channel.close();
    this.capture("federation-invoke-failed", {
      peer_relay_id: call.peerId, target_router_id: call.routerId, reason,
    });
  }

  acceptInvoke(peerId, channel) {
    if (!this.peers.has(peerId) || !channel || typeof channel.send !== "function" ||
        typeof channel.onFrame !== "function" || typeof channel.onClose !== "function") {
      throw new Error("invalid incoming federation invoke channel");
    }
    const ingressId = `fedin.${this.nextIngress++}`;
    if (!this.core.registerNode(ingressId,
      (frame) => channel.send(rewriteStreamId(frame, 1)))) {
      throw new Error("federation ingress limit exceeded");
    }
    const close = () => {
      if (!this.inbound.delete(ingressId)) return;
      this.core.unregisterNode(ingressId);
    };
    this.inbound.set(ingressId, { peerId, channel, opened: false });
    this.capture("federation-invoke-accepted", {
      peer_relay_id: peerId, ingress_id: ingressId,
    });
    channel.onFrame((frame) => {
      const message = decodeFrame(frame);
      if (message.streamId !== 1) throw new Error("invalid federation ingress stream id");
      const ingress = this.inbound.get(ingressId);
      if (!ingress) return;
      if (!ingress.opened) {
        if (message.type !== TYPE.OPEN ||
            (channel.targetRouterId &&
             message.targetRouterId !== channel.targetRouterId)) {
          throw new Error("federation ingress target mismatch");
        }
        ingress.opened = true;
      } else if (message.type === TYPE.OPEN) {
        throw new Error("duplicate federation ingress OPEN");
      }
      this.core.receive(ingressId, frame);
      if (message.type === TYPE.RESET) close();
    });
    channel.onClose(close);
    return ingressId;
  }

  sendMember(relayId, type, routerId, originRelayId, sequence, path) {
    return this.sendControl(relayId, {
      type, router_id: routerId, origin_relay_id: originRelayId, sequence, path,
    });
  }

  sendControl(relayId, fields) {
    const peer = this.peers.get(relayId);
    if (!peer) return false;
    return peer.transport.sendControl({ version: FEDERATION_VERSION, ...fields }) !== false;
  }

  snapshot() {
    return {
      relay_id: this.relayId, peers: this.peers.size,
      local_nodes: this.localNodes.size, remote_candidates: this.candidates.size,
      ...this.metrics,
    };
  }
}

module.exports = {
  FEDERATION_VERSION, MAX_CONTROL_BYTES, MAX_PATH, ControlDecoder,
  stringifyControl, parseControl, FederationBridge, validDnsName, validPath,
};
