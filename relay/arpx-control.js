"use strict";

const MAX_FRAME = 4096;
const MIN_HEARTBEAT_MS = 1000;
const MAX_HEARTBEAT_MS = 60000;

function uintHead(major, input) {
  const value = BigInt(input);
  if (value < 24n) return Buffer.from([(major << 5) | Number(value)]);
  if (value <= 0xffn) return Buffer.from([(major << 5) | 24, Number(value)]);
  if (value <= 0xffffn) {
    const output = Buffer.alloc(3);
    output[0] = (major << 5) | 25;
    output.writeUInt16BE(Number(value), 1);
    return output;
  }
  if (value <= 0xffffffffn) {
    const output = Buffer.alloc(5);
    output[0] = (major << 5) | 26;
    output.writeUInt32BE(Number(value), 1);
    return output;
  }
  const output = Buffer.alloc(9);
  output[0] = (major << 5) | 27;
  output.writeBigUInt64BE(value, 1);
  return output;
}

function text(value) {
  const bytes = Buffer.from(value, "ascii");
  if (bytes.length < 1 || bytes.length !== value.length) throw new Error("invalid ARPX text");
  return Buffer.concat([uintHead(3, bytes.length), bytes]);
}

function array(values) {
  if (!Array.isArray(values) || values.length < 1 || values.length > 8) {
    throw new Error("invalid ARPX array");
  }
  return Buffer.concat([uintHead(4, values.length), ...values.map((value) => text(value))]);
}

function readHead(buffer, cursor, expectedMajor = null) {
  if (cursor.offset >= buffer.length) throw new Error("truncated CBOR head");
  const initial = buffer[cursor.offset++];
  const major = initial >> 5;
  if (expectedMajor !== null && major !== expectedMajor) throw new Error("wrong CBOR major");
  const additional = initial & 31;
  if (additional < 24) return { major, value: BigInt(additional) };
  const bytes = additional === 24 ? 1 : additional === 25 ? 2 :
    additional === 26 ? 4 : additional === 27 ? 8 : 0;
  if (bytes === 0 || cursor.offset + bytes > buffer.length) throw new Error("bad CBOR integer");
  let value = 0n;
  for (let index = 0; index < bytes; index++) {
    value = (value << 8n) | BigInt(buffer[cursor.offset++]);
  }
  if ((bytes === 1 && value < 24n) || (bytes === 2 && value <= 0xffn) ||
      (bytes === 4 && value <= 0xffffn) || (bytes === 8 && value <= 0xffffffffn)) {
    throw new Error("non-canonical CBOR integer");
  }
  return { major, value };
}

function readValue(buffer, cursor, depth = 0) {
  if (depth > 4) throw new Error("CBOR nesting exceeded");
  const head = readHead(buffer, cursor);
  if (head.major === 0) return head.value;
  const length = Number(head.value);
  if (!Number.isSafeInteger(length)) throw new Error("CBOR value too large");
  if (head.major === 3) {
    if (length < 1 || cursor.offset + length > buffer.length) throw new Error("bad CBOR text");
    const value = buffer.subarray(cursor.offset, cursor.offset + length).toString("ascii");
    cursor.offset += length;
    return value;
  }
  if (head.major === 4) {
    if (length > 8) throw new Error("ARPX array too large");
    const output = [];
    for (let index = 0; index < length; index++) {
      output.push(readValue(buffer, cursor, depth + 1));
    }
    return output;
  }
  throw new Error("unsupported ARPX CBOR type");
}

