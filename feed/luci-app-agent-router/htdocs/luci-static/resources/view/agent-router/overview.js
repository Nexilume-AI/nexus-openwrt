'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require agent-router.mode as mode';

const callStats = rpc.declare({ object: 'agent', method: 'stats', expect: {} });
const callRecovery = rpc.declare({ object: 'agent', method: 'recovery', expect: {} });
const callPolicy = rpc.declare({ object: 'agent', method: 'policy', expect: {} });
const callAddresses = rpc.declare({ object: 'agent', method: 'addresses', expect: {} });

function safe(value, fallback) {
	return value === null || value === undefined || value === '' ? (fallback || '—') : value;
}

function badge(ok, yes, no) {
	return E('span', { 'class': 'ar-badge ' + (ok ? 'ar-badge-up' : 'ar-badge-down') }, ok ? yes : no);
}

function card(label, value, note, state) {
	return E('div', { 'class': 'ar-card' }, [
		E('div', { 'class': 'ar-card-label' }, label),
		E('div', { 'class': 'ar-card-value ' + (state || '') }, String(safe(value, '0'))),
		E('div', { 'class': 'ar-card-note' }, note)
	]);
}

function settle(name, promise, fallback) {
	return promise.then(data => ({ name: name, ok: true, data: data }), error => ({
		name: name, ok: false, data: fallback,
		error: error && error.message ? error.message : String(error)
	}));
}

function domainRows(recovery) {
	const domains = recovery.configuration_domains || {};
	return Object.keys(domains).map(function(name) {
		const item = domains[name] || {};
		return E('tr', {}, [
			E('td', {}, E('code', {}, name)),
			E('td', {}, badge(!!item.configuration_ok, _('Healthy'), _('Degraded'))),
			E('td', {}, String(safe(item.reload_attempts, 0))),
			E('td', {}, String(safe(item.failures, 0))),
			E('td', { 'class': 'ar-muted' }, safe(item.last_error))
		]);
	});
}

return view.extend({
	load() {
		return Promise.all([
			settle('stats', callStats(), {}),
			settle('recovery', callRecovery(), {}),
			settle('policy', callPolicy(), {}),
			settle('public IPv6', callAddresses(), {})
		]);
	},

	renderBody(data) {
		const stats = data[0].data || {};
		const recovery = data[1].data || {};
		const policy = data[2].data || {};
		const addresses = data[3].data || {};
		const failures = data.filter(item => !item.ok);
		const recoveryRows = domainRows(recovery);

		return [
			E('div', { 'class': 'ar-hero' }, [
				E('div', {}, [
					E('h2', {}, _('Nexus Agent Routing Plane')),
					E('p', {}, _('Operational view of the capability ARIB/AFIB. Linux IP routing remains independently managed by netifd, routing protocols and fw4.'))
				])
			])
		].concat(failures.map(function(item) {
			return E('div', { 'class': 'alert-message error ar-status-message' }, [
				E('strong', {}, _('%s status is unavailable').format(item.name)),
				E('div', {}, item.error)
			]);
		})).concat([
			E('div', { 'class': 'ar-grid' }, [
				card(_('AFIB routes'), safe(stats.routes, 0), _('Capacity %s').format(safe(stats.max_routes, '—')), stats.routes > 0 ? 'ar-ok' : ''),
				card(_('ARPX sessions'), safe(stats.peer_sessions_up, 0), _('%s configured peers').format(safe(stats.configured_peers, 0)), stats.peer_sessions_up > 0 ? 'ar-ok' : ''),
				card(_('LAN candidates'), safe(stats.discovery_candidates, 0), stats.lan_discovery_enabled ? _('Discovery enabled') : _('Discovery disabled'), stats.lan_discovery_enabled ? 'ar-ok' : 'ar-warn'),
				card(_('Relay tunnels'), safe(stats.relay_tunnels_up, 0), stats.relay_assignment_active ? _('Assignment active') : _('No active assignment'), stats.relay_tunnels_up > 0 ? 'ar-ok' : ''),
				card(_('Public Agent IPv6'), safe(addresses.active, 0), addresses.enabled ? _('%s address capacity').format(safe(addresses.capacity, 0)) : _('Disabled'), addresses.enabled ? 'ar-ok' : '')
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Control-plane health')),
				E('div', { 'class': 'ar-grid' }, [
					card(_('Recovery'), recovery.degraded ? _('Degraded') : _('Healthy'), _('%s configuration failures').format(safe(recovery.configuration_failures, 0)), recovery.degraded ? 'ar-bad' : 'ar-ok'),
					card(_('Route index'), stats.route_index_enabled ? _('Enabled') : _('Disabled'), _('%s buckets').format(safe(stats.route_index_buckets, 0)), stats.route_index_enabled ? 'ar-ok' : 'ar-bad'),
					card(_('Policy RIB'), safe(policy.rules, safe(policy.rule_count, 0)), _('Default: %s').format(safe(policy.default_action, 'allow')), 'ar-ok'),
					card(_('Route memory'), '%s KiB'.format(Math.round(Number(stats.route_memory_bytes || 0) / 1024)), _('Bounded AFIB allocation'), '')
				])
			]),
			E('div', { 'class': 'cbi-section ar-table-wrap' }, [
				E('h3', {}, _('Recovery domains')),
				E('table', { 'class': 'ar-table' }, [
					E('thead', {}, E('tr', {}, [E('th', {}, _('Domain')), E('th', {}, _('Status')), E('th', {}, _('Reloads')), E('th', {}, _('Failures')), E('th', {}, _('Last error'))])),
					E('tbody', {}, recoveryRows.length ? recoveryRows : E('tr', {}, E('td', { 'colspan': 5, 'class': 'ar-muted' }, _('No recovery telemetry available.'))))
				])
			]),
			E('p', { 'class': 'ar-muted' }, _('Updated %s. This page polls bounded metadata only. It does not read prompts, tool arguments, model output, access tokens or task payloads.').format(new Date().toLocaleTimeString()))
		]);
	},

	render(data) {
		const root = E('div', { 'class': 'cbi-map ar-shell' }, this.renderBody(data));
		poll.add(L.bind(function() {
			return this.load().then(L.bind(function(next) {
				dom.content(root, this.renderBody(next));
			}, this));
		}, this), 5);
		return E([], [
			E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
			mode.render('developer'),
			root
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
