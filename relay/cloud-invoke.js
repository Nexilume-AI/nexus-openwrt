"use strict";

const crypto = require("crypto");
const fs = require("fs");
const https = require("https");
const {
  TYPE, RESET, MAX_DATA, decodeFrame, encodeOpen, encodeData, encodeEnd,
  encodeReset, encodeWindowUpdate,
} = require("./relay-tunnel");

const DEFAULT_MAX_RESPONSE_HEADER_BYTES = 8192;
const DEFAULT_RESPONSE_CONTENT_TYPE = "application/octet-stream";

function b64Json(value, name) {
  if (typeof value !== "string" || !/^[A-Za-z0-9_-]+$/.test(value)) {
    throw new Error(`invalid ${name}`);
  }
  const raw = Buffer.from(value, "base64url");
  if (raw.toString("base64url") !== value) throw new Error(`non-canonical ${name}`);
  try { return JSON.parse(raw.toString("utf8")); }
  catch (_error) { throw new Error(`invalid ${name}`); }
}

function isHttpToken(value) {
  if (value.length < 1) return false;
  const punctuation = "!#$%&'*+-.^_`|~";
  for (const character of value) {
    const code = character.charCodeAt(0);
    if (!((code >= 0x30 && code <= 0x39) ||
          (code >= 0x41 && code <= 0x5a) ||
          (code >= 0x61 && code <= 0x7a) ||
          punctuation.includes(character))) return false;
  }
  return true;
}

function isContentType(value) {
  let offset = 0;
  const skipWhitespace = () => {
    while (value[offset] === " " || value[offset] === "\t") offset++;
  };
  const token = () => {
    const start = offset;
    while (offset < value.length && isHttpToken(value[offset])) offset++;
    return offset > start;
  };

  if (!token() || value[offset++] !== "/" || !token()) return false;
  while (offset < value.length) {
    skipWhitespace();
    if (value[offset++] !== ";") return false;
    skipWhitespace();
    if (!token()) return false;
    skipWhitespace();
    if (value[offset++] !== "=") return false;
    skipWhitespace();
    if (value[offset] === '"') {
      offset++;
      let closed = false;
      while (offset < value.length) {
        const code = value.charCodeAt(offset);
        if (value[offset] === '"') {
          offset++;
          closed = true;
          break;
        }
        if (value[offset] === "\\") {
          offset++;
          if (offset >= value.length) return false;
          const escaped = value.charCodeAt(offset++);
          if (escaped !== 0x09 && (escaped < 0x20 || escaped > 0x7e)) return false;
          continue;
        }
        if (code !== 0x09 &&
            code !== 0x20 && code !== 0x21 &&
            (code < 0x23 || code > 0x5b) &&
            (code < 0x5d || code > 0x7e)) return false;
        offset++;
      }
      if (!closed) return false;
    } else if (!token()) {
      return false;
    }
    skipWhitespace();
  }
  return true;
}

function parseRawHttpResponse(buffer, maxHeaderBytes, maxBodyBytes) {
  if (!Buffer.isBuffer(buffer) || buffer.length > maxHeaderBytes + maxBodyBytes) {
    throw new Error("raw HTTP response exceeds bound");
  }
  const separator = buffer.indexOf("\r\n\r\n");
  if (separator < 0 || separator + 4 > maxHeaderBytes) {
    throw new Error("invalid raw HTTP response header boundary");
  }
  const lines = buffer.subarray(0, separator).toString("latin1").split("\r\n");
  const status = /^HTTP\/1\.1 ([2-5][0-9]{2})(?: ([\x20-\x7e]*))?$/.exec(lines.shift() || "");
  if (!status) throw new Error("invalid raw HTTP response status");
  const statusCode = Number(status[1]);
  if (statusCode >= 300 && statusCode <= 399) {
    throw new Error("raw HTTP redirects are forbidden");
  }

  let contentLength = null;
  let contentType = null;
  for (const line of lines) {
    if (line.length < 1 || line[0] === " " || line[0] === "\t") {
      throw new Error("invalid raw HTTP response header folding");
    }
    const colon = line.indexOf(":");
    const name = colon > 0 ? line.slice(0, colon) : "";
    if (!isHttpToken(name)) throw new Error("invalid raw HTTP response header name");
    const value = line.slice(colon + 1).replace(/^[ \t]+|[ \t]+$/g, "");
    for (let index = 0; index < value.length; index++) {
      const code = value.charCodeAt(index);
      if ((code < 0x20 && code !== 0x09) || code > 0x7e) {
        throw new Error("invalid raw HTTP response header value");
      }
    }
    switch (name.toLowerCase()) {
      case "content-length": {
        if (contentLength !== null || !/^[0-9]+$/.test(value)) {
          throw new Error("invalid raw HTTP Content-Length");
        }
        const parsed = Number(value);
        if (!Number.isSafeInteger(parsed) || parsed > maxBodyBytes) {
          throw new Error("raw HTTP response body exceeds bound");
        }
        contentLength = parsed;
        break;
      }
      case "content-type":
        if (contentType !== null || value.length > 127 || !isContentType(value)) {
          throw new Error("invalid raw HTTP Content-Type");
        }
        contentType = value;
        break;
      case "transfer-encoding":
        throw new Error("raw HTTP Transfer-Encoding is forbidden");
      default:
        break;
    }
  }
  if (contentLength === null) throw new Error("raw HTTP Content-Length is required");
  const body = buffer.subarray(separator + 4);
  if (body.length !== contentLength) throw new Error("raw HTTP Content-Length mismatch");
  return {
    statusCode,
    contentType: contentType || DEFAULT_RESPONSE_CONTENT_TYPE,
    body: Buffer.from(body),
  };
}

