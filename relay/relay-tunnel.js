"use strict";

const MAGIC = 0x4e585431;
const VERSION = 1;
const HEADER_SIZE = 24;
const MAX_DATA = 4096;
const MAX_FRAME = HEADER_SIZE + MAX_DATA;
const MIN_WINDOW = 4096;
const MAX_WINDOW = 1048576;
const OPEN_FLAG_FORWARDING_ASSERTION = 0x01;
const OPEN_FLAG_TARGET_AGENT = 0x02;

const TYPE = Object.freeze({
  OPEN: 1,
  ACCEPT: 2,
  DATA: 3,
  END: 4,
  RESET: 5,
  WINDOW_UPDATE: 6,
  PING: 7,
  PONG: 8,
  RESPONSE_START: 9,
});

const RESET = Object.freeze({
  TARGET_UNAVAILABLE: 1,
  STREAM_LIMIT: 2,
  PROTOCOL: 3,
  BACKPRESSURE: 4,
  NODE_DISCONNECTED: 5,
  AUTHORIZATION: 6,
  FEDERATION_HOP_LIMIT: 7,
});

function requireInteger(value, minimum, maximum, name) {
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new Error(`invalid ${name}`);
  }
}

function requireSequence(value) {
  if (typeof value !== "bigint" || value < 1n || value > 0xffffffffffffffffn) {
    throw new Error("invalid sequence");
  }
}

function validText(value, maximum, identifier = false) {
  if (typeof value !== "string" || value.length < 1) return false;
  const bytes = Buffer.from(value, "ascii");
  if (bytes.length !== value.length || bytes.length >= maximum) return false;
  const expression = identifier
    ? /^[A-Za-z0-9._:/@-]+$/
    : /^[\x21-\x7e]+$/;
  return expression.test(value);
}

function putText(parts, value, maximum, identifier = false) {
  if (!validText(value, maximum, identifier)) throw new Error("invalid text field");
  const bytes = Buffer.from(value, "ascii");
  const head = Buffer.alloc(2);
  head.writeUInt16BE(bytes.length);
  parts.push(head, bytes);
}

function readText(payload, cursor, maximum, identifier = false) {
  if (cursor.offset + 2 > payload.length) throw new Error("truncated text length");
  const length = payload.readUInt16BE(cursor.offset);
  cursor.offset += 2;
  if (length < 1 || length >= maximum || cursor.offset + length > payload.length) {
    throw new Error("invalid text length");
  }
  const value = payload.subarray(cursor.offset, cursor.offset + length).toString("ascii");
  cursor.offset += length;
  if (!validText(value, maximum, identifier)) throw new Error("invalid text value");
  return value;
}

function putRegion(parts, value) {
  putText(parts, value, 32, value !== "*");
}

function readRegion(payload, cursor) {
  const value = readText(payload, cursor, 32);
  if (value !== "*" && !validText(value, 32, true)) {
    throw new Error("invalid text value");
  }
  return value;
}

function makeFrame(type, streamId, sequence, payload = Buffer.alloc(0)) {
  requireInteger(type, TYPE.OPEN, TYPE.RESPONSE_START, "type");
  requireInteger(streamId, 0, 0xffffffff, "stream id");
  requireSequence(sequence);
  if (!Buffer.isBuffer(payload) || payload.length > MAX_DATA) {
    throw new Error("invalid payload");
  }
  const frame = Buffer.alloc(HEADER_SIZE + payload.length);
  frame.writeUInt32BE(MAGIC, 0);
  frame[4] = VERSION;
  frame[5] = type;
  frame.writeUInt16BE(0, 6);
  frame.writeUInt32BE(streamId, 8);
  frame.writeUInt32BE(payload.length, 12);
  frame.writeBigUInt64BE(sequence, 16);
  payload.copy(frame, HEADER_SIZE);
  decodeFrame(frame);
  return frame;
}

function encodeOpen(message) {
  requireInteger(message.streamId, 1, 0xffffffff, "stream id");
  requireInteger(message.hopLimit, 1, 32, "hop limit");
  requireInteger(message.maxCostMicrounits, 0, Number.MAX_SAFE_INTEGER,
    "maximum cost");
  requireInteger(message.maxLatencyMs, 1, 0xffffffff, "maximum latency");
  const fixed = Buffer.alloc(16);
  fixed[0] = message.hopLimit;
  fixed[1] = message.streaming ? 1 : 0;
  if (message.forwardingAssertion) fixed[2] |= OPEN_FLAG_FORWARDING_ASSERTION;
  if (message.targetAgent) fixed[2] |= OPEN_FLAG_TARGET_AGENT;
  fixed.writeBigUInt64BE(BigInt(message.maxCostMicrounits), 4);
  fixed.writeUInt32BE(message.maxLatencyMs, 12);
  const parts = [fixed];
  putText(parts, message.targetRouterId, 65, true);
  putText(parts, message.intentClass, 128);
  putText(parts, message.taskId, 65, true);
  putText(parts, message.sourceAgent, 256, true);
  putText(parts, message.tenant, 64, true);
  putRegion(parts, message.region);
  if (message.targetAgent) {
    putText(parts, message.targetAgent, 256, true);
  }
  if (message.forwardingAssertion) {
    putText(parts, message.forwardingAssertion, 2049);
  }
  return makeFrame(TYPE.OPEN, message.streamId, 1n, Buffer.concat(parts));
}

