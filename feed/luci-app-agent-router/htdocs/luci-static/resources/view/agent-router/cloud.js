'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require rpc';
'require ui';
'require poll';
'require dom';
'require agent-router.mode as mode';
'require agent-router.cloud-pairing as pairing';

const STATUS_FILE = '/var/run/nexus-cloud/status.json';
const callService = rpc.declare({ object: 'service', method: 'list', params: [ 'name' ], expect: {} });
const callTlsCheck = rpc.declare({ object: 'nexus-cloud', method: 'check', expect: {} });

function serviceRunning(payload) {
	const service = (payload || {})['nexus-cloud'] || {};
	const instances = service.instances || {};
	return Object.keys(instances).some(key => !!instances[key].running);
}

function statusDocument(content) {
	try {
		return JSON.parse(String(content || ''));
	}
	catch (error) {
		return { state: 'not_started', message: _('The connector has not written status yet.') };
	}
}

function badge(label, tone) {
	return E('span', { 'class': 'ar-badge ar-badge-' + tone }, label);
}

function statusTone(state) {
	if (state === 'online' || state === 'enrolled') return 'up';
	if (state === 'connecting' || state === 'pairing_required' || state === 'tls_descriptor_required') return 'warn';
	if (state === 'not_started' || state === 'stopping') return 'info';
	return 'down';
}

function fact(label, value) {
	return E('div', { 'class': 'ar-role-fact' }, [
		E('span', { 'class': 'ar-muted' }, label),
		E('strong', {}, String(value === undefined || value === null || value === '' ? '—' : value))
	]);
}

function recoverySummary(status) {
	const labels = {
		automatic_retry: _('Automatic retry'),
		reauthorize: _('Pair again'),
		supply_pairing_code: _('Pairing link required'),
		replace_pairing_code: _('Pair again'),
		fix_configuration: _('Fix cloud configuration'),
		fix_agent_configuration: _('Fix Agent configuration'),
		enable_relay_or_switch_transport: _('Enable Relay or use Direct IPv6')
	};
	const action = String(status.recovery_action || '');
	const delay = Number(status.next_retry_seconds || 0);
	if (!action)
		return status.state === 'online' ? _('Healthy sync') : '—';
	return (labels[action] || action.replaceAll('_', ' ')) + (delay > 0 ? ' · %ds'.format(delay) : '');
}

function cloudRelayAvailability(status) {
	if (status && typeof status.cloud_relay_available === 'boolean')
		return status.cloud_relay_available;
	if (status && typeof status.relay_available === 'boolean')
		return status.relay_available;
	return null;
}

function cloudRelaySummary(status) {
	const available = cloudRelayAvailability(status);
	if (available === true)
		return _('Available');
	if (available === false)
		return _('Unavailable at last check');
	return _('Not checked');
}

function localRelaySummary(status) {
	if (status && status.relay_ready === true)
		return _('Ready');
	if (status && status.relay_enabled === true)
		return _('Starting');
	return _('Disabled');
}

function checkSymbol(state) {
	if (state === 'pass') return '✓';
	if (state === 'warn') return '!';
	return '✕';
}

function showTlsChecks(result) {
	const checks = Array.isArray(result && result.checks) ? result.checks : [];
	const items = checks.map(check => E('li', { 'class': 'ar-tls-check ar-tls-check-' + check.state }, [
		E('strong', {}, '%s %s'.format(checkSymbol(check.state), check.label || check.id)),
		E('div', { 'class': 'ar-muted' }, check.message || '')
	]));
	ui.showModal(_('Device TLS verification'), [
		E('p', {}, result && result.message ? result.message : _('No verification result was returned.')),
		E('ul', { 'class': 'ar-check-list' }, items),
		E('p', { 'class': 'ar-muted' }, _('This check never reads or displays the device private key or cloud credential. Save & Apply before checking recent edits.')),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Close'))
		])
	]);
}

function cloudStatus(status, running) {
	const state = String(status.state || (running ? 'starting' : 'stopped'));
	return E('section', { 'class': 'ar-role-summary ar-role-surface' }, [
		E('div', { 'class': 'ar-section-heading' }, [
			E('div', {}, [
				E('h2', {}, _('Nexus Cloud connection')),
				E('p', { 'class': 'ar-muted' }, status.message || _('Connect using a pairing link from Nexus Cloud.'))
			]),
			badge(state.replaceAll('_', ' '), statusTone(state))
		]),
		E('div', { 'class': 'ar-role-facts' }, [
			fact(_('Service'), running ? _('Running') : _('Stopped')),
			fact(_('Configured transport'), status.configured_transport || 'auto'),
			fact(_('Active transport'), status.active_transport || '—'),
			fact(_('Cloud Relay'), cloudRelaySummary(status)),
			fact(_('Local Relay'), localRelaySummary(status)),
			fact(_('Relay'), status.relay_id || '—'),
			fact(_('Relay tunnels'), status.relay_tunnels_up || 0),
			fact(_('Registered Agents'), status.registered || 0),
			fact(_('Eligible Agents'), status.eligible || 0),
			fact(_('Snapshot stage'), status.snapshot_error_stage || '—'),
			fact(_('Recovery'), recoverySummary(status)),
			fact(_('Last cloud HTTP status'), status.last_http_code || '—'),
			fact(_('Updated'), status.updated_at || '—')
		])
	]);
}

