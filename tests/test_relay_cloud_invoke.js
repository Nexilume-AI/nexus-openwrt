"use strict";

const assert = require("assert");
const crypto = require("crypto");
const fs = require("fs");
const https = require("https");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");
const { RelayCore } = require("../relay/relay-core");
const { CloudInvokeMux, RelayJwtVerifier, createCloudInvokeServer } = require("../relay/cloud-invoke");
const {
  TYPE, decodeFrame, encodeAccept, encodeData, encodeEnd, encodeResponseStart,
} = require("../relay/relay-tunnel");

function invocationMetadata() {
  return {
    targetRouterId: "router-nat-a",
    targetAgent: "agent://tenant/e2e",
    sourceAgent: "service://nexus-server",
    tenant: "tenant-e2e",
    intent: "nexus.e2e.edge_probe",
    taskId: "mcp:e2e-1",
    forwardingAssertion: "nfa1.test.payload.signature",
  };
}

async function testCloudMuxResponseStartCompatibility() {
  const core = new RelayCore({ maxNodes: 4, maxPairs: 4, initialWindow: 4096 });
  let destinationSequence = 1n;
  assert(core.registerNode("router-nat-a", (frame) => {
    const message = decodeFrame(frame);
    if (message.type === TYPE.OPEN) {
      assert.strictEqual(message.targetAgent, "agent://tenant/e2e");
      assert.strictEqual(message.forwardingAssertion, "nfa1.test.payload.signature");
      destinationSequence = 2n;
      setImmediate(() => core.receive("router-nat-a", encodeAccept(message.streamId, 200, 4096)));
    } else if (message.type === TYPE.DATA) {
      const envelope = JSON.parse(message.data.toString("utf8"));
      assert.strictEqual(envelope.task_id, "mcp:e2e-1");
    } else if (message.type === TYPE.END) {
      setImmediate(() => {
        core.receive("router-nat-a", encodeResponseStart(message.streamId, destinationSequence++));
        core.receive("router-nat-a", encodeData(message.streamId, destinationSequence++,
          Buffer.from('{"nonce":"n-1","terminal":"sdk"}')));
        core.receive("router-nat-a", encodeEnd(message.streamId, destinationSequence++));
      });
    }
    return true;
  }));
  const mux = new CloudInvokeMux(core, {
    sourceRouterId: "nexus-cloud", maxInflight: 2, maxBodyBytes: 4096, timeoutMs: 1000,
  });
  const body = Buffer.from(JSON.stringify({ task_id: "mcp:e2e-1" }));
  const result = await mux.invoke(invocationMetadata(), body);
  assert.strictEqual(result.statusCode, 200);
  assert.strictEqual(result.contentType, "application/json");
  assert.deepStrictEqual(JSON.parse(result.body.toString()), { nonce: "n-1", terminal: "sdk" });
  mux.close();
}

async function invokeRawResponse(chunks, options = {}) {
  const core = new RelayCore({ maxNodes: 4, maxPairs: 4, initialWindow: 4096 });
  let destinationSequence = 1n;
  assert(core.registerNode("router-nat-a", (frame) => {
    const message = decodeFrame(frame);
    if (message.type === TYPE.OPEN) {
      destinationSequence = 2n;
      setImmediate(() => core.receive("router-nat-a",
        encodeAccept(message.streamId, 200, 4096)));
    } else if (message.type === TYPE.END) {
      setImmediate(() => {
        for (const chunk of chunks) {
          try {
            if (!core.receive("router-nat-a",
              encodeData(message.streamId, destinationSequence++, chunk))) return;
          } catch (_error) {
            return;
          }
        }
        try {
          core.receive("router-nat-a", encodeEnd(message.streamId, destinationSequence++));
        } catch (_error) { /* a fail-closed parser may already have reset the pair */ }
      });
    }
    return true;
  }));
  const mux = new CloudInvokeMux(core, {
    sourceRouterId: "nexus-cloud", maxInflight: 2,
    maxBodyBytes: 4096, timeoutMs: 1000, ...options,
  });
  try {
    return await mux.invoke(invocationMetadata(),
      Buffer.from(JSON.stringify({ task_id: "mcp:e2e-1" })));
  } finally {
    mux.close();
  }
}