function encodeAccept(streamId, statusCode = 200, creditBytes = MIN_WINDOW) {
  requireInteger(statusCode, 200, 599, "status code");
  requireInteger(creditBytes, MIN_WINDOW, MAX_WINDOW, "credit");
  const payload = Buffer.alloc(6);
  payload.writeUInt16BE(statusCode, 0);
  payload.writeUInt32BE(creditBytes, 2);
  return makeFrame(TYPE.ACCEPT, streamId, 1n, payload);
}

function encodeData(streamId, sequence, data) {
  if (!Buffer.isBuffer(data) || data.length < 1 || data.length > MAX_DATA) {
    throw new Error("invalid DATA payload");
  }
  return makeFrame(TYPE.DATA, streamId, sequence, data);
}

function encodeEnd(streamId, sequence) {
  return makeFrame(TYPE.END, streamId, sequence);
}

function encodeReset(streamId, sequence, code) {
  requireInteger(code, 1, 0xffff, "reset code");
  const payload = Buffer.alloc(2);
  payload.writeUInt16BE(code);
  return makeFrame(TYPE.RESET, streamId, sequence, payload);
}

function encodeWindowUpdate(streamId, sequence, creditBytes) {
  requireInteger(creditBytes, 1, MAX_WINDOW, "credit");
  const payload = Buffer.alloc(4);
  payload.writeUInt32BE(creditBytes);
  return makeFrame(TYPE.WINDOW_UPDATE, streamId, sequence, payload);
}

function encodeResponseStart(streamId, sequence, statusCode = 200) {
  requireInteger(statusCode, 200, 200, "status code");
  const payload = Buffer.alloc(2);
  payload.writeUInt16BE(statusCode);
  return makeFrame(TYPE.RESPONSE_START, streamId, sequence, payload);
}

function encodePing(type, sequence) {
  if (type !== TYPE.PING && type !== TYPE.PONG) throw new Error("invalid ping type");
  return makeFrame(type, 0, sequence);
}

