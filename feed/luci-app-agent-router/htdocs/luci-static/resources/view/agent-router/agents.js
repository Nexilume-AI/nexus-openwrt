'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require uci';
'require ui';
'require agent-router.mode as mode';

const LIST_LIMIT = 200;
const callAgents = rpc.declare({ object: 'agent', method: 'agents', params: [ 'limit' ], expect: {} });
const callAddresses = rpc.declare({ object: 'agent', method: 'addresses', expect: {} });

function badge(agent) {
	return E('span', { 'class': 'ar-badge ' + (agent.healthy ? 'ar-badge-up' : 'ar-badge-down') },
		agent.healthy ? _('Active lease') : _('Unhealthy lease'));
}

function seconds(milliseconds) {
	return Math.max(0, Math.round(Number(milliseconds || 0) / 1000));
}

function copyDescriptor(agent, capability, address, routedPrefix, descriptor) {
	const payload = {
		version: 1,
		agent_uri: agent.agent_id,
		intent: capability.intent,
		tenant: agent.tenant,
		route_id: capability.route_id,
		scheme: descriptor.scheme,
		address: address.public_ipv6,
		ipv6_prefix: routedPrefix,
		port: Number(descriptor.port || 7443),
	};
	if (descriptor.scheme === 'https') {
		payload.tls_server_name = descriptor.tlsServerName;
		payload.ca_bundle_id = descriptor.caBundleId;
	}
	const text = JSON.stringify(payload, null, 2);
	if (!navigator.clipboard || !navigator.clipboard.writeText) {
		ui.showModal(_('Direct IPv6 connection descriptor'), [
			E('p', {}, _('Clipboard access is unavailable. Select and copy this JSON manually.')),
			E('pre', { 'class': 'ar-code-wrap' }, text)
		]);
		return;
	}
	navigator.clipboard.writeText(text).then(function() {
		ui.addNotification(null, E('p', {}, _('Direct connection descriptor copied.')));
	});
}

function capabilityDetails(agent, addresses, routedPrefix, descriptor) {
	const capabilities = agent.capabilities || [];
	return E('details', { 'class': 'ar-route-details' }, [
		E('summary', {}, _('%s capability route(s)').format(agent.capability_count || capabilities.length)),
		capabilities.length ? E('ul', { 'class': 'ar-capability-list' }, capabilities.map(function(capability) {
			const address = addresses[capability.route_id];
			return E('li', {}, [
				E('code', {}, capability.intent || '—'),
				E('span', { 'class': 'ar-muted' }, _(' v%s · %s · %ss remaining').format(
					capability.version || 0, capability.healthy ? _('healthy') : _('unhealthy'),
					seconds(capability.remaining_ms))),
				descriptor.ready && address ? E('button', {
					'class': 'btn cbi-button cbi-button-neutral',
					'click': function(event) {
						event.preventDefault();
						copyDescriptor(agent, capability, address, routedPrefix, descriptor);
					}
				}, _('Copy IPv6 connection')) : ''
			]);
		})) : E('p', { 'class': 'ar-muted' }, _('No active capabilities were returned.')),
		agent.capabilities_truncated ? E('p', { 'class': 'ar-muted' }, _('This capability list is truncated.')) : '',
		E('details', { 'class': 'ar-raw' }, [
			E('summary', {}, _('Show native Agent lease metadata')),
			E('pre', {}, JSON.stringify(agent, null, 2))
		])
	]);
}

function matches(agent, query, state) {
	const haystack = [ agent.agent_id, agent.endpoint, agent.tenant ]
		.concat((agent.capabilities || []).map(item => item.intent)).join(' ').toLowerCase();
	return (!query || haystack.indexOf(query) >= 0) &&
		(!state || (state === 'healthy' ? !!agent.healthy : !agent.healthy));
}

