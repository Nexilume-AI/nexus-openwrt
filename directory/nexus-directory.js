#!/usr/bin/env node

"use strict";

const crypto = require("crypto");
const fs = require("fs");
const https = require("https");
const net = require("net");
const path = require("path");
const {
  issueTicket, normalizeKeys, validIdentifier, validRouteId, validDnsName,
} = require("../relay/relay-ticket");

const ASSIGNMENT_TYPE = "application/vnd.nexus.relay-assignment+json";
const CARD_TRUST_TYPE = "application/vnd.nexus.agent-card-trust+json";
const MAX_REQUEST = 1024;

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

function loadConfig(filename) {
  const absolute = path.resolve(filename);
  const raw = fs.readFileSync(absolute, "utf8");
  if (Buffer.byteLength(raw) > 65536) throw new Error("Directory config is too large");
  const config = JSON.parse(raw);
  strictKeys(config, ["listen", "port", "tls", "directoryId",
    "activeTicketKeyId", "ticketKeys",
    "ticketTtlSeconds", "maxConnections", "capture", "relays", "identities",
    "cardTrust", "openMesh"], "config");
  strictKeys(config.tls, ["key", "cert", "ca"], "tls");
  if (net.isIP(config.listen) === 0 ||
      !validIdentifier(config.activeTicketKeyId, 32)) throw new Error("invalid Directory identity");
  config.directoryId = config.directoryId || "directory-standalone";
  if (!validIdentifier(config.directoryId, 64)) throw new Error("invalid Directory replica ID");
  config.port = boundedInteger(config.port, 1, 65535, "port");
  config.ticketTtlSeconds = boundedInteger(config.ticketTtlSeconds || 120, 30, 300,
    "ticketTtlSeconds");
  config.maxConnections = boundedInteger(config.maxConnections || 256, 1, 4096,
    "maxConnections");
  const keys = normalizeKeys(config.ticketKeys);
  if (!keys.has(config.activeTicketKeyId)) throw new Error("active ticket key is unavailable");
  if (typeof config.capture !== "string" || typeof config.tls.key !== "string" ||
      typeof config.tls.cert !== "string" || typeof config.tls.ca !== "string") {
    throw new Error("invalid Directory file paths");
  }
  const base = path.dirname(absolute);
  for (const key of ["key", "cert", "ca"]) config.tls[key] = path.resolve(base, config.tls[key]);
  config.capture = path.resolve(base, config.capture);
  if (config.cardTrust !== undefined) {
    strictKeys(config.cardTrust, ["manifest", "signature"], "cardTrust");
    if (typeof config.cardTrust.manifest !== "string" ||
        typeof config.cardTrust.signature !== "string") {
      throw new Error("invalid cardTrust file paths");
    }
    config.cardTrust.manifest = path.resolve(base, config.cardTrust.manifest);
    config.cardTrust.signature = path.resolve(base, config.cardTrust.signature);
    const manifestSize = fs.statSync(config.cardTrust.manifest).size;
    const signatureSize = fs.statSync(config.cardTrust.signature).size;
    if (manifestSize < 2 || manifestSize > 65536 ||
        signatureSize < 2 || signatureSize > 4096) {
      throw new Error("invalid cardTrust file size");
    }
  }

  strictKeys(config.relays, Object.keys(config.relays || {}), "relays");
  for (const [relayId, relay] of Object.entries(config.relays)) {
    strictKeys(relay, ["routerId", "domainId", "endpoint", "connectIpv4"], `relay ${relayId}`);
    if (!validRouteId(relayId) || !validRouteId(relay.routerId) ||
        !validDnsName(relay.domainId) || typeof relay.endpoint !== "string" ||
        !/^https:\/\/[a-z0-9.-]+:[0-9]{1,5}\/arpx\/v1$/.test(relay.endpoint) ||
        net.isIP(relay.connectIpv4) !== 4) {
      throw new Error("invalid Relay assignment target");
    }
  }
  if (Object.keys(config.relays).length < 1 || Object.keys(config.relays).length > 32) {
    throw new Error("invalid Relay target count");
  }

  config.identities = config.identities || {};
  strictKeys(config.identities, Object.keys(config.identities), "identities");
  for (const [san, identity] of Object.entries(config.identities)) {
    strictKeys(identity, ["routerId", "domainId", "relayId", "relayIds",
      "assignmentId", "leaseSeconds"],
      `identity ${san}`);
    const hasSingle = typeof identity.relayId === "string";
    const hasSet = Array.isArray(identity.relayIds);
    if (hasSingle === hasSet) throw new Error("identity requires relayId or relayIds");
    const relayIds = hasSet ? identity.relayIds : [identity.relayId];
    if (!validDnsName(san) || !validRouteId(identity.routerId) ||
        !validDnsName(identity.domainId) || relayIds.length < 1 || relayIds.length > 4 ||
        new Set(relayIds).size !== relayIds.length ||
        relayIds.some((relayId) => !validRouteId(relayId) ||
          !Object.hasOwn(config.relays, relayId)) ||
        !validIdentifier(identity.assignmentId) || !Number.isInteger(identity.leaseSeconds) ||
        identity.leaseSeconds < 30 || identity.leaseSeconds > 3600 ||
        config.ticketTtlSeconds > identity.leaseSeconds) {
      throw new Error("invalid Directory Node identity");
    }
    identity.relayIds = relayIds;
  }
  if (config.openMesh === undefined) config.openMesh = { enabled: false };
  strictKeys(config.openMesh, ["enabled", "realmId", "relayIds", "leaseSeconds"],
    "openMesh");
  if (typeof config.openMesh.enabled !== "boolean") {
    throw new Error("invalid Open Mesh Directory flag");
  }
  if (config.openMesh.enabled) {
    if (!validIdentifier(config.openMesh.realmId) ||
        !Array.isArray(config.openMesh.relayIds) ||
        config.openMesh.relayIds.length < 1 || config.openMesh.relayIds.length > 4 ||
        new Set(config.openMesh.relayIds).size !== config.openMesh.relayIds.length ||
        config.openMesh.relayIds.some((relayId) => !validRouteId(relayId) ||
          !Object.hasOwn(config.relays, relayId)) ||
        !Number.isInteger(config.openMesh.leaseSeconds) ||
        config.openMesh.leaseSeconds < 30 || config.openMesh.leaseSeconds > 3600 ||
        config.ticketTtlSeconds > config.openMesh.leaseSeconds) {
      throw new Error("invalid Open Mesh Directory realm");
    }
  }
  if ((!config.openMesh.enabled && Object.keys(config.identities).length < 1) ||
      Object.keys(config.identities).length > config.maxConnections) {
    throw new Error("invalid Directory identity count");
  }
  return config;
}