function decodeFrame(frame) {
  if (!Buffer.isBuffer(frame) || frame.length < HEADER_SIZE || frame.length > MAX_FRAME ||
      frame.readUInt32BE(0) !== MAGIC || frame[4] !== VERSION ||
      frame.readUInt16BE(6) !== 0) {
    throw new Error("invalid tunnel frame header");
  }
  const type = frame[5];
  const streamId = frame.readUInt32BE(8);
  const payloadLength = frame.readUInt32BE(12);
  const sequence = frame.readBigUInt64BE(16);
  if (payloadLength !== frame.length - HEADER_SIZE || payloadLength > MAX_DATA ||
      sequence === 0n || type < TYPE.OPEN || type > TYPE.RESPONSE_START) {
    throw new Error("invalid tunnel frame bounds");
  }
  const payload = frame.subarray(HEADER_SIZE);
  const message = { type, streamId, sequence, dataLength: 0 };
  if (type === TYPE.PING || type === TYPE.PONG) {
    if (streamId !== 0 || payload.length !== 0) throw new Error("invalid ping frame");
    return message;
  }
  if (streamId === 0) throw new Error("zero stream id");
  if (type === TYPE.OPEN) {
    if (sequence !== 1n || payload.length < 16 || payload[0] < 1 || payload[0] > 32 ||
        payload[1] > 1 ||
        (payload[2] & ~(OPEN_FLAG_FORWARDING_ASSERTION |
          OPEN_FLAG_TARGET_AGENT)) !== 0 ||
        payload[3] !== 0) {
      throw new Error("invalid OPEN frame");
    }
    message.hopLimit = payload[0];
    message.streaming = payload[1] === 1;
    const cost = payload.readBigUInt64BE(4);
    if (cost > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error("cost is not safely representable");
    message.maxCostMicrounits = Number(cost);
    message.maxLatencyMs = payload.readUInt32BE(12);
    if (message.maxLatencyMs === 0) throw new Error("invalid latency");
    const cursor = { offset: 16 };
    message.targetRouterId = readText(payload, cursor, 65, true);
    message.intentClass = readText(payload, cursor, 128);
    message.taskId = readText(payload, cursor, 65, true);
    message.sourceAgent = readText(payload, cursor, 256, true);
    message.tenant = readText(payload, cursor, 64, true);
    message.region = readRegion(payload, cursor);
    if ((payload[2] & OPEN_FLAG_TARGET_AGENT) !== 0) {
      message.targetAgent = readText(payload, cursor, 256, true);
    } else {
      message.targetAgent = "";
    }
    if ((payload[2] & OPEN_FLAG_FORWARDING_ASSERTION) !== 0) {
      message.forwardingAssertion = readText(payload, cursor, 2049);
    } else {
      message.forwardingAssertion = "";
    }
    if (cursor.offset !== payload.length) throw new Error("trailing OPEN payload");
  } else if (type === TYPE.ACCEPT) {
    if (sequence !== 1n || payload.length !== 6) throw new Error("invalid ACCEPT frame");
    message.statusCode = payload.readUInt16BE(0);
    message.creditBytes = payload.readUInt32BE(2);
    if (message.statusCode < 200 || message.statusCode > 599 ||
        message.creditBytes < MIN_WINDOW || message.creditBytes > MAX_WINDOW) {
      throw new Error("invalid ACCEPT values");
    }
  } else if (type === TYPE.DATA) {
    if (payload.length < 1) throw new Error("empty DATA frame");
    message.data = payload;
    message.dataLength = payload.length;
  } else if (type === TYPE.END) {
    if (payload.length !== 0) throw new Error("invalid END frame");
  } else if (type === TYPE.RESET) {
    if (payload.length !== 2 || payload.readUInt16BE(0) === 0) {
      throw new Error("invalid RESET frame");
    }
    message.resetCode = payload.readUInt16BE(0);
  } else if (type === TYPE.WINDOW_UPDATE) {
    if (payload.length !== 4) throw new Error("invalid WINDOW_UPDATE frame");
    message.creditBytes = payload.readUInt32BE(0);
    if (message.creditBytes < 1 || message.creditBytes > MAX_WINDOW) {
      throw new Error("invalid window credit");
    }
  } else if (type === TYPE.RESPONSE_START) {
    if (payload.length !== 2 || payload.readUInt16BE(0) !== 200) {
      throw new Error("invalid RESPONSE_START frame");
    }
    message.statusCode = 200;
  }
  return message;
}

function rewriteStreamId(frame, streamId) {
  requireInteger(streamId, 1, 0xffffffff, "stream id");
  decodeFrame(frame);
  const output = Buffer.from(frame);
  output.writeUInt32BE(streamId, 8);
  return output;
}

function decrementOpenHopLimit(frame) {
  const message = decodeFrame(frame);
  if (message.type !== TYPE.OPEN || message.hopLimit <= 1) {
    throw new Error("federation hop limit exhausted");
  }
  const output = Buffer.from(frame);
  output[HEADER_SIZE] = message.hopLimit - 1;
  return output;
}

class FrameDecoder {
  constructor(callback) {
    if (typeof callback !== "function") throw new Error("callback is required");
    this.callback = callback;
    this.frame = Buffer.alloc(MAX_FRAME);
    this.used = 0;
    this.expected = 0;
  }

  push(chunk) {
    if (!Buffer.isBuffer(chunk)) throw new Error("chunk must be a Buffer");
    let offset = 0;
    while (offset < chunk.length) {
      const target = this.expected === 0 ? HEADER_SIZE : this.expected;
      const copied = Math.min(target - this.used, chunk.length - offset);
      chunk.copy(this.frame, this.used, offset, offset + copied);
      this.used += copied;
      offset += copied;
      if (this.expected === 0 && this.used === HEADER_SIZE) {
        if (this.frame.readUInt32BE(0) !== MAGIC || this.frame[4] !== VERSION) {
          this.reset();
          throw new Error("invalid tunnel ingress header");
        }
        const payload = this.frame.readUInt32BE(12);
        if (payload > MAX_DATA) {
          this.reset();
          throw new Error("tunnel ingress frame too large");
        }
        this.expected = HEADER_SIZE + payload;
      }
      if (this.expected !== 0 && this.used === this.expected) {
        const complete = Buffer.from(this.frame.subarray(0, this.expected));
        this.reset();
        this.callback(complete, decodeFrame(complete));
      }
    }
  }

  reset() {
    this.used = 0;
    this.expected = 0;
  }
}

module.exports = {
  MAGIC, VERSION, HEADER_SIZE, MAX_DATA, MAX_FRAME, MIN_WINDOW, MAX_WINDOW,
  TYPE, RESET, FrameDecoder, decodeFrame, rewriteStreamId,
  decrementOpenHopLimit, encodeOpen,
  encodeAccept, encodeData, encodeEnd, encodeReset, encodeWindowUpdate,
  encodeResponseStart, encodePing,
};
