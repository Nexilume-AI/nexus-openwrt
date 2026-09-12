"use strict";

const assert = require("assert");
const { RelayCore } = require("../relay/relay-core");
const { ArpxRouteReflector } = require("../relay/arpx-reflector");
const {
  TYPE, RESET, decodeFrame, encodeOpen, encodeAccept, encodeData, encodeEnd,
  encodeResponseStart,
} = require("../relay/relay-tunnel");
const {
  FederationBridge, ControlDecoder, stringifyControl,
} = require("../relay/relay-federation");

function makeOpen(streamId, targetRouterId, hopLimit = 8) {
  return encodeOpen({
    streamId, targetRouterId, hopLimit,
    intentClass: "chip.verilog.verify.lint.v1",
    taskId: `fed-task-${streamId}`,
    sourceAgent: "agent://router-a/caller",
    tenant: "eda", region: "local", streaming: true,
    maxCostMicrounits: 200000, maxLatencyMs: 3000,
    forwardingAssertion: "nfa1.router-a.payload.signature",
  });
}

function capabilityUpdate() {
  return {
    version: 1, type: 2, routerId: "router-a", domainId: "site-a.example",
    epoch: 20n, sequence: 2n, heartbeatMs: 0,
    routeId: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    intent: "chip.verilog.verify.lint.v1", capabilityVersion: 1,
    origin: "agent://router-a/lint", endpoint: "http://127.0.0.1:19001/invoke",
    tenant: "eda", region: "local", costMicrounits: 10n, latencyMs: 20,
    trustLevel: 90, loadPermille: 10, remainingLeaseMs: 60000,
    path: ["router-a"], snapshotId: null,
  };
}

function localControl() {
  const output = [];
  return {
    output,
    sendRoute: (message) => { output.push({ type: "update", message }); return true; },
    sendWithdraw: (routeId) => { output.push({ type: "withdraw", routeId }); return true; },
    sendSnapshotEnd: () => true,
  };
}

function makeRuntime(relayId, relayRouterId) {
  const core = new RelayCore({ maxNodes: 32, maxPairs: 32 });
  const reflector = new ArpxRouteReflector({ relayRouterId, maxRoutes: 32 });
  const bridge = new FederationBridge({ relayId, relayRouterId, core, reflector });
  return { core, reflector, bridge };
}

function memoryLink(left, right, leftConfig, rightConfig) {
  const leftQueue = [];
  const rightQueue = [];
  const makeInvoke = (remote, remotePeerId) => (_metadata, handlers) => {
    let remoteFrame = null;
    let remoteClose = null;
    let closed = false;
    const remoteChannel = {
      send(frame) {
        if (closed) return false;
        handlers.onFrame(Buffer.from(frame));
        return true;
      },
      close() {
        if (closed) return;
        closed = true;
        handlers.onClose("remote-close");
      },
      onFrame(callback) { remoteFrame = callback; },
      onClose(callback) { remoteClose = callback; },
    };
    remote.acceptInvoke(remotePeerId, remoteChannel);
    return {
      send(frame) {
        if (closed || !remoteFrame) return false;
        remoteFrame(Buffer.from(frame));
        return true;
      },
      close() {
        if (closed) return;
        closed = true;
        if (remoteClose) remoteClose("local-close");
      },
    };
  };
  left.registerPeer(leftConfig, {
    sendControl: (message) => { leftQueue.push(message); return true; },
    openInvoke: makeInvoke(right, left.relayId),
  });
  right.registerPeer(rightConfig, {
    sendControl: (message) => { rightQueue.push(message); return true; },
    openInvoke: makeInvoke(left, right.relayId),
  });
  const flush = () => {
    let guard = 0;
    while ((leftQueue.length || rightQueue.length) && guard++ < 1000) {
      while (leftQueue.length) right.receiveControl(left.relayId, leftQueue.shift());
      while (rightQueue.length) left.receiveControl(right.relayId, rightQueue.shift());
    }
    if (guard >= 1000) throw new Error("federation control did not quiesce");
  };
  flush();
  return { flush };
}