function cloudMap(status, openPairing) {
	let m, s, o;
	const relayAvailable = cloudRelayAvailability(status);
	m = new form.Map('nexus_cloud', _('Cloud Agent hosting'),
		_('Publish eligible local Agents through Direct IPv6 or Cloud Relay. The pairing link manages the Cloud address and certificate trust.'));
	s = m.section(form.NamedSection, 'main', 'cloud', _('Connection'));
	s.addremove = false;
	o = s.option(form.Flag, 'enabled', _('Connect this router to Nexus Cloud'));
	o.rmempty = false;
	o = s.option(form.DummyValue, '_cloud_origin', _('Managed Cloud address'));
	o.cfgvalue = function() {
		try {
			const url = new URL(uci.get('nexus_cloud', 'main', 'enrollment_url') || uci.get('nexus_cloud', 'main', 'base_url') || '');
			return url.protocol === 'https:' && !url.username && !url.password ? url.origin : _('Not configured');
		}
		catch (ignored) { return _('Not configured'); }
	};
	o.description = _('Read only. To change the Cloud connection, use a new pairing link.');
	o = s.option(form.Button, '_pair_cloud', _('Cloud pairing'));
	o.inputtitle = _('Pair with Nexus Cloud');
	o.inputstyle = 'action';
	o.onclick = openPairing;
	o.description = _('No manual address, port or certificate download is needed.');
	o = s.option(form.Value, 'display_name', _('Router display name'));
	o.placeholder = _('Use system hostname');
	o.rmempty = true;
	o = s.option(form.ListValue, 'connectivity_mode', _('Cloud connectivity'));
	o.value('auto', _('Auto: Direct IPv6, then Relay (recommended)'));
	o.value('direct_ipv6', _('Direct IPv6 only'));
	o.value('relay', _('Relay only (NAT / no IPv6)'));
	o.default = 'auto';
	o.rmempty = false;
	o.description = relayAvailable === false
		? _('Cloud Relay unavailable at the last check. You can still save Relay mode; the connector will re-check the authenticated Cloud Relay directory after applying the change.')
		: _('Relay uses an outbound mTLS tunnel and does not open an inbound WAN port. Direct IPv6 remains preferred when its public TLS descriptor is healthy.');
	o = s.option(form.Button, '_tls_check', _('Connection verification'));
	o.inputtitle = _('Check Device TLS');
	o.inputstyle = 'apply';
	o.onclick = function() {
		return callTlsCheck().then(showTlsChecks).catch(error =>
			ui.addNotification(_('Device TLS check failed'), E('p', {}, error.message || String(error)), 'error'));
	};
	o.description = _('Runs readable certificate, key matching, expiry, Cloud TLS, and JWT/JWKS checks using the saved configuration.');

	s = m.section(form.NamedSection, 'main', 'cloud', _('Lease policy'));
	s.addremove = false;
	o = s.option(form.Value, 'sync_interval_seconds', _('Sync interval'));
	o.datatype = 'range(30,1800)';
	o.default = '120';
	o.rmempty = false;
	o.description = _('Seconds between local snapshot and cloud lease renewal. Must be shorter than the lease.');
	o = s.option(form.Value, 'lease_seconds', _('Cloud lease duration'));
	o.datatype = 'range(60,3600)';
	o.default = '300';
	o.rmempty = false;

	s = m.section(form.NamedSection, 'main', 'cloud', _('Device identity'));
	s.addremove = false;
	// Identity and trust are configured by pairing, not by form defaults. Leaving
	// these UCI options out of the form preserves existing installations on save.
	o = s.option(form.DummyValue, '_identity_management', _('Certificate management'));
	o.cfgvalue = function() {
		return uci.get('nexus_cloud', 'main', 'identity_mode') === 'manual'
			? _('Existing identity. Pair again to enable automatic certificate management.')
			: _('Managed by Nexus Cloud');
	};
	o.description = _('Pairing configures certificates and trust automatically. Managed certificates renew automatically; the private key stays on this Router.');
	return m;
}

return view.extend({
	handleSave: function(ev) {
		return this.super('handleSave', [ev]).then(function() {
			return ui.changes.apply(false);
		}).then(() => {
			this.formEdited = false;
		});
	},
	openPairing: function() {
		// Pairing changes managed UCI fields outside this form. Never let an old
		// form draft overwrite the newly installed identity/trust configuration.
		return uci.changes().then(changes => {
			if (this.formEdited || (changes.nexus_cloud || []).length || (changes.agent_gateway || []).length) {
				ui.addNotification(null, E('p', {}, _('Save & Apply or discard pending configuration changes before pairing.')), 'warning');
				return;
			}
			return pairing.open(() => window.location.reload());
		}).catch(() => ui.addNotification(null, E('p', {}, _('Could not check pending changes. Refresh this page before pairing.')), 'error'));
	},
	loadStatus: function() {
		return Promise.all([
			fs.read(STATUS_FILE).catch(() => ''),
			callService('nexus-cloud').catch(() => ({}))
		]).then(data => ({
			status: statusDocument(data[0]),
			running: serviceRunning(data[1])
		}));
	},

	load() {
		return Promise.all([
			uci.load('nexus_cloud'),
			this.loadStatus()
		]);
	},

	render(data) {
		const status = data[1].status;
		const running = data[1].running;
		return cloudMap(status, () => this.openPairing()).render().then(formNode => {
			formNode.addEventListener('input', () => { this.formEdited = true; });
			formNode.addEventListener('change', () => { this.formEdited = true; });
			this.statusRoot = E('div', {}, [ cloudStatus(status, running) ]);
			poll.add(() => this.loadStatus().then(result => {
				if (this.statusRoot)
					dom.content(this.statusRoot, cloudStatus(result.status, result.running));
			}), 3);
			return E([], [
				E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }),
				mode.render('developer'),
				E('div', { 'class': 'cbi-map ar-shell' }, [ this.statusRoot, formNode ])
			]);
		});
	}
});
