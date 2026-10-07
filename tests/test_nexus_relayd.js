"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const crypto = require("crypto");

const { RelayCore } = require("../relay/relay-core");
const {
  TYPE, RESET, FrameDecoder, decodeFrame, encodeOpen, encodeAccept,
  encodeData, encodeEnd, encodeWindowUpdate, encodeResponseStart,
} = require("../relay/relay-tunnel");
const {
  ArpxControl, decodeArpx, encodeArpx, encodeArpxMessage,
} = require("../relay/arpx-control");
const { ArpxRouteReflector } = require("../relay/arpx-reflector");
const { loadConfig, resolveCertificateDns } = require("../relay/nexus-relayd");
const { TicketVerifier, issueTicket, normalizeKeys } = require("../relay/relay-ticket");
const { loadConfig: loadDirectoryConfig } = require("../directory/nexus-directory");

const TICKET_SECRET = crypto.randomBytes(48).toString("base64url");

function testPackagedTicketKeysFailClosed() {
  // Historical public fixture, never a deployment key. Reject it under any ID,
  // including as a secondary verification key during key rotation.
  const historical = Buffer.from("0123456789abcdef".repeat(2)).toString("base64url");
  for (const keys of [{ renamed: historical }, { fresh: TICKET_SECRET, old: historical }]) {
    assert.throws(() => normalizeKeys(keys), /public sample ticket key/);
  }
  assert.throws(() => loadConfig(path.join(__dirname, "../relay/relay.config.example.json")),
    /ticket key.*not provisioned/);
  assert.throws(() => loadDirectoryConfig(path.join(__dirname, "../directory/directory.config.example.json")),
    /ticket key.*not provisioned/);
  for (const size of [32, 48, 64]) {
    assert.strictEqual(normalizeKeys({ generated: crypto.randomBytes(size).toString("base64url") })
      .get("generated").length, size);
  }
}

function makeOpen(streamId, targetRouterId = "router-b") {
  return encodeOpen({
    streamId,
    targetRouterId,
    intentClass: "chip.verilog.verify.lint.v1",
    taskId: `task-${streamId}`,
    sourceAgent: "agent://tenant-a/caller",
    targetAgent: "agent://tenant-a/target",
    tenant: "tenant-a",
    region: "*",
    hopLimit: 8,
    streaming: false,
    maxCostMicrounits: 200000,
    maxLatencyMs: 3000,
    forwardingAssertion: "nfa1.router-a.payload.signature",
  });
}

function registerCapture(core, routerId, reject = false) {
  const frames = [];
  assert(core.registerNode(routerId, (frame) => {
    if (reject) return false;
    frames.push(Buffer.from(frame));
    return true;
  }));
  return frames;
}

function testPairLifecycle() {
  const core = new RelayCore({ maxNodes: 4, maxPairs: 4 });
  const a = registerCapture(core, "router-a");
  const b = registerCapture(core, "router-b");

  assert(core.receive("router-a", makeOpen(1)));
  assert.strictEqual(b.length, 1);
  assert.strictEqual(decodeFrame(b[0]).type, TYPE.OPEN);
  assert.strictEqual(decodeFrame(b[0]).streamId, 2);
  assert.strictEqual(decodeFrame(b[0]).targetRouterId, "router-b");
  assert.strictEqual(decodeFrame(b[0]).targetAgent,
    "agent://tenant-a/target");
  assert.strictEqual(decodeFrame(b[0]).region, "*");
  assert.strictEqual(
    decodeFrame(b[0]).forwardingAssertion,
    "nfa1.router-a.payload.signature",
  );

  assert(core.receive("router-b", encodeAccept(2, 200, 4096)));
  assert.strictEqual(decodeFrame(a[0]).streamId, 1);
  assert.strictEqual(decodeFrame(a[0]).type, TYPE.ACCEPT);

  assert(core.receive("router-a", encodeData(1, 2n, Buffer.from("request"))));
  assert.strictEqual(decodeFrame(b[1]).streamId, 2);
  assert.strictEqual(decodeFrame(b[1]).data.toString(), "request");

  assert(core.receive("router-b", encodeResponseStart(2, 2n)));
  assert.strictEqual(decodeFrame(a[1]).type, TYPE.RESPONSE_START);
  assert(core.receive("router-b", encodeData(2, 3n, Buffer.from("response"))));
  assert.strictEqual(decodeFrame(a[2]).streamId, 1);
  assert.strictEqual(decodeFrame(a[2]).data.toString(), "response");

  assert(core.receive("router-a", encodeEnd(1, 3n)));
  assert(core.receive("router-b", encodeEnd(2, 4n)));
  assert.strictEqual(core.snapshot().pairs, 0);
  assert.strictEqual(core.snapshot().pairsOpened, 1);
  assert.strictEqual(core.snapshot().pairsClosed, 1);
  assert.strictEqual(core.snapshot().framesForwarded, 7);
  assert(core.receive("router-b", encodeWindowUpdate(2, 5n, 1024)));
  assert.strictEqual(core.snapshot().lateFramesIgnored, 1);
  assert(core.receive("router-b", encodeEnd(2, 6n)));
  assert.strictEqual(core.snapshot().lateFramesIgnored, 2);
  assert.strictEqual(core.snapshot().protocolErrors, 0);
  assert.throws(
    () => core.receive("router-b", encodeWindowUpdate(200, 1n, 1024)),
    /unknown logical stream/,
  );
}

