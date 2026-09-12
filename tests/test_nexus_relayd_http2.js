"use strict";

const assert = require("assert");
const fs = require("fs");
const http2 = require("http2");
const https = require("https");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");

const { createRelayServer, loadConfig } = require("../relay/nexus-relayd");
const {
  ASSIGNMENT_TYPE,
  CARD_TRUST_TYPE,
  createDirectoryServer,
  loadConfig: loadDirectoryConfig,
} = require("../directory/nexus-directory");
const {
  ArpxDecoder, encodeArpx, encodeArpxMessage,
} = require("../relay/arpx-control");
const {
  TYPE, FrameDecoder, decodeFrame, encodeOpen, encodeAccept, encodeData,
  encodeEnd, encodeResponseStart,
} = require("../relay/relay-tunnel");

const TICKET_SECRET = require("crypto").randomBytes(48).toString("base64url");

function openssl(args, directory) {
  const result = spawnSync("openssl", args, {
    cwd: directory,
    env: { ...process.env, OPENSSL_CONF: path.join(directory, "openssl.cnf") },
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.status !== 0) {
    throw new Error(`openssl ${args[0]} failed: ${result.stderr}`);
  }
}

function issueCertificate(directory, name, identity, usage) {
  openssl(["req", "-newkey", "rsa:2048", "-sha256", "-nodes",
    "-subj", `/CN=${identity}`, "-keyout", `${name}.key`, "-out", `${name}.csr`],
  directory);
  fs.writeFileSync(path.join(directory, `${name}.ext`),
    `subjectAltName=DNS:${identity}\nextendedKeyUsage=${usage}\n` +
    "keyUsage=digitalSignature,keyEncipherment\n");
  openssl(["x509", "-req", "-sha256", "-days", "2", "-in", `${name}.csr`,
    "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
    "-extfile", `${name}.ext`, "-out", `${name}.pem`], directory);
}

function issueOpenMeshCertificate(directory, name, routerId, domainId) {
  const identity = `${routerId}.${domainId}`;
  openssl(["req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
    "-days", "2", "-subj", `/CN=${identity}`,
    "-addext", `subjectAltName=DNS:${identity},DNS:${routerId}.local`,
    "-addext", "extendedKeyUsage=clientAuth",
    "-keyout", `${name}.key`, "-out", `${name}.pem`], directory);
}

function createCertificates(directory) {
  fs.writeFileSync(path.join(directory, "openssl.cnf"),
    "[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=unused\n");
  openssl(["req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
    "-days", "2", "-subj", "/CN=Nexus P4.2 Test CA", "-keyout", "ca.key",
    "-addext", "basicConstraints=critical,CA:TRUE",
    "-addext", "keyUsage=critical,keyCertSign,cRLSign",
    "-out", "ca.pem"], directory);
  issueCertificate(directory, "relay", "relay.p42.test", "serverAuth");
  issueCertificate(directory, "directory", "directory.p43.test", "serverAuth");
  issueCertificate(directory, "router-a", "router-a.p42.test", "clientAuth");
  issueCertificate(directory, "router-b", "router-b.p42.test", "clientAuth");
  issueOpenMeshCertificate(directory, "router-c", "router-c", "mesh.local");
}

function waitFor(predicate, message, timeoutMs = 5000) {
  const started = Date.now();
  return new Promise((resolve, reject) => {
    const poll = () => {
      try {
        if (predicate()) return resolve();
      } catch (error) {
        return reject(error);
      }
      if (Date.now() - started >= timeoutMs) return reject(new Error(message));
      setTimeout(poll, 10);
    };
    poll();
  });
}

function connectNode(directory, port, name, routerId, domainId, ticket,
  tunnelTicket = ticket) {
  const session = http2.connect(`https://relay.p42.test:${port}`, {
    ca: fs.readFileSync(path.join(directory, "ca.pem")),
    cert: fs.readFileSync(path.join(directory, `${name}.pem`)),
    key: fs.readFileSync(path.join(directory, `${name}.key`)),
    servername: "relay.p42.test",
    minVersion: "TLSv1.3",
    maxVersion: "TLSv1.3",
    lookup: (_hostname, options, callback) => options && options.all
      ? callback(null, [{ address: "127.0.0.1", family: 4 }])
      : callback(null, "127.0.0.1", 4),
  });
  const errors = [];
  session.on("error", (error) => errors.push(error));
  const arpxHeaders = {
    ":method": "POST",
    ":path": "/arpx/v1",
    "content-type": "application/arpx+cbor",
    accept: "application/arpx+cbor",
  };
  const tunnelHeaders = {
    ":method": "POST",
    ":path": "/relay/invoke/v1",
    "content-type": "application/vnd.nexus.relay-tunnel.v1",
    accept: "application/vnd.nexus.relay-tunnel.v1",
  };
  if (ticket !== null) arpxHeaders["nexus-relay-ticket"] = ticket;
  if (tunnelTicket !== null) tunnelHeaders["nexus-relay-ticket"] = tunnelTicket;
  const arpx = session.request(arpxHeaders);
  const tunnel = session.request(tunnelHeaders);
  const headers = { arpx: null, tunnel: null };
  const tunnelFrames = [];
  arpx.on("error", (error) => errors.push(error));
  tunnel.on("error", (error) => errors.push(error));
  arpx.on("response", (value) => { headers.arpx = value; });
  const arpxMessages = [];
  const arpxDecoder = new ArpxDecoder((message) => arpxMessages.push(message));
  arpx.on("data", (chunk) => arpxDecoder.push(chunk));
  tunnel.on("response", (value) => { headers.tunnel = value; });
  const decoder = new FrameDecoder((frame) => tunnelFrames.push(Buffer.from(frame)));
  tunnel.on("data", (chunk) => decoder.push(chunk));
  const bootEpoch = BigInt(Date.now()) * 10n +
    BigInt(routerId === "router-a" ? 1 : 2);
  arpx.write(encodeArpx(1, routerId, domainId, bootEpoch, 1n, 5000));
  return {
    session, arpx, arpxMessages, bootEpoch, tunnel, tunnelFrames, headers, errors,
  };
}

function refreshNodeTicket(client, ticket) {
  return new Promise((resolve, reject) => {
    const request = client.session.request({
      ":method": "POST",
      ":path": "/relay/ticket/refresh/v1",
      "content-type": "application/vnd.nexus.relay-ticket-refresh.v1",
      accept: "application/vnd.nexus.relay-ticket-refresh.v1",
      "nexus-relay-ticket": ticket,
    });
    let responseHeaders = null;
    request.on("response", (headers) => { responseHeaders = headers; });
    request.on("error", reject);
    request.on("data", () => reject(new Error("ticket refresh returned a body")));
    request.on("end", () => resolve(responseHeaders));
    request.end();
  });
}

function fetchAssignment(directory, port, name, routerId, domainId,
  relayState = null, assignmentPath = "/v1/relay-assignment") {
  const requestBody = {
    version: 1, router_id: routerId, domain_id: domainId,
  };
  if (relayState !== null) {
    requestBody.current_relay_id = relayState.current;
    requestBody.failed_relay_id = relayState.failed;
  }
  const body = Buffer.from(JSON.stringify(requestBody));
  return new Promise((resolve, reject) => {
    const options = {
      hostname: "directory.p43.test",
      port,
      path: assignmentPath,
      method: "POST",
      ca: fs.readFileSync(path.join(directory, "ca.pem")),
      servername: "directory.p43.test",
      minVersion: "TLSv1.3",
      maxVersion: "TLSv1.3",
      agent: false,
      lookup: (_hostname, options, callback) => options && options.all
        ? callback(null, [{ address: "127.0.0.1", family: 4 }])
        : callback(null, "127.0.0.1", 4),
      headers: {
        Accept: ASSIGNMENT_TYPE,
        "Content-Type": "application/json",
        "Content-Length": body.length,
        Connection: "close",
      },
    };
    if (name !== null) {
      options.cert = fs.readFileSync(path.join(directory, `${name}.pem`));
      options.key = fs.readFileSync(path.join(directory, `${name}.key`));
    }
    const request = https.request(options, (response) => {
      const chunks = [];
      let used = 0;
      response.on("data", (chunk) => {
        used += chunk.length;
        if (used > 4096) request.destroy(new Error("Directory response too large"));
        else chunks.push(chunk);
      });
      response.on("end", () => {
        if (response.statusCode !== 200 || response.headers["content-type"] !== ASSIGNMENT_TYPE) {
          reject(new Error(`Directory rejected Node: ${response.statusCode}`));
          return;
        }
        try { resolve(JSON.parse(Buffer.concat(chunks).toString("utf8"))); }
        catch (error) { reject(error); }
      });
    });
    request.on("error", reject);
    request.end(body);
  });
}

function fetchCardTrust(directory, port, name, signature = false) {
  return new Promise((resolve, reject) => {
    const options = {
      hostname: "directory.p43.test",
      port,
      path: signature ? "/v1/agent-card-trust.sig" : "/v1/agent-card-trust",
      method: "GET",
      ca: fs.readFileSync(path.join(directory, "ca.pem")),
      servername: "directory.p43.test",
      minVersion: "TLSv1.3",
      maxVersion: "TLSv1.3",
      agent: false,
      lookup: (_hostname, options, callback) => options && options.all
        ? callback(null, [{ address: "127.0.0.1", family: 4 }])
        : callback(null, "127.0.0.1", 4),
      headers: { Connection: "close" },
    };
    if (name !== null) {
      options.cert = fs.readFileSync(path.join(directory, `${name}.pem`));
      options.key = fs.readFileSync(path.join(directory, `${name}.key`));
    }
    const request = https.request(options, (response) => {
      const chunks = [];
      response.on("data", (chunk) => chunks.push(chunk));
      response.on("end", () => {
        if (response.statusCode !== 200) {
          reject(new Error(`Directory trust rejected: ${response.statusCode}`));
          return;
        }
        resolve({ type: response.headers["content-type"],
          body: Buffer.concat(chunks) });
      });
    });
    request.on("error", reject);
    request.end();
  });
}

async function closeRuntime(runtime, directoryRuntime, clients, directory) {
  if (directoryRuntime) {
    await new Promise((resolve) => directoryRuntime.shutdown(resolve));
  }
  await new Promise((resolve) => runtime.shutdown(resolve));
  for (const client of clients) {
    try { client.session.destroy(); } catch (_error) {}
  }
  await waitFor(() => runtime.core.snapshot().nodes === 0,
    "Relay did not observe client session shutdown");
  fs.rmSync(directory, { recursive: true, force: true });
}

async function main() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "nexus-relayd-h2-"));
  createCertificates(directory);
  const configPath = path.join(directory, "relay.json");
  fs.writeFileSync(configPath, JSON.stringify({
    listen: "127.0.0.1",
    port: 7444,
    tls: { key: "relay.key", cert: "relay.pem", ca: "ca.pem" },
    relayId: "relay-p43-1",
    relayRouterId: "router-relay-p42",
    relayDomainId: "relay.p42.test",
    heartbeatMs: 5000,
    maxNodes: 4,
    maxPairs: 8,
    maxQueuedBytes: 32768,
    ticketMaxLifetimeSeconds: 300,
    ticketClockSkewSeconds: 5,
    maxUsedTickets: 16,
    ticketKeys: { current: TICKET_SECRET },
    openMesh: { enabled: true, realmId: "seed-east-default" },
    capture: "relay-events.jsonl",
  }));
  const runtime = createRelayServer(loadConfig(configPath));
  await new Promise((resolve, reject) => {
    runtime.server.once("error", reject);
    runtime.server.listen(0, "127.0.0.1", resolve);
  });
  const port = runtime.server.address().port;
  const directoryConfigPath = path.join(directory, "directory.json");
  const trustManifest = Buffer.from('{"directory_id":"directory://test","revision":1}');
  const trustSignature = Buffer.from("untrusted comment: test\nRWTESIGNATURE\n");
  fs.writeFileSync(path.join(directory, "agent-card-trust.json"), trustManifest);
  fs.writeFileSync(path.join(directory, "agent-card-trust.json.sig"), trustSignature);
  fs.writeFileSync(directoryConfigPath, JSON.stringify({
    listen: "127.0.0.1",
    port: 8443,
    tls: { key: "directory.key", cert: "directory.pem", ca: "ca.pem" },
    activeTicketKeyId: "current",
    ticketKeys: { current: TICKET_SECRET },
    ticketTtlSeconds: 120,
    maxConnections: 8,
    capture: "directory-events.jsonl",
    cardTrust: {
      manifest: "agent-card-trust.json",
      signature: "agent-card-trust.json.sig",
    },
    relays: {
      "relay-p43-1": {
        routerId: "router-relay-p42",
        domainId: "relay.p42.test",
        endpoint: `https://relay.p42.test:${port}/arpx/v1`,
        connectIpv4: "127.0.0.1",
      },
      "relay-p43-2": {
        routerId: "router-relay-p43-2",
        domainId: "relay.p42.test",
        endpoint: "https://relay-2.p42.test:7445/arpx/v1",
        connectIpv4: "127.0.0.2",
      },
    },
    identities: {
      "router-a.p42.test": {
        routerId: "router-a", domainId: "site-a.example",
        relayIds: ["relay-p43-1", "relay-p43-2"],
        assignmentId: "assign-router-a", leaseSeconds: 300,
      },
      "router-b.p42.test": {
        routerId: "router-b", domainId: "site-b.example", relayId: "relay-p43-1",
        assignmentId: "assign-router-b", leaseSeconds: 300,
      },
    },
    openMesh: {
      enabled: true,
      realmId: "seed-east-default",
      relayIds: ["relay-p43-1"],
      leaseSeconds: 300,
    },
  }));
  const directoryRuntime = createDirectoryServer(loadDirectoryConfig(directoryConfigPath));
  await new Promise((resolve, reject) => {
    directoryRuntime.server.once("error", reject);
    directoryRuntime.server.listen(0, "127.0.0.1", resolve);
  });
  const directoryPort = directoryRuntime.server.address().port;
  const servedTrust = await fetchCardTrust(directory, directoryPort, null);
  const servedTrustSignature = await fetchCardTrust(directory, directoryPort,
    "router-a", true);
  assert.strictEqual(servedTrust.type, CARD_TRUST_TYPE);
  assert.deepStrictEqual(servedTrust.body, trustManifest);
  assert.deepStrictEqual(servedTrustSignature.body, trustSignature);
  const assignmentA = await fetchAssignment(directory, directoryPort,
    "router-a", "router-a", "site-a.example");
  await assert.rejects(
    fetchAssignment(directory, directoryPort, null, "router-a",
      "site-a.example"),
    /Directory rejected Node: 403/,
  );
  await assert.rejects(
    fetchAssignment(directory, directoryPort, "router-a", "router-wrong",
      "site-a.example"),
    /Directory rejected Node: 403/,
  );
  const assignmentB = await fetchAssignment(directory, directoryPort,
    "router-b", "router-b", "site-b.example");
  assert.strictEqual(assignmentA.relay_id, "relay-p43-1");
  const failedOverA = await fetchAssignment(directory, directoryPort,
    "router-a", "router-a", "site-a.example",
    { current: "relay-p43-1", failed: "relay-p43-1" });
  assert.strictEqual(failedOverA.relay_id, "relay-p43-2");
  const stickyA = await fetchAssignment(directory, directoryPort,
    "router-a", "router-a", "site-a.example",
    { current: "relay-p43-2", failed: "" });
  assert.strictEqual(stickyA.relay_id, "relay-p43-2");
  assert.match(assignmentA.session_ticket, /^nrt1\./);
  const ticketA = assignmentA.session_ticket;
  const ticketB = assignmentB.session_ticket;
  const a = connectNode(directory, port, "router-a", "router-a", "site-a.example", ticketA);
  const b = connectNode(directory, port, "router-b", "router-b", "site-b.example", ticketB);
  const clients = [a, b];
  try {
    await waitFor(() => runtime.core.snapshot().nodes === 2 &&
      a.headers.arpx && a.headers.tunnel && b.headers.arpx && b.headers.tunnel,
      "two mTLS Node registrations did not become active");
    assert.strictEqual(a.headers.arpx[":status"], 200);
    assert.strictEqual(a.headers.tunnel[":status"], 200);
    assert.strictEqual(b.headers.arpx[":status"], 200);
    assert.strictEqual(b.headers.tunnel[":status"], 200);
    await waitFor(() => a.arpxMessages.length >= 1 && b.arpxMessages.length >= 1,
      "Relay ARPX OPEN responses were not received");

    const refreshedA = await fetchAssignment(directory, directoryPort,
      "router-a", "router-a", "site-a.example");
    assert.notStrictEqual(refreshedA.session_ticket, ticketA);
    const refreshHeaders = await refreshNodeTicket(a, refreshedA.session_ticket);
    assert(refreshHeaders);
    assert.strictEqual(refreshHeaders[":status"], 204);
    assert.strictEqual(runtime.core.snapshot().nodes, 2);
    assert.strictEqual(a.session.destroyed, false);

    b.arpx.write(encodeArpxMessage({
      type: 2,
      routerId: "router-b",
      domainId: "site-b.example",
      epoch: b.bootEpoch,
      sequence: 2n,
      heartbeatMs: 0,
      routeId: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      intent: "chip.verilog.verify.lint.v1",
      capabilityVersion: 1,
      origin: "agent://router-b/lint",
      endpoint: "http://127.0.0.1:19001/stream",
      tenant: "eda",
      region: "local",
      costMicrounits: 200000n,
      latencyMs: 20,
      trustLevel: 90,
      loadPermille: 100,
      remainingLeaseMs: 60000,
      path: ["router-b"],
    }));
    await waitFor(() => a.arpxMessages.some((message) => message.type === 2),
      "Relay did not reflect the capability UPDATE");
    const reflected = a.arpxMessages.find((message) => message.type === 2);
    assert.strictEqual(reflected.routerId, "router-relay-p42");
    assert.deepStrictEqual(reflected.path, ["router-b", "router-relay-p42"]);
    assert.strictEqual(runtime.reflector.snapshot().routes, 1);

    a.arpx.write(encodeArpx(5, "router-a", "site-a.example",
      a.bootEpoch, 2n, 0, 7001n));
    await waitFor(() => a.arpxMessages.some((message) =>
      message.type === 6 && message.snapshotId === 7001n),
    "Relay capability snapshot did not terminate");
    assert(a.arpxMessages.filter((message) => message.type === 2).length >= 2);

    let rejectedHeaders = null;
    const invalid = a.session.request({
      ":method": "POST",
      ":path": "/relay/invoke/v1",
      "content-type": "application/octet-stream",
      accept: "application/octet-stream",
      "nexus-relay-ticket": refreshedA.session_ticket,
    });
    invalid.on("response", (headers) => { rejectedHeaders = headers; });
    invalid.on("data", () => {});
    invalid.end();
    await waitFor(() => rejectedHeaders !== null,
      "invalid media type did not receive a response");
    assert.strictEqual(rejectedHeaders[":status"], 400);

    const replay = connectNode(directory, port, "router-a", "router-a",
      "site-a.example", ticketA);
    clients.push(replay);
    await waitFor(() => replay.session.destroyed || replay.session.closed,
      "replayed ticket session was not rejected");
    assert.strictEqual(runtime.core.snapshot().nodes, 2);
    assert.strictEqual(runtime.ticketVerifier.snapshot().replayed, 1);

    const freshA1 = await fetchAssignment(directory, directoryPort,
      "router-a", "router-a", "site-a.example");
    const freshA2 = await fetchAssignment(directory, directoryPort,
      "router-a", "router-a", "site-a.example");
    const mismatched = connectNode(directory, port, "router-a", "router-a",
      "site-a.example", freshA1.session_ticket, freshA2.session_ticket);
    clients.push(mismatched);
    await waitFor(() => mismatched.session.destroyed || mismatched.session.closed,
      "different tickets across streams were not rejected");
    assert.strictEqual(runtime.core.snapshot().nodes, 2);

    const missing = connectNode(directory, port, "router-a", "router-a",
      "site-a.example", null, null);
    clients.push(missing);
    await waitFor(() => missing.session.destroyed || missing.session.closed,
      "missing ticket session was not rejected");
    assert.strictEqual(runtime.core.snapshot().nodes, 2);

    a.tunnel.write(encodeOpen({
      streamId: 1,
      targetRouterId: "router-b",
      intentClass: "chip.verilog.verify.lint.v1",
      taskId: "task-http2-1",
      sourceAgent: "agent://tenant-a/caller",
      tenant: "tenant-a",
      region: "local",
      hopLimit: 8,
      streaming: true,
      maxCostMicrounits: 200000,
      maxLatencyMs: 3000,
    }));
    await waitFor(() => b.tunnelFrames.length >= 1,
      "destination did not receive forwarded OPEN");
    const downstreamOpen = decodeFrame(b.tunnelFrames.shift());
    assert.strictEqual(downstreamOpen.type, TYPE.OPEN);
    assert.strictEqual(downstreamOpen.streamId, 2);

    b.tunnel.write(encodeAccept(2));
    await waitFor(() => a.tunnelFrames.length >= 1,
      "source did not receive forwarded ACCEPT");
    assert.strictEqual(decodeFrame(a.tunnelFrames.shift()).streamId, 1);

    a.tunnel.write(encodeData(1, 2n, Buffer.from("request-over-relay")));
    await waitFor(() => b.tunnelFrames.length >= 1,
      "destination did not receive request DATA");
    assert.strictEqual(decodeFrame(b.tunnelFrames.shift()).data.toString(),
      "request-over-relay");

    b.tunnel.write(encodeResponseStart(2, 2n));
    await waitFor(() => a.tunnelFrames.length >= 1,
      "source did not receive SSE RESPONSE_START");
    assert.strictEqual(decodeFrame(a.tunnelFrames.shift()).type,
      TYPE.RESPONSE_START);

    b.tunnel.write(encodeData(2, 3n, Buffer.from("data: response-over-relay\n\n")));
    await waitFor(() => a.tunnelFrames.length >= 1,
      "source did not receive response DATA");
    assert.strictEqual(decodeFrame(a.tunnelFrames.shift()).data.toString(),
      "data: response-over-relay\n\n");
    a.tunnel.write(encodeEnd(1, 3n));
    b.tunnel.write(encodeEnd(2, 4n));
    await waitFor(() => runtime.core.snapshot().pairs === 0,
      "logical pair did not close");
    assert.strictEqual(runtime.core.snapshot().pairsOpened, 1);
    assert.strictEqual(runtime.core.snapshot().framesForwarded, 7);
    b.arpx.write(encodeArpxMessage({
      type: 3,
      routerId: "router-b",
      domainId: "site-b.example",
      epoch: b.bootEpoch,
      sequence: 3n,
      heartbeatMs: 0,
      routeId: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    }));
    await waitFor(() => a.arpxMessages.some((message) => message.type === 3),
      "Relay did not reflect the capability WITHDRAW");
    assert.strictEqual(runtime.reflector.snapshot().routes, 0);
    const assignmentC = await fetchAssignment(directory, directoryPort,
      "router-c", "router-c", "mesh.local", null,
      "/v1/open-mesh/assignment");
    assert.match(assignmentC.assignment_id, /^mesh-[0-9a-f]{32}$/);
    const c = connectNode(directory, port, "router-c", "router-c",
      "mesh.local", assignmentC.session_ticket);
    clients.push(c);
    await waitFor(() => runtime.core.snapshot().nodes === 3 &&
      c.headers.arpx && c.headers.tunnel,
    "self-signed Open Mesh Node did not join the Relay seed");
    assert.strictEqual(c.headers.arpx[":status"], 200);
    assert.strictEqual(c.headers.tunnel[":status"], 200);
    await waitFor(() => c.arpxMessages.length >= 1,
      "Open Mesh Node did not receive ARPX OPEN");
    a.arpx.write(encodeArpxMessage({
      type: 2,
      routerId: "router-a",
      domainId: "site-a.example",
      epoch: a.bootEpoch,
      sequence: 3n,
      heartbeatMs: 0,
      routeId: "cccccccccccccccccccccccccccccccc",
      intent: "mesh.echo.v1",
      capabilityVersion: 1,
      origin: "agent://router-a/mesh-echo",
      endpoint: "http://127.0.0.1:19002/invoke",
      tenant: "organization-a",
      region: "local",
      costMicrounits: 0n,
      latencyMs: 10,
      trustLevel: 80,
      loadPermille: 0,
      remainingLeaseMs: 60000,
      path: ["router-a"],
    }));
    await waitFor(() => c.arpxMessages.some((message) =>
      message.type === 2 && message.intent === "mesh.echo.v1"),
    "Open Mesh Node did not discover peer Agent capability through Relay");
    a.tunnelFrames.length = 0;
    c.tunnelFrames.length = 0;
    c.tunnel.write(encodeOpen({
      streamId: 11,
      targetRouterId: "router-a",
      intentClass: "mesh.echo.v1",
      taskId: "task-open-mesh-cross-nat",
      sourceAgent: "agent://router-c/caller",
      tenant: "organization-c",
      region: "local",
      hopLimit: 8,
      streaming: false,
      maxCostMicrounits: 0,
      maxLatencyMs: 3000,
    }));
    await waitFor(() => a.tunnelFrames.length >= 1,
      "Open Mesh cross-NAT invoke did not reach target Router");
    const meshOpen = decodeFrame(a.tunnelFrames.shift());
    assert.strictEqual(meshOpen.type, TYPE.OPEN);
    a.tunnel.write(encodeAccept(meshOpen.streamId));
    await waitFor(() => c.tunnelFrames.length >= 1,
      "Open Mesh cross-NAT invoke did not return ACCEPT");
    assert.strictEqual(decodeFrame(c.tunnelFrames.shift()).streamId, 11);
    c.tunnel.write(encodeEnd(11, 2n));
    a.tunnel.write(encodeEnd(meshOpen.streamId, 2n));
    await waitFor(() => runtime.core.snapshot().pairs === 0,
      "Open Mesh cross-NAT invoke pair did not close");
    await assert.rejects(
      fetchAssignment(directory, directoryPort, "router-a", "router-a",
        "wrong.mesh", null, "/v1/open-mesh/assignment"),
      /Directory rejected Node: 403/,
    );
    const relayEvents = fs.readFileSync(path.join(directory, "relay-events.jsonl"), "utf8");
    assert.match(relayEvents, /"event":"ticket-refreshed"/);
    assert.deepStrictEqual(a.errors, []);
    assert.deepStrictEqual(b.errors, []);
  } catch (error) {
    const capturePath = path.join(directory, "relay-events.jsonl");
    const events = fs.existsSync(capturePath) ? fs.readFileSync(capturePath, "utf8") : "";
    const directoryCapture = path.join(directory, "directory-events.jsonl");
    const directoryEvents = fs.existsSync(directoryCapture)
      ? fs.readFileSync(directoryCapture, "utf8") : "";
    error.message += `\nclient-a=${a.errors.map((item) => item.message).join(";")}` +
      `\nclient-b=${b.errors.map((item) => item.message).join(";")}` +
      `\nrelay-events=${events}` + `\ndirectory-events=${directoryEvents}`;
    throw error;
  } finally {
    await closeRuntime(runtime, directoryRuntime, clients, directory);
  }
  process.stdout.write("Nexus Relay P4.6 ticket-bound mTLS HTTP/2 integration passed\n");
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error.message}\n`);
  process.exit(1);
});