class RelayJwtVerifier {
  constructor(options) {
    this.issuer = options.issuer;
    this.audience = options.audience;
    this.keyId = options.keyId;
    this.publicKey = fs.readFileSync(options.publicKey);
    this.maxReplay = options.maxReplay || 4096;
    this.clockSkewSeconds = options.clockSkewSeconds || 5;
    this.used = new Map();
  }

  verify(token, expected, body, now = Math.floor(Date.now() / 1000)) {
    if (typeof token !== "string" || token.length > 4096) throw new Error("invalid Relay JWT");
    const parts = token.split(".");
    if (parts.length !== 3) throw new Error("invalid Relay JWT");
    const header = b64Json(parts[0], "JWT header");
    const claims = b64Json(parts[1], "JWT claims");
    if (header.alg !== "RS256" || header.kid !== this.keyId || header.typ !== "at+jwt") {
      throw new Error("invalid Relay JWT header");
    }
    if (!crypto.verify("RSA-SHA256", Buffer.from(`${parts[0]}.${parts[1]}`),
      this.publicKey, Buffer.from(parts[2], "base64url"))) {
      throw new Error("invalid Relay JWT signature");
    }
    if (claims.iss !== this.issuer || claims.aud !== this.audience ||
        claims.sub !== "service://nexus-server" || claims.scope !== "relay.invoke" ||
        claims.target_router !== expected.targetRouterId ||
        claims.target_agent !== expected.targetAgent || claims.intent !== expected.intent ||
        claims.task_id !== expected.taskId ||
        claims.body_sha256 !== crypto.createHash("sha256").update(body).digest("hex") ||
        !Number.isSafeInteger(claims.iat) || !Number.isSafeInteger(claims.nbf) ||
        !Number.isSafeInteger(claims.exp) || claims.exp <= claims.iat ||
        claims.exp - claims.iat > 120 || claims.iat > now + this.clockSkewSeconds ||
        claims.nbf > claims.iat || claims.nbf > now + this.clockSkewSeconds ||
        claims.exp <= now - this.clockSkewSeconds ||
        typeof claims.jti !== "string" || !/^[a-f0-9]{32}$/.test(claims.jti)) {
      throw new Error("invalid or expired Relay JWT claims");
    }
    for (const [jti, expiry] of this.used) if (expiry <= now) this.used.delete(jti);
    if (this.used.has(claims.jti)) throw new Error("Relay JWT replayed");
    if (this.used.size >= this.maxReplay) throw new Error("Relay JWT replay table full");
    this.used.set(claims.jti, claims.exp);
    return claims;
  }
}

class CloudInvokeMux {
  constructor(core, options = {}) {
    this.core = core;
    this.routerId = options.sourceRouterId || "nexus-cloud";
    this.maxInflight = options.maxInflight || 64;
    this.maxBodyBytes = options.maxBodyBytes || 65536;
    this.maxResponseHeaderBytes = options.maxResponseHeaderBytes ??
      DEFAULT_MAX_RESPONSE_HEADER_BYTES;
    if (!Number.isSafeInteger(this.maxResponseHeaderBytes) ||
        this.maxResponseHeaderBytes < 1 || this.maxResponseHeaderBytes > 65536) {
      throw new Error("invalid Relay response header bound");
    }
    this.timeoutMs = options.timeoutMs || 30000;
    this.nextStreamId = 1;
    this.calls = new Map();
    if (!core.registerNode(this.routerId, (frame) => this.receive(frame))) {
      throw new Error("could not register Nexus cloud Relay source");
    }
  }

