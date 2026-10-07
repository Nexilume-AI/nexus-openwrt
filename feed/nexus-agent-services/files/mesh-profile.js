'use strict';
// Seed-side public descriptor only. Never export private keys or ticket keys.
const fs = require('node:fs');
const { X509Certificate, createHash } = require('node:crypto');
const net = require('node:net');

function normalizePaths(paths, fallback) {
  if (!Array.isArray(paths) || paths.length > 4) throw new Error('INVALID_MESH_PATHS');
  if (!paths.length) paths = [fallback];
  const seen = new Set();
  return paths.map(path => {
    if (!path || Object.keys(path).sort().join(',') !== 'directory_host,directory_port,relay_host,relay_port') throw new Error('INVALID_MESH_PATHS');
    const normalized = {};
    for (const service of ['directory', 'relay']) {
      const host = path[`${service}_host`]; const port = path[`${service}_port`];
      if (typeof host !== 'string' || !host || host.length > 127 ||
          (net.isIP(host) === 0 && !/^(?=.{1,127}$)[a-z0-9]+(?:[a-z0-9.-]*[a-z0-9])?$/.test(host)) ||
          !Number.isInteger(port) || port < 1 || port > 65535) throw new Error('INVALID_MESH_PATHS');
      normalized[`${service}_host`] = host;
      normalized[`${service}_port`] = port;
    }
    const key = JSON.stringify(normalized);
    if (seen.has(key)) throw new Error('DUPLICATE_MESH_PATH');
    seen.add(key); return normalized;
  });
}

function createProfile(paths, directory, ca, directoryCert, relayCert) {
  const relay = directory.relays['open-mesh-relay'];
  const endpoint = new URL(relay.endpoint);
  const fingerprint = pem => createHash('sha256').update(new X509Certificate(pem).raw).digest('hex');
  return {v: 2, seed_id: fingerprint(ca), directory_sha256: fingerprint(directoryCert),
    relay_sha256: fingerprint(relayCert), paths: normalizePaths(paths, {
      directory_host: relay.connectIpv4, directory_port: directory.port,
      relay_host: relay.connectIpv4, relay_port: Number(endpoint.port || 443),
    })};
}

module.exports = { normalizePaths, createProfile };
if (require.main === module) {
  try {
    const input = fs.readFileSync(0, 'utf8');
    if (input.length > 2048) throw new Error('INVALID_MESH_PATHS');
    const root = '/etc/nexus-open-mesh-seed/';
    const profile = createProfile(JSON.parse(input),
      JSON.parse(fs.readFileSync('/etc/nexus-directoryd/directory.json', 'utf8')),
      fs.readFileSync(root + 'ca.pem'), fs.readFileSync(root + 'directory.pem'),
      fs.readFileSync(root + 'relay.pem'));
    process.stdout.write(JSON.stringify(profile));
  } catch (_) { process.stderr.write('Mesh public descriptor is unavailable.\n'); process.exitCode = 1; }
}
