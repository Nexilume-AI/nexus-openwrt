"use strict";

const ROUTE_ID = /^[0-9a-f]{32}$/;

function boundedInteger(value, minimum, maximum, name) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new Error(`invalid ${name}`);
  }
  return value;
}

function boundedText(value, maximum, name) {
  if (typeof value !== "string" || value.length < 1 || value.length > maximum ||
      !/^[\x20-\x7e]+$/.test(value)) {
    throw new Error(`invalid ${name}`);
  }
}

function validateUpdateFields(message) {
  if (!ROUTE_ID.test(message.routeId || "")) throw new Error("invalid route id");
  boundedText(message.intent, 127, "intent");
  boundedText(message.origin, 255, "origin");
  boundedText(message.endpoint, 255, "endpoint");
  boundedText(message.tenant, 63, "tenant");
  boundedText(message.region, 31, "region");
  boundedInteger(message.capabilityVersion, 1, 0xffffffff, "capability version");
  boundedInteger(message.latencyMs, 0, 0xffffffff, "latency");
  boundedInteger(message.trustLevel, 0, 100, "trust level");
  boundedInteger(message.loadPermille, 0, 1000, "load");
  boundedInteger(message.remainingLeaseMs, 1, 3600000, "remaining lease");
  if (typeof message.costMicrounits !== "bigint" || message.costMicrounits < 0n ||
      message.costMicrounits > 0xffffffffffffffffn) {
    throw new Error("invalid route cost");
  }
}

function validateUpdate(message, ownerRouterId, relayRouterId) {
  validateUpdateFields(message);
  if (!Array.isArray(message.path) || message.path.length < 1 ||
      message.path.length > 8 || message.path.at(-1) !== ownerRouterId ||
      message.path.includes(relayRouterId) ||
      new Set(message.path).size !== message.path.length) {
    throw new Error("invalid Relay capability update");
  }
  for (const hop of message.path) boundedText(hop, 64, "capability path hop");
}

function validateFederatedUpdate(message, peerRouterId, relayRouterId) {
  validateUpdateFields(message);
  if (!Array.isArray(message.path) || message.path.length < 2 ||
      message.path.length > 8 || message.path.at(-1) !== peerRouterId ||
      message.path.includes(relayRouterId) ||
      new Set(message.path).size !== message.path.length) {
    throw new Error("invalid federated capability path");
  }
  for (const hop of message.path) boundedText(hop, 64, "capability path hop");
}

class ArpxRouteReflector {
  constructor(options) {
    this.relayRouterId = options.relayRouterId;
    this.maxRoutes = boundedInteger(options.maxRoutes || 4096, 1, 65536, "maxRoutes");
    this.now = options.now || (() => Date.now());
    this.capture = options.capture || (() => {});
    this.nodes = new Map();
    this.federationNodes = new Set();
    this.routes = new Map();
    this.federatedCandidates = new Map();
    this.counters = {
      updatesAccepted: 0, withdrawalsAccepted: 0, updatesReflected: 0,
      withdrawalsReflected: 0, snapshotsServed: 0, routesExpired: 0,
    };
    this.timer = setInterval(() => this.expire(), options.expiryIntervalMs || 1000);
    this.timer.unref();
  }

  registerNode(routerId, control) {
    if (this.nodes.has(routerId)) throw new Error("duplicate ARPX reflector node");
    this.nodes.set(routerId, control);
  }

  registerFederationPeer(relayRouterId, control) {
    this.registerNode(relayRouterId, control);
    this.federationNodes.add(relayRouterId);
  }

  unregisterNode(routerId) {
    this.nodes.delete(routerId);
    if (this.federationNodes.delete(routerId)) {
      for (const [routeId, choices] of [...this.federatedCandidates]) {
        choices.delete(routerId);
        if (choices.size === 0) this.federatedCandidates.delete(routeId);
        this.selectFederated(routeId);
      }
      return;
    }
    const withdrawn = [];
    for (const [routeId, route] of this.routes) {
      if (route.ownerRouterId === routerId) {
        this.routes.delete(routeId);
        withdrawn.push(routeId);
      }
    }
    for (const routeId of withdrawn) {
      this.broadcastWithdraw(routerId, routeId);
      this.selectFederated(routeId);
    }
    if (withdrawn.length > 0) {
      this.capture("capability-owner-disconnected", {
        router_id: routerId, routes_withdrawn: withdrawn.length,
      });
    }
  }

