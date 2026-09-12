"use strict";

const crypto = require("crypto");

const TOKEN_PREFIX = "nrt1";
const MAX_TOKEN_LENGTH = 1536;
const CLAIM_KEYS = ["v", "aid", "relay", "router", "domain", "san", "iat", "exp", "nonce"];

function validIdentifier(value, maximum = 64) {
  return typeof value === "string" && value.length >= 1 && value.length <= maximum &&
    /^[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?$/.test(value);
}

function validRouteId(value) {
  return typeof value === "string" && value.length >= 1 && value.length <= 64 &&
    /^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$/.test(value);
}

function validDnsName(value) {
  if (typeof value !== "string" || value.length < 1 || value.length > 253) return false;
  return value.split(".").every((label) => label.length >= 1 && label.length <= 63 &&
    /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(label));
}

function decodeBase64Url(value, maximum, name) {
  if (typeof value !== "string" || value.length < 1 || value.length > maximum ||
      !/^[A-Za-z0-9_-]+$/.test(value)) throw new Error(`invalid ${name}`);
  const output = Buffer.from(value, "base64url");
  if (output.length < 1 || output.toString("base64url") !== value) {
    throw new Error(`non-canonical ${name}`);
  }
  return output;
}

function normalizeKeys(keys) {
  if (!keys || typeof keys !== "object" || Array.isArray(keys)) {
    throw new Error("ticketKeys must be an object");
  }
  const normalized = new Map();
  for (const [kid, encoded] of Object.entries(keys)) {
    if (!validIdentifier(kid, 32)) throw new Error("invalid ticket key id");
    if (encoded === "GENERATE_SHARED_TICKET_KEY") {
      throw new Error("ticket key is not provisioned; configure a fresh shared Relay/Directory key");
    }
    const secret = decodeBase64Url(encoded, 128, "ticket secret");
    if (secret.length < 32 || secret.length > 64) throw new Error("invalid ticket secret length");
    // The old example was also installed as runtime configuration. Reject its
    // fingerprint regardless of key ID, including secondary rotation keys.
    if (crypto.createHash("sha256").update(secret).digest("hex") ===
        "3eb1bd439947eb762998e566ccc2e099c791118b2f40579cc4f7da2b5061b7f9") {
      throw new Error("known public sample ticket key is forbidden; provision a fresh shared key");
    }
    normalized.set(kid, secret);
  }
  if (normalized.size < 1 || normalized.size > 4) throw new Error("invalid ticket key count");
  return normalized;
}

function canonicalClaims(claims) {
  const output = {};
  for (const key of CLAIM_KEYS) output[key] = claims[key];
  return JSON.stringify(output);
}

function validateClaims(claims, options, nowSeconds) {
  if (!claims || typeof claims !== "object" || Array.isArray(claims) ||
      Object.keys(claims).length !== CLAIM_KEYS.length ||
      CLAIM_KEYS.some((key) => !Object.hasOwn(claims, key)) || claims.v !== 1 ||
      !validIdentifier(claims.aid) || !validRouteId(claims.relay) ||
      !validRouteId(claims.router) || !validDnsName(claims.domain) ||
      !validDnsName(claims.san) || !validIdentifier(claims.nonce, 64) ||
      !Number.isSafeInteger(claims.iat) || !Number.isSafeInteger(claims.exp) ||
      claims.iat < 0 || claims.exp <= claims.iat ||
      claims.exp - claims.iat > options.maxLifetimeSeconds ||
      claims.iat > nowSeconds + options.clockSkewSeconds ||
      claims.exp <= nowSeconds - options.clockSkewSeconds ||
      claims.relay !== options.relayId) {
    throw new Error("invalid or expired Relay ticket claims");
  }
}

function issueTicket(options) {
  const keys = normalizeKeys(options.keys);
  const secret = keys.get(options.kid);
  if (!secret) throw new Error("unknown ticket signing key");
  const nowSeconds = options.nowSeconds === undefined
    ? Math.floor(Date.now() / 1000) : options.nowSeconds;
  const claims = { ...options.claims };
  validateClaims(claims, {
    relayId: claims.relay,
    maxLifetimeSeconds: options.maxLifetimeSeconds || 300,
    clockSkewSeconds: 0,
  }, nowSeconds);
  const payload = Buffer.from(canonicalClaims(claims), "utf8").toString("base64url");
  const signingInput = `${TOKEN_PREFIX}.${options.kid}.${payload}`;
  const signature = crypto.createHmac("sha256", secret).update(signingInput).digest("base64url");
  return `${signingInput}.${signature}`;
}

class TicketVerifier {
  constructor(options) {
    this.keys = normalizeKeys(options.keys);
    if (!validRouteId(options.relayId)) throw new Error("invalid Relay ticket identity");
    this.relayId = options.relayId;
    this.maxLifetimeSeconds = options.maxLifetimeSeconds || 300;
    this.clockSkewSeconds = options.clockSkewSeconds === undefined ? 5 : options.clockSkewSeconds;
    this.maxUsedTickets = options.maxUsedTickets || 4096;
    if (!Number.isInteger(this.maxLifetimeSeconds) || this.maxLifetimeSeconds < 30 ||
        this.maxLifetimeSeconds > 3600 || !Number.isInteger(this.clockSkewSeconds) ||
        this.clockSkewSeconds < 0 || this.clockSkewSeconds > 30 ||
        !Number.isInteger(this.maxUsedTickets) || this.maxUsedTickets < 1 ||
        this.maxUsedTickets > 65536) throw new Error("invalid ticket verifier bounds");
    this.used = new Map();
    this.metrics = { accepted: 0, rejected: 0, replayed: 0, expired: 0 };
  }

  verify(token, certificateDns, nowSeconds = Math.floor(Date.now() / 1000)) {
    try {
      if (typeof token !== "string" || token.length > MAX_TOKEN_LENGTH) {
        throw new Error("invalid Relay ticket length");
      }
      const parts = token.split(".");
      if (parts.length !== 4 || parts[0] !== TOKEN_PREFIX || !validIdentifier(parts[1], 32)) {
        throw new Error("invalid Relay ticket format");
      }
      const secret = this.keys.get(parts[1]);
      if (!secret) throw new Error("unknown Relay ticket key");
      const payload = decodeBase64Url(parts[2], 1400, "ticket payload");
      const supplied = decodeBase64Url(parts[3], 64, "ticket signature");
      const expected = crypto.createHmac("sha256", secret)
        .update(`${parts[0]}.${parts[1]}.${parts[2]}`).digest();
      if (supplied.length !== expected.length || !crypto.timingSafeEqual(supplied, expected)) {
        throw new Error("invalid Relay ticket signature");
      }
      let claims;
      try { claims = JSON.parse(payload.toString("utf8")); }
      catch (_error) { throw new Error("invalid Relay ticket payload"); }
      if (Buffer.from(canonicalClaims(claims), "utf8").compare(payload) !== 0) {
        throw new Error("non-canonical Relay ticket claims");
      }
      validateClaims(claims, this, nowSeconds);
      const certificateNames = Array.isArray(certificateDns)
        ? certificateDns : [certificateDns];
      if (!certificateNames.includes(claims.san)) {
        throw new Error("ticket certificate SAN mismatch");
      }
      return { claims, digest: crypto.createHash("sha256").update(token).digest("hex") };
    } catch (error) {
      this.metrics.rejected++;
      if (/expired/.test(error.message)) this.metrics.expired++;
      throw error;
    }
  }

  bind(token, certificateDns, session, nowSeconds = Math.floor(Date.now() / 1000)) {
    const verified = this.verify(token, certificateDns, nowSeconds);
    for (const [digest, entry] of this.used) {
      if (entry.exp <= nowSeconds) this.used.delete(digest);
    }
    const existing = this.used.get(verified.digest);
    if (existing && existing.session !== session) {
      this.metrics.replayed++;
      throw new Error("Relay ticket replayed on another session");
    }
    if (!existing) {
      if (this.used.size >= this.maxUsedTickets) throw new Error("Relay ticket replay table full");
      this.used.set(verified.digest, { session, exp: verified.claims.exp });
      this.metrics.accepted++;
    }
    return verified.claims;
  }

  snapshot() {
    return { usedTickets: this.used.size, ...this.metrics };
  }
}

module.exports = {
  MAX_TOKEN_LENGTH, TicketVerifier, issueTicket, normalizeKeys,
  validIdentifier, validRouteId, validDnsName,
};