function testCodec() {
  const decoded = [];
  const decoder = new ControlDecoder((message) => decoded.push(message));
  const frame = stringifyControl({
    version: 1, type: "hello", relay_id: "relay-a", boot_epoch: 42n,
  });
  decoder.push(frame.subarray(0, 3));
  decoder.push(frame.subarray(3));
  assert.strictEqual(decoded.length, 1);
  assert.strictEqual(decoded[0].boot_epoch, 42n);
  assert.throws(() => decoder.push(Buffer.alloc(65537)), /buffer exceeded/);
}

function testFederatedMembershipRouteAndInvoke() {
  const a = makeRuntime("relay-a", "router-relay-a");
  const b = makeRuntime("relay-b", "router-relay-b");
  const link = memoryLink(a.bridge, b.bridge, {
    relayId: "relay-b", relayRouterId: "router-relay-b",
    domainId: "relay.example", certificateDns: "relay-b.example", priority: 100,
  }, {
    relayId: "relay-a", relayRouterId: "router-relay-a",
    domainId: "relay.example", certificateDns: "relay-a.example", priority: 100,
  });

  const sourceFrames = [];
  const targetFrames = [];
  assert(a.core.registerNode("router-a", (frame) => {
    sourceFrames.push(Buffer.from(frame)); return true;
  }));
  assert(b.core.registerNode("router-b", (frame) => {
    targetFrames.push(Buffer.from(frame)); return true;
  }));
  const sourceControl = localControl();
  const targetControl = localControl();
  a.reflector.registerNode("router-a", sourceControl);
  b.reflector.registerNode("router-b", targetControl);
  a.bridge.localNodeUp("router-a");
  b.bridge.localNodeUp("router-b");
  link.flush();

  assert(a.core.nodes.has("router-b"));
  assert(b.core.nodes.has("router-a"));
  assert.strictEqual(a.bridge.snapshot().virtualNodes, 1);
  assert.strictEqual(b.bridge.snapshot().virtualNodes, 1);

  a.reflector.receive("router-a", capabilityUpdate());
  link.flush();
  const learned = targetControl.output.find((item) => item.type === "update");
  assert(learned);
  assert.deepStrictEqual(learned.message.path,
    ["router-a", "router-relay-a", "router-relay-b"]);
  assert.strictEqual(b.reflector.snapshot().routes, 1);

  assert(a.core.receive("router-a", makeOpen(1, "router-b", 8)));
  assert.strictEqual(targetFrames.length, 1);
  const remoteOpen = decodeFrame(targetFrames[0]);
  assert.strictEqual(remoteOpen.type, TYPE.OPEN);
  assert.strictEqual(remoteOpen.streamId, 2);
  assert.strictEqual(remoteOpen.hopLimit, 7);
  assert.strictEqual(remoteOpen.forwardingAssertion,
    "nfa1.router-a.payload.signature");

  assert(b.core.receive("router-b", encodeAccept(2, 200, 4096)));
  assert.strictEqual(decodeFrame(sourceFrames[0]).type, TYPE.ACCEPT);
  assert(a.core.receive("router-a", encodeData(1, 2n, Buffer.from("request"))));
  assert.strictEqual(decodeFrame(targetFrames[1]).data.toString(), "request");
  assert(b.core.receive("router-b", encodeResponseStart(2, 2n)));
  assert(b.core.receive("router-b", encodeData(2, 3n, Buffer.from("response"))));
  assert.strictEqual(decodeFrame(sourceFrames.at(-1)).data.toString(), "response");
  assert(a.core.receive("router-a", encodeEnd(1, 3n)));
  assert(b.core.receive("router-b", encodeEnd(2, 4n)));
  assert.strictEqual(a.bridge.snapshot().invokesOpened, 1);
  assert.strictEqual(a.bridge.snapshot().invokesCompleted, 1);

  assert(a.core.receive("router-a", makeOpen(3, "router-b", 1)));
  const hopReset = decodeFrame(sourceFrames.at(-1));
  assert.strictEqual(hopReset.type, TYPE.RESET);
  assert.strictEqual(hopReset.resetCode, RESET.FEDERATION_HOP_LIMIT);
  assert.strictEqual(a.bridge.snapshot().hopLimitRejected, 1);

  a.reflector.unregisterNode("router-a");
  link.flush();
  assert(targetControl.output.some((item) => item.type === "withdraw"));
  b.bridge.localNodeDown("router-b");
  b.core.unregisterNode("router-b");
  b.reflector.unregisterNode("router-b");
  link.flush();
  assert(!a.core.nodes.has("router-b"));

  assert.strictEqual(a.bridge.receiveControl("relay-b", {
    version: 1, type: "member_up", router_id: "router-loop",
    origin_relay_id: "relay-a", sequence: 1, path: ["relay-a", "relay-b"],
  }), false);
  assert.strictEqual(a.bridge.snapshot().memberLoopsRejected, 1);
  a.reflector.close();
  b.reflector.close();
}