  receive(routerId, message) {
    if (!this.nodes.has(routerId)) throw new Error("unregistered ARPX reflector node");
    if (message.type === 2) return this.update(routerId, message);
    if (message.type === 3) return this.withdraw(routerId, message.routeId);
    throw new Error("unsupported reflected ARPX message");
  }

  update(routerId, message) {
    validateUpdate(message, routerId, this.relayRouterId);
    let existing = this.routes.get(message.routeId);
    if (existing && existing.ownerRouterId !== routerId) {
      if (!existing.federated) {
        throw new Error("capability route ownership collision");
      }
      this.routes.delete(message.routeId);
      this.broadcastWithdraw(existing.ownerRouterId, message.routeId);
      existing = null;
    }
    if (!existing && this.routes.size >= this.maxRoutes) {
      throw new Error("Relay capability RIB full");
    }
    const now = this.now();
    this.routes.set(message.routeId, {
      ownerRouterId: routerId,
      federated: false,
      message: { ...message, path: [...message.path] },
      expiresAt: now + message.remainingLeaseMs,
    });
    this.counters.updatesAccepted++;
    this.broadcastRoute(routerId, message, message.remainingLeaseMs);
    this.capture("capability-update", { router_id: routerId, route_id: message.routeId });
    return true;
  }

  receiveFederated(peerRouterId, message) {
    if (!this.federationNodes.has(peerRouterId)) {
      throw new Error("unregistered federation ARPX peer");
    }
    validateFederatedUpdate(message, peerRouterId, this.relayRouterId);
    const existing = this.routes.get(message.routeId);
    if (existing && !existing.federated) {
      throw new Error("federated capability collides with local ownership");
    }
    if (!existing && !this.federatedCandidates.has(message.routeId) &&
        this.routes.size >= this.maxRoutes) {
      throw new Error("Relay capability RIB full");
    }
    let choices = this.federatedCandidates.get(message.routeId);
    if (!choices) {
      choices = new Map();
      this.federatedCandidates.set(message.routeId, choices);
    }
    choices.set(peerRouterId, {
      ownerRouterId: peerRouterId,
      federated: true,
      message: { ...message, path: [...message.path] },
      expiresAt: this.now() + message.remainingLeaseMs,
    });
    this.counters.updatesAccepted++;
    this.selectFederated(message.routeId);
    this.capture("federated-capability-update", {
      peer_router_id: peerRouterId, route_id: message.routeId,
      path: message.path,
    });
    return true;
  }

  receiveFederatedWithdraw(peerRouterId, routeId) {
    if (!this.federationNodes.has(peerRouterId) || !ROUTE_ID.test(routeId || "")) {
      throw new Error("invalid federated capability withdraw");
    }
    const choices = this.federatedCandidates.get(routeId);
    if (!choices || !choices.delete(peerRouterId)) {
      return false;
    }
    if (choices.size === 0) this.federatedCandidates.delete(routeId);
    this.counters.withdrawalsAccepted++;
    this.selectFederated(routeId);
    this.capture("federated-capability-withdraw", {
      peer_router_id: peerRouterId, route_id: routeId,
    });
    return true;
  }

  selectFederated(routeId) {
    const existing = this.routes.get(routeId);
    if (existing && !existing.federated) return false;
    const now = this.now();
    const choices = [...(this.federatedCandidates.get(routeId) || new Map()).values()]
      .filter((candidate) => candidate.expiresAt > now);
    choices.sort((left, right) =>
      left.message.path.length - right.message.path.length ||
      left.ownerRouterId.localeCompare(right.ownerRouterId));
    const next = choices[0] || null;
    if (!next) {
      if (existing && existing.federated) {
        this.routes.delete(routeId);
        this.broadcastWithdraw(existing.ownerRouterId, routeId);
      }
      return false;
    }
    if (existing && existing.federated &&
        existing.ownerRouterId !== next.ownerRouterId) {
      this.broadcastWithdraw(existing.ownerRouterId, routeId);
    }
    this.routes.set(routeId, next);
    const remaining = Math.max(1, Math.min(3600000, next.expiresAt - now));
    this.broadcastRoute(next.ownerRouterId, next.message, remaining);
    return true;
  }

