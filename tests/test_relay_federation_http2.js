"use strict";

const assert = require("assert");
const fs = require("fs");
const http2 = require("http2");
const net = require("net");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");
const { createRelayServer, loadConfig } = require("../relay/nexus-relayd");
const { issueTicket } = require("../relay/relay-ticket");
const { ArpxDecoder, encodeArpx, encodeArpxMessage } = require("../relay/arpx-control");
const {
  FrameDecoder, decodeFrame, encodeOpen, encodeAccept, encodeData, encodeEnd,
  encodeResponseStart,
} = require("../relay/relay-tunnel");

const SECRET = require("crypto").randomBytes(48).toString("base64url");

function openssl(args, directory) {
  const result = spawnSync("openssl", args, {
    cwd: directory, windowsHide: true, encoding: "utf8",
    env: { ...process.env, OPENSSL_CONF: path.join(directory, "openssl.cnf") },
  });
  if (result.status !== 0) throw new Error(`openssl failed: ${result.stderr}`);
}

function certificate(directory, name, dns, usage) {
  openssl(["req", "-newkey", "rsa:2048", "-sha256", "-nodes",
    "-subj", `/CN=${dns}`, "-keyout", `${name}.key`, "-out", `${name}.csr`],
  directory);
  fs.writeFileSync(path.join(directory, `${name}.ext`),
    `subjectAltName=DNS:${dns}\nextendedKeyUsage=${usage}\n` +
    "keyUsage=digitalSignature,keyEncipherment\n");
  openssl(["x509", "-req", "-sha256", "-days", "2", "-in", `${name}.csr`,
    "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
    "-extfile", `${name}.ext`, "-out", `${name}.pem`], directory);
}

function certificates(directory) {
  fs.writeFileSync(path.join(directory, "openssl.cnf"),
    "[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=unused\n");
  openssl(["req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
    "-days", "2", "-subj", "/CN=Nexus Federation Test CA",
    "-keyout", "ca.key", "-addext", "basicConstraints=critical,CA:TRUE",
    "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-out", "ca.pem"],
  directory);
  certificate(directory, "relay-a", "relay-a.fed.test", "serverAuth,clientAuth");
  certificate(directory, "relay-b", "relay-b.fed.test", "serverAuth,clientAuth");
  certificate(directory, "router-a", "router-a.fed.test", "clientAuth");
  certificate(directory, "router-b", "router-b.fed.test", "clientAuth");
}

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const port = server.address().port;
      server.close(() => resolve(port));
    });
  });
}

function waitFor(predicate, message, timeoutMs = 8000) {
  const started = Date.now();
  return new Promise((resolve, reject) => {
    const poll = () => {
      try { if (predicate()) return resolve(); } catch (error) { return reject(error); }
      if (Date.now() - started > timeoutMs) return reject(new Error(message));
      setTimeout(poll, 20);
    };
    poll();
  });
}

function config(directory, name, port, peerName, peerPort) {
  const value = {
    listen: "127.0.0.1", port,
    tls: { key: `${name}.key`, cert: `${name}.pem`, ca: "ca.pem" },
    relayId: name, relayRouterId: `router-${name}`,
    relayDomainId: "relay.fed.test", heartbeatMs: 1000,
    maxNodes: 32, maxPairs: 32, maxQueuedBytes: 65536,
    ticketMaxLifetimeSeconds: 300, ticketClockSkewSeconds: 5,
    maxUsedTickets: 32, maxCapabilityRoutes: 64,
    ticketKeys: { current: SECRET }, capture: `${name}-events.jsonl`,
    federation: {
      enabled: true, maxPeers: 4, maxRemoteRouters: 64,
      maxTransitHops: 8, heartbeatMs: 1000,
      peers: {
        [peerName]: {
          relayRouterId: `router-${peerName}`, domainId: "relay.fed.test",
          certificateDns: `${peerName}.fed.test`,
          endpoint: `https://${peerName}.fed.test:${peerPort}/federation/v1`,
          connectIpv4: "127.0.0.1", priority: 100,
        },
      },
    },
  };
  const filename = path.join(directory, `${name}.json`);
  fs.writeFileSync(filename, JSON.stringify(value));
  return filename;
}