async function testCloudMuxRawHttpRoundTrip() {
  const body = Buffer.from('{"nonce":"n-raw","terminal":"sdk"}');
  const header = Buffer.from(
    `HTTP/1.1 201 Created\r\nContent-Length: ${body.length}\r\n` +
    "Content-Type: application/json; charset=utf-8\r\nConnection: close\r\n\r\n");
  const response = Buffer.concat([header, body]);
  const boundary = header.length - 2;
  const result = await invokeRawResponse([
    response.subarray(0, 7),
    response.subarray(7, boundary),
    response.subarray(boundary, header.length + 5),
    response.subarray(header.length + 5),
  ]);
  assert.strictEqual(result.statusCode, 201);
  assert.strictEqual(result.contentType, "application/json; charset=utf-8");
  assert.deepStrictEqual(JSON.parse(result.body), { nonce: "n-raw", terminal: "sdk" });
}

async function testCloudMuxRejectsMalformedRawHttp() {
  const malformed = [
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n{}",
    "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\nContent-Length: 2\n\n{}",
  ];
  for (const response of malformed) {
    await assert.rejects(() => invokeRawResponse([Buffer.from(response)]),
      /raw HTTP|Relay response header/);
  }
  const invalidContentType = Buffer.concat([
    Buffer.from("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: application/"),
    Buffer.from([1]),
    Buffer.from("json\r\n\r\n{}"),
  ]);
  await assert.rejects(() => invokeRawResponse([invalidContentType]), /raw HTTP/);

  const oversizedHeader = Buffer.from(
    `HTTP/1.1 200 OK\r\nX-Fill: ${"a".repeat(80)}\r\nContent-Length: 0\r\n\r\n`);
  await assert.rejects(
    () => invokeRawResponse([oversizedHeader], { maxResponseHeaderBytes: 64 }),
    /header exceeds bound/);
}

function segment(value) {
  return Buffer.from(JSON.stringify(value)).toString("base64url");
}

function relayToken(privateKey, body, overrides = {}) {
  const now = Math.floor(Date.now() / 1000);
  const header = segment({ alg: "RS256", kid: "relay-rs256-1", typ: "at+jwt" });
  const claims = segment({
    iss: "https://nexus.example/relay",
    aud: "urn:nexus:relay:relay-east-1",
    sub: "service://nexus-server",
    scope: "relay.invoke",
    target_router: "router-nat-a",
    target_agent: "agent://tenant/e2e",
    intent: "nexus.e2e.edge_probe",
    task_id: "mcp:e2e-1",
    body_sha256: crypto.createHash("sha256").update(body).digest("hex"),
    iat: now, nbf: now, exp: now + 60,
    jti: "0123456789abcdef0123456789abcdef",
    ...overrides,
  });
  const input = `${header}.${claims}`;
  return `${input}.${crypto.sign("RSA-SHA256", Buffer.from(input), privateKey).toString("base64url")}`;
}