function certificateDns(socket) {
  const certificate = socket.getPeerCertificate(true);
  if (!socket.authorized || !certificate || typeof certificate.subjectaltname !== "string") {
    return null;
  }
  const names = certificate.subjectaltname.split(/,\s*/)
    .filter((item) => item.startsWith("DNS:")).map((item) => item.slice(4));
  return names.length === 1 && validDnsName(names[0]) ? names[0] : null;
}

function openMeshCertificateNames(socket) {
  const certificate = socket.getPeerCertificate(true);
  if (!certificate || typeof certificate.subjectaltname !== "string") return [];
  const names = certificate.subjectaltname.split(/,\s*/)
    .filter((item) => item.startsWith("DNS:")).map((item) => item.slice(4));
  return names.length >= 1 && names.length <= 4 && names.every(validDnsName)
    ? names : [];
}

function dynamicOpenMeshIdentity(config, body, certificateNames) {
  if (!body || body.version !== 1 || !validRouteId(body.router_id) ||
      !validDnsName(body.domain_id)) return null;
  const san = `${body.router_id}.${body.domain_id}`;
  if (!certificateNames.includes(san)) return null;
  const digest = crypto.createHash("sha256")
    .update(`${config.openMesh.realmId}\0${san}`, "utf8").digest("hex").slice(0, 32);
  return {
    san,
    identity: {
      routerId: body.router_id,
      domainId: body.domain_id,
      relayIds: config.openMesh.relayIds,
      assignmentId: `mesh-${digest}`,
      leaseSeconds: config.openMesh.leaseSeconds,
      meshRealm: config.openMesh.realmId,
    },
  };
}

