'use strict';

// Developer-facing, allowlisted view of Directory targets. Never return the
// source configuration, certificate paths, identities or signing keys to LuCI.
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const tls = require('node:tls');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');
const { loadConfig } = require('./nexus-directory');
const { validRouteId, validDnsName } = require('../relay/relay-ticket');

function fail(code) { const error = new Error(code); error.code = code; throw error; }
function digest(value) { return crypto.createHash('sha256').update(value).digest('hex'); }
function snapshot(filename, root = '/etc/nexus-directoryd') {
  if (path.dirname(filename) !== root || fs.realpathSync(root) !== root ||
      !/^[a-zA-Z0-9._-]+\.json$/.test(path.basename(filename))) fail('DIRECTORY_CONFIG_UNSAFE');
  const stat = fs.lstatSync(filename);
  if (!stat.isFile() || stat.nlink !== 1 || stat.size > 65536) fail('DIRECTORY_CONFIG_UNSAFE');
  const raw = fs.readFileSync(filename);
  return { raw, stat, config: JSON.parse(raw), revision: digest(raw) };
}

function catalog(config, revision, pinnedSeed) {
  const selected = config.openMesh?.enabled ? config.openMesh.relayIds || [] : [];
  return { revision, open_mesh_enabled: config.openMesh?.enabled === true,
    pinned_seed: pinnedSeed, relays: Object.entries(config.relays || {}).map(([id, relay]) => ({
      id, router_id: relay.routerId, domain_id: relay.domainId,
      endpoint: relay.endpoint, connect_ipv4: relay.connectIpv4,
      open_mesh: selected.includes(id),
      identity_references: Object.values(config.identities || {}).filter(identity =>
        (identity.relayIds || [identity.relayId]).includes(id)).length,
    })) };
}

function update(config, rows, pinnedSeed = false) {
  if (!Array.isArray(rows) || rows.length < 1 || rows.length > 32) fail('INVALID_RELAY_COUNT');
  const relays = Object.create(null), selected = [];
  for (const row of rows) {
    if (!row || typeof row !== 'object' || Array.isArray(row) ||
        Object.keys(row).sort().join(',') !== 'connect_ipv4,domain_id,endpoint,id,open_mesh,router_id' ||
        !validRouteId(row.id) || ['constructor', 'prototype', '__proto__'].includes(row.id) ||
        !validRouteId(row.router_id) || !validDnsName(row.domain_id) ||
        typeof row.endpoint !== 'string' || typeof row.open_mesh !== 'boolean' || typeof row.connect_ipv4 !== 'string' ||
        net.isIP(row.connect_ipv4) !== 4) fail('INVALID_RELAY');
    const match = /^https:\/\/([a-z0-9.-]+):([0-9]{1,5})\/arpx\/v1$/.exec(row.endpoint);
    if (!match || !validDnsName(match[1]) || Number(match[2]) < 1 || Number(match[2]) > 65535 ||
        row.endpoint.length >= 256) fail('INVALID_RELAY_ENDPOINT');
    if (Object.hasOwn(relays, row.id)) fail('DUPLICATE_RELAY');
    relays[row.id] = { routerId: row.router_id, domainId: row.domain_id,
      endpoint: row.endpoint, connectIpv4: row.connect_ipv4 };
    if (row.open_mesh) selected.push(row.id);
  }
  for (const identity of Object.values(config.identities || {})) {
    if ((identity.relayIds || [identity.relayId]).some(id => !Object.hasOwn(relays, id))) fail('RELAY_IN_USE');
  }
  if (config.openMesh?.enabled) {
    if (selected.length < 1 || selected.length > 4) fail('INVALID_MESH_RELAY_COUNT');
  } else if (selected.length) fail('OPEN_MESH_DISABLED');
  // v2 join links pin ONE seed Relay certificate and dial destination. Do not
  // silently assign a different Relay while those clients still dial the seed.
  if (pinnedSeed) {
    const old = config.relays['open-mesh-relay'], next = relays['open-mesh-relay'];
    if (!old || !next || old.routerId !== next.routerId || old.domainId !== next.domainId ||
        old.endpoint !== next.endpoint || old.connectIpv4 !== next.connectIpv4 ||
        JSON.stringify(selected) !== JSON.stringify(config.openMesh?.relayIds || [])) fail('PINNED_SEED_RELAY');
  }
  const result = { ...config, relays };
  if (config.openMesh?.enabled) result.openMesh = { ...config.openMesh, relayIds: selected };
  return result;
}

