#!/usr/bin/env node

"use strict";

const fs = require("fs");
const http2 = require("http2");
const net = require("net");
const path = require("path");
const { RelayCore, validRouterId } = require("./relay-core");
const { FederationBridge, validDnsName: validFederationDns } =
  require("./relay-federation");
const { FederationHttp2Network } = require("./relay-federation-http2");
const { FrameDecoder } = require("./relay-tunnel");
const { ArpxControl } = require("./arpx-control");
const { ArpxRouteReflector } = require("./arpx-reflector");
const { TicketVerifier, normalizeKeys } = require("./relay-ticket");

const TICKET_REFRESH_PATH = "/relay/ticket/refresh/v1";
const TICKET_REFRESH_TYPE = "application/vnd.nexus.relay-ticket-refresh.v1";
const { createCloudInvokeServer } = require("./cloud-invoke");

const ARPX_TYPE = "application/arpx+cbor";
const TUNNEL_TYPE = "application/vnd.nexus.relay-tunnel.v1";

function parseArgs(argv) {
  if (argv.length !== 4 || argv[2] !== "--config") {
    throw new Error("Usage: nexus-relayd.js --config /path/to/relay.json");
  }
  return argv[3];
}

function strictKeys(object, allowed, name) {
  if (!object || typeof object !== "object" || Array.isArray(object)) {
    throw new Error(`${name} must be an object`);
  }
  for (const key of Object.keys(object)) {
    if (!allowed.includes(key)) throw new Error(`unknown ${name} key: ${key}`);
  }
}

function boundedInteger(value, minimum, maximum, name) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new Error(`invalid ${name}`);
  }
  return value;
}

function validDnsName(value) {
  if (typeof value !== "string" || value.length < 1 || value.length > 253) return false;
  return value.split(".").every((label) =>
    label.length >= 1 && label.length <= 63 &&
    /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(label));
}