function decodeArpx(frame) {
  if (!Buffer.isBuffer(frame) || frame.length < 5 || frame.length > MAX_FRAME ||
      frame.readUInt32BE(0) !== frame.length - 4) throw new Error("bad ARPX frame");
  const payload = frame.subarray(4);
  const cursor = { offset: 0 };
  const map = readHead(payload, cursor, 5);
  const count = Number(map.value);
  if (count !== 7 && count !== 8 && count !== 20) throw new Error("bad ARPX field count");
  const values = [];
  for (let index = 0; index < count; index++) {
    const key = readHead(payload, cursor, 0).value;
    if (key !== BigInt(index)) throw new Error("non-canonical ARPX key order");
    values.push(readValue(payload, cursor));
  }
  if (cursor.offset !== payload.length) throw new Error("trailing ARPX bytes");
  const type = Number(values[1]);
  if ((type === 2 && count !== 20) || (type === 3 && count !== 8) ||
      ((type === 5 || type === 6) && count !== 8) ||
      ((type === 1 || type === 4) && count !== 7)) {
    throw new Error("ARPX type and field count mismatch");
  }
  return {
    version: Number(values[0]),
    type,
    routerId: values[2],
    domainId: values[3],
    epoch: values[4],
    sequence: values[5],
    heartbeatMs: Number(values[6]),
    routeId: (type === 2 || type === 3) ? values[7] : null,
    intent: type === 2 ? values[8] : null,
    capabilityVersion: type === 2 ? Number(values[9]) : 0,
    origin: type === 2 ? values[10] : null,
    endpoint: type === 2 ? values[11] : null,
    tenant: type === 2 ? values[12] : null,
    region: type === 2 ? values[13] : null,
    costMicrounits: type === 2 ? values[14] : 0n,
    latencyMs: type === 2 ? Number(values[15]) : 0,
    trustLevel: type === 2 ? Number(values[16]) : 0,
    loadPermille: type === 2 ? Number(values[17]) : 0,
    remainingLeaseMs: type === 2 ? Number(values[18]) : 0,
    path: type === 2 ? values[19] : [],
    snapshotId: count === 8 && (type === 5 || type === 6)
      ? values[7] : null,
  };
}

function encodeArpxMessage(message) {
  const type = Number(message.type);
  const count = type === 2 ? 20 : (type === 3 || type === 5 || type === 6) ? 8 : 7;
  const fields = [
    uintHead(5, count),
    uintHead(0, 0), uintHead(0, 1),
    uintHead(0, 1), uintHead(0, type),
    uintHead(0, 2), text(message.routerId),
    uintHead(0, 3), text(message.domainId),
    uintHead(0, 4), uintHead(0, message.epoch),
    uintHead(0, 5), uintHead(0, message.sequence),
    uintHead(0, 6), uintHead(0, message.heartbeatMs || 0),
  ];
  if (type === 3) {
    fields.push(uintHead(0, 7), text(message.routeId));
  } else if (type === 5 || type === 6) {
    fields.push(uintHead(0, 7), uintHead(0, message.snapshotId));
  } else if (type === 2) {
    fields.push(
      uintHead(0, 7), text(message.routeId),
      uintHead(0, 8), text(message.intent),
      uintHead(0, 9), uintHead(0, message.capabilityVersion),
      uintHead(0, 10), text(message.origin),
      uintHead(0, 11), text(message.endpoint),
      uintHead(0, 12), text(message.tenant),
      uintHead(0, 13), text(message.region),
      uintHead(0, 14), uintHead(0, message.costMicrounits),
      uintHead(0, 15), uintHead(0, message.latencyMs),
      uintHead(0, 16), uintHead(0, message.trustLevel),
      uintHead(0, 17), uintHead(0, message.loadPermille),
      uintHead(0, 18), uintHead(0, message.remainingLeaseMs),
      uintHead(0, 19), array(message.path),
    );
  }
  const payload = Buffer.concat(fields);
  if (payload.length + 4 > MAX_FRAME) throw new Error("ARPX frame bound exceeded");
  const frame = Buffer.alloc(payload.length + 4);
  frame.writeUInt32BE(payload.length);
  payload.copy(frame, 4);
  return frame;
}

function encodeArpx(type, routerId, domainId, epoch, sequence, heartbeatMs,
  snapshotId = null) {
  return encodeArpxMessage({
    type, routerId, domainId, epoch, sequence, heartbeatMs, snapshotId,
  });
}

class ArpxDecoder {
  constructor(callback) {
    this.callback = callback;
    this.frame = Buffer.alloc(MAX_FRAME);
    this.used = 0;
    this.expected = 0;
  }

  push(chunk) {
    let offset = 0;
    while (offset < chunk.length) {
      const target = this.expected === 0 ? 4 : this.expected;
      const copied = Math.min(target - this.used, chunk.length - offset);
      chunk.copy(this.frame, this.used, offset, offset + copied);
      this.used += copied;
      offset += copied;
      if (this.expected === 0 && this.used === 4) {
        const payload = this.frame.readUInt32BE(0);
        if (payload < 1 || payload > MAX_FRAME - 4) {
          this.reset();
          throw new Error("ARPX frame bound exceeded");
        }
        this.expected = payload + 4;
      }
      if (this.expected !== 0 && this.used === this.expected) {
        const frame = Buffer.from(this.frame.subarray(0, this.expected));
        this.reset();
        this.callback(decodeArpx(frame));
      }
    }
  }