  invoke(metadata, body) {
    if (!Buffer.isBuffer(body) || body.length < 1 || body.length > this.maxBodyBytes ||
        this.calls.size >= this.maxInflight) {
      return Promise.reject(new Error("cloud invocation capacity exceeded"));
    }
    const streamId = this.allocateStreamId();
    return new Promise((resolve, reject) => {
      const call = {
        streamId, metadata, body, offset: 0, credit: 0, nextSequence: 2n,
        response: [], responseBytes: 0, accepted: false, responseStarted: false,
        rawHeaderLength: null, resolve, reject, timer: null,
      };
      call.timer = setTimeout(() => this.fail(call, "Relay invocation timed out", true), this.timeoutMs);
      call.timer.unref();
      this.calls.set(streamId, call);
      try {
        this.core.receive(this.routerId, encodeOpen({
          streamId,
          targetRouterId: metadata.targetRouterId,
          intentClass: metadata.intent,
          taskId: metadata.taskId,
          sourceAgent: metadata.sourceAgent,
          targetAgent: metadata.targetAgent,
          tenant: metadata.tenant,
          region: "*",
          hopLimit: 8,
          streaming: false,
          maxCostMicrounits: Number.MAX_SAFE_INTEGER,
          maxLatencyMs: this.timeoutMs,
          forwardingAssertion: metadata.forwardingAssertion,
        }));
      } catch (error) {
        this.finish(call);
        reject(error);
      }
    });
  }

  allocateStreamId() {
    for (let attempts = 0; attempts < 0x7fffffff; attempts++) {
      const streamId = this.nextStreamId;
      this.nextStreamId += 2;
      if (this.nextStreamId > 0xffffffff) this.nextStreamId = 1;
      if (!this.calls.has(streamId)) return streamId;
    }
    throw new Error("Relay cloud stream id space exhausted");
  }

  receive(frame) {
    const message = decodeFrame(frame);
    const call = this.calls.get(message.streamId);
    if (!call) return false;
    try {
      if (message.type === TYPE.ACCEPT) {
        call.accepted = true;
        call.credit += message.creditBytes;
        this.pump(call);
      } else if (message.type === TYPE.WINDOW_UPDATE) {
        call.credit += message.creditBytes;
        this.pump(call);
      } else if (message.type === TYPE.RESPONSE_START) {
        if (call.responseStarted || call.responseBytes !== 0) {
          throw new Error("invalid Relay response framing");
        }
        call.responseStarted = true;
      } else if (message.type === TYPE.DATA) {
        call.responseBytes += message.data.length;
        const responseBound = call.responseStarted ? this.maxBodyBytes :
          this.maxBodyBytes + this.maxResponseHeaderBytes;
        if (call.responseBytes > responseBound) throw new Error("Relay response exceeds bound");
        call.response.push(Buffer.from(message.data));
        if (!call.responseStarted && call.rawHeaderLength === null) {
          const buffered = Buffer.concat(call.response, call.responseBytes);
          const separator = buffered.indexOf("\r\n\r\n");
          if (separator >= 0) {
            call.rawHeaderLength = separator + 4;
            if (call.rawHeaderLength > this.maxResponseHeaderBytes) {
              throw new Error("Relay response header exceeds bound");
            }
          } else if (call.responseBytes >= this.maxResponseHeaderBytes) {
            throw new Error("Relay response header exceeds bound");
          }
        }
        this.core.receive(this.routerId,
          encodeWindowUpdate(call.streamId, call.nextSequence++, message.data.length));
      } else if (message.type === TYPE.END) {
        const body = Buffer.concat(call.response);
        const result = call.responseStarted ? {
          statusCode: 200,
          contentType: "application/json",
          body,
        } : parseRawHttpResponse(
          body, this.maxResponseHeaderBytes, this.maxBodyBytes);
        this.finish(call);
        call.resolve(result);
      } else if (message.type === TYPE.RESET) {
        const statusCode = message.resetCode === RESET.TARGET_UNAVAILABLE ? 503 :
          message.resetCode === RESET.AUTHORIZATION ? 403 : 502;
        this.finish(call);
        call.resolve({ statusCode, contentType: "application/json",
          body: Buffer.from(JSON.stringify({ error: "relay_reset", code: message.resetCode })) });
      }
      return true;
    } catch (error) {
      this.fail(call, error.message, true);
      return false;
    }
  }

  pump(call) {
    if (!call.accepted) return;
    while (call.offset < call.body.length && call.credit > 0) {
      const length = Math.min(MAX_DATA, call.credit, call.body.length - call.offset);
      const chunk = call.body.subarray(call.offset, call.offset + length);
      this.core.receive(this.routerId, encodeData(call.streamId, call.nextSequence++, chunk));
      call.offset += length;
      call.credit -= length;
    }
    if (call.offset === call.body.length && !call.requestEnded) {
      call.requestEnded = true;
      this.core.receive(this.routerId, encodeEnd(call.streamId, call.nextSequence++));
    }
  }