function loadConfig(filename) {
  const absolute = path.resolve(filename);
  const raw = fs.readFileSync(absolute, "utf8");
  if (Buffer.byteLength(raw) > 65536) throw new Error("Relay config is too large");
  const config = JSON.parse(raw.replace(/^\uFEFF/, ""));
  strictKeys(config, ["listen", "port", "tls", "relayId", "relayRouterId", "relayDomainId",
    "heartbeatMs", "maxNodes", "maxPairs", "maxQueuedBytes", "capture",
    "ticketKeys", "ticketMaxLifetimeSeconds", "ticketClockSkewSeconds",
    "maxUsedTickets", "maxCapabilityRoutes", "federation", "cloudIngress",
    "openMesh"], "config");
  strictKeys(config.tls, ["key", "cert", "ca"], "tls");
  if (net.isIP(config.listen) !== 4 ||
      !validRouterId(config.relayId) ||
      !validRouterId(config.relayRouterId) ||
      !validDnsName(config.relayDomainId)) {
    throw new Error("invalid Relay address or identity");
  }
  config.port = boundedInteger(config.port, 1, 65535, "port");
  config.heartbeatMs = boundedInteger(config.heartbeatMs || 5000, 1000, 60000,
    "heartbeatMs");
  config.maxNodes = boundedInteger(config.maxNodes || 256, 1, 4096, "maxNodes");
  config.maxPairs = boundedInteger(config.maxPairs || 1024, 1, 65536, "maxPairs");
  config.maxQueuedBytes = boundedInteger(config.maxQueuedBytes || 262144, 8192,
    16777216, "maxQueuedBytes");
  config.ticketMaxLifetimeSeconds = boundedInteger(
    config.ticketMaxLifetimeSeconds || 300, 30, 3600, "ticketMaxLifetimeSeconds");
  config.ticketClockSkewSeconds = boundedInteger(
    config.ticketClockSkewSeconds === undefined ? 5 : config.ticketClockSkewSeconds,
    0, 30, "ticketClockSkewSeconds");
  config.maxUsedTickets = boundedInteger(config.maxUsedTickets || 4096, 1, 65536,
    "maxUsedTickets");
  config.maxCapabilityRoutes = boundedInteger(config.maxCapabilityRoutes || 4096,
    1, 65536, "maxCapabilityRoutes");
  normalizeKeys(config.ticketKeys);
  if (config.openMesh === undefined) config.openMesh = { enabled: false };
  strictKeys(config.openMesh, ["enabled", "realmId"], "openMesh");
  if (typeof config.openMesh.enabled !== "boolean" ||
      (config.openMesh.enabled && !validRouterId(config.openMesh.realmId))) {
    throw new Error("invalid Open Mesh Relay configuration");
  }
  if (config.federation === undefined) {
    config.federation = { enabled: false, peers: {} };
  }
  strictKeys(config.federation, ["enabled", "maxPeers", "maxRemoteRouters",
    "maxTransitHops", "heartbeatMs", "peers"], "federation");
  if (typeof config.federation.enabled !== "boolean") {
    throw new Error("invalid federation enabled flag");
  }
  config.federation.maxPeers = boundedInteger(config.federation.maxPeers || 16,
    1, 64, "federation maxPeers");
  config.federation.maxRemoteRouters = boundedInteger(
    config.federation.maxRemoteRouters || 4096, 1, 65536,
    "federation maxRemoteRouters");
  config.federation.maxTransitHops = boundedInteger(
    config.federation.maxTransitHops || 8, 1, 8, "federation maxTransitHops");
  config.federation.heartbeatMs = boundedInteger(
    config.federation.heartbeatMs || 5000, 1000, 60000,
    "federation heartbeatMs");
  strictKeys(config.federation.peers || {},
    Object.keys(config.federation.peers || {}), "federation peers");
  const federationPeerIds = Object.keys(config.federation.peers || {});
  if (federationPeerIds.length > config.federation.maxPeers ||
      (config.federation.enabled && federationPeerIds.length < 1)) {
    throw new Error("invalid federation peer count");
  }
  for (const relayId of federationPeerIds) {
    const peer = config.federation.peers[relayId];
    strictKeys(peer, ["relayRouterId", "domainId", "certificateDns", "endpoint",
      "connectIpv4", "priority"], `federation peer ${relayId}`);
    if (!validRouterId(relayId) || relayId === config.relayId ||
        !validRouterId(peer.relayRouterId) || !validDnsName(peer.domainId) ||
        !validFederationDns(peer.certificateDns) ||
        typeof peer.endpoint !== "string" ||
        !/^https:\/\/[a-z0-9.-]+:[0-9]{1,5}\/federation\/v1$/.test(peer.endpoint) ||
        net.isIP(peer.connectIpv4) !== 4 || !Number.isInteger(peer.priority) ||
        peer.priority < 0 || peer.priority > 65535) {
      throw new Error("invalid federation peer");
    }
  }
  if (typeof config.capture !== "string" || typeof config.tls.key !== "string" ||
      typeof config.tls.cert !== "string" || typeof config.tls.ca !== "string") {
    throw new Error("invalid Relay file paths");
  }
  const base = path.dirname(absolute);
  for (const key of ["key", "cert", "ca"]) config.tls[key] = path.resolve(base, config.tls[key]);
  config.capture = path.resolve(base, config.capture);
  if (config.cloudIngress === undefined) config.cloudIngress = { enabled: false };
  strictKeys(config.cloudIngress, ["enabled", "listen", "port", "relayId",
    "sourceRouterId", "clientDns", "maxInflight", "maxBodyBytes", "timeoutMs",
    "tls", "jwt"], "cloudIngress");
  if (typeof config.cloudIngress.enabled !== "boolean") {
    throw new Error("invalid cloudIngress enabled flag");
  }
  if (config.cloudIngress.enabled) {
    const cloud = config.cloudIngress;
    strictKeys(cloud.tls, ["key", "cert", "ca"], "cloudIngress tls");
    strictKeys(cloud.jwt, ["issuer", "keyId", "publicKey", "maxReplay",
      "clockSkewSeconds"], "cloudIngress jwt");
    if (net.isIP(cloud.listen) !== 4 || !validRouterId(cloud.relayId) ||
        cloud.relayId !== config.relayId || !validRouterId(cloud.sourceRouterId) ||
        !validDnsName(cloud.clientDns) || typeof cloud.jwt.issuer !== "string" ||
        !validRouterId(cloud.jwt.keyId)) {
      throw new Error("invalid cloudIngress identity");
    }
    cloud.port = boundedInteger(cloud.port, 1, 65535, "cloudIngress port");
    cloud.maxInflight = boundedInteger(cloud.maxInflight || 64, 1, 1024,
      "cloudIngress maxInflight");
    cloud.maxBodyBytes = boundedInteger(cloud.maxBodyBytes || 65536, 1024, 1048576,
      "cloudIngress maxBodyBytes");
    cloud.timeoutMs = boundedInteger(cloud.timeoutMs || 30000, 1000, 120000,
      "cloudIngress timeoutMs");
    cloud.jwt.maxReplay = boundedInteger(cloud.jwt.maxReplay || 4096, 1, 65536,
      "cloudIngress jwt maxReplay");
    cloud.jwt.clockSkewSeconds = boundedInteger(cloud.jwt.clockSkewSeconds === undefined ? 5 : cloud.jwt.clockSkewSeconds,
      0, 30, "cloudIngress jwt clockSkewSeconds");
    for (const key of ["key", "cert", "ca"]) cloud.tls[key] = path.resolve(base, cloud.tls[key]);
    cloud.jwt.publicKey = path.resolve(base, cloud.jwt.publicKey);
  }
  return config;
}