function testRelayJwtBoundary() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "nexus-relay-jwt-"));
  try {
    const { privateKey, publicKey } = crypto.generateKeyPairSync("rsa", { modulusLength: 2048 });
    const publicFile = path.join(directory, "relay-jwt-public.pem");
    fs.writeFileSync(publicFile, publicKey.export({ type: "spki", format: "pem" }), { mode: 0o600 });
    const verifier = new RelayJwtVerifier({
      issuer: "https://nexus.example/relay",
      audience: "urn:nexus:relay:relay-east-1",
      keyId: "relay-rs256-1",
      publicKey: publicFile,
      maxReplay: 4,
      clockSkewSeconds: 0,
    });
    const body = Buffer.from('{"task_id":"mcp:e2e-1"}');
    const expected = {
      targetRouterId: "router-nat-a",
      targetAgent: "agent://tenant/e2e",
      intent: "nexus.e2e.edge_probe",
      taskId: "mcp:e2e-1",
    };
    const token = relayToken(privateKey, body);
    assert.throws(() => verifier.verify(token, expected, Buffer.from("tampered")), /claims/);
    assert.strictEqual(verifier.verify(token, expected, body).target_router, "router-nat-a");
    assert.throws(() => verifier.verify(token, expected, body), /replayed/);
    const wrongTarget = relayToken(privateKey, body, {
      target_router: "router-other",
      jti: "1123456789abcdef0123456789abcdef",
    });
    assert.throws(() => verifier.verify(wrongTarget, expected, body), /claims/);
    const now = Math.floor(Date.now() / 1000);
    const futureIssued = relayToken(privateKey, body, {
      iat: now + 30,
      nbf: now,
      exp: now + 60,
      jti: "2123456789abcdef0123456789abcdef",
    });
    assert.throws(() => verifier.verify(futureIssued, expected, body, now), /claims/);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

function openssl(args, directory) {
  const result = spawnSync("openssl", args, {
    cwd: directory,
    env: { ...process.env, OPENSSL_CONF: path.join(directory, "openssl.cnf") },
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.status !== 0) throw new Error(`openssl ${args[0]} failed: ${result.stderr}`);
}

function createCloudCertificates(directory) {
  fs.writeFileSync(path.join(directory, "openssl.cnf"),
    "[req]\ndistinguished_name=dn\nprompt=no\n[dn]\nCN=unused\n");
  for (const ca of ["server-ca", "client-ca"]) {
    openssl(["req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", "2",
      "-subj", `/CN=${ca}`, "-keyout", `${ca}.key`,
      "-addext", "basicConstraints=critical,CA:TRUE",
      "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-out", `${ca}.pem`], directory);
  }
  for (const [name, dns, ca, usage] of [
    ["relay-cloud", "cloud.relay.test", "server-ca", "serverAuth"],
    ["nexus-server", "server.nexus.test", "client-ca", "clientAuth"],
  ]) {
    openssl(["req", "-newkey", "rsa:2048", "-sha256", "-nodes", "-subj", `/CN=${dns}`,
      "-keyout", `${name}.key`, "-out", `${name}.csr`], directory);
    fs.writeFileSync(path.join(directory, `${name}.ext`),
      `subjectAltName=DNS:${dns}\nextendedKeyUsage=${usage}\nkeyUsage=digitalSignature,keyEncipherment\n`);
    openssl(["x509", "-req", "-sha256", "-days", "2", "-in", `${name}.csr`,
      "-CA", `${ca}.pem`, "-CAkey", `${ca}.key`, "-CAcreateserial", "-extfile", `${name}.ext`,
      "-out", `${name}.pem`], directory);
  }
}

function cloudRequest(directory, port, body, token, overrides = {}, identity = true) {
  return new Promise((resolve, reject) => {
    const options = {
      hostname: "cloud.relay.test", port, path: "/cloud/invoke/v1", method: "POST",
      ca: fs.readFileSync(path.join(directory, "server-ca.pem")), servername: "cloud.relay.test",
      minVersion: "TLSv1.3", maxVersion: "TLSv1.3", agent: false,
      lookup: (_hostname, settings, callback) => settings && settings.all
        ? callback(null, [{ address: "127.0.0.1", family: 4 }]) : callback(null, "127.0.0.1", 4),
      headers: {
        Authorization: `Bearer ${token}`,
        "Content-Type": "application/vnd.nexus.agent-envelope+json",
        "Content-Length": body.length,
        "X-Nexus-Source-Router": "nexus-cloud",
        "X-Nexus-Target-Router": "router-nat-a",
        "X-Nexus-Target-Agent": "agent://tenant/e2e",
        "X-Nexus-Source-Agent": "service://nexus-server",
        "X-Nexus-Tenant": "tenant-e2e",
        "X-Nexus-Intent": "nexus.e2e.edge_probe",
        "X-Nexus-Task-Id": "mcp:e2e-1",
        "X-Nexus-Forwarding-Assertion": "nfa1.test.payload.signature",
        ...overrides,
      },
    };
    if (identity) {
      options.cert = fs.readFileSync(path.join(directory, "nexus-server.pem"));
      options.key = fs.readFileSync(path.join(directory, "nexus-server.key"));
    }
    if (token === null) delete options.headers.Authorization;
    const request = https.request(options, (response) => {
      const chunks = [];
      response.on("data", (chunk) => chunks.push(chunk));
      response.on("end", () => resolve({
        statusCode: response.statusCode,
        headers: response.headers,
        body: Buffer.concat(chunks),
      }));
    });
    request.on("error", reject);
    request.end(body);
  });
}

async function testCloudHttpsSecurityBoundary() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "nexus-relay-cloud-tls-"));
  let ingress;
  try {
    createCloudCertificates(directory);
    const { privateKey, publicKey } = crypto.generateKeyPairSync("rsa", { modulusLength: 2048 });
    fs.writeFileSync(path.join(directory, "relay-jwt-public.pem"),
      publicKey.export({ type: "spki", format: "pem" }));
    const core = new RelayCore({ maxNodes: 4, maxPairs: 4, initialWindow: 4096 });
    let sequence = 1n;
    assert(core.registerNode("router-nat-a", (frame) => {
      const message = decodeFrame(frame);
      if (message.type === TYPE.OPEN) {
        sequence = 2n;
        setImmediate(() => core.receive("router-nat-a", encodeAccept(message.streamId, 200, 4096)));
      } else if (message.type === TYPE.END) {
        setImmediate(() => {
          const body = Buffer.from('{"ok":true}');
          const raw = Buffer.concat([Buffer.from(
            `HTTP/1.1 200 OK\r\nContent-Length: ${body.length}\r\n` +
            "Content-Type: application/json; charset=utf-8\r\n\r\n"), body]);
          const split = raw.indexOf("\r\n\r\n") + 2;
          core.receive("router-nat-a", encodeData(
            message.streamId, sequence++, raw.subarray(0, split)));
          core.receive("router-nat-a", encodeData(
            message.streamId, sequence++, raw.subarray(split)));
          core.receive("router-nat-a", encodeEnd(message.streamId, sequence++));
        });
      }
      return true;
    }));
    ingress = createCloudInvokeServer(core, {
      relayId: "relay-east-1", sourceRouterId: "nexus-cloud", clientDns: "server.nexus.test",
      maxInflight: 4, maxBodyBytes: 4096, timeoutMs: 2000,
      tls: { key: path.join(directory, "relay-cloud.key"), cert: path.join(directory, "relay-cloud.pem"), ca: path.join(directory, "client-ca.pem") },
      jwt: { issuer: "https://nexus.example/relay", keyId: "relay-rs256-1", publicKey: path.join(directory, "relay-jwt-public.pem"), clockSkewSeconds: 0 },
    });
    await new Promise((resolve) => ingress.server.listen(0, "127.0.0.1", resolve));
    const port = ingress.server.address().port;
    const body = Buffer.from('{"task_id":"mcp:e2e-1"}');
    assert.strictEqual((await cloudRequest(directory, port, body, null)).statusCode, 401);
    const token = relayToken(privateKey, body);
    const success = await cloudRequest(directory, port, body, token);
    assert.strictEqual(success.statusCode, 200);
    assert.strictEqual(success.headers["content-type"], "application/json; charset=utf-8");
    assert.deepStrictEqual(JSON.parse(success.body), { ok: true });
    assert.strictEqual((await cloudRequest(directory, port, body, token)).statusCode, 401);
    const wrongSourceToken = relayToken(privateKey, body, { jti: "3123456789abcdef0123456789abcdef" });
    assert.strictEqual((await cloudRequest(directory, port, body, wrongSourceToken,
      { "X-Nexus-Source-Router": "other-cloud" })).statusCode, 401);
    await assert.rejects(() => cloudRequest(directory, port, body,
      relayToken(privateKey, body, { jti: "4123456789abcdef0123456789abcdef" }), {}, false));
  } finally {
    if (ingress) await new Promise((resolve) => ingress.close(resolve));
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

testRelayJwtBoundary();
testCloudMuxResponseStartCompatibility()
  .then(() => testCloudMuxRawHttpRoundTrip())
  .then(() => testCloudMuxRejectsMalformedRawHttp())
  .then(() => testCloudHttpsSecurityBoundary()).then(() => {
  process.stdout.write("relay cloud invoke tests passed\n");
}).catch((error) => {
  process.stderr.write(`${error.stack}\n`);
  process.exitCode = 1;
});