function testStableSelectionAndPeerFailure() {
  const runtime = makeRuntime("relay-local", "router-relay-local");
  const sent = { a: [], b: [] };
  const transport = (bucket) => ({
    sendControl: (message) => { sent[bucket].push(message); return true; },
    openInvoke: () => ({ send: () => true, close: () => {} }),
  });
  runtime.bridge.registerPeer({
    relayId: "relay-a", relayRouterId: "router-relay-a",
    domainId: "relay.example", certificateDns: "relay-a.example", priority: 50,
  }, transport("a"));
  runtime.bridge.registerPeer({
    relayId: "relay-b", relayRouterId: "router-relay-b",
    domainId: "relay.example", certificateDns: "relay-b.example", priority: 100,
  }, transport("b"));
  runtime.bridge.receiveControl("relay-a", {
    version: 1, type: "member_up", router_id: "router-z",
    origin_relay_id: "relay-origin", sequence: 1,
    path: ["relay-origin", "relay-a"],
  });
  runtime.bridge.receiveControl("relay-b", {
    version: 1, type: "member_up", router_id: "router-z",
    origin_relay_id: "relay-origin", sequence: 1,
    path: ["relay-origin", "relay-b"],
  });
  assert.strictEqual(runtime.bridge.selected.get("router-z").peerId, "relay-b");
  runtime.bridge.unregisterPeer("relay-b", "test-failure");
  assert.strictEqual(runtime.bridge.selected.get("router-z").peerId, "relay-a");
  runtime.reflector.close();
}

function testFederatedCapabilityFailover() {
  const reflector = new ArpxRouteReflector({
    relayRouterId: "router-relay-local", maxRoutes: 8,
  });
  const local = localControl();
  reflector.registerNode("router-local", local);
  reflector.registerFederationPeer("router-relay-a", localControl());
  reflector.registerFederationPeer("router-relay-b", localControl());
  const base = capabilityUpdate();
  reflector.receiveFederated("router-relay-a", {
    ...base, path: ["router-origin", "router-relay-a"],
  });
  reflector.receiveFederated("router-relay-b", {
    ...base, path: ["router-origin", "router-relay-b"],
  });
  assert.strictEqual(reflector.routes.get(base.routeId).ownerRouterId,
    "router-relay-a");
  reflector.unregisterNode("router-relay-a");
  assert.strictEqual(reflector.routes.get(base.routeId).ownerRouterId,
    "router-relay-b");
  const lastUpdate = local.output.filter((item) => item.type === "update").at(-1);
  assert.deepStrictEqual(lastUpdate.message.path,
    ["router-origin", "router-relay-b", "router-relay-local"]);
  reflector.close();
}

testCodec();
testFederatedMembershipRouteAndInvoke();
testStableSelectionAndPeerFailure();
testFederatedCapabilityFailover();
process.stdout.write("Nexus Relay P6.2.3 federation tests passed\n");