function certificateDnsNames(certificate) {
  if (!certificate || typeof certificate.subjectaltname !== "string") return [];
  return certificate.subjectaltname.split(/,\s*/).filter((item) => item.startsWith("DNS:"))
    .map((item) => item.slice(4));
}

function resolveCertificateDnsNames(socket, openMesh = false) {
  if ((!socket.authorized && !openMesh) || socket.alpnProtocol !== "h2" ||
      socket.getProtocol() !== "TLSv1.3") return null;
  const names = certificateDnsNames(socket.getPeerCertificate(true));
  if (names.length < 1 || names.length > (openMesh ? 4 : 1) ||
      names.some((name) => !validDnsName(name))) return null;
  return names;
}

function resolveCertificateDns(socket) {
  const names = resolveCertificateDnsNames(socket, false);
  return names && names.length === 1 ? names[0] : null;
}

function createRelayServer(config) {
  fs.mkdirSync(path.dirname(config.capture), { recursive: true });
  fs.writeFileSync(config.capture, "", "utf8");
  const capture = (event, fields = {}) => {
    try {
      fs.appendFileSync(config.capture,
        `${JSON.stringify({ at: new Date().toISOString(), event, ...fields })}\n`, "utf8");
    } catch (error) {
      // Logging must never tear down active forwarding sessions.
      process.stderr.write(`nexus-relayd: capture failed: ${error.message}\n`);
    }
  };
  const core = new RelayCore({ maxNodes: config.maxNodes, maxPairs: config.maxPairs });
  const reflector = new ArpxRouteReflector({
    relayRouterId: config.relayRouterId,
    maxRoutes: config.maxCapabilityRoutes,
    capture,
  });
  const federation = config.federation.enabled ? new FederationBridge({
    relayId: config.relayId,
    relayRouterId: config.relayRouterId,
    core,
    reflector,
    maxRemoteRouters: config.federation.maxRemoteRouters,
    maxTransitHops: config.federation.maxTransitHops,
    capture,
  }) : null;
  const federationNetwork = federation ? new FederationHttp2Network({
    relayId: config.relayId,
    bridge: federation,
    peers: config.federation.peers,
    tls: config.tls,
    maxQueuedBytes: config.maxQueuedBytes,
    heartbeatMs: config.federation.heartbeatMs,
    capture,
  }) : null;
  const ticketVerifier = new TicketVerifier({
    keys: config.ticketKeys,
    relayId: config.relayId,
    maxLifetimeSeconds: config.ticketMaxLifetimeSeconds,
    clockSkewSeconds: config.ticketClockSkewSeconds,
    maxUsedTickets: config.maxUsedTickets,
  });
  const cloudIngress = config.cloudIngress.enabled
    ? createCloudInvokeServer(core, config.cloudIngress, capture) : null;
  const states = new Map();
  let nextNodeSnapshotId = 1n;
  const server = http2.createSecureServer({
    key: fs.readFileSync(config.tls.key),
    cert: fs.readFileSync(config.tls.cert),
    ca: fs.readFileSync(config.tls.ca),
    requestCert: true,
    /* Open Mesh sessions authenticate with the short-lived Directory ticket,
     * which is bound to the presented certificate SAN. Managed/Cloud Relay
     * configs omit openMesh and retain strict CA-authenticated mTLS. */
    rejectUnauthorized: !config.openMesh.enabled,
    maxHeaderSize: 8192,
    minVersion: "TLSv1.3",
    maxVersion: "TLSv1.3",
    allowHTTP1: false,
    settings: { maxConcurrentStreams: 4 },
  });

  function closeState(state, reason) {
    if (state.closed) return;
    state.closed = true;
    if (state.expiryTimer) clearTimeout(state.expiryTimer);
    if (state.reflectorActive) reflector.unregisterNode(state.identity.routerId);
    if (state.arpx) state.arpx.close();
    if (state.active) {
      core.unregisterNode(state.identity.routerId);
      if (federation) federation.localNodeDown(state.identity.routerId);
    }
    capture("node-disconnected", {
      router_id: state.identity ? state.identity.routerId : "unbound", reason,
    });
  }

  function activate(state) {
    if (state.active || !state.arpxReady || !state.tunnel || state.closed) return;
    const sender = (frame) => {
      if (state.tunnel.destroyed || state.tunnel.closed ||
          state.tunnel.writableLength + frame.length > config.maxQueuedBytes) return false;
      state.tunnel.write(frame);
      return true;
    };
    if (federation) federation.prepareLocalNode(state.identity.routerId);
    if (!core.registerNode(state.identity.routerId, sender)) {
      capture("node-registration-rejected", { router_id: state.identity.routerId });
      state.session.goaway(http2.constants.NGHTTP2_REFUSED_STREAM);
      state.session.destroy();
      return;
    }
    state.active = true;
    if (federation) federation.localNodeUp(state.identity.routerId);
    capture("node-registered", {
      router_id: state.identity.routerId,
      domain_id: state.identity.domainId,
      certificate_dns: state.identity.certificateDns,
      assignment_id: state.identity.assignmentId,
    });
  }

  function stateFor(session) {
    let state = states.get(session);
    if (state) return state;
    const certificateDnsNames = resolveCertificateDnsNames(
      session.socket, config.openMesh.enabled);
    if (!certificateDnsNames) return null;
    state = {
      session, certificateDnsNames, identity: null, ticket: null, expiryTimer: null,
      arpx: null, arpxReady: false, reflectorActive: false, tunnel: null,
      active: false, closed: false,
    };
    states.set(session, state);
    session.on("close", () => {
      closeState(state, "h2-session-closed");
      states.delete(session);
    });
    session.on("error", (error) => capture("h2-session-error", {
      router_id: state.identity ? state.identity.routerId : "unbound",
      error: error.message,
    }));
    return state;
  }

  function armTicketExpiry(state, claims) {
    if (state.expiryTimer) clearTimeout(state.expiryTimer);
    const remainingMs = Math.max(1, claims.exp * 1000 - Date.now());
    state.expiryTimer = setTimeout(() => {
      capture("ticket-expired", {
        router_id: state.identity.routerId,
        assignment_id: state.identity.assignmentId,
      });
      state.session.destroy();
    }, remainingMs);
    state.expiryTimer.unref();
  }

  function authenticateTicket(state, headers, allowRefresh = false) {
    const token = headers["nexus-relay-ticket"];
    if (typeof token !== "string") throw new Error("missing Relay session ticket");
    if (state.ticket !== null) {
      if (token === state.ticket) return;
      if (!allowRefresh) throw new Error("Relay ticket differs across streams");
      const verified = ticketVerifier.verify(token, state.certificateDnsNames);
      const claims = verified.claims;
      if (!state.identity || claims.router !== state.identity.routerId ||
          claims.domain !== state.identity.domainId ||
          claims.san !== state.identity.certificateDns ||
          claims.aid !== state.identity.assignmentId) {
        throw new Error("Relay ticket refresh identity mismatch");
      }
      ticketVerifier.bind(token, state.certificateDnsNames, state.session);
      state.ticket = token;
      armTicketExpiry(state, claims);
      capture("ticket-refreshed", {
        router_id: state.identity.routerId,
        assignment_id: state.identity.assignmentId,
      });
      return;
    }
    const claims = ticketVerifier.bind(token, state.certificateDnsNames, state.session);
    state.ticket = token;
    state.identity = {
      routerId: claims.router,
      domainId: claims.domain,
      certificateDns: claims.san,
      assignmentId: claims.aid,
    };
    armTicketExpiry(state, claims);
  }

  server.on("session", (session) => {
    const state = stateFor(session);
    if (!state) {
      capture("session-identity-rejected");
      session.goaway(http2.constants.NGHTTP2_INADEQUATE_SECURITY);
      session.destroy();
    }
  });

  server.on("stream", (stream, headers) => {
    if (federationNetwork &&
        (headers[":path"] === "/federation/v1" ||
         headers[":path"] === "/federation/invoke/v1")) {
      const certificateDns = resolveCertificateDns(stream.session.socket);
      federationNetwork.acceptStream(stream, headers, certificateDns);
      return;
    }
    const state = stateFor(stream.session);
    stream.on("error", (error) => capture("h2-stream-error", {
      router_id: state && state.identity ? state.identity.routerId : "unbound",
      path: typeof headers[":path"] === "string" ? headers[":path"] : "unknown",
      error: error.message,
    }));
    if (!state || headers[":method"] !== "POST") {
      stream.respond({ ":status": 403 });
      stream.end();
      return;
    }
    const ticketRefresh = headers[":path"] === TICKET_REFRESH_PATH &&
      headers["content-type"] === TICKET_REFRESH_TYPE &&
      headers.accept === TICKET_REFRESH_TYPE;
    try {
      authenticateTicket(state, headers, ticketRefresh && state.active);
    } catch (error) {
      capture("ticket-rejected", {
        certificate_dns: state.identity ? state.identity.certificateDns : "unbound",
        error: error.message,
      });
      stream.respond({ ":status": 401 });
      stream.end();
      state.session.destroy();
      return;
    }
    const protocolError = (error) => {
      capture("protocol-error", {
        router_id: state.identity.routerId, error: error.message,
      });
      stream.close(http2.constants.NGHTTP2_PROTOCOL_ERROR);
      state.session.destroy();
    };
    if (ticketRefresh) {
      stream.respond({ ":status": 204 });
      stream.end();
      return;
    }
    if (headers[":path"] === "/arpx/v1" && headers["content-type"] === ARPX_TYPE &&
        headers.accept === ARPX_TYPE &&
        !state.arpx) {
      stream.respond({ ":status": 200, "content-type": ARPX_TYPE });
      state.arpx = new ArpxControl({
        routerId: config.relayRouterId,
        domainId: config.relayDomainId,
        peerRouterId: state.identity.routerId,
        peerDomainId: state.identity.domainId,
        heartbeatMs: config.heartbeatMs,
        write: (frame) => {
          if (stream.destroyed || stream.closed ||
              stream.writableLength + frame.length > config.maxQueuedBytes) {
            throw new Error("ARPX output backpressure");
          }
          return stream.write(frame);
        },
        onReady: () => {
          reflector.registerNode(state.identity.routerId, state.arpx);
          state.reflectorActive = true;
          state.arpxReady = true;
          const snapshotId = nextNodeSnapshotId++;
          if (state.arpx.sendSnapshotRequest(snapshotId) === false) {
            throw new Error("ARPX Node snapshot request backpressure");
          }
          capture("node-snapshot-requested", {
            router_id: state.identity.routerId,
            snapshot_id: snapshotId.toString(),
          });
          activate(state);
        },
        onRoute: (message) => reflector.receive(state.identity.routerId, message),
        onSnapshotRequest: (snapshotId) =>
          reflector.snapshotTo(state.identity.routerId, snapshotId),
        onError: protocolError,
      });
      stream.on("data", (chunk) => state.arpx.push(chunk));
      stream.on("close", () => {
        if (!state.closed) state.session.destroy();
      });
      return;
    }
    if (headers[":path"] === "/relay/invoke/v1" &&
        headers["content-type"] === TUNNEL_TYPE && headers.accept === TUNNEL_TYPE &&
        !state.tunnel) {
      stream.respond({ ":status": 200, "content-type": TUNNEL_TYPE });
      state.tunnel = stream;
      const decoder = new FrameDecoder((frame) => {
        try {
          if (!state.active) throw new Error("tunnel data before ARPX registration");
          core.receive(state.identity.routerId, frame);
        } catch (error) {
          protocolError(error);
        }
      });
      stream.on("data", (chunk) => {
        try { decoder.push(chunk); } catch (error) { protocolError(error); }
      });
      stream.on("close", () => {
        if (!state.closed) state.session.destroy();
      });
      activate(state);
      return;
    }
    stream.respond({ ":status": 400 });
    stream.end();
  });

  server.on("tlsClientError", (error) => capture("tls-client-error", {
    error: error.message,
  }));
  server.on("error", (error) => capture("server-error", { error: error.message }));
  let stopping = false;
  const shutdown = (callback) => {
    if (stopping) {
      if (typeof callback === "function") {
        if (server.listening) server.once("close", callback);
        else callback();
      }
      return;
    }
    stopping = true;
    if (federationNetwork) federationNetwork.stop();
    for (const state of states.values()) state.session.destroy();
    reflector.close();
    let pending = cloudIngress && cloudIngress.server.listening ? 2 : 1;
    const closed = () => {
      pending--;
      if (pending === 0 && typeof callback === "function") callback();
    };
    server.close(closed);
    if (cloudIngress && cloudIngress.server.listening) cloudIngress.close(closed);
  };
  return {
    server, core, reflector, federation, federationNetwork, ticketVerifier,
    cloudIngress,
    capture, startFederation: () => {
      if (federationNetwork) federationNetwork.start();
    }, shutdown,
  };
}

function main() {
  const config = loadConfig(parseArgs(process.argv));
  const runtime = createRelayServer(config);
  runtime.server.listen(config.port, config.listen, () => {
    runtime.startFederation();
    runtime.capture("listening", { address: config.listen, port: config.port });
    process.stdout.write(`Nexus Relay P6.2.3 listening on ${config.listen}:${config.port}\n`);
  });
  if (runtime.cloudIngress) {
    runtime.cloudIngress.server.listen(config.cloudIngress.port,
      config.cloudIngress.listen, () => runtime.capture("cloud-invoke-listening", {
        address: config.cloudIngress.listen, port: config.cloudIngress.port,
      }));
  }
  const shutdown = () => {
    runtime.shutdown(() => process.exit(0));
    setTimeout(() => process.exit(1), 5000).unref();
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

if (require.main === module) {
  try { main(); } catch (error) {
    process.stderr.write(`nexus-relayd: ${error.message}\n`);
    process.exit(1);
  }
}

module.exports = { loadConfig, resolveCertificateDns, createRelayServer };