function testWindowAndConcurrentMapping() {
  const core = new RelayCore({ maxNodes: 3, maxPairs: 4 });
  registerCapture(core, "router-a");
  const b = registerCapture(core, "router-b");

  assert(core.receive("router-a", makeOpen(1)));
  assert(core.receive("router-a", makeOpen(3)));
  assert.deepStrictEqual(b.map((frame) => decodeFrame(frame).streamId), [2, 4]);
  assert(core.receive("router-b", encodeAccept(2, 200, 4096)));
  assert(core.receive("router-a", encodeData(1, 2n, Buffer.alloc(4096, 1))));
  assert.throws(
    () => core.receive("router-a", encodeData(1, 3n, Buffer.from("x"))),
    /flow-control/,
  );

  const isolated = new RelayCore({ maxNodes: 3, maxPairs: 2 });
  registerCapture(isolated, "router-a");
  registerCapture(isolated, "router-b");
  assert(isolated.receive("router-a", makeOpen(1)));
  assert(isolated.receive("router-b", encodeAccept(2, 200, 4096)));
  assert(isolated.receive("router-a", encodeData(1, 2n, Buffer.alloc(4096, 2))));
  assert(isolated.receive("router-b", encodeWindowUpdate(2, 2n, 4096)));
  assert(isolated.receive("router-a", encodeData(1, 3n, Buffer.from("ok"))));
}

function testStreamingCreditAfterRequestHalfClose() {
  const core = new RelayCore({ maxNodes: 2, maxPairs: 2 });
  const source = registerCapture(core, "router-a");
  const destination = registerCapture(core, "router-b");
  assert(core.receive("router-a", makeOpen(1)));
  assert(core.receive("router-b", encodeAccept(2, 200, 4096)));
  assert(core.receive("router-a", encodeWindowUpdate(1, 2n, 4096)));
  assert(core.receive("router-a", encodeData(1, 3n, Buffer.from("request"))));
  assert(core.receive("router-a", encodeEnd(1, 4n)));
  assert(core.receive("router-b", encodeWindowUpdate(2, 2n, 7)));
  assert(core.receive("router-b", encodeResponseStart(2, 3n)));
  assert(core.receive("router-b", encodeData(2, 4n, Buffer.from("data: one\n\n"))));

  // END only half-closes the source DATA direction. The source must still be
  // able to replenish credit while the destination is streaming its response.
  assert(core.receive("router-a", encodeWindowUpdate(1, 5n, 11)));
  assert(core.receive("router-b", encodeData(2, 5n, Buffer.from("data: two\n\n"))));
  assert(core.receive("router-a", encodeWindowUpdate(1, 6n, 11)));
  assert(core.receive("router-b", encodeEnd(2, 6n)));
  assert.strictEqual(core.snapshot().pairs, 0);
  assert.strictEqual(core.snapshot().protocolErrors, 0);
  assert(source.length > 0);
  assert(destination.length > 0);
}