function ticket(relayId, routerId, domainId, san) {
  const now = Math.floor(Date.now() / 1000);
  return issueTicket({
    keys: { current: SECRET }, kid: "current", nowSeconds: now,
    claims: {
      v: 1, aid: `assign-${routerId}`, relay: relayId, router: routerId,
      domain: domainId, san, iat: now, exp: now + 120,
      nonce: `nonce-${routerId}-${now}`,
    },
  });
}

function connectNode(directory, relayName, port, routerId, domainId) {
  const san = `${routerId}.fed.test`;
  const sessionTicket = ticket(relayName, routerId, domainId, san);
  const session = http2.connect(`https://${relayName}.fed.test:${port}`, {
    ca: fs.readFileSync(path.join(directory, "ca.pem")),
    cert: fs.readFileSync(path.join(directory, `${routerId}.pem`)),
    key: fs.readFileSync(path.join(directory, `${routerId}.key`)),
    servername: `${relayName}.fed.test`, minVersion: "TLSv1.3", maxVersion: "TLSv1.3",
    lookup: (_hostname, options, callback) => options && options.all
      ? callback(null, [{ address: "127.0.0.1", family: 4 }])
      : callback(null, "127.0.0.1", 4),
  });
  const arpx = session.request({
    ":method": "POST", ":path": "/arpx/v1",
    "content-type": "application/arpx+cbor", accept: "application/arpx+cbor",
    "nexus-relay-ticket": sessionTicket,
  });
  const tunnel = session.request({
    ":method": "POST", ":path": "/relay/invoke/v1",
    "content-type": "application/vnd.nexus.relay-tunnel.v1",
    accept: "application/vnd.nexus.relay-tunnel.v1",
    "nexus-relay-ticket": sessionTicket,
  });
  const headers = { arpx: null, tunnel: null };
  const arpxMessages = [];
  const tunnelFrames = [];
  arpx.on("response", (value) => { headers.arpx = value; });
  tunnel.on("response", (value) => { headers.tunnel = value; });
  arpx.on("data", (chunk) => new ArpxDecoder(() => {}).push(chunk));
  const arpxDecoder = new ArpxDecoder((message) => arpxMessages.push(message));
  arpx.removeAllListeners("data");
  arpx.on("data", (chunk) => arpxDecoder.push(chunk));
  const tunnelDecoder = new FrameDecoder((frame) => tunnelFrames.push(Buffer.from(frame)));
  tunnel.on("data", (chunk) => tunnelDecoder.push(chunk));
  const epoch = BigInt(Date.now()) * 10n + BigInt(routerId.endsWith("a") ? 1 : 2);
  arpx.write(encodeArpx(1, routerId, domainId, epoch, 1n, 1000));
  return { session, arpx, tunnel, headers, arpxMessages, tunnelFrames, epoch };
}