function restart() {
  const result = spawnSync('/etc/init.d/nexus-directoryd', ['restart'], { stdio: 'ignore', timeout: 8000 });
  if (result.status !== 0) fail('DIRECTORY_RESTART_FAILED');
}
async function healthy(config) {
  const cert = new crypto.X509Certificate(fs.readFileSync(config.tls.cert));
  const servername = cert.subjectAltName?.split(', ').find(x => x.startsWith('DNS:'))?.slice(4);
  if (!servername) return false;
  const host = config.listen === '0.0.0.0' ? '127.0.0.1' : config.listen === '::' ? '::1' : config.listen;
  for (let attempt = 0; attempt < 5; attempt++) {
    await new Promise(resolve => setTimeout(resolve, 300));
    const service = spawnSync('/sbin/ubus', ['-S', 'call', 'service', 'list', '{"name":"nexus-directoryd"}'],
      { encoding: 'utf8', timeout: 2000, maxBuffer: 65536 });
    let running = false;
    try { running = Object.values(JSON.parse(service.stdout)['nexus-directoryd'].instances).some(x => x.running); } catch (_) { /* Not ready. */ }
    if (!running) continue;
    const ready = await new Promise(resolve => {
      const socket = tls.connect({ host, port: config.port, servername,
        ca: fs.readFileSync(config.tls.ca), rejectUnauthorized: true });
      const done = result => { socket.destroy(); resolve(result); };
      socket.setTimeout(1000, () => done(false));
      socket.once('error', () => done(false));
      socket.once('secureConnect', () => done(socket.authorized && digest(socket.getPeerCertificate().raw) === digest(cert.raw)));
    });
    if (ready) return true;
  }
  return false;
}

// The RPC facade serializes managed writes. Revision is independently checked
// here too, including immediately before rename, to detect external edits.
async function apply(filename, request, options = {}) {
  const read = () => snapshot(filename, options.root);
  const before = read();
  if (!request || Object.keys(request).sort().join(',') !== 'relays,revision' ||
      request.revision !== before.revision) fail('CONFIGURATION_CHANGED');
  const next = update(before.config, request.relays, options.pinnedSeed);
  if (JSON.stringify(next) === JSON.stringify(before.config)) return { state: 'unchanged' };
  const temp = `${filename}.${crypto.randomUUID()}.pending`;
  const backup = `${filename}.${crypto.randomUUID()}.backup`;
  const validate = options.validate || loadConfig;
  const reload = options.restart || restart, check = options.healthy || healthy;
  const raw = Buffer.from(JSON.stringify(next, null, 2) + '\n');
  if (raw.length > 65536) fail('DIRECTORY_CONFIG_TOO_LARGE');
  let replaced = false;
  try {
    fs.writeFileSync(temp, raw, { flag: 'wx', mode: 0o600 });
    if (process.platform !== 'win32') fs.chownSync(temp, before.stat.uid, before.stat.gid);
    fs.chmodSync(temp, before.stat.mode & 0o640);
    let validated;
    try { validated = validate(temp); } catch (_) { fail('DIRECTORY_CONFIG_INVALID'); }
    if (read().revision !== before.revision) fail('CONFIGURATION_CHANGED');
    fs.copyFileSync(filename, backup, fs.constants.COPYFILE_EXCL);
    fs.chmodSync(backup, 0o600);
    fs.renameSync(temp, filename); replaced = true;
    if (options.enabled) {
      reload();
      if (!await check(validated)) fail('DIRECTORY_HEALTH_FAILED');
    }
    return { state: options.enabled ? 'ready' : 'saved_stopped' };
  } catch (error) {
    if (replaced) {
      // Never overwrite a third-party edit made while health verification ran.
      if (read().revision !== digest(raw)) fail('ROLLBACK_CONFLICT');
      if (process.platform !== 'win32') fs.chownSync(backup, before.stat.uid, before.stat.gid);
      fs.chmodSync(backup, before.stat.mode & 0o777);
      fs.renameSync(backup, filename);
      if (options.enabled) {
        try { reload(); if (!await check(validate(filename))) fail('ROLLBACK_FAILED'); }
        catch (_) { fail('ROLLBACK_FAILED'); }
      }
      fail('APPLY_FAILED');
    }
    throw error;
  } finally {
    if (fs.existsSync(temp)) fs.unlinkSync(temp);
    // Keep the old protected configuration if an external edit prevented rollback.
    if (fs.existsSync(backup) && (!replaced || read().revision === digest(raw))) fs.unlinkSync(backup);
  }
}

async function main() {
  const [action, filename, enabled] = process.argv.slice(2);
  const before = snapshot(filename);
  const pinnedSeed = filename === '/etc/nexus-directoryd/directory.json' && fs.existsSync('/etc/nexus-open-mesh-seed/ca.pem');
  if (action === 'get') {
    process.stdout.write(JSON.stringify({ ok: true, ...catalog(before.config, before.revision, pinnedSeed) })); return;
  }
  if (action !== 'set') fail('INVALID_REQUEST');
  let input = '';
  for await (const chunk of process.stdin) {
    input += chunk;
    if (Buffer.byteLength(input) > 32768) fail('INVALID_REQUEST');
  }
  const result = await apply(filename, JSON.parse(input), { pinnedSeed, enabled: enabled === '1' });
  process.stdout.write(JSON.stringify({ ok: true, ...result }));
}
module.exports = { catalog, update, snapshot, apply };
if (require.main === module) main().catch(error => {
  const code = typeof error.code === 'string' && /^[A-Z_]+$/.test(error.code) ? error.code : 'DIRECTORY_CONFIG_UNAVAILABLE';
  // No raw exception/configuration data: it may contain paths or secret fields.
  process.stdout.write(JSON.stringify({ ok: false, code }));
});
