"use strict";

const {
  TYPE, RESET, MIN_WINDOW, MAX_WINDOW, decodeFrame, rewriteStreamId,
  encodeReset, encodePing,
} = require("./relay-tunnel");

function validRouterId(value) {
  return typeof value === "string" && /^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$/.test(value);
}

function endpointKey(routerId, streamId) {
  return `${routerId}\u0000${streamId}`;
}

class RelayCore {
  constructor(options = {}) {
    this.maxNodes = options.maxNodes || 256;
    this.maxPairs = options.maxPairs || 1024;
    this.initialWindow = options.initialWindow || MIN_WINDOW;
    this.nodes = new Map();
    this.endpoints = new Map();
    this.pairs = new Set();
    this.closedEndpoints = new Set();
    this.metrics = {
      nodesRegistered: 0,
      nodesDisconnected: 0,
      pairsOpened: 0,
      pairsClosed: 0,
      framesForwarded: 0,
      targetMisses: 0,
      protocolErrors: 0,
      backpressureResets: 0,
      lateFramesIgnored: 0,
    };
    if (!Number.isInteger(this.maxNodes) || this.maxNodes < 1 || this.maxNodes > 4096 ||
        !Number.isInteger(this.maxPairs) || this.maxPairs < 1 || this.maxPairs > 65536 ||
        !Number.isInteger(this.initialWindow) || this.initialWindow < MIN_WINDOW ||
        this.initialWindow > MAX_WINDOW) {
      throw new Error("invalid RelayCore bounds");
    }
  }

  registerNode(routerId, sender) {
    if (!validRouterId(routerId) || typeof sender !== "function" ||
        this.nodes.has(routerId) || this.nodes.size >= this.maxNodes) {
      return false;
    }
    this.nodes.set(routerId, {
      routerId,
      sender,
      nextEvenStreamId: 2,
      largestOddStreamId: 0,
    });
    this.metrics.nodesRegistered++;
    return true;
  }

  unregisterNode(routerId) {
    const node = this.nodes.get(routerId);
    if (!node) return false;
    const affected = [...this.pairs].filter(
      (pair) => pair.source === node || pair.destination === node,
    );
    for (const pair of affected) {
      if (pair.source === node) {
        this.safeSend(pair.destination,
          encodeReset(pair.destinationStreamId, pair.sourceRx,
            RESET.NODE_DISCONNECTED));
      } else {
        this.safeSend(pair.source,
          encodeReset(pair.sourceStreamId, pair.destinationRx,
            RESET.NODE_DISCONNECTED));
      }
      this.releasePair(pair);
    }
    this.nodes.delete(routerId);
    const prefix = `${routerId}\u0000`;
    for (const key of this.closedEndpoints) {
      if (key.startsWith(prefix)) this.closedEndpoints.delete(key);
    }
    this.metrics.nodesDisconnected++;
    return true;
  }

  receive(routerId, frame) {
    const node = this.nodes.get(routerId);
    if (!node) throw new Error("unregistered node");
    let message;
    try {
      message = decodeFrame(frame);
      if (message.type === TYPE.PING) {
        if (!this.safeSend(node, encodePing(TYPE.PONG, message.sequence))) {
          throw new Error("PING response backpressure");
        }
        return true;
      }
      if (message.type === TYPE.PONG) return true;
      if (message.type === TYPE.OPEN) return this.openPair(node, frame, message);
      const pair = this.endpoints.get(endpointKey(routerId, message.streamId));
      if (!pair) {
        if (this.closedEndpoints.has(endpointKey(routerId, message.streamId))) {
          this.metrics.lateFramesIgnored++;
          return true;
        }
        throw new Error("unknown logical stream");
      }
      return pair.source === node
        ? this.forwardFromSource(pair, frame, message)
        : this.forwardFromDestination(pair, frame, message);
    } catch (error) {
      this.metrics.protocolErrors++;
      throw error;
    }
  }

  openPair(source, frame, message) {
    if ((message.streamId & 1) === 0 || message.streamId <= source.largestOddStreamId) {
      throw new Error("invalid or replayed Node stream id");
    }
    source.largestOddStreamId = message.streamId;
    if (this.pairs.size >= this.maxPairs) {
      this.safeSend(source, encodeReset(message.streamId, 1n, RESET.STREAM_LIMIT));
      return false;
    }
    const destination = this.nodes.get(message.targetRouterId);
    if (!destination) {
      this.metrics.targetMisses++;
      this.safeSend(source,
        encodeReset(message.streamId, 1n, RESET.TARGET_UNAVAILABLE));
      return false;
    }
    if (destination.nextEvenStreamId > 0xfffffffe) {
      this.safeSend(source, encodeReset(message.streamId, 1n, RESET.STREAM_LIMIT));
      return false;
    }
    const destinationStreamId = destination.nextEvenStreamId;
    destination.nextEvenStreamId += 2;
    const pair = {
      source,
      destination,
      sourceStreamId: message.streamId,
      destinationStreamId,
      sourceRx: 2n,
      destinationRx: 1n,
      sourceCredit: 0,
      destinationCredit: this.initialWindow,
      accepted: false,
      sourceClosed: false,
      destinationClosed: false,
    };
    this.pairs.add(pair);
    this.endpoints.set(endpointKey(source.routerId, pair.sourceStreamId), pair);
    this.endpoints.set(endpointKey(destination.routerId, pair.destinationStreamId), pair);
    if (!this.safeSend(destination, rewriteStreamId(frame, destinationStreamId))) {
      this.metrics.backpressureResets++;
      this.safeSend(source,
        encodeReset(pair.sourceStreamId, 1n, RESET.BACKPRESSURE));
      this.releasePair(pair);
      return false;
    }
    this.metrics.pairsOpened++;
    this.metrics.framesForwarded++;
    return true;
  }