async function main() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "nexus-fed-h2-"));
  certificates(directory);
  const portA = await freePort();
  const portB = await freePort();
  const runtimeA = createRelayServer(loadConfig(
    config(directory, "relay-a", portA, "relay-b", portB)));
  const runtimeB = createRelayServer(loadConfig(
    config(directory, "relay-b", portB, "relay-a", portA)));
  await new Promise((resolve) => runtimeA.server.listen(portA, "127.0.0.1", resolve));
  await new Promise((resolve) => runtimeB.server.listen(portB, "127.0.0.1", resolve));
  runtimeA.startFederation();
  runtimeB.startFederation();
  const routerA = connectNode(directory, "relay-a", portA, "router-a", "site-a.example");
  const routerB = connectNode(directory, "relay-b", portB, "router-b", "site-b.example");
  try {
    await waitFor(() => runtimeA.federation.snapshot().peersUp >= 1 &&
      runtimeB.federation.snapshot().peersUp >= 1,
    "mTLS federation control adjacency did not establish");
    await waitFor(() => runtimeA.core.nodes.has("router-b") &&
      runtimeB.core.nodes.has("router-a") && routerA.headers.arpx &&
      routerA.headers.tunnel && routerB.headers.arpx && routerB.headers.tunnel,
    "federated Router membership did not converge");

    routerB.arpx.write(encodeArpxMessage({
      type: 2, routerId: "router-b", domainId: "site-b.example",
      epoch: routerB.epoch, sequence: 2n, heartbeatMs: 0,
      routeId: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      intent: "chip.verilog.verify.lint.v1", capabilityVersion: 1,
      origin: "agent://router-b/lint", endpoint: "http://127.0.0.1:19001/invoke",
      tenant: "eda", region: "local", costMicrounits: 1n, latencyMs: 1,
      trustLevel: 100, loadPermille: 0, remainingLeaseMs: 60000,
      path: ["router-b"],
    }));
    await waitFor(() => routerA.arpxMessages.some((message) => message.type === 2),
      "federated ARPX capability did not reach Router A");
    const update = routerA.arpxMessages.find((message) => message.type === 2);
    assert.deepStrictEqual(update.path,
      ["router-b", "router-relay-b", "router-relay-a"]);

    routerA.tunnel.write(encodeOpen({
      streamId: 1, targetRouterId: "router-b", hopLimit: 8,
      intentClass: "chip.verilog.verify.lint.v1", taskId: "fed-http2-task",
      sourceAgent: "agent://router-a/caller", tenant: "eda", region: "local",
      streaming: true, maxCostMicrounits: 100, maxLatencyMs: 3000,
      forwardingAssertion: "nfa1.router-a.payload.signature",
    }));
    await waitFor(() => routerB.tunnelFrames.length >= 1,
      "federated Invoke OPEN did not reach Router B");
    const open = decodeFrame(routerB.tunnelFrames[0]);
    assert.strictEqual(open.hopLimit, 7);
    routerB.tunnel.write(encodeAccept(open.streamId, 200, 4096));
    await waitFor(() => routerA.tunnelFrames.length >= 1,
      "federated Invoke ACCEPT did not return to Router A");
    routerA.tunnel.write(encodeData(1, 2n, Buffer.from("request")));
    await waitFor(() => routerB.tunnelFrames.length >= 2,
      "federated Invoke request DATA did not reach Router B");
    routerB.tunnel.write(encodeResponseStart(open.streamId, 2n));
    routerB.tunnel.write(encodeData(open.streamId, 3n, Buffer.from("response")));
    await waitFor(() => routerA.tunnelFrames.some((frame) =>
      decodeFrame(frame).data && decodeFrame(frame).data.toString() === "response"),
    "federated Invoke response DATA did not return");
    routerA.tunnel.write(encodeEnd(1, 3n));
    routerB.tunnel.write(encodeEnd(open.streamId, 4n));

    await new Promise((resolve) => runtimeB.shutdown(resolve));
    await waitFor(() => !runtimeA.core.nodes.has("router-b"),
      "federation peer failure did not withdraw remote Router", 10000);
    assert(runtimeA.federation.snapshot().peersDown >= 1);
    process.stdout.write("Nexus Relay P6.2.3 real mTLS HTTP/2 federation passed\n");
  } finally {
    routerA.session.destroy();
    routerB.session.destroy();
    if (runtimeA.server.listening) await new Promise((resolve) => runtimeA.shutdown(resolve));
    if (runtimeB.server.listening) await new Promise((resolve) => runtimeB.shutdown(resolve));
    await new Promise((resolve) => setTimeout(resolve, 100));
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error.message}\n`);
  process.exitCode = 1;
});