  withdraw(routerId, routeId) {
    if (!ROUTE_ID.test(routeId || "")) throw new Error("invalid capability withdraw");
    const existing = this.routes.get(routeId);
    if (!existing) return false;
    if (existing.ownerRouterId !== routerId) {
      throw new Error("capability route ownership mismatch");
    }
    this.routes.delete(routeId);
    this.counters.withdrawalsAccepted++;
    this.broadcastWithdraw(routerId, routeId);
    this.capture("capability-withdraw", { router_id: routerId, route_id: routeId });
    return true;
  }

  reflectedMessage(message, remainingLeaseMs) {
    return {
      ...message,
      remainingLeaseMs,
      path: [...message.path, this.relayRouterId],
    };
  }

  broadcastRoute(ownerRouterId, message, remainingLeaseMs) {
    const reflected = this.reflectedMessage(message, remainingLeaseMs);
    for (const [routerId, control] of this.nodes) {
      if (routerId === ownerRouterId) continue;
      try {
        if (control.sendRoute(reflected) === false) throw new Error("ARPX output backpressure");
        this.counters.updatesReflected++;
      } catch (error) {
        if (typeof control.fail === "function") control.fail(error);
      }
    }
  }

  broadcastWithdraw(ownerRouterId, routeId) {
    for (const [routerId, control] of this.nodes) {
      if (routerId === ownerRouterId) continue;
      try {
        if (control.sendWithdraw(routeId) === false) throw new Error("ARPX output backpressure");
        this.counters.withdrawalsReflected++;
      } catch (error) {
        if (typeof control.fail === "function") control.fail(error);
      }
    }
  }

  snapshotTo(routerId, snapshotId) {
    const control = this.nodes.get(routerId);
    if (!control) throw new Error("snapshot requested by unknown node");
    const now = this.now();
    for (const route of this.routes.values()) {
      if (route.ownerRouterId === routerId || route.expiresAt <= now) continue;
      const remaining = Math.max(1, Math.min(3600000, route.expiresAt - now));
      if (control.sendRoute(this.reflectedMessage(route.message, remaining)) === false) {
        throw new Error("ARPX snapshot output backpressure");
      }
      this.counters.updatesReflected++;
    }
    if (control.sendSnapshotEnd(snapshotId) === false) {
      throw new Error("ARPX snapshot output backpressure");
    }
    this.counters.snapshotsServed++;
  }

  expire() {
    const now = this.now();
    for (const [routeId, choices] of [...this.federatedCandidates]) {
      let expired = 0;
      for (const [peerRouterId, candidate] of [...choices]) {
        if (candidate.expiresAt > now) continue;
        choices.delete(peerRouterId);
        expired++;
      }
      if (choices.size === 0) this.federatedCandidates.delete(routeId);
      if (expired > 0) {
        this.counters.routesExpired += expired;
        this.selectFederated(routeId);
      }
    }
    for (const [routeId, route] of [...this.routes]) {
      if (route.federated) continue;
      if (route.expiresAt > now) continue;
      this.routes.delete(routeId);
      this.counters.routesExpired++;
      this.broadcastWithdraw(route.ownerRouterId, routeId);
      this.capture("capability-expired", {
        router_id: route.ownerRouterId, route_id: routeId,
      });
    }
  }

  snapshot() {
    return { nodes: this.nodes.size, routes: this.routes.size, ...this.counters };
  }

  close() {
    clearInterval(this.timer);
    this.nodes.clear();
    this.federationNodes.clear();
    this.routes.clear();
    this.federatedCandidates.clear();
  }
}

module.exports = {
  ArpxRouteReflector, validateUpdate, validateFederatedUpdate,
};