  fail(call, message, sendReset) {
    if (!this.calls.has(call.streamId)) return;
    if (sendReset) {
      try { this.core.receive(this.routerId, encodeReset(call.streamId, call.nextSequence++, RESET.PROTOCOL)); }
      catch (_error) { /* pair may already be gone */ }
    }
    this.finish(call);
    call.reject(new Error(message));
  }

  finish(call) {
    clearTimeout(call.timer);
    this.calls.delete(call.streamId);
  }

  close() {
    for (const call of [...this.calls.values()]) this.fail(call, "Relay is shutting down", true);
    this.core.unregisterNode(this.routerId);
  }
}

function header(request, name, maximum = 2048) {
  const value = request.headers[name];
  if (typeof value !== "string" || value.length < 1 || value.length > maximum || /[\r\n]/.test(value)) {
    throw new Error(`missing or invalid ${name}`);
  }
  return value;
}

function certificateDns(socket) {
  const certificate = socket.getPeerCertificate(true);
  if (!socket.authorized || !certificate || typeof certificate.subjectaltname !== "string") return [];
  return certificate.subjectaltname.split(/,\s*/).filter((item) => item.startsWith("DNS:"))
    .map((item) => item.slice(4));
}

function createCloudInvokeServer(core, config, capture = () => {}) {
  const mux = new CloudInvokeMux(core, config);
  const verifier = new RelayJwtVerifier({
    ...config.jwt,
    audience: `urn:nexus:relay:${config.relayId}`,
  });
  const server = https.createServer({
    key: fs.readFileSync(config.tls.key),
    cert: fs.readFileSync(config.tls.cert),
    ca: fs.readFileSync(config.tls.ca),
    requestCert: true,
    rejectUnauthorized: true,
    minVersion: "TLSv1.3",
    maxVersion: "TLSv1.3",
    maxHeaderSize: 8192,
  }, (request, response) => {
    if (request.method !== "POST" || request.url !== "/cloud/invoke/v1" ||
        request.headers["content-type"] !== "application/vnd.nexus.agent-envelope+json" ||
        !certificateDns(request.socket).includes(config.clientDns)) {
      response.writeHead(403, { Connection: "close" }); response.end(); return;
    }
    const chunks = [];
    let used = 0;
    request.on("data", (chunk) => {
      used += chunk.length;
      if (used > config.maxBodyBytes) request.destroy(new Error("request body exceeds bound"));
      else chunks.push(chunk);
    });
    request.on("error", () => { if (!response.headersSent) response.destroy(); });
    request.on("end", async () => {
      const body = Buffer.concat(chunks);
      try {
        const metadata = {
          sourceRouterId: header(request, "x-nexus-source-router", 64),
          targetRouterId: header(request, "x-nexus-target-router", 64),
          targetAgent: header(request, "x-nexus-target-agent", 255),
          sourceAgent: header(request, "x-nexus-source-agent", 255),
          tenant: header(request, "x-nexus-tenant", 63),
          intent: header(request, "x-nexus-intent", 127),
          taskId: header(request, "x-nexus-task-id", 64),
          forwardingAssertion: header(request, "x-nexus-forwarding-assertion", 2048),
        };
        if (metadata.sourceRouterId !== config.sourceRouterId) {
          throw new Error("invalid Nexus cloud source router");
        }
        const authorization = header(request, "authorization", 4096);
        if (!authorization.startsWith("Bearer ")) throw new Error("missing Relay JWT");
        verifier.verify(authorization.slice(7), metadata, body);
        const result = await mux.invoke(metadata, body);
        capture("cloud-invoke-completed", {
          target_router_id: metadata.targetRouterId, task_id: metadata.taskId,
          status: result.statusCode, request_bytes: body.length, response_bytes: result.body.length,
        });
        response.writeHead(result.statusCode, {
          "Content-Type": result.contentType || "application/json",
          "Content-Length": result.body.length,
          "Cache-Control": "no-store",
          Connection: "close",
        });
        response.end(result.body);
      } catch (error) {
        capture("cloud-invoke-rejected", { error: error.message });
        const status = /JWT|certificate|authorization|source router/i.test(error.message) ? 401 : 502;
        const body = Buffer.from(JSON.stringify({ error: status === 401 ? "unauthorized" : "relay_unavailable" }));
        response.writeHead(status, { "Content-Type": "application/json", "Content-Length": body.length, Connection: "close" });
        response.end(body);
      }
    });
  });
  return { server, mux, verifier, close: (callback) => { mux.close(); server.close(callback); } };
}

module.exports = { CloudInvokeMux, RelayJwtVerifier, createCloudInvokeServer };