  forwardFromSource(pair, frame, message) {
    if (message.sequence !== pair.sourceRx ||
        (!pair.accepted && message.type !== TYPE.RESET) ||
        message.type === TYPE.ACCEPT || message.type === TYPE.OPEN ||
        message.type === TYPE.RESPONSE_START ||
        (pair.sourceClosed && message.type !== TYPE.RESET &&
         message.type !== TYPE.WINDOW_UPDATE)) {
      throw new Error("invalid source stream state or sequence");
    }
    if (message.type === TYPE.DATA) {
      if (message.dataLength > pair.sourceCredit) throw new Error("source flow-control exceeded");
      pair.sourceCredit -= message.dataLength;
    } else if (message.type === TYPE.WINDOW_UPDATE) {
      pair.destinationCredit = this.addCredit(pair.destinationCredit, message.creditBytes);
    } else if (message.type === TYPE.END) {
      pair.sourceClosed = true;
    }
    pair.sourceRx++;
    return this.forwardOrReset(pair, pair.destination,
      rewriteStreamId(frame, pair.destinationStreamId), message, true);
  }

  forwardFromDestination(pair, frame, message) {
    if (message.sequence !== pair.destinationRx || message.type === TYPE.OPEN ||
        (pair.destinationClosed && message.type !== TYPE.RESET)) {
      throw new Error("invalid destination stream state or sequence");
    }
    if (!pair.accepted) {
      if (message.type !== TYPE.ACCEPT && message.type !== TYPE.RESET) {
        throw new Error("destination did not ACCEPT first");
      }
      if (message.type === TYPE.ACCEPT) {
        pair.accepted = true;
        pair.sourceCredit = message.creditBytes;
      }
    } else if (message.type === TYPE.ACCEPT) {
      throw new Error("duplicate ACCEPT");
    }
    if (message.type === TYPE.DATA) {
      if (message.dataLength > pair.destinationCredit) {
        throw new Error("destination flow-control exceeded");
      }
      pair.destinationCredit -= message.dataLength;
    } else if (message.type === TYPE.WINDOW_UPDATE) {
      pair.sourceCredit = this.addCredit(pair.sourceCredit, message.creditBytes);
    } else if (message.type === TYPE.END) {
      pair.destinationClosed = true;
    }
    pair.destinationRx++;
    return this.forwardOrReset(pair, pair.source,
      rewriteStreamId(frame, pair.sourceStreamId), message, false);
  }

  forwardOrReset(pair, target, frame, message, fromSource) {
    if (!this.safeSend(target, frame)) {
      this.metrics.backpressureResets++;
      const other = fromSource ? pair.source : pair.destination;
      const streamId = fromSource ? pair.sourceStreamId : pair.destinationStreamId;
      const sequence = fromSource ? pair.destinationRx : pair.sourceRx;
      this.safeSend(other, encodeReset(streamId, sequence, RESET.BACKPRESSURE));
      this.releasePair(pair);
      return false;
    }
    this.metrics.framesForwarded++;
    if (message.type === TYPE.RESET ||
        (pair.sourceClosed && pair.destinationClosed)) {
      this.releasePair(pair);
    }
    return true;
  }

  addCredit(current, increment) {
    if (increment > MAX_WINDOW - current) throw new Error("flow-control window overflow");
    return current + increment;
  }

  safeSend(node, frame) {
    try {
      return node.sender(frame) !== false;
    } catch (_error) {
      return false;
    }
  }

  releasePair(pair) {
    if (!this.pairs.delete(pair)) return;
    this.rememberClosedEndpoint(endpointKey(
      pair.source.routerId, pair.sourceStreamId));
    this.rememberClosedEndpoint(endpointKey(
      pair.destination.routerId, pair.destinationStreamId));
    this.endpoints.delete(endpointKey(pair.source.routerId, pair.sourceStreamId));
    this.endpoints.delete(endpointKey(
      pair.destination.routerId, pair.destinationStreamId));
    this.metrics.pairsClosed++;
  }

  rememberClosedEndpoint(key) {
    const limit = this.maxPairs * 2;
    if (this.closedEndpoints.size >= limit) {
      const oldest = this.closedEndpoints.values().next().value;
      if (oldest !== undefined) this.closedEndpoints.delete(oldest);
    }
    this.closedEndpoints.add(key);
  }

  snapshot() {
    return {
      nodes: this.nodes.size,
      pairs: this.pairs.size,
      ...this.metrics,
    };
  }
}

module.exports = { RelayCore, validRouterId };