function testMissReplayDisconnectAndBackpressure() {
  const core = new RelayCore({ maxNodes: 3, maxPairs: 2 });
  const a = registerCapture(core, "router-a");
  assert.strictEqual(core.receive("router-a", makeOpen(1, "router-missing")), false);
  let reset = decodeFrame(a[0]);
  assert.strictEqual(reset.type, TYPE.RESET);
  assert.strictEqual(reset.resetCode, RESET.TARGET_UNAVAILABLE);
  assert.throws(() => core.receive("router-a", makeOpen(1, "router-missing")),
    /replayed/);

  const connected = new RelayCore({ maxNodes: 3, maxPairs: 2 });
  const source = registerCapture(connected, "router-a");
  registerCapture(connected, "router-b");
  assert(connected.receive("router-a", makeOpen(1)));
  assert(connected.receive("router-b", encodeAccept(2)));
  assert(connected.unregisterNode("router-b"));
  reset = decodeFrame(source[source.length - 1]);
  assert.strictEqual(reset.type, TYPE.RESET);
  assert.strictEqual(reset.sequence, 2n);
  assert.strictEqual(reset.resetCode, RESET.NODE_DISCONNECTED);
  assert.strictEqual(connected.snapshot().pairs, 0);

  const blocked = new RelayCore({ maxNodes: 3, maxPairs: 2 });
  const caller = registerCapture(blocked, "router-a");
  registerCapture(blocked, "router-b", true);
  assert.strictEqual(blocked.receive("router-a", makeOpen(1)), false);
  reset = decodeFrame(caller[0]);
  assert.strictEqual(reset.resetCode, RESET.BACKPRESSURE);
  assert.strictEqual(blocked.snapshot().pairs, 0);
}

function testFragmentedDecoderAndArpx() {
  const first = makeOpen(1);
  const second = encodeAccept(2);
  const combined = Buffer.concat([first, second]);
  const decoded = [];
  const ingress = new FrameDecoder((_frame, message) => decoded.push(message));
  for (let index = 0; index < combined.length; index += 3) {
    ingress.push(combined.subarray(index, Math.min(index + 3, combined.length)));
  }
  assert.deepStrictEqual(decoded.map((message) => message.type),
    [TYPE.OPEN, TYPE.ACCEPT]);

  const arpx = encodeArpx(1, "router-relay", "relay.example", 10n, 1n, 5000);
  const message = decodeArpx(arpx);
  assert.strictEqual(message.routerId, "router-relay");
  assert.strictEqual(message.sequence, 1n);
  assert.strictEqual(message.heartbeatMs, 5000);

  const maximumEpoch = 0x7fffffffffffffffn;
  const maximumEpochMessage = decodeArpx(encodeArpx(
    1, "router-relay", "relay.example", maximumEpoch, 1n, 5000));
  assert.strictEqual(maximumEpochMessage.epoch, maximumEpoch);

  const errors = [];
  const control = new ArpxControl({
    routerId: "router-relay",
    domainId: "relay.example",
    peerRouterId: "router-a",
    peerDomainId: "site-a.example",
    heartbeatMs: 5000,
    write: () => true,
    onReady: () => assert.fail("mismatched ARPX identity became ready"),
    onError: (error) => errors.push(error),
  });
  control.push(encodeArpx(1, "router-wrong", "site-a.example", 20n, 1n, 5000));
  assert.strictEqual(errors.length, 1);
  assert.match(errors[0].message, /identity/);
  control.close();

  const output = [];
  let snapshotControl;
  snapshotControl = new ArpxControl({
    routerId: "router-relay",
    domainId: "relay.example",
    peerRouterId: "router-a",
    peerDomainId: "site-a.example",
    heartbeatMs: 5000,
    write: (frame) => { output.push(Buffer.from(frame)); return true; },
    onReady: () => snapshotControl.sendSnapshotRequest(42n),
    onError: (error) => assert.fail(error.message),
  });
  snapshotControl.push(encodeArpx(
    1, "router-a", "site-a.example", 21n, 1n, 5000));
  assert.strictEqual(decodeArpx(output[0]).type, 1);
  assert.strictEqual(decodeArpx(output[1]).type, 5);
  assert.strictEqual(decodeArpx(output[1]).snapshotId, 42n);
  assert.throws(() => snapshotControl.sendSnapshotRequest(0n),
    /snapshot request ID/);
  snapshotControl.close();
}

function capabilityUpdate(overrides = {}) {
  return {
    version: 1,
    type: 2,
    routerId: "router-b",
    domainId: "site-b.example",
    epoch: 20n,
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
    snapshotId: null,
    ...overrides,
  };
}

function reflectorControl() {
  const output = [];
  return {
    output,
    sendRoute: (message) => { output.push({ kind: "update", message }); return true; },
    sendWithdraw: (routeId) => { output.push({ kind: "withdraw", routeId }); return true; },
    sendSnapshotEnd: (snapshotId) => {
      output.push({ kind: "snapshot-end", snapshotId }); return true;
    },
  };
}