function createDirectoryServer(config) {
  fs.mkdirSync(path.dirname(config.capture), { recursive: true });
  fs.writeFileSync(config.capture, "", "utf8");
  const capture = (event, fields = {}) => {
    try {
      fs.appendFileSync(config.capture,
        `${JSON.stringify({ at: new Date().toISOString(), event, ...fields })}\n`);
    } catch (error) {
      process.stderr.write(`nexus-directory: capture failed: ${error.message}\n`);
    }
  };
  const server = https.createServer({
    key: fs.readFileSync(config.tls.key),
    cert: fs.readFileSync(config.tls.cert),
    ca: fs.readFileSync(config.tls.ca),
    requestCert: true,
    // The assignment path still requires socket.authorized and an exact SAN.
    // A signed Card trust bundle is public metadata and may be fetched without
    // a client certificate, while retaining normal HTTPS server authentication.
    rejectUnauthorized: false,
    minVersion: "TLSv1.3",
    maxVersion: "TLSv1.3",
    maxHeaderSize: 8192,
  }, (request, response) => {
    let san = certificateDns(request.socket);
    let identity = san === null ? null : config.identities[san];
    const openMeshRequest = config.openMesh.enabled &&
      request.url === "/v1/open-mesh/assignment";
    const openMeshNames = openMeshRequest
      ? openMeshCertificateNames(request.socket) : [];
    if (config.cardTrust && request.method === "GET" &&
        (request.url === "/v1/agent-card-trust" ||
         request.url === "/v1/agent-card-trust.sig")) {
      const signature = request.url.endsWith(".sig");
      const body = fs.readFileSync(signature ? config.cardTrust.signature :
        config.cardTrust.manifest);
      capture("card-trust-served", {
        certificate_dns: san, artifact: signature ? "signature" : "manifest",
      });
      response.writeHead(200, {
        "Content-Type": signature ? "application/octet-stream" : CARD_TRUST_TYPE,
        "Content-Length": body.length,
        "Cache-Control": "no-store",
        Connection: "close",
      });
      response.end(body);
      return;
    }
    const length = Number(request.headers["content-length"]);
    if ((!identity && !openMeshRequest) ||
        (openMeshRequest && openMeshNames.length === 0) ||
        request.method !== "POST" ||
        (!openMeshRequest && request.url !== "/v1/relay-assignment") ||
        request.headers.accept !== ASSIGNMENT_TYPE ||
        request.headers["content-type"] !== "application/json" ||
        (openMeshRequest && request.headers.authorization !== undefined) ||
        request.headers["transfer-encoding"] !== undefined || !Number.isInteger(length) ||
        length < 1 || length > MAX_REQUEST) {
      capture("request-rejected", { certificate_dns: san || "unbound" });
      response.writeHead(identity ? 400 : 403, { Connection: "close" });
      response.end();
      return;
    }
    const chunks = [];
    let used = 0;
    request.on("data", (chunk) => {
      used += chunk.length;
      if (used > length || used > MAX_REQUEST) request.destroy();
      else chunks.push(chunk);
    });
    request.on("end", () => {
      if (used !== length) return;
      let body;
      try { body = JSON.parse(Buffer.concat(chunks).toString("utf8")); }
      catch (_error) { body = null; }
      const bodyKeys = body && !Array.isArray(body) ? Object.keys(body) : [];
      const legacy = bodyKeys.length === 3;
      const ha = bodyKeys.length === 5 &&
        bodyKeys.includes("current_relay_id") && bodyKeys.includes("failed_relay_id");
      if (openMeshRequest && body && !Array.isArray(body)) {
        const dynamic = dynamicOpenMeshIdentity(config, body, openMeshNames);
        identity = dynamic && dynamic.identity;
        san = dynamic && dynamic.san;
      }
      if (!body || Array.isArray(body) || !identity || (!legacy && !ha) ||
          body.version !== 1 || body.router_id !== identity.routerId ||
          body.domain_id !== identity.domainId ||
          (ha && (typeof body.current_relay_id !== "string" ||
            typeof body.failed_relay_id !== "string" ||
            (body.current_relay_id !== "" && !validRouteId(body.current_relay_id)) ||
            (body.failed_relay_id !== "" && !validRouteId(body.failed_relay_id))))) {
        capture("request-identity-rejected", { certificate_dns: san });
        response.writeHead(403, { Connection: "close" });
        response.end();
        return;
      }
      const currentRelayId = ha ? body.current_relay_id : "";
      const failedRelayId = ha ? body.failed_relay_id : "";
      let relayId = identity.relayIds.includes(currentRelayId) &&
          currentRelayId !== failedRelayId ? currentRelayId :
        identity.relayIds.find((candidate) => candidate !== failedRelayId);
      if (!relayId) {
        capture("assignment-unavailable", {
          certificate_dns: san, router_id: identity.routerId,
          failed_relay_id: failedRelayId,
        });
        response.writeHead(503, { Connection: "close", "Retry-After": "1" });
        response.end();
        return;
      }
      const relay = config.relays[relayId];
      const now = Math.floor(Date.now() / 1000);
      const ticket = issueTicket({
        keys: config.ticketKeys,
        kid: config.activeTicketKeyId,
        nowSeconds: now,
        maxLifetimeSeconds: config.ticketTtlSeconds,
        claims: {
          v: 1, aid: identity.assignmentId, relay: relayId,
          router: identity.routerId, domain: identity.domainId, san,
          iat: now, exp: now + config.ticketTtlSeconds,
          nonce: crypto.randomBytes(16).toString("hex"),
        },
      });
      const assignment = Buffer.from(JSON.stringify({
        version: 1,
        assignment_id: identity.assignmentId,
        relay_id: relayId,
        relay_router_id: relay.routerId,
        relay_domain_id: relay.domainId,
        relay_endpoint: relay.endpoint,
        connect_ipv4: relay.connectIpv4,
        session_ticket: ticket,
        lease_seconds: identity.leaseSeconds,
      }), "ascii");
      capture("assignment-issued", {
        certificate_dns: san, router_id: identity.routerId,
        assignment_id: identity.assignmentId, relay_id: relayId,
        directory_id: config.directoryId,
        mesh_realm: identity.meshRealm || "managed",
        ticket_sha256: crypto.createHash("sha256").update(ticket).digest("hex"),
      });
      response.writeHead(200, {
        "Content-Type": ASSIGNMENT_TYPE,
        "Content-Length": assignment.length,
        "X-Nexus-Directory-Id": config.directoryId,
        ...(identity.meshRealm ? { "X-Nexus-Mesh-Realm": identity.meshRealm } : {}),
        Connection: "close",
      });
      response.end(assignment);
    });
  });
  server.maxConnections = config.maxConnections;
  server.on("tlsClientError", (error) => capture("tls-client-error", { error: error.message }));
  server.on("error", (error) => capture("server-error", { error: error.message }));
  return { server, capture, shutdown: (callback) => server.close(callback) };
}

function main() {
  if (process.argv.length !== 4 || process.argv[2] !== "--config") {
    throw new Error("Usage: nexus-directory.js --config /path/to/directory.json");
  }
  const config = loadConfig(process.argv[3]);
  const runtime = createDirectoryServer(config);
  runtime.server.listen(config.port, config.listen, () => {
    runtime.capture("listening", { address: config.listen, port: config.port });
    process.stdout.write(`Nexus Directory P6.2.4 listening on ${config.listen}:${config.port}\n`);
  });
  const shutdown = () => runtime.shutdown(() => process.exit(0));
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

if (require.main === module) {
  try { main(); } catch (error) {
    process.stderr.write(`nexus-directory: ${error.message}\n`);
    process.exit(1);
  }
}

module.exports = { ASSIGNMENT_TYPE, CARD_TRUST_TYPE, loadConfig, createDirectoryServer };