  reset() {
    this.used = 0;
    this.expected = 0;
  }
}

class ArpxControl {
  constructor(options) {
    this.routerId = options.routerId;
    this.domainId = options.domainId;
    this.peerRouterId = options.peerRouterId;
    this.peerDomainId = options.peerDomainId;
    this.heartbeatMs = options.heartbeatMs;
    this.write = options.write;
    this.onReady = options.onReady;
    this.onRoute = options.onRoute || (() => {});
    this.onSnapshotRequest = options.onSnapshotRequest || null;
    this.onError = options.onError;
    this.bootEpoch = (BigInt(Date.now()) * 1000n + BigInt(process.pid)) &
      0x7fffffffffffffffn;
    this.remoteEpoch = 0n;
    this.expectedSequence = 1n;
    this.localSequence = 0n;
    this.opened = false;
    this.timer = null;
    this.decoder = new ArpxDecoder((message) => this.receive(message));
  }

  push(chunk) {
    try {
      this.decoder.push(chunk);
    } catch (error) {
      this.onError(error);
    }
  }

  receive(message) {
    if (message.version !== 1 || message.routerId !== this.peerRouterId ||
        message.domainId !== this.peerDomainId ||
        message.sequence !== this.expectedSequence) {
      throw new Error("ARPX identity or sequence mismatch");
    }
    if (!this.opened) {
      if (message.type !== 1 || message.sequence !== 1n ||
          message.heartbeatMs < MIN_HEARTBEAT_MS ||
          message.heartbeatMs > MAX_HEARTBEAT_MS) {
        throw new Error("invalid ARPX OPEN");
      }
      this.opened = true;
      this.remoteEpoch = message.epoch;
      this.expectedSequence = 2n;
      this.localSequence = 1n;
      this.write(encodeArpx(1, this.routerId, this.domainId, this.bootEpoch,
        1n, this.heartbeatMs));
      this.timer = setInterval(() => this.sendHeartbeat(), this.heartbeatMs);
      this.timer.unref();
      this.onReady();
      return;
    }
    if (message.epoch !== this.remoteEpoch || message.heartbeatMs !== 0 ||
        message.type < 2 || message.type > 6) {
      throw new Error("invalid established ARPX message");
    }
    this.expectedSequence++;
    if (message.type === 2 || message.type === 3) {
      this.onRoute(message);
    } else if (message.type === 5 && message.snapshotId !== null) {
      if (this.onSnapshotRequest) this.onSnapshotRequest(message.snapshotId);
      else this.sendSnapshotEnd(message.snapshotId);
    }
  }

  sendMessage(message) {
    if (!this.opened) throw new Error("ARPX control is not ready");
    this.localSequence++;
    return this.write(encodeArpxMessage({
      ...message,
      routerId: this.routerId,
      domainId: this.domainId,
      epoch: this.bootEpoch,
      sequence: this.localSequence,
      heartbeatMs: 0,
    }));
  }

  sendRoute(message) {
    return this.sendMessage({ ...message, type: 2 });
  }

  sendWithdraw(routeId) {
    return this.sendMessage({ type: 3, routeId });
  }

  sendSnapshotRequest(snapshotId) {
    if (typeof snapshotId !== "bigint" || snapshotId <= 0n) {
      throw new Error("invalid ARPX snapshot request ID");
    }
    return this.sendMessage({ type: 5, snapshotId });
  }

  sendSnapshotEnd(snapshotId) {
    return this.sendMessage({ type: 6, snapshotId });
  }

  fail(error) {
    this.onError(error);
  }

  sendHeartbeat() {
    try {
      this.localSequence++;
      this.write(encodeArpx(4, this.routerId, this.domainId, this.bootEpoch,
        this.localSequence, 0));
    } catch (error) {
      this.onError(error);
    }
  }

  close() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }
}

module.exports = {
  ArpxControl, ArpxDecoder, decodeArpx, encodeArpx, encodeArpxMessage,
};