function testCapabilityCodecAndReflector() {
  const decoded = decodeArpx(encodeArpxMessage(capabilityUpdate()));
  assert.strictEqual(decoded.routeId, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  assert.strictEqual(decoded.intent, "chip.verilog.verify.lint.v1");
  assert.strictEqual(decoded.costMicrounits, 200000n);
  assert.deepStrictEqual(decoded.path, ["router-b"]);

  let now = 1000;
  const reflector = new ArpxRouteReflector({
    relayRouterId: "router-relay", maxRoutes: 2, now: () => now,
  });
  const a = reflectorControl();
  const b = reflectorControl();
  const c = reflectorControl();
  reflector.registerNode("router-a", a);
  reflector.registerNode("router-b", b);
  reflector.receive("router-b", capabilityUpdate({ remainingLeaseMs: 5000 }));
  assert.strictEqual(a.output.length, 1);
  assert.deepStrictEqual(a.output[0].message.path, ["router-b", "router-relay"]);
  assert.strictEqual(b.output.length, 0);

  now = 2000;
  reflector.registerNode("router-c", c);
  reflector.snapshotTo("router-c", 77n);
  assert.strictEqual(c.output[0].message.remainingLeaseMs, 4000);
  assert.deepStrictEqual(c.output.map((item) => item.kind), ["update", "snapshot-end"]);
  assert.strictEqual(c.output[1].snapshotId, 77n);

  assert.throws(() => reflector.receive("router-a", capabilityUpdate({
    routerId: "router-a", domainId: "site-a.example", path: ["router-a"],
  })), /ownership collision/);
  reflector.unregisterNode("router-b");
  assert.strictEqual(reflector.snapshot().routes, 0);
  assert.strictEqual(a.output.at(-1).kind, "withdraw");
  assert.strictEqual(c.output.at(-1).kind, "withdraw");

  reflector.registerNode("router-b", b);
  reflector.receive("router-b", capabilityUpdate({ remainingLeaseMs: 10 }));
  now = 2011;
  reflector.expire();
  assert.strictEqual(reflector.snapshot().routesExpired, 1);
  assert.strictEqual(reflector.snapshot().routes, 0);
  reflector.close();
}

function testTransitCapabilityReflection() {
  const reflector = new ArpxRouteReflector({
    relayRouterId: "router-relay", maxRoutes: 4,
  });
  const source = reflectorControl();
  const target = reflectorControl();
  reflector.registerNode("router-a", source);
  reflector.registerNode("router-b", target);

  reflector.receive("router-a", capabilityUpdate({
    routerId: "router-a",
    domainId: "site-a.example",
    routeId: "cccccccccccccccccccccccccccccccc",
    origin: "agent://router-c/transit-target",
    path: ["router-c", "router-a"],
  }));
  assert.deepStrictEqual(target.output[0].message.path,
    ["router-c", "router-a", "router-relay"]);
  assert.throws(() => reflector.receive("router-a", capabilityUpdate({
    routerId: "router-a",
    domainId: "site-a.example",
    routeId: "dddddddddddddddddddddddddddddddd",
    path: ["router-c", "router-c", "router-a"],
  })), /invalid Relay capability update/);
  reflector.close();
}
function testConfigAndCertificateIdentity() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "nexus-relayd-"));
  const filename = path.join(directory, "relay.json");
  const config = {
    listen: "127.0.0.1",
    port: 7444,
    tls: { key: "relay.key", cert: "relay.pem", ca: "ca.pem" },
    relayId: "relay-p43",
    relayRouterId: "router-relay",
    relayDomainId: "relay.example",
    capture: "events.jsonl",
    ticketKeys: { current: TICKET_SECRET },
  };
  fs.writeFileSync(filename, JSON.stringify(config));
  const loaded = loadConfig(filename);
  assert.strictEqual(loaded.relayId, "relay-p43");
  fs.writeFileSync(filename, `\uFEFF${JSON.stringify(config)}`, "utf8");
  assert.strictEqual(loadConfig(filename).relayId, "relay-p43");
  for (const listen of ["::", "::1", "fd74:6e65:7875::2"]) {
    fs.writeFileSync(filename, JSON.stringify({ ...config, listen }));
    assert.strictEqual(loadConfig(filename).listen, listen);
  }
  for (const listen of ["localhost", "[::1]", "2001:db8::bad::", ""] ) {
    fs.writeFileSync(filename, JSON.stringify({ ...config, listen }));
    assert.throws(() => loadConfig(filename), /invalid Relay address or identity/);
  }
  const socket = {
    authorized: true,
    alpnProtocol: "h2",
    getProtocol: () => "TLSv1.3",
    getPeerCertificate: () => ({ subjectaltname: "DNS:router-a.example" }),
  };
  assert.strictEqual(resolveCertificateDns(socket), "router-a.example");
  socket.getPeerCertificate = () => ({
    subjectaltname: "DNS:router-a.example, DNS:router-b.example",
  });
  assert.strictEqual(resolveCertificateDns(socket), null);

  config.federation = {
    enabled: true, maxPeers: 4, maxRemoteRouters: 64,
    maxTransitHops: 8, heartbeatMs: 5000,
    peers: {
      "relay-b": {
        relayRouterId: "router-relay-b", domainId: "relay.example",
        certificateDns: "relay-b.example",
        endpoint: "https://relay-b.example:7444/federation/v1",
        connectIpv4: "192.0.2.41", priority: 100,
      },
    },
  };
  fs.writeFileSync(filename, JSON.stringify(config));
  assert.strictEqual(loadConfig(filename).federation.enabled, true);
  config.federation.peers["relay-b"].endpoint =
    "http://relay-b.example:7444/federation/v1";
  fs.writeFileSync(filename, JSON.stringify(config));
  assert.throws(() => loadConfig(filename), /invalid federation peer/);
  delete config.federation;

  config.ticketKeys.current = "dG9vLXNob3J0";
  fs.writeFileSync(filename, JSON.stringify(config));
  assert.throws(() => loadConfig(filename), /ticket secret length/);
  config.ticketKeys.current = TICKET_SECRET;
  config.relayDomainId = "relay..example";
  fs.writeFileSync(filename, JSON.stringify(config));
  assert.throws(() => loadConfig(filename), /invalid Relay address or identity/);
  config.relayDomainId = "relay.example";
  config.unknown = true;
  fs.writeFileSync(filename, JSON.stringify(config));
  assert.throws(() => loadConfig(filename), /unknown config key/);
  fs.rmSync(directory, { recursive: true, force: true });
}