return view.extend({
	load() {
		return Promise.all([ callAgents(LIST_LIMIT), callAddresses(), uci.load('agent_gateway') ])
			.then(data => ({
				ok: true,
				result: data[0],
				addresses: data[1] || {},
				descriptor: {
					enabled: uci.get('agent_gateway', 'main', 'public_descriptor_enabled') === '1',
					tlsServerName: uci.get('agent_gateway', 'main', 'public_tls_server_name') || '',
					caBundleId: uci.get('agent_gateway', 'main', 'public_ca_bundle_id') || '',
					port: uci.get('agent_gateway', 'main', 'public_ingress_port') || '7443',
					scheme: uci.get('agent_gateway', 'main', 'public_transport') === 'http' ? 'http' : 'https'
				}
			}), error => ({ ok: false, error: error }));
	},

	refreshView() {
		if (this.root && this.payload) dom.content(this.root, this.renderBody(this.payload));
	},

	renderBody(payload) {
		if (!payload.ok) return [
			E('div', { 'class': 'ar-hero' }, E('div', {}, [
				E('h2', {}, _('Local Agents')),
				E('p', {}, _('Agents that registered capability leases on this router.'))
			])),
			E('div', { 'class': 'alert-message error ar-status-message' }, [
				E('strong', {}, _('Could not read local Agent leases.')),
				E('div', {}, payload.error && payload.error.message ? payload.error.message : String(payload.error || _('Unknown RPC error')))
			])
		];

		const result = payload.result || { agents: [] };
		const agents = result.agents || [];
		const addressMap = {};
		const routedPrefix = (payload.addresses || {}).prefix || '';
		const descriptor = payload.descriptor || {};
		descriptor.ready = !!(descriptor.enabled &&
			(descriptor.scheme === 'http' || (descriptor.tlsServerName && descriptor.caBundleId)));
		((payload.addresses || {}).addresses || []).forEach(function(address) {
			addressMap[address.route_id] = address;
		});
		const query = String(this.query || '').trim().toLowerCase();
		const filtered = agents.filter(agent => matches(agent, query, this.stateFilter || ''));
		return [
			E('div', { 'class': 'ar-hero' }, [
				E('div', {}, [
					E('h2', {}, _('Local Agents')),
					E('p', {}, _('Shows terminal Agents with active local capability leases, grouped by Agent identity, endpoint and tenant.'))
				]),
				E('span', { 'class': 'ar-phase' }, _('%s Agent(s)').format(agents.length))
			]),
			E('div', { 'class': 'alert-message notice ar-status-message' }, [
				E('strong', {}, _('What “connected” means here')),
				E('div', {}, result.transport_connection_tracked === false
					? _('The router tracks renewable capability leases, not a permanent application socket. An Agent disappears after all of its leases expire or are withdrawn.')
					: _('The router also tracks the Agent transport session.'))
			]),
			descriptor.enabled && !descriptor.ready ? E('div', { 'class': 'alert-message warning ar-status-message' },
				_('HTTPS descriptor publishing is enabled but the TLS certificate identity or CA bundle label is missing. Complete Public Agent IPv6 settings before sharing an address.')) : '',
			result.truncated ? E('div', { 'class': 'alert-message warning ar-status-message' },
				_('The Agent list is truncated at %s entries.').format(LIST_LIMIT)) : '',
			E('div', { 'class': 'ar-toolbar ar-toolbar-short' }, [
				E('input', { 'type': 'search', 'class': 'cbi-input-text',
					'placeholder': _('Search Agent, tenant, endpoint or capability'), 'value': this.query || '',
					'input': L.bind(function(event) { this.query = event.target.value; this.refreshView(); }, this) }),
				E('select', { 'class': 'cbi-input-select',
					'change': L.bind(function(event) { this.stateFilter = event.target.value; this.refreshView(); }, this) }, [
					E('option', { 'value': '', 'selected': !this.stateFilter }, _('All lease states')),
					E('option', { 'value': 'healthy', 'selected': this.stateFilter === 'healthy' }, _('Healthy only')),
					E('option', { 'value': 'unhealthy', 'selected': this.stateFilter === 'unhealthy' }, _('Unhealthy only'))
				])
			]),
			E('p', { 'class': 'ar-muted' }, _('%s of %s visible · generation %s · refreshed %s').format(
				filtered.length, agents.length, result.generation || 0, new Date().toLocaleTimeString())),
			E('div', { 'class': 'cbi-section ar-table-wrap' }, E('table', { 'class': 'ar-table' }, [
				E('thead', {}, E('tr', {}, [
					E('th', {}, _('Agent')), E('th', {}, _('Tenant / endpoint')),
					E('th', {}, _('Lease state')), E('th', {}, _('Capabilities'))
				])),
				E('tbody', {}, filtered.length ? filtered.map(function(agent) {
					return E('tr', {}, [
						E('td', {}, E('code', { 'class': 'ar-code-wrap' }, agent.agent_id || '—')),
						E('td', {}, [ agent.tenant || '—', E('div', { 'class': 'ar-muted ar-code-wrap' }, agent.endpoint || '—') ]),
						E('td', {}, [ badge(agent), E('div', { 'class': 'ar-muted' },
							_('%ss until earliest expiry').format(seconds(agent.earliest_remaining_ms))) ]),
						E('td', {}, capabilityDetails(agent, addressMap, routedPrefix, descriptor))
					]);
				}) : E('tr', {}, E('td', { 'colspan': 4, 'class': 'ar-muted' },
					agents.length ? _('No Agents match the active filters.') : _('No terminal Agent currently has an active local capability lease.'))))
			]))
		];
	},

	render(payload) {
		this.payload = payload;
		this.root = E('div', { 'class': 'cbi-map ar-shell' }, this.renderBody(payload));
		poll.add(L.bind(function() {
			return this.load().then(L.bind(function(next) { this.payload = next; this.refreshView(); }, this));
		}, this), 5);
		return E([], [ E('link', { 'rel': 'stylesheet', 'href': L.resource('agent-router/agent-router.css') + '?v=#PKG_VERSION' }), mode.render('developer'), this.root ]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