function testTicketBinding() {
  const keys = { current: TICKET_SECRET };
  const claims = {
    v: 1,
    aid: "assign-p43-1",
    relay: "relay-p43",
    router: "router-a",
    domain: "site-a.example",
    san: "router-a.example",
    iat: 1000,
    exp: 1060,
    nonce: "nonce-p43-0001",
  };
  const ticket = issueTicket({ keys, kid: "current", claims, nowSeconds: 1000 });
  const verifier = new TicketVerifier({
    keys, relayId: "relay-p43", maxLifetimeSeconds: 300,
    clockSkewSeconds: 0, maxUsedTickets: 4,
  });
  const firstSession = {};
  assert.strictEqual(
    verifier.bind(ticket, "router-a.example", firstSession, 1000).router,
    "router-a",
  );
  assert.strictEqual(
    verifier.bind(ticket, "router-a.example", firstSession, 1001).aid,
    "assign-p43-1",
  );
  assert.throws(() => verifier.bind(ticket, "router-a.example", {}, 1001), /replayed/);
  assert.throws(() => verifier.verify(ticket, "router-b.example", 1001), /SAN mismatch/);
  assert.throws(() => verifier.verify(`${ticket.slice(0, -1)}${ticket.endsWith("A") ? "B" : "A"}`,
    "router-a.example", 1001), /signature/);
  assert.throws(() => verifier.verify(ticket, "router-a.example", 1061), /expired/);
  assert.strictEqual(verifier.snapshot().accepted, 1);
  assert.strictEqual(verifier.snapshot().replayed, 1);
}

function main() {
  testPackagedTicketKeysFailClosed();
  testPairLifecycle();
  testWindowAndConcurrentMapping();
  testStreamingCreditAfterRequestHalfClose();
  testMissReplayDisconnectAndBackpressure();
  testFragmentedDecoderAndArpx();
  testCapabilityCodecAndReflector();
  testTransitCapabilityReflection();
  testConfigAndCertificateIdentity();
  testTicketBinding();
  process.stdout.write("Nexus Relay P4.6 tests passed\n");
}

main();
